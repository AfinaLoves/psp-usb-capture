#include "commands.h"

#include <libusb.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "image_io.h"
#include "pixel_format.h"
#include "usb_link.h"

namespace {

void print_hello(const pspcap_header &hdr, const pspcap_hello &hello)
{
    char build[sizeof(hello.build) + 1] = {};
    std::memcpy(build, hello.build, sizeof(hello.build));
    std::printf("PSP: pspcap %u.%u.%u (%s), protocol v%u, HEN 0x%08X minor %u, USB %s speed\n",
                (hello.plugin_version >> 16) & 0xFF, (hello.plugin_version >> 8) & 0xFF,
                hello.plugin_version & 0xFF, build, hdr.version, hello.hen_version,
                hello.hen_minor, hello.usb_speed == 2 ? "high" : "full");
}

// Exchanges HELLO on an open link. Prints errors; closes the link on failure.
bool handshake(UsbLink &link)
{
    pspcap_header hdr{};
    pspcap_hello hello{};
    std::string err;
    bool ok = link.handshake(hdr, hello, err);
    if (hdr.type == PSPCAP_PKT_HELLO)
        print_hello(hdr, hello);
    if (!ok) {
        std::fprintf(stderr, "%s\n", err.c_str());
        link.close();
    }
    return ok;
}

// Opens the link and exchanges HELLO. Prints errors; returns false on failure.
bool connect(UsbLink &link)
{
    std::string err;
    if (link.open(err) != UsbLink::OpenResult::Ok) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return false;
    }
    return handshake(link);
}

// One ping; returns the round trip in microseconds, or -1.
int64_t ping(UsbLink &link, std::string &err)
{
    pspcap_ping req{now_us()};
    pspcap_header hdr{};
    std::vector<uint8_t> reply;
    if (link.request(PSPCAP_CMD_PING, &req, sizeof(req), PSPCAP_PKT_PONG, hdr, reply, 1000, err) != 0)
        return -1;
    uint64_t t1 = now_us();
    pspcap_ping echo{};
    if (reply.size() != sizeof(echo)) {
        err = "PONG has a wrong size";
        return -1;
    }
    std::memcpy(&echo, reply.data(), sizeof(echo));
    if (echo.pc_time_us != req.pc_time_us) {
        err = "PONG echoes a different ping";
        return -1;
    }
    return static_cast<int64_t>(t1 - req.pc_time_us);
}

// A short read in the middle of raw data is the PSP giving up early: it sends
// the closing packet right away. Returns true and fills `out` if `buf` holds
// exactly such a packet of `type`.
template <typename T>
bool parse_early_packet(const uint8_t *buf, int got, uint8_t type, T &out)
{
    if (got != static_cast<int>(sizeof(pspcap_header) + sizeof(T)))
        return false;
    pspcap_header hdr{};
    std::memcpy(&hdr, buf, sizeof(hdr));
    if (hdr.magic != PSPCAP_MAGIC || hdr.type != type)
        return false;
    std::memcpy(&out, buf + sizeof(hdr), sizeof(T));
    return true;
}

std::string clock_now()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

} // namespace

int run_probe()
{
    UsbLink link;
    std::string err;
    if (!link.init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!connect(link))
        return 1;

    constexpr int kPings = 100;
    int64_t min_us = INT64_MAX, max_us = 0, sum_us = 0;
    for (int i = 0; i < kPings; i++) {
        int64_t rtt = ping(link, err);
        if (rtt < 0) {
            std::fprintf(stderr, "ping %d failed: %s\n", i + 1, err.c_str());
            return 1;
        }
        min_us = std::min(min_us, rtt);
        max_us = std::max(max_us, rtt);
        sum_us += rtt;
    }
    std::printf("%d pings: RTT min %.3f ms, avg %.3f ms, max %.3f ms\n", kPings, min_us / 1000.0,
                sum_us / 1000.0 / kPings, max_us / 1000.0);
    return 0;
}

