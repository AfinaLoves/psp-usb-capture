# WinUSB driver for pspcap (one time)

Windows doesn't know the plugin's USB device (`054C:01CF`), so the client
needs the generic **WinUSB** driver bound to it. You do this once with
[Zadig](https://zadig.akeo.ie/); after that it just works.

## Steps

1. Download Zadig from <https://zadig.akeo.ie/> (a single exe, nothing to install).
2. On the PSP, start a game with `pspcap.prx` enabled and plug in the USB
   cable. The PSP must **not** be in USB mode from the XMB — just the game
   running with the cable connected.
3. Run Zadig (it asks for admin rights).
4. **Options → List All Devices**.
5. In the drop-down, pick the device whose **USB ID** is **`054C` `01CF`**.
   The name may be empty or "Unknown Device"; go by the USB ID only.
6. On the right, choose **WinUSB** and click **Install Driver** (or
   **Replace Driver**).
7. Check: `pspcap.exe --probe` should print a `PSP: pspcap ...` line and ping
   times.

## Leave these alone

| USB ID | What it is | Why not |
|---|---|---|
| `054C 01C8` | PSP in USB mode (Memory Stick) | replace its driver and the drive letter is gone |
| `054C 01C9` | PSPLink / RemoteJoyLite | RemoteJoyLite has its own PC driver |

## Undo

Device Manager → "Universal Serial Bus devices" → the `054C:01CF` device →
Uninstall device, with "Delete the driver software" ticked. Other devices are
not affected.
