#include "audio.h"

#include <pspkernel.h>
#include <pspsdk.h>
#include <string.h>

#include "systemctrl.h"

#define AUDIO_MODULE "sceAudio_Driver"
#define AUDIO_LIB    "sceAudio"

#define NID_ChReserve           0x5EC81C55
#define NID_SetChannelDataLen   0xCB2E439E
#define NID_ChangeChannelConfig 0x95FD0C2D
#define NID_Output              0x8C1009B2
#define NID_OutputBlocking      0x136CAF51
#define NID_OutputPanned        0xE2D56B2D
#define NID_OutputPannedBlocking 0x13F592BC
#define NID_SRCChReserve        0x38553111
#define NID_SRCOutputBlocking   0xE0727056
#define NID_Output2Reserve      0x01562BA3
#define NID_Output2ChangeLength 0x63F2889C
#define NID_Output2OutputBlocking 0x2D53F36E

#define FORMAT_MONO 0x10
#define SOURCES     10 // 8 channels + SRC + Output2

// What a source plays: learned from the reserve calls.
typedef struct {
    u32 samples;
    u32 rate;
    u32 channels;
} source_config;

static source_config g_src[SOURCES];
static volatile int g_capture;
static void (*g_on_data)(void);
static int g_hooked;
static u32 g_dropped;

// Counters for PSPCAP_PKT_AUDIO_DIAG, see audio_diag().
static pspcap_audio_source_diag g_diag[SOURCES];
static u32 g_last_call_us[SOURCES];

// ---- ring of captured blocks ----------------------------------------------------
// Record: u32 pcm bytes | pspcap_audio_info | PCM, padded to 4 bytes.
// A length of RING_SKIP means "the rest up to the end of the ring is unused".
// Writers are game threads inside their syscalls, the reader is our thread;
// both sides work with interrupts off (single core: that is atomic).

#define RING_SIZE  (128 * 1024) // power of 2: the u32 counters wrap cleanly; ~185 ms of 4 stereo channels
#define RING_SKIP  0xFFFFFFFFu
#define MAX_BLOCK  (16 * 1024) // larger blocks are dropped

static u8 g_ring[RING_SIZE] __attribute__((aligned(64)));
static u32 g_head; // bytes written, ever
static u32 g_tail; // bytes read, ever

static void ring_push(u32 source, int vol_left, int vol_right, const void *pcm)
{
    const source_config *c = &g_src[source];
    u32 bytes = c->samples * c->channels * 2;
    if (!c->samples || bytes > MAX_BLOCK) {
        g_dropped++;
        return;
    }
    u32 need = 4 + sizeof(pspcap_audio_info) + ((bytes + 3) & ~3u);

    int intr = pspSdkDisableInterrupts();
    u32 pos = g_head % RING_SIZE;
    u32 pad = pos + need > RING_SIZE ? RING_SIZE - pos : 0;
    if (RING_SIZE - (g_head - g_tail) < need + pad) {
        g_dropped++;
        pspSdkEnableInterrupts(intr);
        return;
    }
    if (pad) {
        *(u32 *)(g_ring + pos) = RING_SKIP;
        g_head += pad;
        pos = 0;
    }
    pspcap_audio_info info;
    info.source = source;
    info.rate = c->rate;
    info.channels = c->channels;
    info.samples = c->samples;
    info.bytes = bytes;
    info.vol_left = (u16)(vol_left > 0x8000 ? 0x8000 : vol_left < 0 ? 0 : vol_left);
    info.vol_right = (u16)(vol_right > 0x8000 ? 0x8000 : vol_right < 0 ? 0 : vol_right);
    info.psp_time_us = (u64)sceKernelGetSystemTimeWide();
    *(u32 *)(g_ring + pos) = bytes;
    memcpy(g_ring + pos + 4, &info, sizeof(info));
    memcpy(g_ring + pos + 4 + sizeof(info), pcm, bytes);
    g_head += need;
    g_diag[source].captured++;
    pspSdkEnableInterrupts(intr);
}

int audio_pop(pspcap_audio_info *info, void *pcm, int max)
{
    int intr = pspSdkDisableInterrupts();
    for (;;) {
        if (g_tail == g_head) {
            pspSdkEnableInterrupts(intr);
            return 0;
        }
        u32 pos = g_tail % RING_SIZE;
        u32 bytes = *(u32 *)(g_ring + pos);
        if (bytes == RING_SKIP) {
            g_tail += RING_SIZE - pos;
            continue;
        }
        memcpy(info, g_ring + pos + 4, sizeof(*info));
        int ok = (int)bytes <= max;
        if (ok)
            memcpy(pcm, g_ring + pos + 4 + sizeof(*info), bytes);
        g_tail += 4 + sizeof(*info) + ((bytes + 3) & ~3u);
        pspSdkEnableInterrupts(intr);
        if (!ok) {
            g_dropped++;
            return 0;
        }
        return (int)bytes;
    }
}

u32 audio_dropped(void)
{
    return g_dropped;
}

