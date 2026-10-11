// Plays the PSP's sound sources mixed together (SDL audio, 44.1 kHz stereo).
#pragma once

#include <SDL.h>

#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "pspcap_protocol.h"

class AudioOut {
public:
    AudioOut() = default;
    ~AudioOut();
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;

    bool open(std::string &err);
    void close();

    // How the PSP/PC clock drift is absorbed: Drop drops or repeats single
    // samples; Resample plays each source a little faster or slower (at most
    // 0.5 %, steered by its queue), interpolating with a windowed sinc.
    enum class Drift { Drop, Resample };
    void set_drift(Drift mode);

    void set_volume(float gain); // 1 = as the game plays it

    // Debugging dropouts: records every block as it arrives (dir/in.bin) and
    // what the device played (dir/out.raw, s16 stereo 44.1 kHz, with
    // dir/out_times.bin: device frame index and PC time of each callback).
    // Formats: tools/audio_dump.py. mark() adds a user marker to in.bin.
    bool start_dump(const std::string &dir, std::string &err);
    void mark(uint32_t n);
    void flush_dump(); // writes what the device played; call from the main thread

    // Queues one block from the PSP. Thread-safe (called by the receiver).
    void push(const pspcap_audio_info &info, const int16_t *pcm);

    uint64_t underruns();
    uint64_t trims(); // times a queue was cut back because it grew too long
    uint64_t holes(); // all-zero blocks inside sound that were filled in

    // Diagnostics: per second since open, for the busiest source.
    void rates(double &pushed_per_s, double &played_per_s, double &device_per_s);

    // Diagnostics: "src:in/played/queue" for every source that got data
    // since the previous call (counts since then, queue now, in frames).
    std::string debug_line();

private:
    struct Frame {
        float l, r;
    };
    struct Source {
        std::deque<Frame> queue;
        bool playing = false; // false while (re)building the safety margin
        int correction = 0;   // this callback: > 0 drop, < 0 repeat that many samples
        double frac = 0;      // Resample: position between queue[kHalf - 1] and queue[kHalf]
        double step = 1;      // Resample: input frames per output frame
        double error = 0;     // Resample: smoothed queue error, frames
        uint64_t in_frames = 0;  // frames ever appended to the queue
        uint64_t out_frames = 0; // frames ever removed from its front (played, trimmed)
        bool last_silent = true; // the previous block was all zeros
        bool hole = false;       // ... and came right after sound: maybe a dropout
        uint64_t hole_start = 0; // where that block starts (in in_frames)
        size_t hole_len = 0;
        std::vector<Frame> last_sound; // the latest block that was not silent
    };

    void fill_hole(Source &s, const std::vector<Frame> &next);

    void mix_drop(Source &s, int index, float &l, float &r, int i, int frames);
    void mix_resample(Source &s, int index, float &l, float &r);

    static void SDLCALL callback(void *self, Uint8 *stream, int len);
    void mix(int16_t *out, int frames);

    Drift drift_ = Drift::Resample;
    float volume_ = 1; // guarded by mu_
    SDL_AudioDeviceID dev_ = 0;
    std::mutex mu_;
    Source sources_[10];
    uint64_t underruns_ = 0;
    uint64_t trims_ = 0;
    uint64_t holes_ = 0;
    uint64_t opened_ticks_ = 0;
    uint64_t device_frames_ = 0;          // frames the device asked for
    uint64_t pushed_[10] = {};            // input frames received per source
    uint64_t played_[10] = {};            // input frames consumed per source
    uint64_t last_pushed_[10] = {};
    uint64_t last_played_[10] = {};
    uint64_t last_underruns_ = 0;

    std::mutex dump_mu_; // in_file_ (receiver thread and mark())
    std::FILE *in_file_ = nullptr;
    std::FILE *out_file_ = nullptr;
    std::FILE *times_file_ = nullptr;
    bool dumping_ = false;                                  // guarded by mu_
    std::vector<int16_t> out_buf_;                          // guarded by mu_
    std::vector<std::pair<uint64_t, uint64_t>> out_times_; // guarded by mu_
};
