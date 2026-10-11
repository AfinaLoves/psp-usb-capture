#include "session.h"

#include <pspkernel.h>
#include <string.h>

#include "audio.h"
#include "display.h"
#include "log.h"
#include "pspcap_protocol.h"
#include "systemctrl.h"
#include "usb.h"
#include "version.h"

#define CMD_TIMEOUT_US  500000
#define SEND_TIMEOUT_US 1000000

// The same 2 MiB of VRAM through its four aliases (user/kernel, cached/uncached).
static const void *const g_vram_alias[] = {
    [PSPCAP_BENCH_VRAM_USER_UC] = (const void *)0x44000000,
    [PSPCAP_BENCH_VRAM_USER] = (const void *)0x04000000,
    [PSPCAP_BENCH_VRAM_KERNEL] = (const void *)0x84000000,
    [PSPCAP_BENCH_VRAM_KERNEL_UC] = (const void *)0xA4000000,
};

typedef union {
    pspcap_header header;
    u8 raw[PSPCAP_MAX_PACKET];
} packet_buf;

static packet_buf g_rx __attribute__((aligned(64)));
static packet_buf g_tx __attribute__((aligned(64)));
// Two transfer buffers: one is filled while the other is on the wire.
// Bench runs reuse buffer 0 for their test pattern.
#define STREAM_BUF_SIZE PSPCAP_BENCH_RAM_MAX
static u32 g_stream_buf[2][STREAM_BUF_SIZE / 4] __attribute__((aligned(64)));

// Threads moving bulk data must outrank the game, see docs/notes.md.
#define STREAM_PRIORITY 0x10

#define FRAME_WIDTH  480
#define FRAME_HEIGHT 272

static u32 g_tx_seq;
static u32 g_yield_us; // pspcap_stream_request.yield_us
static int g_usb_speed;

static u64 now_us(void)
{
    return (u64)sceKernelGetSystemTimeWide();
}

static int send_packet(int type, const void *payload, u32 len)
{
    pspcap_header *h = &g_tx.header;
    h->magic = PSPCAP_MAGIC;
    h->version = PSPCAP_PROTO_VERSION;
    h->type = (u8)type;
    h->flags = 0;
    h->seq = g_tx_seq++;
    h->length = len;
    h->timestamp_us = now_us();
    if (len)
        memcpy(g_tx.raw + sizeof(*h), payload, len);

    int ret = usb_send(&g_tx, sizeof(*h) + len, SEND_TIMEOUT_US);
    return ret < 0 ? ret : 0;
}

static void send_error(u32 code, u32 detail)
{
    pspcap_error err = { code, detail };
    send_packet(PSPCAP_PKT_ERROR, &err, sizeof(err));
}

static void handle_hello(void)
{
    pspcap_hello hello;
    memset(&hello, 0, sizeof(hello));
    hello.plugin_version = PSPCAP_VERSION_NUM;
    hello.hen_version = (u32)sctrlHENGetVersion();
    hello.hen_minor = (u32)sctrlHENGetMinorVersion();
    hello.usb_speed = (u32)g_usb_speed;
    strncpy(hello.build, PSPCAP_GIT_REV, sizeof(hello.build) - 1);

    send_packet(PSPCAP_PKT_HELLO, &hello, sizeof(hello));
    log_printf("HELLO from PC");
}

static void handle_ping(const void *payload)
{
    send_packet(PSPCAP_PKT_PONG, payload, sizeof(pspcap_ping));
}

static const char *bench_source_name(u32 source)
{
    switch (source) {
    case PSPCAP_BENCH_RAM: return "ram";
    case PSPCAP_BENCH_VRAM_USER_UC: return "vram@44";
    case PSPCAP_BENCH_VRAM_USER: return "vram@04";
    case PSPCAP_BENCH_VRAM_KERNEL: return "vram@84";
    case PSPCAP_BENCH_VRAM_KERNEL_UC: return "vram@A4";
    default: return "?";
    }
}