void audio_set_capture(int on)
{
    // A repeated STREAM_START (new options) must not throw away queued sound.
    if (on && !g_capture) {
        int intr = pspSdkDisableInterrupts();
        g_tail = g_head; // start fresh
        pspSdkEnableInterrupts(intr);
    }
    g_capture = on;
}

// Hooks run on game threads inside their syscalls (k1 = user): the PCM
// pointers are the game's, readable from kernel code as they are. They call
// the original first, so the block reaches the hardware on time, and copy it
// afterwards (the game has not touched the buffer yet: it is still in here).
// They do not wake the stream thread for every block: it outranks the game's
// audio thread and would delay it (measured: 42 blocks/s instead of 43.07).
// Sound goes out with the video frames; the thread is woken only when the
// ring is half full, e.g. while the game is not flipping frames.
// Every output call is counted, whatever the capture state, so the PC can
// compare what the game played with what reached it.
static void diag_enter(u32 source)
{
    pspcap_audio_source_diag *d = &g_diag[source];
    u32 now = (u32)sceKernelGetSystemTimeWide();
    if (d->calls && now - g_last_call_us[source] > d->max_gap_us)
        d->max_gap_us = now - g_last_call_us[source];
    g_last_call_us[source] = now;
    d->calls++;
    u32 n = g_src[source].samples;
    if (!d->min_samples || n < d->min_samples)
        d->min_samples = n;
    if (n > d->max_samples)
        d->max_samples = n;
}

static void diag_leave(u32 source, u32 fn, int ret)
{
    pspcap_audio_source_diag *d = &g_diag[source];
    d->funcs |= fn;
    if (ret < 0) {
        d->failed++;
        d->last_error = (u32)ret;
    } else {
        d->last_ret = ret;
    }
}

void audio_diag(pspcap_audio_diag *out)
{
    int intr = pspSdkDisableInterrupts();
    memcpy(out->src, g_diag, sizeof(out->src));
    out->ring_used = g_head - g_tail;
    for (int i = 0; i < SOURCES; i++) {
        g_diag[i].max_gap_us = 0;
        g_diag[i].min_samples = 0;
        g_diag[i].max_samples = 0;
    }
    pspSdkEnableInterrupts(intr);
}

static void capture(u32 source, int vol_left, int vol_right, const void *pcm)
{
    if (!g_capture || !pcm || source >= SOURCES)
        return;
    ring_push(source, vol_left, vol_right, pcm);
    if (g_on_data && g_head - g_tail > RING_SIZE / 2) {
        u32 k1 = pspSdkSetK1(0);
        g_on_data();
        pspSdkSetK1(k1);
    }
}

// ---- hooks ------------------------------------------------------------------------

static int (*o_ChReserve)(int, int, int);
static int (*o_SetChannelDataLen)(int, int);
static int (*o_ChangeChannelConfig)(int, int);
static int (*o_Output)(int, int, void *);
static int (*o_OutputBlocking)(int, int, void *);
static int (*o_OutputPanned)(int, int, int, void *);
static int (*o_OutputPannedBlocking)(int, int, int, void *);
static int (*o_SRCChReserve)(int, int, int);
static int (*o_SRCOutputBlocking)(int, void *);
static int (*o_Output2Reserve)(int);
static int (*o_Output2ChangeLength)(int);
static int (*o_Output2OutputBlocking)(int, void *);

static int h_ChReserve(int channel, int samples, int format)
{
    int ret = o_ChReserve(channel, samples, format);
    if (ret >= 0 && ret < 8) {
        g_src[ret].samples = (u32)samples;
        g_src[ret].rate = 44100;
        g_src[ret].channels = format == FORMAT_MONO ? 1 : 2;
    }
    return ret;
}

static int h_SetChannelDataLen(int channel, int samples)
{
    int ret = o_SetChannelDataLen(channel, samples);
    if (ret >= 0 && channel >= 0 && channel < 8)
        g_src[channel].samples = (u32)samples;
    return ret;
}

static int h_ChangeChannelConfig(int channel, int format)
{
    int ret = o_ChangeChannelConfig(channel, format);
    if (ret >= 0 && channel >= 0 && channel < 8)
        g_src[channel].channels = format == FORMAT_MONO ? 1 : 2;
    return ret;
}

static int h_Output(int channel, int vol, void *buf)
{
    int ok = (u32)channel < 8;
    if (ok)
        diag_enter((u32)channel);
    int ret = o_Output(channel, vol, buf);
    if (ok)
        diag_leave((u32)channel, PSPCAP_AUDIO_FN_OUTPUT, ret);
    if (ret >= 0 && ok)
        capture((u32)channel, vol, vol, buf);
    return ret;
}

static int h_OutputBlocking(int channel, int vol, void *buf)
{
    int ok = (u32)channel < 8;
    if (ok)
        diag_enter((u32)channel);
    int ret = o_OutputBlocking(channel, vol, buf);
    if (ok)
        diag_leave((u32)channel, PSPCAP_AUDIO_FN_OUTPUT_BLOCKING, ret);
    if (ret >= 0 && ok)
        capture((u32)channel, vol, vol, buf);
    return ret;
}

