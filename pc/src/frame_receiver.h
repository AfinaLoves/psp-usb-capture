// Background thread that keeps a stream running and hands out the newest frame.
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <functional>
#include <thread>
#include <vector>

#include "pspcap_protocol.h"

class FrameReceiver {
public:
    // Rolling numbers over the last second or so.
    struct Stats {
        bool connected = false;
        std::string status;    // "waiting for the PSP", errors...
        double game_fps = 0;   // flips per second on the PSP
        double recv_fps = 0;   // frames that arrived per second
        double mib_s = 0;      // pixel data per second
        uint64_t skipped = 0;  // flips that never reached us, since connect
        uint32_t pixel_format = 0;
        double psp_send_ms = 0; // copy + send time of the last frame on the PSP
        double psp_copy_ms = 0; // CPU part of it (copying/converting pixels)
        double audio_kib_s = 0; // sound data per second
        uint32_t psp_audio_dropped = 0; // blocks the PSP could not keep (ring full)
        uint32_t psp_audio_sent = 0;    // PSP counter at the last STATS
        uint64_t pc_audio_blocks = 0;   // blocks received, same moment
    };

    // `flags`/`priority`/`yield_us` as in pspcap_stream_request.
    explicit FrameReceiver(uint32_t flags = 0, uint32_t priority = 0, uint32_t yield_us = 0)
        : req_{flags, priority, yield_us} {}
    ~FrameReceiver();
    FrameReceiver(const FrameReceiver &) = delete;
    FrameReceiver &operator=(const FrameReceiver &) = delete;

    // Where PKT_AUDIO blocks go (called on the receiver thread). Set before start().
    using AudioSink = std::function<void(const pspcap_audio_info &, const int16_t *)>;
    void set_audio_sink(AudioSink sink) { audio_sink_ = std::move(sink); }

    void start();
    void stop();

    // If a frame arrived since the last call, swaps it (RGBA8888) into `rgba`
    // and returns true.
    bool take_frame(std::vector<uint8_t> &rgba, int &width, int &height);

    Stats stats();

    // New stream options, sent to the PSP while streaming (no reconnect).
    void set_options(uint32_t flags, uint32_t priority, uint32_t yield_us);

private:
    void run();
    void set_status(bool connected, const std::string &status);

    pspcap_stream_request request();

    pspcap_stream_request req_; // guarded by mu_
    std::atomic<bool> options_changed_{false};
    AudioSink audio_sink_;
    std::thread thread_;
    std::atomic<bool> quit_{false};

    std::mutex mu_;
    std::vector<uint8_t> latest_;
    int width_ = 0;
    int height_ = 0;
    bool fresh_ = false;
    Stats stats_;
};
