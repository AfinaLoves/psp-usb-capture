// pspcap — PC client for PSP screen + audio capture over USB.
// Live video + sound window with a settings menu, plus the USB link tools.

#include <glad/glad.h>

#include <SDL.h>
#include <libusb.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "commands.h"
#include "pspcap_protocol.h"
#include "audio_out.h"
#include "frame_receiver.h"
#include "pixel_format.h"
#include "renderer.h"
#include "settings.h"
#include "settings_menu.h"

namespace {

constexpr int kPspWidth = 480;
constexpr int kPspHeight = 272;

void print_versions()
{
    SDL_version sdl;
    SDL_GetVersion(&sdl);
    const libusb_version *usb = libusb_get_version();

    std::printf("pspcap %s\n", PSPCAP_VERSION);
    std::printf("  SDL    %d.%d.%d\n", sdl.major, sdl.minor, sdl.patch);
    std::printf("  libusb %u.%u.%u.%u\n", usb->major, usb->minor, usb->micro, usb->nano);
}

void print_usage()
{
    std::printf("usage: pspcap [command]\n"
                "  (none)          live video window; Esc opens the settings menu, F11\n"
                "                  toggles fullscreen; settings are kept in pspcap.ini\n"
                "  --probe         HELLO + 100 pings, then exit\n"
                "  --monitor       wait for the PSP, ping every second, report (dis)connects\n"
                "  --bench [MiB]   USB throughput matrix, MiB per run (default 16)\n"
                "  --snapshot [file.png]  save the PSP screen (default snapshot-<time>.png)\n"
                "  --fps [s]       game frame rate without streaming (default 5 s)\n"
                "  --load          game fps while USB carries bench data only (no copying)\n"
                "  --trace [s]     debug: start a stream and print every packet (default 3 s)\n"
                "  --help          this text\n"
                "window options (stream tuning, see docs/notes.md):\n"
                "  --565           convert 32-bit frames to 16 bits on the PSP\n"
                "  --uncached      read VRAM uncached (old, slow way, for comparison)\n"
                "  --dma           copy frames with the PSP's DMA controller (least CPU)\n"
                "  --audio         also play the game's sound (experimental)\n"
                "  --grab          take the whole frame at once, send it at low priority\n"
                "                  (--prio then sets the sending priority, default 0x20)\n"
                "  --prio N        PSP stream thread priority, e.g. 0x30 (default: plugin's)\n"
                "  --label TEXT    show TEXT in the window title (live tests)\n"
                "  --delay S       count down S seconds in the title before streaming\n"
                "  --yield N       with --565: sleep N us every %d converted rows (0..%d,\n"
                "                  default 200; 0 = never)\n"
                "  --drift MODE    how sound follows the PSP clock: resample (default,\n"
                "                  rate within 0.5%%) or drop (drop/repeat single samples)\n"
                "  --control FILE  re-read FILE twice a second; when it changes, apply the\n"
                "                  window options in it (same syntax, e.g. --565 --yield 50\n"
                "                  --label \"try 2\") without reconnecting\n",
                PSPCAP_YIELD_ROWS, PSPCAP_YIELD_MAX_US);
}

struct WindowOptions {
    uint32_t flags = 0;
    uint32_t priority = 0;
    uint32_t yield_us = 200; // best measured value for --565, docs/notes.md
    std::string label;
    int delay_s = 0;
    std::string control; // --control FILE
    AudioOut::Drift drift = AudioOut::Drift::Resample;
    std::string dump; // --dump DIR
    bool stream_given = false; // stream options on the command line override pspcap.ini
    bool drift_given = false;
};

int run_window(const WindowOptions &opt);

// Splits a control file into arguments: whitespace separated, "double quotes"
// group words. Lines starting with # are comments.
std::vector<std::string> split_args(const std::string &text)
{
    std::vector<std::string> args;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        size_t i = 0;
        while (i < line.size()) {
            while (i < line.size() && std::isspace(static_cast<unsigned char>(line[i])))
                i++;
            if (i >= line.size() || line[i] == '#')
                break;
            std::string arg;
            if (line[i] == '"') {
                size_t end = line.find('"', i + 1);
                if (end == std::string::npos)
                    end = line.size();
                arg = line.substr(i + 1, end - i - 1);
                i = end + 1;
            } else {
                while (i < line.size() && !std::isspace(static_cast<unsigned char>(line[i])))
                    arg += line[i++];
            }
            args.push_back(arg);
        }
    }
    return args;
}

