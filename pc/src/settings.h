// Window settings: what the Esc menu edits and pspcap.ini stores.
#pragma once

#include <cstdint>
#include <string>

struct Settings {
    // Stream (sent to the PSP, can change while streaming).
    bool rgb565 = true;      // convert 32-bit frames to 16 bits on the PSP
    bool audio = true;       // capture the game's sound
    uint32_t yield_us = 200; // with rgb565: let the PSP's sound thread run
    uint32_t priority = 0;   // PSP stream thread priority, 0 = plugin default
    bool dma = false;        // copy frames with the DMA controller (advanced)

    // PC side.
    int volume = 100;          // percent, 0..200
    bool resample = true;      // follow the PSP clock by resampling (else drop/repeat)
    bool fullscreen = false;
    bool integer_scale = false; // only whole multiples of 480x272
    bool show_stats = false;    // numbers in the title bar

    uint32_t stream_flags() const;
};

// pspcap.ini next to the executable.
std::string settings_path();

// Missing file or keys keep the defaults. Returns false only on a read error.
bool load_settings(const std::string &path, Settings &s);
bool save_settings(const std::string &path, const Settings &s);