static int bench_params_ok(const pspcap_bench_request *req)
{
    u32 max;
    if (req->source == PSPCAP_BENCH_RAM)
        max = PSPCAP_BENCH_RAM_MAX;
    else if (req->source <= PSPCAP_BENCH_VRAM_KERNEL_UC)
        max = PSPCAP_BENCH_VRAM_MAX;
    else
        return 0;

    return req->chunk_bytes > 0 && req->chunk_bytes <= max && req->chunk_bytes % 512 == 0 &&
           req->total_bytes > 0 && req->total_bytes % req->chunk_bytes == 0 &&
           req->depth >= 1 && req->depth <= PSPCAP_BENCH_MAX_DEPTH &&
           req->depth <= USB_SEND_SLOTS && req->priority < 0x80;
}

static void handle_bench(const pspcap_bench_request *req_in)
{
    pspcap_bench_request req = *req_in;
    if (!bench_params_ok(&req)) {
        send_error(PSPCAP_ERR_BAD_PARAM, PSPCAP_CMD_BENCH);
        return;
    }
    if (send_packet(PSPCAP_PKT_BENCH_BEGIN, &req, sizeof(req)) < 0)
        return;

    if (req.source == PSPCAP_BENCH_RAM) {
        for (u32 i = 0; i < STREAM_BUF_SIZE / 4; i++)
            g_stream_buf[0][i] = i;
    }
    const void *src = req.source == PSPCAP_BENCH_RAM ? (const void *)g_stream_buf[0]
                                                     : g_vram_alias[req.source];
    u32 chunks = req.total_bytes / req.chunk_bytes;
    u32 queued = 0, done = 0, status = 0, detail = 0;
    const char *phase = "";

    int old_priority = sceKernelGetThreadCurrentPriority();
    if (req.priority)
        sceKernelChangeThreadPriority(0, req.priority);

    u64 t0 = now_us();

    // Requests on one endpoint complete in order, so slot = index % depth.
    while (queued < chunks && queued < req.depth) {
        if (usb_send_start(queued % req.depth, src, req.chunk_bytes) < 0) {
            status = PSPCAP_ERR_USB;
            detail = (u32)usb_last_error();
            phase = "start";
            break;
        }
        queued++;
    }
    while (!status && done < queued) {
        int slot = done % req.depth;
        int ret = usb_send_wait(slot, SEND_TIMEOUT_US);
        if (ret != (int)req.chunk_bytes) {
            status = PSPCAP_ERR_USB;
            detail = ret == USB_ERR_REQUEST ? (u32)usb_last_error() : (u32)ret;
            phase = "wait";
            break;
        }
        done++;
        if (queued < chunks) {
            if (usb_send_start(slot, src, req.chunk_bytes) < 0) {
                status = PSPCAP_ERR_USB;
                detail = (u32)usb_last_error();
                phase = "restart";
                break;
            }
            queued++;
        }
    }

    u32 elapsed = (u32)(now_us() - t0);
    if (req.priority)
        sceKernelChangeThreadPriority(0, old_priority);

    pspcap_bench_result res;
    memset(&res, 0, sizeof(res));
    res.status = status;
    res.bytes_sent = done * req.chunk_bytes;
    res.elapsed_us = elapsed;
    res.detail = detail;
    send_packet(PSPCAP_PKT_BENCH_END, &res, sizeof(res));

    log_printf("bench %s chunk=%u depth=%u prio=0x%X: %u bytes in %u us, status %u %s 0x%08X",
               bench_source_name(req.source), (unsigned int)req.chunk_bytes,
               (unsigned int)req.depth, (unsigned int)req.priority,
               (unsigned int)res.bytes_sent, (unsigned int)elapsed, (unsigned int)status, phase,
               (unsigned int)detail);
}

// ---- snapshot -------------------------------------------------------------------

// Uncached alias of any address: always sees what the GE wrote, but every
// read is a separate bus access, which makes copying a frame slow.
static const u8 *uncached(const void *p)
{
    return (const u8 *)(((u32)p & 0x1FFFFFFF) | 0x40000000);
}

