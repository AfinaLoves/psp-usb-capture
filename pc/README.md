# pc/ — the client (`pspcap.exe`)

C++17, CMake, MSVC; SDL2, OpenGL 3.3 (glad), libusb, Dear ImGui and stb via
vcpkg (`vcpkg.json`).

- `frame_receiver.*` — USB thread: keeps the stream running, reassembles frames
- `audio_out.*` — sound: per-source queues, clock sync, dropout patching
- `renderer.*` — draws the frame
- `settings*.*`, `settings_menu.*` — the Esc menu and `pspcap.ini`
- `commands.*` — `--probe`, `--bench`, `--snapshot`, `--trace` and friends
- `tests/` — pixel format conversion test (`ctest --test-dir pc/build -C Release`)

The PSP's USB device must be bound to WinUSB (Zadig, once) — see
`docs/usb-driver.md`.
