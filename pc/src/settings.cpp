#include "settings.h"

#include <SDL.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "pspcap_protocol.h"

uint32_t Settings::stream_flags() const
{
    uint32_t f = 0;
    if (rgb565)
        f |= PSPCAP_STREAM_RGB565;
    if (audio)
        f |= PSPCAP_STREAM_AUDIO;
    if (dma)
        f |= PSPCAP_STREAM_DMA;
    return f;
}

std::string settings_path()
{
    std::string dir;
    if (char *base = SDL_GetBasePath()) {
        dir = base;
        SDL_free(base);
    }
    return dir + "pspcap.ini";
}

namespace {

bool parse_bool(const std::string &v)
{
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

} // namespace

bool load_settings(const std::string &path, Settings &s)
{
    std::ifstream f(path);
    if (!f)
        return true; // no file yet: defaults
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[')
            continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq), val = line.substr(eq + 1);
        while (!key.empty() && key.back() == ' ')
            key.pop_back();
        while (!val.empty() && val.front() == ' ')
            val.erase(val.begin());
        const long num = std::strtol(val.c_str(), nullptr, 0);
        if (key == "rgb565")
            s.rgb565 = parse_bool(val);
        else if (key == "audio")
            s.audio = parse_bool(val);
        else if (key == "yield_us")
            s.yield_us = static_cast<uint32_t>(std::clamp<long>(num, 0, PSPCAP_YIELD_MAX_US));
        else if (key == "priority")
            s.priority = num >= 0x08 && num <= 0x7F ? static_cast<uint32_t>(num) : 0;
        else if (key == "dma")
            s.dma = parse_bool(val);
        else if (key == "volume")
            s.volume = static_cast<int>(std::clamp<long>(num, 0, 200));
        else if (key == "resample")
            s.resample = parse_bool(val);
        else if (key == "fullscreen")
            s.fullscreen = parse_bool(val);
        else if (key == "integer_scale")
            s.integer_scale = parse_bool(val);
        else if (key == "show_stats")
            s.show_stats = parse_bool(val);
    }
    return true;
}

bool save_settings(const std::string &path, const Settings &s)
{
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;
    auto b = [](bool v) { return v ? "1" : "0"; };
    f << "# pspcap settings (written by the Esc menu)\n"
      << "rgb565=" << b(s.rgb565) << "\n"
      << "audio=" << b(s.audio) << "\n"
      << "yield_us=" << s.yield_us << "\n"
      << "priority=" << s.priority << "\n"
      << "dma=" << b(s.dma) << "\n"
      << "volume=" << s.volume << "\n"
      << "resample=" << b(s.resample) << "\n"
      << "fullscreen=" << b(s.fullscreen) << "\n"
      << "integer_scale=" << b(s.integer_scale) << "\n"
      << "show_stats=" << b(s.show_stats) << "\n";
    return static_cast<bool>(f);
}