// Cached kernel alias. Reading a frame through it is much faster, but the GE
// writes VRAM behind the cache, so stale lines must be dropped first.
static const u8 *cached(const void *p)
{
    return (const u8 *)(((u32)p & 0x1FFFFFFF) | 0x80000000);
}

// Byte stream over the two transfer buffers: every full buffer is queued on
// USB, and the other one is reused once its previous transfer is done.
typedef struct {
    int idx;
    u32 fill;
    u32 in_flight[2]; // bytes queued per buffer, 0 = free
    u32 detail;       // cause of the first error
    u32 copy_us;      // CPU time spent in memcpy
} stream_state;

static int stream_wait(stream_state *st, int idx)
{
    if (!st->in_flight[idx])
        return 0;
    int ret = usb_send_wait(idx, SEND_TIMEOUT_US);
    if (ret != (int)st->in_flight[idx]) {
        st->detail = ret == USB_ERR_REQUEST ? (u32)usb_last_error() : (u32)ret;
        return -1;
    }
    st->in_flight[idx] = 0;
    return 0;
}

static int stream_flush(stream_state *st)
{
    if (!st->fill)
        return 0;
    if (usb_send_start(st->idx, g_stream_buf[st->idx], st->fill) < 0) {
        st->detail = (u32)usb_last_error();
        return -1;
    }
    st->in_flight[st->idx] = st->fill;
    st->idx ^= 1;
    st->fill = 0;
    return stream_wait(st, st->idx);
}

static int stream_write(stream_state *st, const u8 *src, u32 len)
{
    while (len) {
        u32 n = STREAM_BUF_SIZE - st->fill;
        if (n > len)
            n = len;
        u64 t0 = now_us();
        memcpy((u8 *)g_stream_buf[st->idx] + st->fill, src, n);
        st->copy_us += (u32)(now_us() - t0);
        st->fill += n;
        src += n;
        len -= n;
        if (st->fill == STREAM_BUF_SIZE && stream_flush(st) < 0)
            return -1;
    }
    return 0;
}

static int stream_finish(stream_state *st)
{
    if (stream_flush(st) < 0)
        return -1;
    if (stream_wait(st, 0) < 0 || stream_wait(st, 1) < 0)
        return -1;
    return 0;
}

static int frame_ok(const display_frame *fb)
{
    return fb->addr && fb->pixel_format >= PSPCAP_PIXEL_565 &&
           fb->pixel_format <= PSPCAP_PIXEL_8888 && fb->stride >= FRAME_WIDTH;
}

static u16 g_row565[FRAME_WIDTH] __attribute__((aligned(64)));

static void row_8888_to_565(const u32 *src, u16 *dst, int n)
{
    for (int i = 0; i < n; i++) {
        u32 p = src[i]; // bytes R, G, B, A
        dst[i] = (u16)(((p >> 3) & 0x001F) | ((p >> 5) & 0x07E0) | ((p >> 8) & 0xF800));
    }
}

// sceDmacMemcpy lives in the LowIO module; resolved on first use.
typedef int (*dmac_memcpy_fn)(void *dst, const void *src, SceSize n);
static dmac_memcpy_fn g_dmac_memcpy;
static int g_dmac_looked_up;

static dmac_memcpy_fn dmac_memcpy(void)
{
    if (!g_dmac_looked_up) {
        g_dmac_looked_up = 1;
        g_dmac_memcpy = (dmac_memcpy_fn)sctrlHENFindFunction("sceLowIO_Driver", "sceDmac",
                                                             0x617F3FE6);
        log_printf("sceDmacMemcpy at 0x%08X", (unsigned int)g_dmac_memcpy);
    }
    return g_dmac_memcpy;
}

// ---- grab mode: whole frame at once, then send at low priority -------------

#define GRAB_BUF_SIZE (512 * FRAME_HEIGHT * 4) // stride 512, 8888
#define GRAB_PRIORITY STREAM_PRIORITY            // while taking the frame
#define GRAB_SEND_PRIORITY 0x20                  // default while sending it

typedef SceUID (*alloc_partition_fn)(SceUID partition, const char *name, int type, SceSize size,
                                     void *addr);