// Window options; returns false (after printing why) on a bad option.
bool parse_window_options(int argc, char **argv, WindowOptions &opt)
{
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--565") == 0) {
            opt.flags |= PSPCAP_STREAM_RGB565;
            opt.stream_given = true;
        } else if (std::strcmp(argv[i], "--uncached") == 0) {
            opt.flags |= PSPCAP_STREAM_UNCACHED;
            opt.stream_given = true;
        } else if (std::strcmp(argv[i], "--dma") == 0) {
            opt.flags |= PSPCAP_STREAM_DMA;
            opt.stream_given = true;
        } else if (std::strcmp(argv[i], "--grab") == 0) {
            opt.flags |= PSPCAP_STREAM_GRAB;
            opt.stream_given = true;
        } else if (std::strcmp(argv[i], "--audio") == 0) {
            opt.flags |= PSPCAP_STREAM_AUDIO;
            opt.stream_given = true;
        } else if (std::strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            opt.label = argv[++i];
        } else if (std::strcmp(argv[i], "--delay") == 0 && i + 1 < argc) {
            opt.delay_s = std::atoi(argv[++i]);
            if (opt.delay_s < 0 || opt.delay_s > 60) {
                std::fprintf(stderr, "--delay must be 0..60\n");
                return false;
            }
        } else if (std::strcmp(argv[i], "--yield") == 0 && i + 1 < argc) {
            long us = std::strtol(argv[++i], nullptr, 0);
            if (us < 0 || us > PSPCAP_YIELD_MAX_US) {
                std::fprintf(stderr, "--yield must be 0..%d\n", PSPCAP_YIELD_MAX_US);
                return false;
            }
            opt.yield_us = static_cast<uint32_t>(us);
            opt.stream_given = true;
        } else if (std::strcmp(argv[i], "--drift") == 0 && i + 1 < argc) {
            const char *mode = argv[++i];
            opt.drift_given = true;
            if (std::strcmp(mode, "resample") == 0) {
                opt.drift = AudioOut::Drift::Resample;
            } else if (std::strcmp(mode, "drop") == 0) {
                opt.drift = AudioOut::Drift::Drop;
            } else {
                std::fprintf(stderr, "--drift must be resample or drop\n");
                return false;
            }
        } else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            opt.dump = argv[++i];
        } else if (std::strcmp(argv[i], "--control") == 0 && i + 1 < argc) {
            opt.control = argv[++i];
        } else if (std::strcmp(argv[i], "--prio") == 0 && i + 1 < argc) {
            opt.priority = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
            if (opt.priority < 0x08 || opt.priority > 0x7F) {
                std::fprintf(stderr, "--prio must be 0x08..0x7F\n");
                return false;
            }
            opt.stream_given = true;
        } else {
            print_usage();
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    SDL_SetMainReady();
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    const char *cmd = argc > 1 ? argv[1] : nullptr;
    if (!cmd || std::strcmp(cmd, "--565") == 0 || std::strcmp(cmd, "--uncached") == 0 ||
        std::strcmp(cmd, "--dma") == 0 || std::strcmp(cmd, "--grab") == 0 ||
        std::strcmp(cmd, "--audio") == 0 ||
        std::strcmp(cmd, "--prio") == 0 || std::strcmp(cmd, "--yield") == 0 ||
        std::strcmp(cmd, "--control") == 0 || std::strcmp(cmd, "--dump") == 0 ||
        std::strcmp(cmd, "--label") == 0 || std::strcmp(cmd, "--delay") == 0) {
        WindowOptions opt;
        if (!parse_window_options(argc, argv, opt))
            return 2;
        return run_window(opt);
    }
    if (std::strcmp(cmd, "--probe") == 0)
        return run_probe();
    if (std::strcmp(cmd, "--monitor") == 0)
        return run_monitor();
    if (std::strcmp(cmd, "--bench") == 0) {
        int mb = argc > 2 ? std::atoi(argv[2]) : 16;
        if (mb < 1 || mb > 1024) {
            std::fprintf(stderr, "--bench: MiB must be 1..1024\n");
            return 2;
        }
        return run_bench(static_cast<unsigned>(mb));
    }
    if (std::strcmp(cmd, "--snapshot") == 0)
        return run_snapshot(argc > 2 ? argv[2] : nullptr);
    if (std::strcmp(cmd, "--fps") == 0) {
        int seconds = argc > 2 ? std::atoi(argv[2]) : 5;
        if (seconds < 1 || seconds > 600) {
            std::fprintf(stderr, "--fps: seconds must be 1..600\n");
            return 2;
        }
        return run_fps(static_cast<unsigned>(seconds));
    }
    if (std::strcmp(cmd, "--load") == 0)
        return run_load();
    if (std::strcmp(cmd, "--trace") == 0)
        return run_trace(argc > 2 ? static_cast<unsigned>(std::atoi(argv[2])) : 3);
    print_usage();
    return std::strcmp(cmd, "--help") == 0 ? 0 : 2;
}

