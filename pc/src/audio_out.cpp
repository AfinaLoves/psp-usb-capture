#include "audio_out.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <share.h>

#include "usb_link.h"

namespace {

constexpr int kRate = 44100;
// Blocks arrive in bursts between video frames, so each source keeps a margin
// before it starts (or restarts after running dry). The small drift between
// the PSP's and the PC's clocks is absorbed either by resampling (below) or,
// in Drop mode, by dropping or repeating single samples, spread over a
// callback, while the queue is outside a band around the target (linear
// interpolation audibly dulled the sound). Trimming is only a last resort
// that bounds the delay.
constexpr size_t kStartFrames = kRate * 80 / 1000;
constexpr long kTargetFrames = kRate * 100 / 1000;
constexpr long kBandFrames = kRate * 30 / 1000;
constexpr int kMaxCorrection = 8; // samples per callback (512 frames: ~1.6 %)
constexpr size_t kMaxFrames = kRate * 400 / 1000;
constexpr size_t kTrimToFrames = kRate * 150 / 1000;
// Undertale sometimes hands in a single all-zero block (23 ms) in the middle
// of its music, about 6 times a minute (tools/audio_dump.py). A zero block
// between two blocks with sound is filled in before it plays.
constexpr size_t kHoleFade = 64; // frames blended into the next block at the end

// Resample mode. The rate follows the queue slowly (bursts of ~33 ms per video
// frame average out) and stays within 0.5 %: enough for the clocks (~0.3 %),
// too little to hear as pitch. A 16-tap windowed sinc keeps the highs that
// linear interpolation dulled; at frac 0 it passes samples through unchanged.
constexpr int kTaps = 16;
constexpr int kHalf = kTaps / 2;
constexpr int kPhases = 256;
constexpr double kMaxStep = 0.005;
constexpr double kErrorSmoothing = 0.05; // per callback (~12 ms): ~0.25 s
constexpr double kGain = 0.1;            // step change per second of queue error

struct SincTable {
    float h[kPhases + 1][kTaps];
    SincTable()
    {
        const double pi = 3.14159265358979323846;
        for (int p = 0; p <= kPhases; p++) {
            const double frac = static_cast<double>(p) / kPhases;
            double sum = 0;
            for (int k = 0; k < kTaps; k++) {
                // Tap k sits at queue[k]; the output point is kHalf - 1 + frac.
                const double x = k - (kHalf - 1) - frac;
                const double sinc = x == 0 ? 1 : std::sin(pi * x) / (pi * x);
                const double w = 0.42 + 0.5 * std::cos(pi * x / kHalf) +
                                 0.08 * std::cos(2 * pi * x / kHalf); // Blackman
                h[p][k] = static_cast<float>(std::abs(x) < kHalf ? sinc * w : 0);
                sum += h[p][k];
            }
            for (int k = 0; k < kTaps; k++)
                h[p][k] = static_cast<float>(h[p][k] / sum);
        }
    }
};
const SincTable kSinc;

} // namespace

AudioOut::~AudioOut()
{
    close();
}

bool AudioOut::open(std::string &err)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        err = std::string("SDL audio init failed: ") + SDL_GetError();
        return false;
    }
    SDL_AudioSpec want{};
    want.freq = kRate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = &AudioOut::callback;
    want.userdata = this;
    SDL_AudioSpec have{};
    dev_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!dev_) {
        err = std::string("cannot open the audio device: ") + SDL_GetError();
        return false;
    }
    opened_ticks_ = SDL_GetPerformanceCounter();
    std::printf("audio device: %d Hz, format 0x%X, %d ch, %d samples\n", have.freq,
                have.format, have.channels, have.samples);
    SDL_PauseAudioDevice(dev_, 0);
    return true;
}

void AudioOut::close()
{
    if (dev_) {
        SDL_CloseAudioDevice(dev_);
        dev_ = 0;
    }
    flush_dump();
    for (std::FILE **f : {&in_file_, &out_file_, &times_file_}) {
        if (*f) {
            std::fclose(*f);
            *f = nullptr;
        }
    }
}