typedef void *(*block_head_fn)(SceUID block);

static u8 *g_grab_buf;
static int g_grab_tried;

// Full-frame buffer from the kernel partition, allocated on first use.
// NULL if there is no room: grab mode then falls back to the other paths.
static u8 *grab_buffer(void)
{
    if (g_grab_tried)
        return g_grab_buf;
    g_grab_tried = 1;

    alloc_partition_fn alloc = (alloc_partition_fn)sctrlHENFindFunction(
        "sceSystemMemoryManager", "SysMemUserForUser", 0x237DBD4F);
    block_head_fn head = (block_head_fn)sctrlHENFindFunction(
        "sceSystemMemoryManager", "SysMemUserForUser", 0x9D9A5BA1);
    if (!alloc || !head) {
        log_printf("grab buffer: sysmem functions not found");
        return 0;
    }
    SceUID block = alloc(1 /* kernel */, "pspcap_grab", 1 /* PSP_SMEM_High */,
                         GRAB_BUF_SIZE + 64, 0);
    if (block < 0) {
        log_printf("grab buffer: alloc of %u bytes failed: 0x%08X",
                   (unsigned int)GRAB_BUF_SIZE, (unsigned int)block);
        return 0;
    }
    g_grab_buf = (u8 *)(((u32)head(block) + 63) & ~63u);
    log_printf("grab buffer: %u bytes at 0x%08X", (unsigned int)GRAB_BUF_SIZE,
               (unsigned int)g_grab_buf);
    return g_grab_buf;
}

// Queues `total` bytes straight from `buf` in 64 KiB slices, two in flight.
static int send_slices(stream_state *st, const u8 *buf, u32 total)
{
    for (u32 off = 0; off < total;) {
        u32 n = total - off < STREAM_BUF_SIZE ? total - off : STREAM_BUF_SIZE;
        int idx = st->idx;
        if (usb_send_start(idx, buf + off, n) < 0) {
            st->detail = (u32)usb_last_error();
            return -1;
        }
        st->in_flight[idx] = n;
        st->idx ^= 1;
        if (stream_wait(st, st->idx) < 0)
            return -1;
        off += n;
    }
    return 0;
}

// Grab mode: called at GRAB_PRIORITY right after the flip. Takes the whole
// frame in one go, drops to `send_priority` and sends it from the buffer.
static u32 send_frame_grabbed(const display_frame *fb, u32 frame_id, u32 flags, int send_priority,
                              pspcap_frame_end *end)
{
    u32 bpp = fb->pixel_format == PSPCAP_PIXEL_8888 ? 4 : 2;
    u32 src_row = (u32)fb->stride * bpp;
    u32 grab_bytes = src_row * FRAME_HEIGHT;
    int to565 = (flags & PSPCAP_STREAM_RGB565) && fb->pixel_format == PSPCAP_PIXEL_8888;

    memset(end, 0, sizeof(*end));
    u64 t0 = now_us();

    // Take the frame: everything until here runs ahead of the game.
    sceKernelDcacheWritebackInvalidateAll();
    int ret = -1;
    if (dmac_memcpy())
        ret = g_dmac_memcpy(g_grab_buf, fb->addr, grab_bytes);
    if (ret < 0) {
        // No DMA (or it refused the size): copy through the cache instead.
        memcpy(g_grab_buf, cached(fb->addr), grab_bytes);
        sceKernelDcacheWritebackInvalidateAll();
    }
    end->copy_us = (u32)(now_us() - t0); // time the game may have waited

    sceKernelChangeThreadPriority(0, send_priority);

    pspcap_frame_info info;
    memset(&info, 0, sizeof(info));
    info.width = FRAME_WIDTH;
    info.height = FRAME_HEIGHT;
    info.pixel_format = to565 ? PSPCAP_PIXEL_565 : (u32)fb->pixel_format;
    info.row_pixels = to565 ? FRAME_WIDTH : (u32)fb->stride;
    info.bytes = info.row_pixels * (to565 ? 2 : bpp) * FRAME_HEIGHT;
    info.source_addr = (u32)fb->addr;
    info.source_stride = (u32)fb->stride;
    info.frame_id = frame_id;
    info.capture_us = t0;

    stream_state st;
    memset(&st, 0, sizeof(st));
    if (send_packet(PSPCAP_PKT_FRAME_BEGIN, &info, sizeof(info)) < 0) {
        end->status = PSPCAP_ERR_USB;
    } else if (!to565) {
        if (send_slices(&st, g_grab_buf, grab_bytes) < 0)
            end->status = PSPCAP_ERR_USB;
    } else {
        // Convert at the low priority, from the grabbed copy (no tearing).
        const u8 *src = cached(g_grab_buf);
        for (int y = 0; y < FRAME_HEIGHT && !end->status; y++) {
            row_8888_to_565((const u32 *)(src + y * src_row), g_row565, FRAME_WIDTH);
            if (stream_write(&st, (const u8 *)g_row565, FRAME_WIDTH * 2) < 0)
                end->status = PSPCAP_ERR_USB;
        }
    }
    if (!end->status && stream_finish(&st) < 0)
        end->status = PSPCAP_ERR_USB;
    end->detail = st.detail;
    end->elapsed_us = (u32)(now_us() - t0);
    if (send_packet(PSPCAP_PKT_FRAME_END, end, sizeof(*end)) < 0 && !end->status)
        end->status = PSPCAP_ERR_USB;

    sceKernelChangeThreadPriority(0, GRAB_PRIORITY);
    return end->status;
}

