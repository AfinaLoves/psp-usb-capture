# psp-usb-capture

**[⬇ Download pspcap for Windows](https://github.com/AfinaLoves/psp-usb-capture/releases/latest/download/pspcap-win64.zip)**
· **[⬇ Download the PSP plugin (pspcap.prx)](https://github.com/AfinaLoves/psp-usb-capture/releases/latest/download/pspcap.prx)**
· [all releases](https://github.com/AfinaLoves/psp-usb-capture/releases)

Stream your PSP's screen **and sound** to a Windows PC over a plain USB cable.
No capture card, no video-out cable. Think RemoteJoy, but written from scratch
and with game audio.

It's two pieces:

- `pspcap.prx` — a kernel plugin for the PSP. It grabs every frame the game
  shows, taps the game's audio output and pushes both through its own USB
  driver.
- `pspcap.exe` — a small Windows client that shows the picture in a resizable
  window (or fullscreen) and plays the sound.

## Does it work?

## Does it work?

[![Sans fight recorded with pspcap](https://img.youtube.com/vi/TW87dOylzV8/maxresdefault.jpg)](https://youtu.be/TW87dOylzV8)

*Full Sans fight on PSP 3000, captured with pspcap (screen + audio, no camera).*

Yes, with caveats. What I've actually tested:

| | |
|---|---|
| Console | PSP-3000 (9g), PRO family CFW (PROMOD-C2p) |
| PC | Windows 10, any GPU with OpenGL 3.3 |
| Games | Undertale (homebrew port): full 30 fps with sound. Project DIVA 2nd: works, but the game drops from ~27 to ~19 fps while streaming (it's CPU bound) |

Other models and CFWs may well work; I just haven't tried them. The plugin
only runs inside games and homebrew, not in the XMB.

Known limitations:

- the picture is the PSP's native 480×272, scaled up with nearest neighbour
  (shaders / nicer upscaling are planned);
- heavy 3D games lose some frame rate while streaming;
- latency hasn't been measured properly yet; it feels fine for playing.

## Quick start

1. **Download** the [client](https://github.com/AfinaLoves/psp-usb-capture/releases/latest/download/pspcap-win64.zip)
   (unzip anywhere) and the [plugin](https://github.com/AfinaLoves/psp-usb-capture/releases/latest/download/pspcap.prx).
2. **Plugin.** Connect the PSP in USB mode and copy `pspcap.prx` into
   `seplugins/` on the Memory Stick. Add this line to `seplugins/game.txt`
   (make a backup of the file first):
   ```
   ms0:/seplugins/pspcap.prx 1
   ```
   If you use RemoteJoyLite, disable it in `game.txt` (set it to `0`): it
   brings its own USB driver and the two fight over the port.
3. **Start a game** on the PSP and connect the cable (no USB mode, just the
   game running with the cable plugged in).
4. **Driver, once.** Windows has no driver for the plugin's USB device, so bind
   it to WinUSB with [Zadig](https://zadig.akeo.ie/). Step by step:
   [docs/usb-driver.md](docs/usb-driver.md).
5. **Run** `pspcap.exe`. The window finds the PSP by itself and reconnects if
   you pull the cable or restart the game.

## Settings

Press **Esc** in the window for the settings menu, **F11** for fullscreen.

- **16-bit color** — on by default. The PSP converts 32-bit frames before
  sending them: half the USB traffic and no tearing in 32-bit games.
- **Sound breathing room** — tiny pauses while converting a frame so the
  PSP's sound thread keeps up. 200 µs is the sweet spot; much higher starts
  slowing the picture.
- **Integer scaling** — only whole multiples of 480×272. Sharpest pixels,
  wider black borders.
- **Game sound**, **volume**, **smooth clock sync** (follows the PSP's clock by
  resampling by at most 0.5 %; turn it off to drop/repeat single samples
  instead).
- **Advanced** — DMA frame copy, the plugin's thread priority, numbers in the
  title bar.

**Save** writes `pspcap.ini` next to the exe. Command-line options override
the file for that run (`pspcap.exe --help` lists them; there are also a few
debugging tools like `--probe`, `--bench` and `--snapshot`).

## Troubleshooting

- **"PSP not found"** — is a game running with the plugin, cable connected?
  Is RemoteJoyLite off? `seplugins/pspcap.log` on the Memory Stick tells
  whether the plugin started.
- **"no WinUSB driver"** — run Zadig again, check the USB ID is `054C 01CF`.
- **Torn frames** in 32-bit games — keep 16-bit color on.
- **Sound runs slow or crackles** — leave "sound breathing room" at about 200.

## Building

### Plugin

Built by the `psp-plugin` GitHub Actions workflow in the `pspdev/pspdev`
container; grab the `pspcap-prx` artifact from a run. Locally, with the PSPSDK
installed: `make -C psp` (`LOG=0` turns off the log file).

### Client

Visual Studio 2022 (Build Tools are enough), CMake 3.21+ and
[vcpkg](https://github.com/microsoft/vcpkg). Dependencies (SDL2, libusb, glad,
Dear ImGui, stb) are pulled by vcpkg in manifest mode; it uses `VCPKG_ROOT` if
set, otherwise `C:\vcpkg`.

```powershell
cmake -S pc -B pc/build -G "Visual Studio 17 2022" -A x64
cmake --build pc/build --config Release
pc\build\Release\pspcap.exe
```

## How it works

- The plugin hooks `sceDisplaySetFrameBuf` to know when a new frame is shown,
  reads it from VRAM through the CPU cache (much faster than uncached reads)
  and sends it from a high-priority thread, skipping frames when the PC
  can't keep up so the game is never blocked.
- `sceAudioOutput*` calls are hooked too: each block goes into a small ring
  and travels between video frames.
- The PC side mixes the PSP's sound channels, keeps ~100 ms of buffer and
  slowly adjusts the playback rate to follow the PSP's clock.
- Both sides share one protocol header, [common/pspcap_protocol.h](common/pspcap_protocol.h).

The less obvious findings (why the thread priority matters, why 16-bit mode
needs the pauses, and so on) are in [docs/notes.md](docs/notes.md).

## License

[MIT](LICENSE).
