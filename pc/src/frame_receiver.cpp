#include "frame_receiver.h"

#include <libusb.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "pixel_format.h"
#include "usb_link.h"

namespace {

constexpr uint32_t kMaxFrameBytes = 1024 * 1024;
constexpr uint32_t kMaxAudioBytes = 64 * 1024;

// Counters for one stats window.
struct Window {
    uint64_t start_us = 0;
    uint32_t first_id = 0;
    uint32_t last_id = 0;
    uint32_t frames = 0;
    uint64_t bytes = 0;
    uint64_t audio_bytes = 0;
};

// Prints what the PSP's sound hooks saw since the previous report: per source,
// calls and captured blocks per second (PSP clock), failed calls, block length,
// the functions used and the longest pause between calls.
void print_audio_diag(const pspcap_audio_diag &d, const pspcap_audio_diag &prev, bool have_prev)
{
    const double secs = have_prev ? (d.psp_time_us - prev.psp_time_us) / 1e6 : 0;
    if (secs <= 0)
        return;
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char stamp[16];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
    std::string line;
    char buf[192];
    for (int i = 0; i < 10; i++) {
        const pspcap_audio_source_diag &s = d.src[i];
        const pspcap_audio_source_diag &p = prev.src[i];
        const uint32_t calls = s.calls - p.calls;
        if (!calls && !s.funcs)
            continue;
        std::snprintf(buf, sizeof(buf),
                      " %d: calls %.2f/s, failed +%u (last 0x%08X), captured %.2f/s, len %u-%u, "
                      "ret %d, fn 0x%X, gap %.1f ms |",
                      i, calls / secs, s.failed - p.failed, s.last_error,
                      (s.captured - p.captured) / secs, s.min_samples, s.max_samples, s.last_ret,
                      s.funcs, s.max_gap_us / 1000.0);
        line += buf;
    }
    std::printf("[%s] psp audio over %.1f s:%s lost +%u, ring %u B\n", stamp, secs, line.c_str(),
                d.lost - prev.lost, d.ring_used);
}

} // namespace

FrameReceiver::~FrameReceiver()
{
    stop();
}

void FrameReceiver::start()
{
    if (thread_.joinable())
        return;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
}

void FrameReceiver::stop()
{
    quit_ = true;
    if (thread_.joinable())
        thread_.join();
}

bool FrameReceiver::take_frame(std::vector<uint8_t> &rgba, int &width, int &height)
{
    std::lock_guard<std::mutex> lock(mu_);
    if (!fresh_)
        return false;
    rgba.swap(latest_);
    width = width_;
    height = height_;
    fresh_ = false;
    return true;
}

FrameReceiver::Stats FrameReceiver::stats()
{
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

pspcap_stream_request FrameReceiver::request()
{
    std::lock_guard<std::mutex> lock(mu_);
    return req_;
}

void FrameReceiver::set_options(uint32_t flags, uint32_t priority, uint32_t yield_us)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        req_ = {flags, priority, yield_us};
    }
    options_changed_ = true;
}

void FrameReceiver::set_status(bool connected, const std::string &status)
{
    std::lock_guard<std::mutex> lock(mu_);
    stats_.connected = connected;
    stats_.status = status;
    if (!connected) {
        stats_.game_fps = stats_.recv_fps = stats_.mib_s = 0;
        stats_.skipped = 0;
    }
}

