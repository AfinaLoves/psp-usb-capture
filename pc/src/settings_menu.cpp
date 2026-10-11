#include "settings_menu.h"

#include <imgui.h>
#include <imgui_impl_opengl3.h>

#include "pspcap_protocol.h"

bool SettingsMenu::init(SDL_Window *window)
{
    window_ = window;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // our own pspcap.ini is enough
    io.BackendPlatformName = "pspcap-sdl2";
    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowRounding = 6;
    style.FrameRounding = 4;
    style.WindowPadding = ImVec2(14, 12);
    ready_ = ImGui_ImplOpenGL3_Init("#version 330 core");
    last_ticks_ = SDL_GetPerformanceCounter();
    return ready_;
}

void SettingsMenu::shutdown()
{
    if (!ready_)
        return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui::DestroyContext();
    ready_ = false;
}

// A minimal SDL2 platform layer: the menu only needs the mouse.
bool SettingsMenu::handle_event(const SDL_Event &ev)
{
    if (!ready_ || !visible_)
        return false;
    ImGuiIO &io = ImGui::GetIO();
    switch (ev.type) {
    case SDL_MOUSEMOTION:
        io.AddMousePosEvent(static_cast<float>(ev.motion.x), static_cast<float>(ev.motion.y));
        return true;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP: {
        int button = ev.button.button == SDL_BUTTON_LEFT    ? 0
                     : ev.button.button == SDL_BUTTON_RIGHT ? 1
                     : ev.button.button == SDL_BUTTON_MIDDLE ? 2
                                                            : -1;
        if (button >= 0)
            io.AddMouseButtonEvent(button, ev.type == SDL_MOUSEBUTTONDOWN);
        return true;
    }
    case SDL_MOUSEWHEEL:
        io.AddMouseWheelEvent(static_cast<float>(ev.wheel.x), static_cast<float>(ev.wheel.y));
        return true;
    default:
        return false;
    }
}

namespace {

void hint(const char *text)
{
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", text);
}

} // namespace

SettingsMenu::Result SettingsMenu::frame(Settings &s, const Status &status,
                                         const std::string &ini_path)
{
    Result r;
    if (!ready_ || !visible_)
        return r;

    ImGuiIO &io = ImGui::GetIO();
    int w = 0, h = 0, dw = 0, dh = 0;
    SDL_GetWindowSize(window_, &w, &h);
    SDL_GL_GetDrawableSize(window_, &dw, &dh);
    io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
    if (w > 0 && h > 0)
        io.DisplayFramebufferScale = ImVec2(static_cast<float>(dw) / w, static_cast<float>(dh) / h);
    unsigned long long now = SDL_GetPerformanceCounter();
    io.DeltaTime = static_cast<float>(now - last_ticks_) /
                   static_cast<float>(SDL_GetPerformanceFrequency());
    if (io.DeltaTime <= 0)
        io.DeltaTime = 1.0f / 60;
    last_ticks_ = now;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("pspcap " PSPCAP_VERSION, nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

    if (status.connected)
        ImGui::Text("PSP: %.0f fps, received %.0f fps", status.game_fps, status.recv_fps);
    else
        ImGui::TextWrapped("%s", status.text.c_str());
    if (status.connected && s.audio)
        ImGui::TextDisabled("sound: %llu dropouts, %llu gaps patched", status.underruns,
                            status.holes);
    ImGui::Separator();

    ImGui::SeparatorText("Video");
    r.stream |= ImGui::Checkbox("16-bit color", &s.rgb565);
    hint("Converts 32-bit frames to 16 bits on the PSP. Half the USB traffic,\n"
         "no tearing in 32-bit games. Recommended.");
    if (s.rgb565) {
        int y = static_cast<int>(s.yield_us);
        ImGui::SetNextItemWidth(180);
        if (ImGui::SliderInt("Sound breathing room", &y, 0, 1000, "%d us")) {
            s.yield_us = static_cast<uint32_t>(y);
            r.stream = true;
        }
        hint("Short pauses while converting a frame, so the PSP's sound keeps\n"
             "its speed. 200 is a good value; much more slows the picture down.");
    }
    r.display |= ImGui::Checkbox("Fullscreen (F11)", &s.fullscreen);
    r.display |= ImGui::Checkbox("Integer scaling", &s.integer_scale);
    hint("Only whole multiples of 480x272: sharpest pixels, wider black borders.");

    ImGui::SeparatorText("Sound");
    r.stream |= ImGui::Checkbox("Game sound", &s.audio);
    if (s.audio) {
        ImGui::SetNextItemWidth(180);
        r.audio |= ImGui::SliderInt("Volume", &s.volume, 0, 200, "%d%%");
        r.audio |= ImGui::Checkbox("Smooth clock sync", &s.resample);
        hint("Plays the sound up to 0.5% faster or slower to follow the PSP's clock.\n"
             "Off: drops or repeats single samples instead (may click).");
    }

    if (ImGui::CollapsingHeader("Advanced")) {
        r.stream |= ImGui::Checkbox("Copy frames with DMA", &s.dma);
        hint("Less CPU on the PSP, ignores 16-bit color. Experimental.");
        static const char *prios[] = {"plugin default", "0x10", "0x18", "0x20", "0x30"};
        static const unsigned values[] = {0, 0x10, 0x18, 0x20, 0x30};
        int current = 0;
        for (int i = 0; i < 5; i++)
            if (values[i] == s.priority)
                current = i;
        ImGui::SetNextItemWidth(180);
        if (ImGui::Combo("Stream priority", &current, prios, 5)) {
            s.priority = values[current];
            r.stream = true;
        }
        hint("Priority of the plugin's thread on the PSP. Lower numbers win;\n"
             "games usually run at 0x20. Default (0x10) is the tested one.");
        ImGui::Checkbox("Numbers in the title bar", &s.show_stats);
    }

    ImGui::Separator();
    if (ImGui::Button("Resume (Esc)"))
        visible_ = false;
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        saved_note_ = save_settings(ini_path, s) ? "saved" : "could not save";
        saved_note_until_ = SDL_GetTicks() + 2000;
    }
    ImGui::SameLine();
    if (ImGui::Button("Quit"))
        r.quit = true;
    if (static_cast<int>(saved_note_until_ - SDL_GetTicks()) > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", saved_note_.c_str());
    }

    ImGui::End();
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    return r;
}
