#include "display.h"

#include <pspsdk.h>

#include "systemctrl.h"

// User exports of sceDisplay_Service; their NIDs are not randomized.
#define NID_sceDisplaySetFrameBuf      0x289D82FE
#define NID_sceDisplayGetFrameBuf      0xEEDA2E54
#define NID_sceDisplayWaitVblankStart  0x984C27E7

#define PSP_DISPLAY_SETBUF_NEXTFRAME 1

typedef int (*set_frame_buf_fn)(void *topaddr, int bufferwidth, int pixelformat, int sync);
typedef int (*get_frame_buf_fn)(void **topaddr, int *bufferwidth, int *pixelformat, int sync);
typedef int (*wait_vblank_fn)(void);

static get_frame_buf_fn g_get_frame_buf;
static wait_vblank_fn g_wait_vblank;

// Hook state. The latest frame is written by the game's thread and read by
// ours; both sides do it with interrupts off (single core: that is atomic).
static set_frame_buf_fn g_set_frame_buf; // the original
static void (*g_on_flip)(void);
static volatile int g_notify;
static display_frame g_latest;
static u32 g_flips;

int display_init(void)
{
    g_get_frame_buf = (get_frame_buf_fn)sctrlHENFindFunction(
        "sceDisplay_Service", "sceDisplay", NID_sceDisplayGetFrameBuf);
    g_wait_vblank = (wait_vblank_fn)sctrlHENFindFunction(
        "sceDisplay_Service", "sceDisplay", NID_sceDisplayWaitVblankStart);
    return g_get_frame_buf && g_wait_vblank ? 0 : -1;
}

int display_get_frame(display_frame *frame)
{
    if (!g_get_frame_buf)
        return -1;

    void *addr = 0;
    int stride = 0, format = 0;
    // NEXTFRAME: the buffer the game handed in last, i.e. its newest finished frame.
    int ret = g_get_frame_buf(&addr, &stride, &format, PSP_DISPLAY_SETBUF_NEXTFRAME);
    if (ret < 0)
        return ret;

    frame->addr = addr;
    frame->stride = stride;
    frame->pixel_format = format;
    return 0;
}

void display_wait_vblank(void)
{
    if (g_wait_vblank)
        g_wait_vblank();
}

// Runs on the game's thread inside its syscall. The original is called with
// the game's k1, so it still validates the game's arguments as user pointers.
static int hook_set_frame_buf(void *topaddr, int bufferwidth, int pixelformat, int sync)
{
    int ret = g_set_frame_buf(topaddr, bufferwidth, pixelformat, sync);
    if (ret < 0 || !topaddr)
        return ret;

    int intr = pspSdkDisableInterrupts();
    g_latest.addr = topaddr;
    g_latest.stride = bufferwidth;
    g_latest.pixel_format = pixelformat;
    g_flips++;
    pspSdkEnableInterrupts(intr);

    if (g_notify && g_on_flip) {
        // Our kernel objects reject callers marked as user mode by k1:
        // act as kernel for the callback, then restore the game's k1.
        u32 k1 = pspSdkSetK1(0);
        g_on_flip();
        pspSdkSetK1(k1);
    }
    return ret;
}

int display_hook_install(void (*on_flip)(void))
{
    u32 addr = sctrlHENFindFunction("sceDisplay_Service", "sceDisplay", NID_sceDisplaySetFrameBuf);
    if (!addr)
        return -1;
    g_set_frame_buf = (set_frame_buf_fn)addr;
    g_on_flip = on_flip;
    sctrlHENPatchSyscall((void *)addr, hook_set_frame_buf);
    return 0;
}

void display_hook_remove(void)
{
    if (g_set_frame_buf)
        sctrlHENPatchSyscall(hook_set_frame_buf, g_set_frame_buf);
    g_set_frame_buf = 0;
}

int display_hook_installed(void)
{
    return g_set_frame_buf != 0;
}

void display_set_notify(int on)
{
    g_notify = on;
}

u32 display_flip_count(void)
{
    return g_flips;
}

int display_latest(display_frame *frame, u32 *flip_id)
{
    int intr = pspSdkDisableInterrupts();
    *frame = g_latest;
    *flip_id = g_flips;
    pspSdkEnableInterrupts(intr);
    return *flip_id ? 0 : -1;
}