// Sends FRAME_BEGIN, the pixels and FRAME_END. Returns the FRAME_END status
// (0 = ok); `end` gets the details. `flags` are PSPCAP_STREAM_*.
static u32 send_frame(const display_frame *fb, u32 frame_id, u32 flags, pspcap_frame_end *end)
{
    int dma = (flags & PSPCAP_STREAM_DMA) && dmac_memcpy();
    int to565 = !dma && (flags & PSPCAP_STREAM_RGB565) && fb->pixel_format == PSPCAP_PIXEL_8888;
    u32 bpp = fb->pixel_format == PSPCAP_PIXEL_8888 ? 4 : 2;
    // DMA copies whole framebuffer rows in big blocks; the PC crops them.
    u32 row_pixels = dma ? (u32)fb->stride : FRAME_WIDTH;
    u32 row_bytes = row_pixels * (to565 ? 2 : bpp);

    pspcap_frame_info info;
    memset(&info, 0, sizeof(info));
    info.width = FRAME_WIDTH;
    info.height = FRAME_HEIGHT;
    info.pixel_format = to565 ? PSPCAP_PIXEL_565 : (u32)fb->pixel_format;
    info.bytes = row_bytes * FRAME_HEIGHT;
    info.source_addr = (u32)fb->addr;
    info.source_stride = (u32)fb->stride;
    info.frame_id = frame_id;
    info.row_pixels = row_pixels;
    info.capture_us = now_us();

    memset(end, 0, sizeof(*end));
    if (send_packet(PSPCAP_PKT_FRAME_BEGIN, &info, sizeof(info)) < 0) {
        end->status = PSPCAP_ERR_USB;
        return end->status;
    }

    u32 src_row = (u32)fb->stride * bpp;
    stream_state st;
    memset(&st, 0, sizeof(st));

    u64 t0 = now_us();
    if (!(flags & PSPCAP_STREAM_UNCACHED)) {
        // The cache is only 16 KiB: writing back and dropping all of it is far
        // cheaper than walking the frame's range. Afterwards no stale line of
        // the frame can shadow what the GE drew, and no dirty line of our
        // buffers can later overwrite what the DMA put there.
        sceKernelDcacheWritebackInvalidateAll();
    }
    u32 extra_us = (u32)(now_us() - t0); // cache maintenance and conversion

    if (dma) {
        const u8 *src = (const u8 *)fb->addr;
        for (u32 off = 0; off < info.bytes;) {
            u32 n = info.bytes - off < STREAM_BUF_SIZE ? info.bytes - off : STREAM_BUF_SIZE;
            u64 tc = now_us();
            int ret = g_dmac_memcpy(g_stream_buf[st.idx], src + off, n);
            st.copy_us += (u32)(now_us() - tc);
            if (ret < 0) {
                end->status = PSPCAP_ERR_DMA;
                st.detail = (u32)ret;
                break;
            }
            st.fill = n;
            if (stream_flush(&st) < 0) {
                end->status = PSPCAP_ERR_USB;
                break;
            }
            off += n;
        }
    } else {
        const u8 *src = flags & PSPCAP_STREAM_UNCACHED ? uncached(fb->addr) : cached(fb->addr);
        for (int y = 0; y < FRAME_HEIGHT; y++) {
            const u8 *row = src + y * src_row;
            if (to565) {
                u64 tc = now_us();
                row_8888_to_565((const u32 *)row, g_row565, FRAME_WIDTH);
                extra_us += (u32)(now_us() - tc);
                row = (const u8 *)g_row565;
                // Converting keeps the CPU busy for milliseconds at a priority
                // above the system's sound thread, which then feeds the
                // hardware late: the game's sound plays ~2.6 % slow (measured).
                if (g_yield_us && (y + 1) % PSPCAP_YIELD_ROWS == 0)
                    sceKernelDelayThread(g_yield_us);
            }
            if (stream_write(&st, row, row_bytes) < 0) {
                end->status = PSPCAP_ERR_USB;
                break;
            }
        }
    }
    if (!end->status && stream_finish(&st) < 0)
        end->status = PSPCAP_ERR_USB;
    end->detail = st.detail;
    end->elapsed_us = (u32)(now_us() - t0);
    end->copy_us = st.copy_us + extra_us;

    if (send_packet(PSPCAP_PKT_FRAME_END, end, sizeof(*end)) < 0 && !end->status)
        end->status = PSPCAP_ERR_USB;
    return end->status;
}