int run_monitor()
{
    UsbLink link;
    std::string err;
    if (!link.init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }

    std::printf("Waiting for the PSP (Ctrl+C to quit)...\n");
    UsbLink::OpenResult last_open = UsbLink::OpenResult::Ok;
    for (;;) {
        if (!link.is_open()) {
            UsbLink::OpenResult r = link.open(err);
            if (r != UsbLink::OpenResult::Ok) {
                // Explain only when the reason changes (e.g. NotFound -> NoDriver).
                if (r != last_open && r != UsbLink::OpenResult::NotFound)
                    std::printf("[%s] %s\n", clock_now().c_str(), err.c_str());
                last_open = r;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            last_open = UsbLink::OpenResult::Ok;
            std::printf("[%s] connected\n", clock_now().c_str());
            if (!handshake(link)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                continue;
            }
        }

        int64_t worst = 0, sum = 0;
        int n = 0;
        bool lost = false;
        for (int i = 0; i < 5; i++) {
            int64_t rtt = ping(link, err);
            if (rtt < 0) {
                lost = true;
                break;
            }
            worst = std::max(worst, rtt);
            sum += rtt;
            n++;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        if (lost) {
            std::printf("[%s] disconnected (%s)\n", clock_now().c_str(), err.c_str());
            link.close();
            continue;
        }
        std::printf("[%s] ok, RTT avg %.3f ms, max %.3f ms\n", clock_now().c_str(),
                    sum / 1000.0 / n, worst / 1000.0);
    }
}

namespace {

struct BenchConfig {
    uint32_t source;
    uint32_t chunk_kib;
    uint32_t depth;
    uint32_t priority; // 0 = plugin default
};

struct BenchRow {
    BenchConfig cfg;
    bool ok;
    double pc_mib_s;
    double psp_mib_s;
    std::string note;
};

const char *source_name(uint32_t source)
{
    switch (source) {
    case PSPCAP_BENCH_RAM: return "RAM";
    case PSPCAP_BENCH_VRAM_USER_UC: return "VRAM@44";
    case PSPCAP_BENCH_VRAM_USER: return "VRAM@04";
    case PSPCAP_BENCH_VRAM_KERNEL: return "VRAM@84";
    case PSPCAP_BENCH_VRAM_KERNEL_UC: return "VRAM@A4";
    default: return "?";
    }
}

std::string priority_name(uint32_t priority)
{
    if (!priority)
        return "def";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%02X", priority);
    return buf;
}

std::string psp_failure(const pspcap_bench_result &res)
{
    char msg[96];
    std::snprintf(msg, sizeof(msg), "PSP failed after %u bytes: status %u, detail 0x%08X",
                  res.bytes_sent, res.status, res.detail);
    return msg;
}

// Runs one configuration. Returns false if the link looks dead.
bool bench_one(UsbLink &link, const BenchConfig &cfg, uint32_t total, std::vector<uint8_t> &buf,
               BenchRow &row)
{
    row = {cfg, false, 0, 0, ""};
    pspcap_bench_request req{cfg.source, cfg.chunk_kib * 1024, total, cfg.depth, cfg.priority};
    pspcap_header hdr{};
    std::vector<uint8_t> reply;
    std::string err;

    if (link.request(PSPCAP_CMD_BENCH, &req, sizeof(req), PSPCAP_PKT_BENCH_BEGIN, hdr, reply, 2000,
                     err) != 0) {
        row.note = err;
        return hdr.type == PSPCAP_PKT_ERROR; // a refusal is fine, silence is not
    }

    uint64_t t0 = now_us();
    uint32_t received = 0;
    bool pattern_ok = true;
    while (received < total) {
        int want = static_cast<int>(std::min<uint32_t>(total - received,
                                                       static_cast<uint32_t>(buf.size())));
        int got = 0;
        int rc = link.read_raw(buf.data(), want, got, 3000);
        if (rc != 0 || got != want) {
            pspcap_bench_result res{};
            if (rc == 0 && parse_early_packet(buf.data(), got, PSPCAP_PKT_BENCH_END, res)) {
                row.note = psp_failure(res);
                return true;
            }
            char msg[96];
            std::snprintf(msg, sizeof(msg), "data stopped after %u of %u bytes (%s)",
                          received + static_cast<uint32_t>(got), total, usb_error_name(rc));
            row.note = msg;
            link.drain();
            return true;
        }
        if (cfg.source == PSPCAP_BENCH_RAM && pattern_ok) {
            const uint32_t chunk = req.chunk_bytes;
            for (int off = 0; off < got; off += 4) {
                uint32_t word;
                std::memcpy(&word, buf.data() + off, 4);
                if (word != ((received + off) % chunk) / 4) {
                    pattern_ok = false;
                    break;
                }
            }
        }
        received += static_cast<uint32_t>(got);
    }
    uint64_t t1 = now_us();

    if (link.recv(hdr, reply, 2000) != 0 || hdr.type != PSPCAP_PKT_BENCH_END ||
        reply.size() != sizeof(pspcap_bench_result)) {
        row.note = "no BENCH_END";
        return true;
    }
    pspcap_bench_result res{};
    std::memcpy(&res, reply.data(), sizeof(res));

    constexpr double kMiB = 1024.0 * 1024.0;
    row.pc_mib_s = total / kMiB / ((t1 - t0) / 1e6);
    row.psp_mib_s = res.elapsed_us ? res.bytes_sent / kMiB / (res.elapsed_us / 1e6) : 0;
    row.ok = res.status == 0 && pattern_ok;
    if (res.status != 0)
        row.note = psp_failure(res);
    else if (!pattern_ok)
        row.note = "data corrupted";
    return true;
}

} // namespace

int run_bench(unsigned megabytes)
{
    UsbLink link;
    std::string err;
    if (!link.init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!connect(link))
        return 1;

    const uint32_t total = megabytes * 1024u * 1024u;
    std::vector<uint8_t> buf(1024 * 1024);
    std::vector<BenchRow> rows;
    bool alive = true;

    std::printf("\n%u MiB per run\n", megabytes);
    std::printf("%-8s %8s %6s %5s %10s %10s  %s\n", "src", "chunk", "depth", "prio", "PC MiB/s",
                "PSP MiB/s", "result");

    auto run = [&](const BenchConfig &cfg) {
        if (!alive)
            return false;
        BenchRow row;
        alive = bench_one(link, cfg, total, buf, row);
        std::printf("%-8s %6u K %6u %5s %10.2f %10.2f  %s\n", source_name(cfg.source),
                    cfg.chunk_kib, cfg.depth, priority_name(cfg.priority).c_str(), row.pc_mib_s,
                    row.psp_mib_s, row.ok ? "ok" : row.note.c_str());
        rows.push_back(row);
        if (!alive)
            std::printf("PSP stopped answering, skipping the rest.\n");
        return row.ok;
    };

    // RAM: queue depth at the default priority, then a more urgent thread.
    for (uint32_t depth = 1; depth <= PSPCAP_BENCH_MAX_DEPTH; depth++)
        run({PSPCAP_BENCH_RAM, 64, depth, 0});
    run({PSPCAP_BENCH_RAM, 64, 1, 0x10});
    run({PSPCAP_BENCH_RAM, 64, PSPCAP_BENCH_MAX_DEPTH, 0x10});

    // VRAM: which aliases the USB driver accepts at all, then big chunks on those.
    const uint32_t aliases[] = {PSPCAP_BENCH_VRAM_USER_UC, PSPCAP_BENCH_VRAM_USER,
                                PSPCAP_BENCH_VRAM_KERNEL, PSPCAP_BENCH_VRAM_KERNEL_UC};
    std::vector<uint32_t> working;
    for (uint32_t src : aliases) {
        if (run({src, 64, 1, 0}))
            working.push_back(src);
    }
    for (uint32_t src : working) {
        run({src, 512, 1, 0});
        run({src, 512, PSPCAP_BENCH_MAX_DEPTH, 0x10});
    }

    std::printf("\nMarkdown:\n\n| Source | Chunk | Depth | Priority | PC MiB/s | PSP MiB/s | Result |\n"
                "|---|---|---|---|---|---|---|\n");
    for (const BenchRow &r : rows) {
        std::printf("| %s | %u KiB | %u | %s | %.2f | %.2f | %s |\n", source_name(r.cfg.source),
                    r.cfg.chunk_kib, r.cfg.depth, priority_name(r.cfg.priority).c_str(),
                    r.pc_mib_s, r.psp_mib_s, r.ok ? "ok" : r.note.c_str());
    }
    return alive ? 0 : 1;
}

int run_snapshot(const char *path)
{
    UsbLink link;
    std::string err;
    if (!link.init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!connect(link))
        return 1;

    pspcap_header hdr{};
    std::vector<uint8_t> reply;
    if (link.request(PSPCAP_CMD_SNAPSHOT, nullptr, 0, PSPCAP_PKT_FRAME_BEGIN, hdr, reply, 2000,
                     err) != 0) {
        std::fprintf(stderr, "Snapshot failed: %s\n", err.c_str());
        return 1;
    }
    pspcap_frame_info info{};
    if (reply.size() != sizeof(info)) {
        std::fprintf(stderr, "FRAME_BEGIN has a wrong size (%zu bytes)\n", reply.size());
        return 1;
    }
    std::memcpy(&info, reply.data(), sizeof(info));

    const unsigned bpp = psp_bytes_per_pixel(info.pixel_format);
    if (!bpp || !info.width || !info.height || info.width > 1024 || info.height > 1024 ||
        info.row_pixels < info.width || info.bytes != info.row_pixels * info.height * bpp) {
        std::fprintf(stderr, "Implausible frame: %ux%u, format %u, %u bytes\n", info.width,
                     info.height, info.pixel_format, info.bytes);
        link.drain();
        return 1;
    }

    std::vector<uint8_t> raw(info.bytes);
    uint64_t t0 = now_us();
    uint32_t received = 0;
    while (received < info.bytes) {
        int want = static_cast<int>(std::min<uint32_t>(info.bytes - received, 1024 * 1024));
        int got = 0;
        int rc = link.read_raw(raw.data() + received, want, got, 3000);
        if (rc != 0 || got != want) {
            pspcap_frame_end end{};
            if (rc == 0 && parse_early_packet(raw.data() + received, got, PSPCAP_PKT_FRAME_END, end))
                std::fprintf(stderr, "PSP failed after %u bytes: status %u, detail 0x%08X\n",
                             received, end.status, end.detail);
            else
                std::fprintf(stderr, "Frame data stopped after %u of %u bytes (%s)\n",
                             received + static_cast<uint32_t>(got), info.bytes,
                             usb_error_name(rc));
            return 1;
        }
        received += static_cast<uint32_t>(got);
    }
    uint64_t t1 = now_us();

    pspcap_frame_end end{};
    if (link.recv(hdr, reply, 2000) != 0 || hdr.type != PSPCAP_PKT_FRAME_END ||
        reply.size() != sizeof(end)) {
        std::fprintf(stderr, "No FRAME_END after the frame data\n");
        return 1;
    }
    std::memcpy(&end, reply.data(), sizeof(end));
    if (end.status != 0) {
        std::fprintf(stderr, "PSP reports status %u, detail 0x%08X\n", end.status, end.detail);
        return 1;
    }

    const size_t pixels = static_cast<size_t>(info.width) * info.height;
    std::vector<uint8_t> rgba(pixels * 4);
    psp_frame_to_rgba(info.pixel_format, raw.data(), info.width, info.height, info.row_pixels,
                      rgba.data());

    std::string out = path ? path : "";
    if (out.empty()) {
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char name[64];
        std::strftime(name, sizeof(name), "snapshot-%Y%m%d-%H%M%S.png", &tm);
        out = name;
    }
    if (!write_png(out, static_cast<int>(info.width), static_cast<int>(info.height),
                   rgba.data())) {
        std::fprintf(stderr, "Cannot write %s\n", out.c_str());
        return 1;
    }

    std::printf("Saved %s: %ux%u, format %s, framebuffer 0x%08X stride %u\n", out.c_str(),
                info.width, info.height, psp_format_name(info.pixel_format), info.source_addr,
                info.source_stride);
    std::printf("PSP copy+send %.1f ms, PC receive %.1f ms (%u bytes)\n", end.elapsed_us / 1000.0,
                (t1 - t0) / 1000.0, info.bytes);
    return 0;
}

int run_fps(unsigned seconds)
{
    UsbLink link;
    std::string err;
    if (!link.init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!connect(link))
        return 1;

    auto query = [&](pspcap_stats &st) {
        pspcap_header hdr{};
        std::vector<uint8_t> reply;
        if (link.request(PSPCAP_CMD_STATS, nullptr, 0, PSPCAP_PKT_STATS, hdr, reply, 1000, err) != 0)
            return false;
        if (reply.size() != sizeof(st)) {
            err = "STATS has a wrong size";
            return false;
        }
        std::memcpy(&st, reply.data(), sizeof(st));
        return true;
    };

    pspcap_stats a{}, b{};
    if (!query(a)) {
        std::fprintf(stderr, "STATS failed: %s\n", err.c_str());
        return 1;
    }
    if (!a.hooked) {
        std::fprintf(stderr, "The flip hook is not installed on the PSP (see pspcap.log).\n");
        return 1;
    }
    std::printf("Counting game flips for %u s, one line per second...\n", seconds);
    pspcap_stats prev = a;
    for (unsigned i = 0; i < seconds; i++) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!query(b)) {
            std::fprintf(stderr, "STATS failed: %s\n", err.c_str());
            return 1;
        }
        double step = (b.psp_time_us - prev.psp_time_us) / 1e6;
        std::printf("[%s] %5.1f frames/s\n", clock_now().c_str(), (b.flips - prev.flips) / step);
        prev = b;
    }

    double dt = (b.psp_time_us - a.psp_time_us) / 1e6;
    std::printf("Game: %.2f frames/s (%u flips in %.2f s), streaming: %s, frames streamed "
                "since plugin start: %u\n",
                (b.flips - a.flips) / dt, b.flips - a.flips, dt, b.streaming ? "yes" : "no",
                b.frames_sent);
    return 0;
}

