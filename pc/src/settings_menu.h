// The Esc menu: a small Dear ImGui panel over the picture.
#pragma once

#include <SDL.h>

#include <string>

#include "settings.h"

class SettingsMenu {
public:
    struct Status {
        bool connected = false;
        std::string text; // "streaming", "PSP not found..." etc.
        double game_fps = 0;
        double recv_fps = 0;
        unsigned long long underruns = 0;
        unsigned long long holes = 0;
    };

    // What the last frame() changed.
    struct Result {
        bool stream = false;  // flags / priority / yield: send to the PSP
        bool audio = false;   // volume / clock sync
        bool display = false; // fullscreen / scaling
        bool quit = false;
    };

    bool init(SDL_Window *window);
    void shutdown();

    bool visible() const { return visible_; }
    void toggle() { visible_ = !visible_; }

    // Feeds input to the menu; returns true if the menu used the event.
    bool handle_event(const SDL_Event &ev);

    // Builds and draws the menu (call after the picture, before swapping).
    Result frame(Settings &s, const Status &status, const std::string &ini_path);

private:
    SDL_Window *window_ = nullptr;
    bool ready_ = false;
    bool visible_ = false;
    unsigned long long last_ticks_ = 0;
    std::string saved_note_;
    unsigned saved_note_until_ = 0;
};