namespace {

std::string window_title(const FrameReceiver::Stats &s)
{
    char buf[256];
    if (!s.connected) {
        std::snprintf(buf, sizeof(buf), "pspcap %s - %s", PSPCAP_VERSION, s.status.c_str());
    } else {
        std::snprintf(buf, sizeof(buf),
                      "pspcap - game %.1f fps | received %.1f fps | %.1f MiB/s | skipped %llu | "
                      "%s, PSP send %.1f ms, copy %.1f ms | audio %.0f KiB/s",
                      s.game_fps, s.recv_fps, s.mib_s, static_cast<unsigned long long>(s.skipped),
                      psp_format_name(s.pixel_format), s.psp_send_ms, s.psp_copy_ms,
                      s.audio_kib_s);
    }
    return buf;
}

int run_window(const WindowOptions &opt)
{
    print_versions();

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    SDL_Window *window = SDL_CreateWindow(
        "pspcap " PSPCAP_VERSION, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        kPspWidth * 2, kPspHeight * 2,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    SDL_GLContext gl = SDL_GL_CreateContext(window);
    if (!gl || !gladLoadGLLoader(reinterpret_cast<GLADloadproc>(SDL_GL_GetProcAddress))) {
        std::fprintf(stderr, "OpenGL 3.3 setup failed: %s\n", SDL_GetError());
        if (gl)
            SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);
    std::printf("  OpenGL %s (%s)\n", reinterpret_cast<const char *>(glGetString(GL_VERSION)),
                reinterpret_cast<const char *>(glGetString(GL_RENDERER)));

    Renderer renderer;
    std::string err;
    if (!renderer.init(err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        SDL_GL_DeleteContext(gl);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    const std::string ini_path = settings_path();
    Settings settings;
    load_settings(ini_path, settings);
    uint32_t extra_flags = 0; // command-line only: --uncached, --grab
    if (opt.stream_given) {
        settings.rgb565 = (opt.flags & PSPCAP_STREAM_RGB565) != 0;
        settings.audio = (opt.flags & PSPCAP_STREAM_AUDIO) != 0;
        settings.dma = (opt.flags & PSPCAP_STREAM_DMA) != 0;
        settings.yield_us = opt.yield_us;
        settings.priority = opt.priority;
        extra_flags = opt.flags & (PSPCAP_STREAM_UNCACHED | PSPCAP_STREAM_GRAB);
    }
    if (opt.drift_given)
        settings.resample = opt.drift == AudioOut::Drift::Resample;
    auto stream_flags = [&]() { return settings.stream_flags() | extra_flags; };

    std::printf("stream options: flags 0x%X, priority 0x%X (0 = plugin default), yield %u us, "
                "label \"%s\"\n",
                stream_flags(), settings.priority, settings.yield_us, opt.label.c_str());
    FrameReceiver receiver(stream_flags(), settings.priority, settings.yield_us);
    AudioOut audio;
    audio.set_drift(settings.resample ? AudioOut::Drift::Resample : AudioOut::Drift::Drop);
    audio.set_volume(settings.volume / 100.0f);
    if (!opt.dump.empty()) {
        if (audio.start_dump(opt.dump, err))
            std::printf("dumping sound to %s (Space in the window adds a marker)\n",
                        opt.dump.c_str());
        else
            std::fprintf(stderr, "%s\n", err.c_str());
    }
    uint32_t marks = 0;
    uint32_t mark_shown_until = 0;
    // Always open: the sound can be switched on later from the menu.
    {
        if (audio.open(err))
            receiver.set_audio_sink(
                [&audio](const pspcap_audio_info &info, const int16_t *pcm) { audio.push(info, pcm); });
        else
            std::fprintf(stderr, "no sound: %s\n", err.c_str());
    }
    std::string prefix = opt.label.empty() ? "" : opt.label + " | ";
    bool with_audio = settings.audio;
    std::string control_text;
    uint32_t control_changes = 0;
    uint32_t last_control = 0;
    // Live tests without restarting anything: new options from the control file.
    auto poll_control = [&]() {
        std::ifstream f(opt.control, std::ios::binary);
        if (!f)
            return;
        std::stringstream ss;
        ss << f.rdbuf();
        if (ss.str() == control_text)
            return;
        control_text = ss.str();
        std::vector<std::string> args = split_args(control_text);
        std::vector<char *> argv{const_cast<char *>("pspcap")};
        for (std::string &a : args)
            argv.push_back(a.data());
        WindowOptions next;
        if (!parse_window_options(static_cast<int>(argv.size()), argv.data(), next)) {
            std::fprintf(stderr, "control file ignored: bad options\n");
            return;
        }
        receiver.set_options(next.flags, next.priority, next.yield_us);
        audio.set_drift(next.drift);
        settings.rgb565 = (next.flags & PSPCAP_STREAM_RGB565) != 0;
        settings.audio = (next.flags & PSPCAP_STREAM_AUDIO) != 0;
        settings.dma = (next.flags & PSPCAP_STREAM_DMA) != 0;
        settings.yield_us = next.yield_us;
        settings.priority = next.priority;
        settings.resample = next.drift == AudioOut::Drift::Resample;
        extra_flags = next.flags & (PSPCAP_STREAM_UNCACHED | PSPCAP_STREAM_GRAB);
        audio.mark(1000 + ++control_changes); // where each variant starts in the dump
        prefix = next.label.empty() ? "" : next.label + " | ";
        with_audio = (next.flags & PSPCAP_STREAM_AUDIO) != 0;
        std::time_t t = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &t);
        char stamp[16];
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
        std::printf("[%s] === control: flags 0x%X, priority 0x%X, yield %u us, drift %s, label \"%s\"\n",
                    stamp, next.flags, next.priority, next.yield_us,
                    next.drift == AudioOut::Drift::Resample ? "resample" : "drop",
                    next.label.c_str());
    };
    auto status_line = [&]() {
        FrameReceiver::Stats s = receiver.stats();
        std::string line = window_title(s);
        if (with_audio && s.connected) {
            char buf[192];
            double in_s = 0, out_s = 0, dev_s = 0;
            audio.rates(in_s, out_s, dev_s);
            std::snprintf(buf, sizeof(buf),
                          " | PSP drops %u, PC underruns %llu, holes filled %llu, trims %llu | "
                          "in %.0f/s, played %.0f/s, device %.0f/s | blocks PSP-PC %lld",
                          s.psp_audio_dropped, static_cast<unsigned long long>(audio.underruns()),
                          static_cast<unsigned long long>(audio.holes()),
                          static_cast<unsigned long long>(audio.trims()), in_s, out_s, dev_s,
                          static_cast<long long>(s.psp_audio_sent) -
                              static_cast<long long>(s.pc_audio_blocks));
            line += buf;
        }
        return line;
    };

    SettingsMenu menu;
    if (!menu.init(window))
        std::fprintf(stderr, "the settings menu could not start\n");
    auto apply_display = [&]() {
        SDL_SetWindowFullscreen(window, settings.fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    };
    if (settings.fullscreen)
        apply_display();
    // A short title for everyday use; the numbers with "Numbers in the title
    // bar", a test label or a control file.
    auto title = [&]() {
        if (settings.show_stats || !prefix.empty() || !opt.control.empty())
            return prefix + status_line();
        FrameReceiver::Stats st = receiver.stats();
        std::string t = "pspcap " PSPCAP_VERSION " - ";
        t += st.connected ? "streaming" : st.status;
        return t + " (Esc: settings)";
    };

    std::vector<uint8_t> frame;
    int frame_w = 0, frame_h = 0;
    uint32_t last_title = 0;
    uint32_t last_log = 0;
    const uint32_t start_at = SDL_GetTicks() + static_cast<uint32_t>(opt.delay_s) * 1000;
    bool started = false;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT)
                running = false;
            else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_ESCAPE && !ev.key.repeat)
                menu.toggle();
            else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F11 && !ev.key.repeat) {
                settings.fullscreen = !settings.fullscreen;
                apply_display();
            } else if (menu.handle_event(ev))
                continue;
            else if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_SPACE &&
                     !ev.key.repeat && !opt.dump.empty()) {
                // The player marks what they heard; the dump has the same marker.
                audio.mark(++marks);
                std::time_t t = std::time(nullptr);
                std::tm tm{};
                localtime_s(&tm, &t);
                char stamp[16];
                std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
                std::printf("[%s] *** MARK %u\n", stamp, marks);
                mark_shown_until = SDL_GetTicks() + 1000;
                last_title = 0;
            }
        }

        if (receiver.take_frame(frame, frame_w, frame_h))
            renderer.upload(frame.data(), frame_w, frame_h);

        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window, &w, &h);
        renderer.draw(w, h, settings.integer_scale);
        if (menu.visible()) {
            FrameReceiver::Stats st = receiver.stats();
            SettingsMenu::Status ms;
            ms.connected = st.connected;
            ms.text = st.status;
            ms.game_fps = st.game_fps;
            ms.recv_fps = st.recv_fps;
            ms.underruns = audio.underruns();
            ms.holes = audio.holes();
            SettingsMenu::Result changed = menu.frame(settings, ms, ini_path);
            if (changed.stream) {
                receiver.set_options(stream_flags(), settings.priority, settings.yield_us);
                with_audio = settings.audio;
            }
            if (changed.audio) {
                audio.set_volume(settings.volume / 100.0f);
                audio.set_drift(settings.resample ? AudioOut::Drift::Resample
                                                  : AudioOut::Drift::Drop);
            }
            if (changed.display)
                apply_display();
            if (changed.quit)
                running = false;
        }
        SDL_GL_SwapWindow(window);

        uint32_t now = SDL_GetTicks();
        if (!started) {
            // Live tests: the player watches this title, not the chat.
            if (static_cast<int32_t>(now - start_at) >= 0) {
                receiver.start();
                started = true;
            } else if (now - last_title >= 200) {
                last_title = now;
                std::string t = prefix + "starts in " +
                                std::to_string((start_at - now + 999) / 1000) + " s";
                SDL_SetWindowTitle(window, t.c_str());
            }
            continue;
        }
        if (!opt.control.empty() && now - last_control >= 500) {
            last_control = now;
            poll_control();
        }
        if (now - last_title >= 500) {
            last_title = now;
            std::string mark = static_cast<int32_t>(mark_shown_until - now) > 0
                                   ? "MARK " + std::to_string(marks) + " | "
                                   : "";
            SDL_SetWindowTitle(window, (mark + title()).c_str());
        }
        // Same numbers on the console once a second, so runs can be logged.
        if (now - last_log >= 1000) {
            last_log = now;
            audio.flush_dump();
            std::time_t t = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &t);
            char stamp[16];
            std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
            std::printf("[%s] %s%s\n", stamp, prefix.c_str(), status_line().c_str());
            if (with_audio)
                std::printf("[%s] audio in/played/queue:%s\n", stamp, audio.debug_line().c_str());
        }
    }

    receiver.stop();
    menu.shutdown();
    renderer.destroy();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace
