// Console commands talking to the PSP over USB.
#pragma once

// HELLO + 100 pings. Returns the process exit code.
int run_probe();

// Waits for the PSP, pings once a second, reports (dis)connects. Runs until Ctrl+C.
int run_monitor();

// USB throughput matrix, `megabytes` per configuration.
int run_bench(unsigned megabytes);

// Grabs the frame on the PSP's screen into a PNG (default name: snapshot-<time>.png).
int run_snapshot(const char *path);

// Game frame rate (sceDisplaySetFrameBuf calls per second) over `seconds`.
int run_fps(unsigned seconds);

// Debug: starts a stream and prints every packet for `seconds`.
int run_trace(unsigned seconds);

// Game frame rate while the USB link is busy with benchmark data only
// (no frame copying), next to idle measurements.
int run_load();