int run_trace(unsigned seconds)
{
    UsbLink link;
    std::string err;
    if (!link.init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!connect(link))
        return 1;

    const pspcap_stream_request req{PSPCAP_STREAM_AUDIO, 0, 0};
    std::printf("STREAM_START send: %s\n",
                usb_error_name(link.send(PSPCAP_CMD_STREAM_START, &req, sizeof(req))));
    std::vector<uint8_t> raw(1024 * 1024);
    std::vector<uint8_t> payload;
    uint64_t t0 = now_us();
    while (now_us() - t0 < seconds * 1000000ull) {
        pspcap_header hdr{};
        int rc = link.recv(hdr, payload, 500);
        double t = (now_us() - t0) / 1000.0;
        if (rc != 0) {
            std::printf("%8.1f ms  recv: %s\n", t, usb_error_name(rc));
            if (rc != LIBUSB_ERROR_TIMEOUT)
                break;
            continue;
        }
        std::printf("%8.1f ms  packet type 0x%02X, %zu bytes payload", t, hdr.type, payload.size());
        if (hdr.type == PSPCAP_PKT_STATS && payload.size() == sizeof(pspcap_stats)) {
            pspcap_stats st{};
            std::memcpy(&st, payload.data(), sizeof(st));
            std::printf(": flips %u, sent %u, streaming %u, hooks 0x%X (1 flip, 2 grab, 4 audio), "
                        "audio sent %u dropped %u",
                        st.flips, st.frames_sent, st.streaming, st.hooked, st.audio_sent,
                        st.audio_dropped);
        } else if (hdr.type == PSPCAP_PKT_AUDIO && payload.size() == sizeof(pspcap_audio_info)) {
            pspcap_audio_info ai{};
            std::memcpy(&ai, payload.data(), sizeof(ai));
            std::printf(": AUDIO source %u, %u Hz, %u ch, %u samples, vol %u/%u, %u bytes, "
                        "psp_us %llu",
                        ai.source, ai.rate, ai.channels, ai.samples, ai.vol_left, ai.vol_right,
                        ai.bytes, static_cast<unsigned long long>(ai.psp_time_us));
            int got = 0;
            rc = link.read_raw(raw.data(), static_cast<int>(std::min<uint32_t>(ai.bytes, 1u << 20)),
                               got, 2000);
            if (rc != 0 || got != static_cast<int>(ai.bytes))
                std::printf(" (pcm read: %s, %d bytes)", usb_error_name(rc), got);
        } else if (hdr.type == PSPCAP_PKT_ERROR && payload.size() == sizeof(pspcap_error)) {
            pspcap_error e{};
            std::memcpy(&e, payload.data(), sizeof(e));
            std::printf(": ERROR %u detail 0x%X", e.code, e.detail);
        } else if (hdr.type == PSPCAP_PKT_FRAME_BEGIN && payload.size() == sizeof(pspcap_frame_info)) {
            pspcap_frame_info info{};
            std::memcpy(&info, payload.data(), sizeof(info));
            std::printf(": frame %u, fmt %u, %u bytes\n", info.frame_id, info.pixel_format,
                        info.bytes);
            int got = 0;
            rc = link.read_raw(raw.data(), static_cast<int>(std::min<uint32_t>(info.bytes, 1u << 20)),
                               got, 2000);
            std::printf("%8.1f ms  raw: %s, %d bytes", (now_us() - t0) / 1000.0,
                        usb_error_name(rc), got);
        } else if (hdr.type == PSPCAP_PKT_FRAME_END && payload.size() == sizeof(pspcap_frame_end)) {
            pspcap_frame_end end{};
            std::memcpy(&end, payload.data(), sizeof(end));
            std::printf(": status %u detail 0x%08X, %u us (copy %u us)", end.status, end.detail,
                        end.elapsed_us, end.copy_us);
        }
        std::printf("\n");
    }
    link.send(PSPCAP_CMD_STREAM_STOP, nullptr, 0);
    return 0;
}