namespace {

// in.bin record: kind 0 = block (followed by `bytes` of PCM as received),
// kind 1 = marker (source = its number: user markers count from 1, control
// file changes from 1001).
struct DumpRecord {
    uint32_t kind;
    uint32_t source;
    uint32_t samples;
    uint32_t channels;
    uint32_t bytes;
    uint16_t vol_left, vol_right;
    uint64_t psp_time_us;
    uint64_t pc_time_us;
};
static_assert(sizeof(DumpRecord) == 40, "dump record layout");

} // namespace

bool AudioOut::start_dump(const std::string &dir, std::string &err)
{
    std::lock_guard<std::mutex> dump_lock(dump_mu_);
    // Shared, so the dump can be analysed while the client still runs.
    in_file_ = _fsopen((dir + "/in.bin").c_str(), "wb", _SH_DENYNO);
    out_file_ = _fsopen((dir + "/out.raw").c_str(), "wb", _SH_DENYNO);
    times_file_ = _fsopen((dir + "/out_times.bin").c_str(), "wb", _SH_DENYNO);
    if (!in_file_ || !out_file_ || !times_file_) {
        err = "cannot create the dump files in " + dir;
        return false;
    }
    std::lock_guard<std::mutex> lock(mu_);
    dumping_ = true;
    return true;
}

void AudioOut::mark(uint32_t n)
{
    std::lock_guard<std::mutex> lock(dump_mu_);
    if (!in_file_)
        return;
    DumpRecord rec{};
    rec.kind = 1;
    rec.source = n;
    rec.pc_time_us = now_us();
    std::fwrite(&rec, sizeof(rec), 1, in_file_);
    std::fflush(in_file_);
}

void AudioOut::flush_dump()
{
    std::vector<int16_t> samples;
    std::vector<std::pair<uint64_t, uint64_t>> times;
    {
        std::lock_guard<std::mutex> lock(mu_);
        samples.swap(out_buf_);
        times.swap(out_times_);
    }
    if (out_file_ && !samples.empty())
        std::fwrite(samples.data(), sizeof(int16_t), samples.size(), out_file_);
    if (times_file_ && !times.empty())
        std::fwrite(times.data(), sizeof(times[0]), times.size(), times_file_);
}

void AudioOut::push(const pspcap_audio_info &info, const int16_t *pcm)
{
    if (info.source >= 10 || !info.samples || !info.rate || (info.channels != 1 && info.channels != 2))
        return;
    const float gl = info.vol_left / 32768.0f / 32768.0f;
    const float gr = info.vol_right / 32768.0f / 32768.0f;
    const unsigned ch = info.channels;
    auto sample = [&](uint32_t i) {
        float l = pcm[i * ch] * gl;
        float r = pcm[i * ch + (ch - 1)] * gr;
        return Frame{l, r};
    };

    {
        std::lock_guard<std::mutex> dump_lock(dump_mu_);
        if (in_file_) {
            DumpRecord rec{};
            rec.kind = 0;
            rec.source = info.source;
            rec.samples = info.samples;
            rec.channels = info.channels;
            rec.bytes = info.samples * info.channels * 2;
            rec.vol_left = info.vol_left;
            rec.vol_right = info.vol_right;
            rec.psp_time_us = info.psp_time_us;
            rec.pc_time_us = now_us();
            std::fwrite(&rec, sizeof(rec), 1, in_file_);
            std::fwrite(pcm, 1, rec.bytes, in_file_);
        }
    }

    std::vector<Frame> block;
    block.reserve(info.samples);
    if (info.rate == static_cast<uint32_t>(kRate)) {
        for (uint32_t i = 0; i < info.samples; i++)
            block.push_back(sample(i));
    } else {
        // Linear resampling, block by block (good enough for "some sound").
        const double step = static_cast<double>(info.rate) / kRate;
        for (double t = 0; t < info.samples - 1; t += step) {
            uint32_t i = static_cast<uint32_t>(t);
            float f = static_cast<float>(t - i);
            Frame a = sample(i), b = sample(i + 1);
            block.push_back({a.l + (b.l - a.l) * f, a.r + (b.r - a.r) * f});
        }
    }
    const bool silent =
        std::all_of(pcm, pcm + info.samples * ch, [](int16_t v) { return v == 0; });

    std::lock_guard<std::mutex> lock(mu_);
    Source &s = sources_[info.source];
    pushed_[info.source] += info.samples;
    if (silent) {
        // One zero block right after sound may be a dropout; a second one in a
        // row means the sound really stopped.
        s.hole = !s.last_silent;
        s.hole_start = s.in_frames;
        s.hole_len = block.size();
    } else {
        if (s.hole)
            fill_hole(s, block);
        s.hole = false;
        s.last_sound = block;
    }
    s.last_silent = silent;
    s.queue.insert(s.queue.end(), block.begin(), block.end());
    s.in_frames += block.size();
    if (s.queue.size() > kMaxFrames) {
        const size_t cut = s.queue.size() - kTrimToFrames;
        s.queue.erase(s.queue.begin(), s.queue.begin() + static_cast<long>(cut));
        s.out_frames += cut;
        trims_++;
    }
}