static int h_OutputPanned(int channel, int left, int right, void *buf)
{
    int ok = (u32)channel < 8;
    if (ok)
        diag_enter((u32)channel);
    int ret = o_OutputPanned(channel, left, right, buf);
    if (ok)
        diag_leave((u32)channel, PSPCAP_AUDIO_FN_PANNED, ret);
    if (ret >= 0 && ok)
        capture((u32)channel, left, right, buf);
    return ret;
}

static int h_OutputPannedBlocking(int channel, int left, int right, void *buf)
{
    int ok = (u32)channel < 8;
    if (ok)
        diag_enter((u32)channel);
    int ret = o_OutputPannedBlocking(channel, left, right, buf);
    if (ok)
        diag_leave((u32)channel, PSPCAP_AUDIO_FN_PANNED_BLOCKING, ret);
    if (ret >= 0 && ok)
        capture((u32)channel, left, right, buf);
    return ret;
}

static int h_SRCChReserve(int samples, int freq, int channels)
{
    int ret = o_SRCChReserve(samples, freq, channels);
    if (ret >= 0) {
        g_src[PSPCAP_AUDIO_SRC_SRC].samples = (u32)samples;
        g_src[PSPCAP_AUDIO_SRC_SRC].rate = freq ? (u32)freq : 44100;
        g_src[PSPCAP_AUDIO_SRC_SRC].channels = channels == 1 ? 1 : 2;
    }
    return ret;
}

static int h_SRCOutputBlocking(int vol, void *buf)
{
    diag_enter(PSPCAP_AUDIO_SRC_SRC);
    int ret = o_SRCOutputBlocking(vol, buf);
    diag_leave(PSPCAP_AUDIO_SRC_SRC, PSPCAP_AUDIO_FN_SRC, ret);
    if (ret >= 0)
        capture(PSPCAP_AUDIO_SRC_SRC, vol, vol, buf);
    return ret;
}

static int h_Output2Reserve(int samples)
{
    int ret = o_Output2Reserve(samples);
    if (ret >= 0) {
        g_src[PSPCAP_AUDIO_SRC_OUTPUT2].samples = (u32)samples;
        g_src[PSPCAP_AUDIO_SRC_OUTPUT2].rate = 44100;
        g_src[PSPCAP_AUDIO_SRC_OUTPUT2].channels = 2;
    }
    return ret;
}

static int h_Output2ChangeLength(int samples)
{
    int ret = o_Output2ChangeLength(samples);
    if (ret >= 0)
        g_src[PSPCAP_AUDIO_SRC_OUTPUT2].samples = (u32)samples;
    return ret;
}

static int h_Output2OutputBlocking(int vol, void *buf)
{
    diag_enter(PSPCAP_AUDIO_SRC_OUTPUT2);
    int ret = o_Output2OutputBlocking(vol, buf);
    diag_leave(PSPCAP_AUDIO_SRC_OUTPUT2, PSPCAP_AUDIO_FN_OUTPUT2, ret);
    if (ret >= 0)
        capture(PSPCAP_AUDIO_SRC_OUTPUT2, vol, vol, buf);
    return ret;
}

typedef struct {
    u32 nid;
    void *hook;
    void **orig;
} hook_entry;

#define HOOK(name) { NID_##name, (void *)h_##name, (void **)&o_##name }
static const hook_entry g_hooks[] = {
    HOOK(ChReserve),      HOOK(SetChannelDataLen),   HOOK(ChangeChannelConfig),
    HOOK(Output),         HOOK(OutputBlocking),      HOOK(OutputPanned),
    HOOK(OutputPannedBlocking), HOOK(SRCChReserve),  HOOK(SRCOutputBlocking),
    HOOK(Output2Reserve), HOOK(Output2ChangeLength), HOOK(Output2OutputBlocking),
};
#define HOOK_COUNT (int)(sizeof(g_hooks) / sizeof(g_hooks[0]))

int audio_hooks_install(void (*on_data)(void))
{
    if (g_hooked)
        return g_hooked;
    // Resolve everything first: hook all or nothing.
    u32 addr[HOOK_COUNT];
    for (int i = 0; i < HOOK_COUNT; i++) {
        addr[i] = sctrlHENFindFunction(AUDIO_MODULE, AUDIO_LIB, g_hooks[i].nid);
        if (!addr[i])
            return 0;
    }
    g_on_data = on_data;
    for (int i = 0; i < HOOK_COUNT; i++) {
        *g_hooks[i].orig = (void *)addr[i];
        sctrlHENPatchSyscall((void *)addr[i], g_hooks[i].hook);
    }
    g_hooked = HOOK_COUNT;
    return g_hooked;
}

void audio_hooks_remove(void)
{
    if (!g_hooked)
        return;
    g_capture = 0;
    for (int i = 0; i < HOOK_COUNT; i++)
        sctrlHENPatchSyscall(g_hooks[i].hook, *g_hooks[i].orig);
    g_hooked = 0;
}

int audio_hooks_installed(void)
{
    return g_hooked != 0;
}