int run_load()
{
    UsbLink link;
    std::string err;
    if (!link.init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!connect(link))
        return 1;

    auto query = [&](pspcap_stats &st) {
        pspcap_header hdr{};
        std::vector<uint8_t> reply;
        if (link.request(PSPCAP_CMD_STATS, nullptr, 0, PSPCAP_PKT_STATS, hdr, reply, 1000, err) != 0 ||
            reply.size() != sizeof(st))
            return false;
        std::memcpy(&st, reply.data(), sizeof(st));
        return true;
    };
    auto fps = [](const pspcap_stats &a, const pspcap_stats &b) {
        return (b.flips - a.flips) / ((b.psp_time_us - a.psp_time_us) / 1e6);
    };

    std::vector<uint8_t> buf(1024 * 1024);
    pspcap_stats a{}, b{};

    // Game frame rate while the USB link only moves data: no frame copying at
    // all. Compare with the idle rows to see what USB traffic alone costs.
    std::printf("%-34s %8s %10s\n", "load", "game fps", "USB MiB/s");
    auto idle = [&](const char *name) {
        if (!query(a))
            return false;
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!query(b))
            return false;
        std::printf("%-34s %8.1f %10s\n", name, fps(a, b), "-");
        return true;
    };
    auto load = [&](const char *name, const BenchConfig &cfg, unsigned mib) {
        BenchRow row;
        if (!query(a))
            return false;
        bool alive = bench_one(link, cfg, mib * 1024u * 1024u, buf, row);
        if (!alive || !query(b))
            return false;
        std::printf("%-34s %8.1f %10.2f  %s\n", name, fps(a, b), row.pc_mib_s,
                    row.ok ? "" : row.note.c_str());
        return true;
    };

    bool ok = idle("idle") &&
              load("64 KiB x1, prio 0x10", {PSPCAP_BENCH_RAM, 64, 1, 0x10}, 64) &&
              load("64 KiB x4, prio 0x10", {PSPCAP_BENCH_RAM, 64, 4, 0x10}, 64) &&
              load("16 KiB x1, prio 0x10 (4x requests)", {PSPCAP_BENCH_RAM, 16, 1, 0x10}, 48) &&
              load("64 KiB x1, default prio", {PSPCAP_BENCH_RAM, 64, 1, 0}, 32) &&
              idle("idle again");
    if (!ok) {
        std::fprintf(stderr, "load test failed: %s\n", err.c_str());
        return 1;
    }
    return 0;
}