static void handle_snapshot(void)
{
    int old_priority = sceKernelGetThreadCurrentPriority();
    sceKernelChangeThreadPriority(0, STREAM_PRIORITY);

    // Right after vblank the game has just flipped, so the shown buffer stays
    // untouched for at least a frame while we copy it.
    display_wait_vblank();

    display_frame fb;
    memset(&fb, 0, sizeof(fb));
    int ret = display_get_frame(&fb);
    if (ret < 0 || !frame_ok(&fb)) {
        sceKernelChangeThreadPriority(0, old_priority);
        send_error(PSPCAP_ERR_NO_FRAME, ret < 0 ? (u32)ret : (u32)fb.pixel_format);
        log_printf("snapshot: no frame (ret 0x%08X, addr 0x%08X, fmt %d, stride %d)",
                   (unsigned int)ret, (unsigned int)fb.addr, fb.pixel_format, fb.stride);
        return;
    }

    pspcap_frame_end end;
    send_frame(&fb, display_flip_count(), 0, &end);
    sceKernelChangeThreadPriority(0, old_priority);

    log_printf("snapshot fmt=%d stride=%d addr=0x%08X: %u us (copy %u us), status %u 0x%08X",
               fb.pixel_format, fb.stride, (unsigned int)fb.addr, (unsigned int)end.elapsed_us,
               (unsigned int)end.copy_us, (unsigned int)end.status, (unsigned int)end.detail);
}

// ---- streaming ------------------------------------------------------------------

static int g_streaming;
static int g_saved_priority;
static u32 g_stream_flags;
static u32 g_audio_sent;
static u32 g_audio_lost; // popped from the ring, but the send failed
static u8 g_pcm[16 * 1024] __attribute__((aligned(64)));
static int g_send_priority; // grab mode: priority while sending
static u32 g_last_sent_id;
static u32 g_frames_sent;

