// Access to the game's framebuffer through sceDisplay (resolved at runtime).
#ifndef PSPCAP_DISPLAY_H
#define PSPCAP_DISPLAY_H

#include <psptypes.h>

typedef struct {
    void *addr;       // as set by the game (usually a VRAM address)
    int stride;       // row length in pixels, usually 512
    int pixel_format; // PSP_DISPLAY_PIXEL_FORMAT_* == enum pspcap_pixel_format
} display_frame;

// Looks up the sceDisplay functions. Returns 0 if all were found.
int display_init(void);

// The framebuffer that is (or will be at the next vblank) on screen.
// Returns 0, or a negative sce error / -1 if display_init() failed.
int display_get_frame(display_frame *frame);

// Blocks until the next vertical blank starts.
void display_wait_vblank(void);

// Hooks the game's sceDisplaySetFrameBuf syscall. The hook always calls the
// original first; it records every successful flip and, while notifications
// are on, calls `on_flip` (on the game's thread, so it must be quick).
// Returns 0, or -1 if the function was not found.
int display_hook_install(void (*on_flip)(void));
void display_hook_remove(void);
int display_hook_installed(void);
void display_set_notify(int on);

// Number of flips seen since the hook was installed.
u32 display_flip_count(void);

// The frame of the most recent flip and its flip number (1-based).
// Returns -1 if there was no flip yet.
int display_latest(display_frame *frame, u32 *flip_id);

#endif