// Fills the zero block before `next` with the previous sound played backwards
// (it joins the sound before it without a jump), fading into the start of
// `next` at the end, also backwards, so that join is smooth too.
void AudioOut::fill_hole(Source &s, const std::vector<Frame> &next)
{
    // Not if it is already playing (Resample mode reads kTaps frames ahead).
    if (s.hole_start < s.out_frames + kTaps || s.last_sound.empty() || next.empty())
        return;
    const size_t at = static_cast<size_t>(s.hole_start - s.out_frames);
    if (at + s.hole_len > s.queue.size())
        return;
    const size_t n = s.last_sound.size();
    const size_t fade = std::min({kHoleFade, s.hole_len, next.size()});
    for (size_t i = 0; i < s.hole_len; i++) {
        Frame f = s.last_sound[n - 1 - i % n];
        const size_t left = s.hole_len - i; // frames until the join with `next`
        if (left <= fade) {
            const Frame &g = next[left - 1];
            const float w = static_cast<float>(fade - left + 1) / (fade + 1);
            f = {f.l + (g.l - f.l) * w, f.r + (g.r - f.r) * w};
        }
        s.queue[at + i] = f;
    }
    holes_++;
}

uint64_t AudioOut::holes()
{
    std::lock_guard<std::mutex> lock(mu_);
    return holes_;
}

void AudioOut::rates(double &pushed_per_s, double &played_per_s, double &device_per_s)
{
    std::lock_guard<std::mutex> lock(mu_);
    double secs = static_cast<double>(SDL_GetPerformanceCounter() - opened_ticks_) /
                  static_cast<double>(SDL_GetPerformanceFrequency());
    int busiest = 0;
    for (int i = 1; i < 10; i++)
        if (pushed_[i] > pushed_[busiest])
            busiest = i;
    pushed_per_s = secs > 0 ? pushed_[busiest] / secs : 0;
    played_per_s = secs > 0 ? played_[busiest] / secs : 0;
    device_per_s = secs > 0 ? device_frames_ / secs : 0;
}

std::string AudioOut::debug_line()
{
    std::lock_guard<std::mutex> lock(mu_);
    std::string line;
    char buf[64];
    for (int i = 0; i < 10; i++) {
        uint64_t in = pushed_[i] - last_pushed_[i];
        uint64_t out = played_[i] - last_played_[i];
        last_pushed_[i] = pushed_[i];
        last_played_[i] = played_[i];
        if (!in && !out && sources_[i].queue.empty())
            continue;
        std::snprintf(buf, sizeof(buf), " %d:%llu/%llu/%zu", i,
                      static_cast<unsigned long long>(in), static_cast<unsigned long long>(out),
                      sources_[i].queue.size());
        line += buf;
    }
    std::snprintf(buf, sizeof(buf), " underruns +%llu",
                  static_cast<unsigned long long>(underruns_ - last_underruns_));
    last_underruns_ = underruns_;
    return line + buf;
}

uint64_t AudioOut::trims()
{
    std::lock_guard<std::mutex> lock(mu_);
    return trims_;
}

uint64_t AudioOut::underruns()
{
    std::lock_guard<std::mutex> lock(mu_);
    return underruns_;
}

void SDLCALL AudioOut::callback(void *self, Uint8 *stream, int len)
{
    static_cast<AudioOut *>(self)->mix(reinterpret_cast<int16_t *>(stream),
                                       len / static_cast<int>(2 * sizeof(int16_t)));
}