static void send_stats(void)
{
    pspcap_stats st;
    memset(&st, 0, sizeof(st));
    st.flips = display_flip_count();
    st.frames_sent = g_frames_sent;
    st.streaming = (u32)g_streaming;
    st.hooked = (display_hook_installed() ? PSPCAP_STATS_HOOKED : 0) |
                (g_grab_buf ? PSPCAP_STATS_GRAB : 0) |
                (audio_hooks_installed() ? PSPCAP_STATS_AUDIO : 0);
    st.audio_sent = g_audio_sent;
    st.audio_dropped = audio_dropped();
    st.psp_time_us = now_us();
    if (send_packet(PSPCAP_PKT_STATS, &st, sizeof(st)) < 0 || !g_streaming ||
        !(g_stream_flags & PSPCAP_STREAM_AUDIO))
        return;
    pspcap_audio_diag diag;
    memset(&diag, 0, sizeof(diag));
    audio_diag(&diag);
    diag.lost = g_audio_lost;
    diag.psp_time_us = now_us();
    send_packet(PSPCAP_PKT_AUDIO_DIAG, &diag, sizeof(diag));
}

// Once per plugin run: every thread with its priority, to see what the
// stream thread competes with (the log is slow, so not on every start).
static void log_threads(void)
{
    static int done;
    if (done)
        return;
    done = 1;
    SceUID ids[64];
    int count = 0;
    if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 64, &count) < 0)
        return;
    log_printf("%d threads (name prio/init attr status):", count);
    for (int i = 0; i < count && i < 64; i++) {
        SceKernelThreadInfo ti;
        memset(&ti, 0, sizeof(ti));
        ti.size = sizeof(ti);
        if (sceKernelReferThreadStatus(ids[i], &ti) < 0)
            continue;
        log_printf("  %s 0x%X/0x%X 0x%08X %d", ti.name, (unsigned int)ti.currentPriority,
                   (unsigned int)ti.initPriority, (unsigned int)ti.attr, (int)ti.status);
    }
}

static void stream_start(const pspcap_stream_request *req)
{
    if (req->flags & (PSPCAP_STREAM_DMA | PSPCAP_STREAM_GRAB))
        dmac_memcpy();
    int grab = (req->flags & PSPCAP_STREAM_GRAB) && grab_buffer();
    // Grab mode waits for flips at the grab priority and sends at `priority`.
    u32 priority = grab ? GRAB_PRIORITY : req->priority ? req->priority : STREAM_PRIORITY;
    g_send_priority = req->priority ? (int)req->priority : GRAB_SEND_PRIORITY;
    if (!g_streaming)
        g_saved_priority = sceKernelGetThreadCurrentPriority();
    // A repeated START just updates the options.
    sceKernelChangeThreadPriority(0, (int)priority);
    g_stream_flags = req->flags;
    g_yield_us = req->yield_us;
    if (!g_streaming)
        g_last_sent_id = display_flip_count(); // only flips from now on
    g_streaming = 1;
    display_set_notify(1);
    audio_set_capture((req->flags & PSPCAP_STREAM_AUDIO) != 0);
    log_printf("stream started, flags 0x%X, priority 0x%X, grab %s (send at 0x%X), yield %u us",
               (unsigned int)req->flags, (unsigned int)priority, grab ? "on" : "off",
               (unsigned int)g_send_priority, (unsigned int)req->yield_us);
    log_threads();
}

static void stream_stop(const char *why)
{
    if (!g_streaming)
        return;
    display_set_notify(0);
    audio_set_capture(0);
    g_streaming = 0;
    sceKernelChangeThreadPriority(0, g_saved_priority);
    log_printf("stream stopped (%s), %u frames sent so far", why, (unsigned int)g_frames_sent);
}

// Sends every captured sound block (small: they go out between frames).
static void stream_audio(void)
{
    pspcap_audio_info info;
    int n;
    while ((n = audio_pop(&info, g_pcm, sizeof(g_pcm))) > 0) {
        if (send_packet(PSPCAP_PKT_AUDIO, &info, sizeof(info)) < 0 ||
            usb_send(g_pcm, n, SEND_TIMEOUT_US) != n) {
            g_audio_lost++;
            return;
        }
        g_audio_sent++;
    }
}

