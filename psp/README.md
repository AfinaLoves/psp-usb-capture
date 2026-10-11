# psp/ — the plugin (`pspcap.prx`)

Kernel-mode plugin in C on the PSPSDK, built in GitHub Actions
(`pspdev/pspdev` image).

- `display.c` — `sceDisplaySetFrameBuf` hook, latest frame info
- `audio.c` — `sceAudio*` hooks and the ring of captured blocks
- `usb.c` — the USB device driver (`sceUsbbd*`, bulk IN + OUT)
- `session.c` — commands from the PC, frame and sound streaming
- `SystemCtrlForKernel.S`, `systemctrl.h` — stubs for the PRO CFW's SystemCtrl

The log goes to `ms0:/seplugins/pspcap.log` (build with `LOG=0` to drop it).