void AudioOut::mix(int16_t *out, int frames)
{
    std::lock_guard<std::mutex> lock(mu_);
    for (Source &s : sources_) {
        if (!s.playing && s.queue.size() >= kStartFrames) {
            s.playing = true;
            s.frac = 0;
            s.step = 1;
            s.error = 0;
        }
        long error = static_cast<long>(s.queue.size()) - kTargetFrames;
        if (drift_ == Drift::Resample) {
            // Slow proportional control of the playback rate.
            s.error += (error - s.error) * kErrorSmoothing;
            s.step = 1 + std::clamp(s.error / kRate * kGain, -kMaxStep, kMaxStep);
            continue;
        }
        // One decision per callback (~12 ms): > 0 drops, < 0 repeats samples.
        long outside = std::abs(error) - kBandFrames;
        int n = outside <= 0 ? 0
                             : static_cast<int>(std::min<long>(kMaxCorrection,
                                                               1 + outside / kBandFrames));
        s.correction = error > 0 ? n : -n;
    }
    for (int i = 0; i < frames; i++) {
        float l = 0, r = 0;
        for (Source &s : sources_) {
            if (!s.playing)
                continue;
            const int index = static_cast<int>(&s - sources_);
            if (drift_ == Drift::Resample)
                mix_resample(s, index, l, r);
            else
                mix_drop(s, index, l, r, i, frames);
        }
        out[i * 2] = static_cast<int16_t>(std::clamp(l * volume_, -1.0f, 1.0f) * 32767.0f);
        out[i * 2 + 1] = static_cast<int16_t>(std::clamp(r * volume_, -1.0f, 1.0f) * 32767.0f);
        device_frames_++;
    }
    if (dumping_) {
        out_times_.emplace_back(device_frames_ - frames, now_us());
        out_buf_.insert(out_buf_.end(), out, out + frames * 2);
    }
}

void AudioOut::mix_drop(Source &s, int index, float &l, float &r, int i, int frames)
{
    if (s.queue.empty()) {
        s.playing = false;
        underruns_++;
        return;
    }
    l += s.queue.front().l;
    r += s.queue.front().r;
    // Corrections land on evenly spaced samples of this callback.
    int n = std::abs(s.correction);
    bool fix = n && (i + 1) % (frames / (n + 1) + 1) == 0;
    if (fix && s.correction < 0)
        return; // repeat: keep this sample for the next output frame
    s.queue.pop_front();
    played_[index]++;
    s.out_frames++;
    if (fix && s.correction > 0 && !s.queue.empty()) {
        s.queue.pop_front(); // drop the next one
        played_[index]++;
        s.out_frames++;
    }
}

void AudioOut::mix_resample(Source &s, int index, float &l, float &r)
{
    if (s.queue.size() < static_cast<size_t>(kTaps)) {
        s.playing = false;
        underruns_++;
        return;
    }
    // Blend the two nearest phases of the table.
    const double pos = s.frac * kPhases;
    const int p = std::min(static_cast<int>(pos), kPhases - 1);
    const float t = static_cast<float>(pos - p);
    const float *a = kSinc.h[p];
    const float *b = kSinc.h[p + 1];
    float sl = 0, sr = 0;
    for (int k = 0; k < kTaps; k++) {
        const float w = a[k] + (b[k] - a[k]) * t;
        sl += s.queue[k].l * w;
        sr += s.queue[k].r * w;
    }
    l += sl;
    r += sr;
    s.frac += s.step;
    while (s.frac >= 1 && !s.queue.empty()) {
        s.frac -= 1;
        s.queue.pop_front();
        played_[index]++;
        s.out_frames++;
    }
}

void AudioOut::set_volume(float gain)
{
    std::lock_guard<std::mutex> lock(mu_);
    volume_ = std::max(0.0f, gain);
}

void AudioOut::set_drift(Drift mode)
{
    std::lock_guard<std::mutex> lock(mu_);
    drift_ = mode;
    for (Source &s : sources_) {
        s.frac = 0;
        s.step = 1;
        s.error = 0;
        s.correction = 0;
    }
}