// Sends the newest flipped frame, if it is newer than the last one sent.
// Flips that happen meanwhile collapse into one wake-up: those are skipped.
static void stream_next(void)
{
    display_frame fb;
    u32 id = 0;
    if (display_latest(&fb, &id) < 0 || id == g_last_sent_id || !frame_ok(&fb))
        return;

    pspcap_frame_end end;
    g_last_sent_id = id;
    u32 status;
    if ((g_stream_flags & PSPCAP_STREAM_GRAB) && g_grab_buf &&
        (u32)fb.stride * (fb.pixel_format == PSPCAP_PIXEL_8888 ? 4 : 2) * FRAME_HEIGHT <=
            GRAB_BUF_SIZE)
        status = send_frame_grabbed(&fb, id, g_stream_flags, g_send_priority, &end);
    else
        status = send_frame(&fb, id, g_stream_flags, &end);
    if (status == 0) {
        g_frames_sent++;
        return;
    }
    // The PC stopped reading or the cable is gone: do not keep pushing.
    stream_stop(usb_attached() ? "send failed" : "detached");
}

static void handle_packet(int size)
{
    const pspcap_header *h = &g_rx.header;
    const void *payload = g_rx.raw + sizeof(*h);

    if (size < (int)sizeof(*h) || h->magic != PSPCAP_MAGIC ||
        h->length != (u32)size - sizeof(*h)) {
        send_error(PSPCAP_ERR_BAD_PACKET, (u32)size);
        return;
    }
    // HELLO always works so the PC can report a version mismatch.
    if (h->version != PSPCAP_PROTO_VERSION && h->type != PSPCAP_CMD_HELLO) {
        send_error(PSPCAP_ERR_BAD_VERSION, h->version);
        return;
    }

    switch (h->type) {
    case PSPCAP_CMD_HELLO:
        handle_hello();
        break;
    case PSPCAP_CMD_PING:
        if (h->length != sizeof(pspcap_ping))
            send_error(PSPCAP_ERR_BAD_PACKET, h->type);
        else
            handle_ping(payload);
        break;
    case PSPCAP_CMD_BENCH:
        if (h->length != sizeof(pspcap_bench_request))
            send_error(PSPCAP_ERR_BAD_PACKET, h->type);
        else
            handle_bench(payload);
        break;
    case PSPCAP_CMD_SNAPSHOT:
        if (h->length != 0)
            send_error(PSPCAP_ERR_BAD_PACKET, h->type);
        else
            handle_snapshot();
        break;
    case PSPCAP_CMD_STREAM_START: {
        const pspcap_stream_request *req = payload;
        if (h->length != sizeof(*req)) {
            send_error(PSPCAP_ERR_BAD_PACKET, h->type);
            break;
        }
        if ((req->priority && (req->priority < 0x08 || req->priority > 0x7F)) ||
            req->yield_us > PSPCAP_YIELD_MAX_US) {
            send_error(PSPCAP_ERR_BAD_PARAM, h->type);
            break;
        }
        if (!display_hook_installed()) {
            send_error(PSPCAP_ERR_NO_FRAME, h->type);
            break;
        }
        stream_start(req);
        send_stats();
        break;
    }
    case PSPCAP_CMD_STREAM_STOP:
        stream_stop("pc");
        send_stats();
        break;
    case PSPCAP_CMD_STATS:
        send_stats();
        break;
    default:
        send_error(PSPCAP_ERR_UNKNOWN_CMD, h->type);
        break;
    }
}

void session_run(int usb_speed)
{
    g_usb_speed = usb_speed;

    for (;;) {
        int n = usb_recv_or_wake(&g_rx, sizeof(g_rx), CMD_TIMEOUT_US);
        if (n == USB_WOKEN) {
            if (g_streaming) {
                stream_audio();
                stream_next();
                stream_audio(); // what piled up while the frame was on the wire
            }
            continue;
        }
        if (n == USB_ERR_TIMEOUT)
            continue;
        if (n == USB_ERR_DETACHED || !usb_attached()) {
            stream_stop("detached");
            return;
        }
        if (n < 0) {
            // Still attached (e.g. the PC app went away mid-transfer): keep serving.
            sceKernelDelayThread(10000);
            continue;
        }
        handle_packet(n);
    }
}