void FrameReceiver::run()
{
    UsbLink link;
    std::string err;
    if (!link.init(err)) {
        set_status(false, err);
        return;
    }

    std::vector<uint8_t> raw(kMaxFrameBytes);
    std::vector<uint8_t> audio(kMaxAudioBytes);
    std::vector<uint8_t> rgba;
    std::vector<uint8_t> payload;
    Window win;
    uint32_t prev_id = 0;
    uint64_t skipped = 0;
    uint64_t last_packet_us = 0;
    uint64_t last_stats_us = 0;
    uint64_t audio_blocks = 0;
    pspcap_audio_diag prev_diag{};
    bool have_diag = false;

    auto disconnect = [&](const std::string &why) {
        link.close();
        set_status(false, why);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    };

    while (!quit_) {
        if (!link.is_open()) {
            if (link.open(err) != UsbLink::OpenResult::Ok) {
                set_status(false, err);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            pspcap_header hdr{};
            pspcap_hello hello{};
            if (!link.handshake(hdr, hello, err)) {
                disconnect(err);
                continue;
            }
            std::vector<uint8_t> reply;
            options_changed_ = false;
            const pspcap_stream_request req = request();
            if (link.request(PSPCAP_CMD_STREAM_START, &req, sizeof(req), PSPCAP_PKT_STATS, hdr,
                             reply, 1000, err) != 0) {
                disconnect("STREAM_START failed: " + err);
                continue;
            }
            set_status(true, "streaming");
            have_diag = false;
            win = Window{};
            win.start_us = now_us();
            prev_id = 0;
            skipped = 0;
            last_packet_us = now_us();
        }

        if (options_changed_.exchange(false)) {
            // The PSP takes a repeated START as an options change; its STATS
            // reply is handled below like any other.
            const pspcap_stream_request req = request();
            link.send(PSPCAP_CMD_STREAM_START, &req, sizeof(req));
        }

        pspcap_header hdr{};
        int rc = link.recv(hdr, payload, 500);
        if (rc == LIBUSB_ERROR_TIMEOUT) {
            // Quiet for a while: either the game stopped flipping (loading) or
            // the PSP gave up streaming. Ask; the STATS reply tells which.
            if (now_us() - last_packet_us > 2000000) {
                link.send(PSPCAP_CMD_STATS, nullptr, 0);
                last_packet_us = now_us();
            }
            continue;
        }
        if (rc != 0) {
            disconnect(std::string("connection lost (") + usb_error_name(rc) + ")");
            continue;
        }
        last_packet_us = now_us();
        if (last_packet_us - last_stats_us > 2000000) {
            link.send(PSPCAP_CMD_STATS, nullptr, 0);
            last_stats_us = last_packet_us;
        }
        if (hdr.type == PSPCAP_PKT_AUDIO && payload.size() == sizeof(pspcap_audio_info)) {
            pspcap_audio_info ai{};
            std::memcpy(&ai, payload.data(), sizeof(ai));
            if (ai.bytes > kMaxAudioBytes || ai.bytes != ai.samples * ai.channels * 2) {
                disconnect("implausible audio header");
                continue;
            }
            int got = 0;
            rc = link.read_raw(audio.data(), static_cast<int>(ai.bytes), got, 2000);
            if (rc != 0 || got != static_cast<int>(ai.bytes)) {
                disconnect("audio data cut short");
                continue;
            }
            audio_blocks++;
            if (audio_sink_)
                audio_sink_(ai, reinterpret_cast<const int16_t *>(audio.data()));
            win.audio_bytes += ai.bytes;
            continue;
        }
        if (hdr.type == PSPCAP_PKT_AUDIO_DIAG && payload.size() == sizeof(pspcap_audio_diag)) {
            pspcap_audio_diag d{};
            std::memcpy(&d, payload.data(), sizeof(d));
            print_audio_diag(d, prev_diag, have_diag);
            prev_diag = d;
            have_diag = true;
            continue;
        }
        if (hdr.type == PSPCAP_PKT_STATS && payload.size() == sizeof(pspcap_stats)) {
            pspcap_stats st{};
            std::memcpy(&st, payload.data(), sizeof(st));
            {
                std::lock_guard<std::mutex> lock(mu_);
                stats_.psp_audio_dropped = st.audio_dropped;
                stats_.psp_audio_sent = st.audio_sent;
                stats_.pc_audio_blocks = audio_blocks;
            }
            if (!st.streaming) {
                const pspcap_stream_request req = request();
                link.send(PSPCAP_CMD_STREAM_START, &req, sizeof(req));
            }
            continue;
        }
        if (hdr.type != PSPCAP_PKT_FRAME_BEGIN || payload.size() != sizeof(pspcap_frame_info))
            continue;

        pspcap_frame_info info{};
        std::memcpy(&info, payload.data(), sizeof(info));
        const unsigned bpp = psp_bytes_per_pixel(info.pixel_format);
        if (!bpp || info.row_pixels < info.width ||
            info.bytes != info.row_pixels * info.height * bpp || info.bytes > kMaxFrameBytes) {
            disconnect("implausible frame header");
            continue;
        }

        uint32_t received = 0;
        bool broken = false;
        while (received < info.bytes) {
            int got = 0;
            int want = static_cast<int>(info.bytes - received);
            rc = link.read_raw(raw.data() + received, want, got, 2000);
            if (rc != 0 || got != want) {
                broken = true; // a short read is the PSP aborting; resync from scratch
                break;
            }
            received += static_cast<uint32_t>(got);
        }
        pspcap_frame_end end{};
        if (!broken) {
            if (link.recv(hdr, payload, 2000) != 0 || hdr.type != PSPCAP_PKT_FRAME_END ||
                payload.size() != sizeof(end))
                broken = true;
            else
                std::memcpy(&end, payload.data(), sizeof(end));
        }
        if (broken || end.status != 0) {
            disconnect(broken ? "frame data cut short" : "PSP failed to send a frame");
            continue;
        }

        rgba.resize(static_cast<size_t>(info.width) * info.height * 4);
        psp_frame_to_rgba(info.pixel_format, raw.data(), info.width, info.height, info.row_pixels,
                          rgba.data());

        if (prev_id && info.frame_id > prev_id + 1)
            skipped += info.frame_id - prev_id - 1;
        prev_id = info.frame_id;
        if (!win.first_id)
            win.first_id = info.frame_id;
        win.last_id = info.frame_id;
        win.frames++;
        win.bytes += info.bytes;

        uint64_t now = now_us();
        std::lock_guard<std::mutex> lock(mu_);
        latest_.swap(rgba);
        width_ = static_cast<int>(info.width);
        height_ = static_cast<int>(info.height);
        fresh_ = true;
        stats_.skipped = skipped;
        stats_.pixel_format = info.pixel_format;
        stats_.psp_send_ms = end.elapsed_us / 1000.0;
        stats_.psp_copy_ms = end.copy_us / 1000.0;
        if (now - win.start_us >= 1000000) {
            double dt = (now - win.start_us) / 1e6;
            stats_.recv_fps = win.frames / dt;
            stats_.game_fps = (win.last_id - win.first_id + 1) / dt;
            stats_.mib_s = win.bytes / (1024.0 * 1024.0) / dt;
            stats_.audio_kib_s = win.audio_bytes / 1024.0 / dt;
            win = Window{};
            win.start_us = now;
        }
    }

    if (link.is_open())
        link.send(PSPCAP_CMD_STREAM_STOP, nullptr, 0);
}
