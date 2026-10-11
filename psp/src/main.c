// pspcap — PSP screen + audio capture over USB.
// USB link, snapshot, video stream and sound, see common/pspcap_protocol.h.

#include <pspkernel.h>
#include <pspsdk.h>

#include "audio.h"
#include "display.h"
#include "log.h"
#include "session.h"
#include "systemctrl.h"
#include "usb.h"
#include "version.h"

PSP_MODULE_INFO("pspcap", PSP_MODULE_KERNEL, 0, 1);

static int g_usb_init_ret = -1;

static int main_thread(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    log_reset();
    log_printf("pspcap %s (%s) started", PSPCAP_VERSION, PSPCAP_GIT_REV);
    log_printf("HEN version 0x%08X, minor %d",
               (unsigned int)sctrlHENGetVersion(), sctrlHENGetMinorVersion());

    log_printf("display_init: %d", display_init());

    log_printf("usb_init: 0x%08X", (unsigned int)g_usb_init_ret);
    if (g_usb_init_ret < 0)
        goto out;

    int ret = usb_start();
    log_printf("usb_start: 0x%08X", (unsigned int)ret);
    if (ret != 0)
        goto out;

    // The hook only records flips and calls the original until the PC starts a stream.
    log_printf("flip hook: %d", display_hook_install(usb_wake));

    // module_start already tried; the audio module may come up a little after
    // us, so keep trying for a few seconds.
    int audio = audio_hooks_installed();
    for (int i = 0; i < 20 && !audio; i++) {
        sceKernelDelayThread(250 * 1000);
        audio = audio_hooks_install(usb_wake);
    }
    log_printf("audio hooks: %d", audio_hooks_installed());

    for (;;) {
        int speed = usb_wait_attach(60 * 1000 * 1000);
        if (speed < 0)
            continue;
        log_printf("USB attached, %s speed", speed == 2 ? "high" : "full");
        session_run(speed);
        log_printf("USB detached");
    }

out:
    sceKernelExitDeleteThread(0);
    return 0;
}

int module_start(SceSize args, void *argp)
{
    // The driver must be registered before the bus is started (in the thread).
    g_usb_init_ret = usb_init();

    // As early as possible, so the hooks see the game reserve its audio
    // channels (they learn sample counts and formats from that).
    audio_hooks_install(usb_wake);

    SceUID thid = sceKernelCreateThread("pspcap_main", main_thread, 0x20, 0x2000, 0, NULL);
    if (thid >= 0)
        sceKernelStartThread(thid, args, argp);
    return 0;
}

int module_stop(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    audio_hooks_remove();
    display_hook_remove();
    usb_term();
    log_printf("pspcap stopped");
    return 0;
}
