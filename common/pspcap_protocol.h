/*
 * pspcap wire protocol — shared by the PSP plugin (C) and the PC client (C++).
 *
 * Transport: one vendor-specific USB interface with two bulk endpoints.
 *   PSP -> PC  (bulk IN,  PSPCAP_EP_IN):  packets and raw stream data
 *   PC  -> PSP (bulk OUT, PSPCAP_EP_OUT): command packets
 *
 * Every packet is a pspcap_header followed by `length` payload bytes, sent as
 * a single bulk transfer. Packets are always shorter than one high-speed
 * max-packet (512 bytes), so each one ends with a short packet and a host read
 * of PSPCAP_MAX_PACKET bytes returns exactly one packet.
 *
 * Raw data (benchmark stream, later video) is sent without headers, in sizes
 * announced by the preceding packet. All fields are little-endian, which is
 * native on both the PSP (MIPS LE) and x86.
 *
 * Changing anything here: bump PSPCAP_PROTO_VERSION and update both sides.
 */
#ifndef PSPCAP_PROTOCOL_H
#define PSPCAP_PROTOCOL_H

#include <stdint.h>

#define PSPCAP_PROTO_VERSION 10

/* VID is filled in by the PSP USB bus driver; the PID is ours.
 * 01C8 = PSP mass storage, 01C9 = PSPLink/RemoteJoy, 02D2 = PSP Slim. */
#define PSPCAP_USB_VID       0x054C
#define PSPCAP_USB_PID       0x01CF
#define PSPCAP_USB_INTERFACE 0
#define PSPCAP_EP_IN         0x81
#define PSPCAP_EP_OUT        0x02

#define PSPCAP_MAGIC      0x50414350u /* "PCAP" in memory */
#define PSPCAP_MAX_PACKET 512         /* header + payload, either direction */

enum pspcap_type {
    /* PC -> PSP */
    PSPCAP_CMD_HELLO = 0x01, /* no payload           -> PKT_HELLO */
    PSPCAP_CMD_PING  = 0x02, /* pspcap_ping          -> PKT_PONG */
    PSPCAP_CMD_BENCH = 0x03, /* pspcap_bench_request -> PKT_BENCH_BEGIN, raw data, PKT_BENCH_END */
    PSPCAP_CMD_SNAPSHOT = 0x04, /* no payload        -> PKT_FRAME_BEGIN, raw pixels, PKT_FRAME_END */
    PSPCAP_CMD_STREAM_START = 0x05, /* pspcap_stream_request -> PKT_STATS, then frames as the game flips */
    PSPCAP_CMD_STREAM_STOP  = 0x06, /* no payload    -> PKT_STATS (frames already queued come first) */
    PSPCAP_CMD_STATS        = 0x07, /* no payload    -> PKT_STATS */

    /* PSP -> PC */
    PSPCAP_PKT_HELLO       = 0x81, /* pspcap_hello */
    PSPCAP_PKT_PONG        = 0x82, /* pspcap_ping (echoed); PSP time is in the header */
    PSPCAP_PKT_BENCH_BEGIN = 0x83, /* pspcap_bench_request (as accepted) */
    PSPCAP_PKT_BENCH_END   = 0x84, /* pspcap_bench_result */
    PSPCAP_PKT_FRAME_BEGIN = 0x85, /* pspcap_frame_info */
    PSPCAP_PKT_FRAME_END   = 0x86, /* pspcap_frame_end */
    PSPCAP_PKT_STATS       = 0x87, /* pspcap_stats */
    PSPCAP_PKT_AUDIO       = 0x88, /* pspcap_audio_info, then `bytes` of raw PCM */
    PSPCAP_PKT_AUDIO_DIAG  = 0x89, /* pspcap_audio_diag, right after PKT_STATS while streaming sound */
    PSPCAP_PKT_ERROR       = 0xFF  /* pspcap_error */
};

typedef struct pspcap_header {
    uint32_t magic;        /* PSPCAP_MAGIC */
    uint8_t  version;      /* PSPCAP_PROTO_VERSION of the sender */
    uint8_t  type;         /* enum pspcap_type */
    uint16_t flags;        /* reserved, 0 */
    uint32_t seq;          /* per-direction packet counter */
    uint32_t length;       /* payload bytes after the header */
    uint64_t timestamp_us; /* sender clock: PSP sceKernelGetSystemTimeWide, PC steady clock */
} pspcap_header;

typedef struct pspcap_hello {
    uint32_t plugin_version; /* 0x00MMmmpp */
    uint32_t hen_version;    /* sctrlHENGetVersion() */
    uint32_t hen_minor;      /* sctrlHENGetMinorVersion() */
    uint32_t usb_speed;      /* 1 = full speed, 2 = high speed */
    char     build[16];      /* git revision, NUL-terminated */
} pspcap_hello;

typedef struct pspcap_ping {
    uint64_t pc_time_us; /* PC clock when the ping was sent, echoed back */
} pspcap_ping;

/* Where the benchmark data comes from. VRAM content is whatever is on screen;
 * the four VRAM sources are the same memory seen through different aliases. */
enum pspcap_bench_source {
    PSPCAP_BENCH_RAM           = 0, /* plugin's static buffer, filled with a known pattern */
    PSPCAP_BENCH_VRAM_USER_UC  = 1, /* 0x44000000 */
    PSPCAP_BENCH_VRAM_USER     = 2, /* 0x04000000 */
    PSPCAP_BENCH_VRAM_KERNEL   = 3, /* 0x84000000 */
    PSPCAP_BENCH_VRAM_KERNEL_UC = 4 /* 0xA4000000 */
};

#define PSPCAP_BENCH_RAM_MAX  (64 * 1024)   /* size of the plugin's buffer */
#define PSPCAP_BENCH_VRAM_MAX (1024 * 1024) /* half of the 2 MiB VRAM */
#define PSPCAP_BENCH_MAX_DEPTH 4

typedef struct pspcap_bench_request {
    uint32_t source;      /* enum pspcap_bench_source */
    uint32_t chunk_bytes; /* per USB request, multiple of 512 */
    uint32_t total_bytes; /* multiple of chunk_bytes */
    uint32_t depth;       /* USB requests in flight on the PSP: 1..PSPCAP_BENCH_MAX_DEPTH */
    uint32_t priority;    /* PSP thread priority for the run (lower = more urgent), 0 = unchanged */
} pspcap_bench_request;

/* RAM source pattern: 32-bit word i of every chunk is i. */

typedef struct pspcap_bench_result {
    uint32_t status;     /* 0 = ok, otherwise enum pspcap_error_code */
    uint32_t bytes_sent;
    uint32_t elapsed_us; /* PSP clock, first request queued to last one done */
    uint32_t detail;     /* on failure: sce error code or request retcode */
} pspcap_bench_result;

/* Pixel formats, numbered like PSP_DISPLAY_PIXEL_FORMAT_*. Little-endian
 * pixels with red in the lowest bits:
 *   565:  R 0-4,  G 5-10, B 11-15
 *   5551: R 0-4,  G 5-9,  B 10-14, A 15
 *   4444: R 0-3,  G 4-7,  B 8-11,  A 12-15
 *   8888: bytes R, G, B, A
 * Alpha in a framebuffer means nothing for display and is ignored. */
enum pspcap_pixel_format {
    PSPCAP_PIXEL_565  = 0,
    PSPCAP_PIXEL_5551 = 1,
    PSPCAP_PIXEL_4444 = 2,
    PSPCAP_PIXEL_8888 = 3
};

/* A frame is sent as `height` rows of `row_pixels` pixels each, `bytes` in
 * total; only the first `width` pixels of a row are picture. Usually the PSP
 * crops rows to `width`; with PSPCAP_STREAM_DMA it sends whole framebuffer
 * rows (stride 512) and the PC crops. */
typedef struct pspcap_frame_info {
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;  /* enum pspcap_pixel_format */
    uint32_t bytes;         /* raw data that follows */
    uint32_t source_addr;   /* framebuffer address on the PSP (diagnostics) */
    uint32_t source_stride; /* framebuffer row length in pixels (diagnostics) */
    uint32_t frame_id;      /* game flip counter for this frame (gaps = skipped frames) */
    uint32_t row_pixels;    /* pixels per row in the data, >= width */
    uint64_t capture_us;    /* PSP clock when copying started */
} pspcap_frame_info;

/* Stream options. Defaults (flags 0, priority 0): VRAM read through the
 * cache, pixels sent as the game draws them, plugin's default priority. */
#define PSPCAP_STREAM_RGB565   0x1u /* convert 8888 frames to 565 on the PSP (halves USB) */
#define PSPCAP_STREAM_UNCACHED 0x2u /* read VRAM uncached, the protocol v4 way (comparison) */
#define PSPCAP_STREAM_DMA      0x4u /* copy VRAM with the DMA controller, whole rows; ignores RGB565 */
/* Grab the whole frame at once right after the flip (DMA into a full-frame
 * buffer, at high priority, so the game cannot draw over it meanwhile), then
 * drop to `priority` and send it from there. RGB565 is converted while
 * sending. Falls back to the other modes if the buffer cannot be allocated. */
#define PSPCAP_STREAM_GRAB     0x8u
#define PSPCAP_STREAM_AUDIO    0x10u /* also send the game's sound (PKT_AUDIO) */

/* A STREAM_START while streaming only changes the options. */
typedef struct pspcap_stream_request {
    uint32_t flags;    /* PSPCAP_STREAM_* */
    uint32_t priority; /* stream thread priority (with GRAB: while sending), 0 = default */
    uint32_t yield_us; /* RGB565: sleep this long after every PSPCAP_YIELD_ROWS converted
                          rows, so lower priority system threads (sound) get the CPU; 0 = never */
} pspcap_stream_request;

#define PSPCAP_YIELD_ROWS   16
#define PSPCAP_YIELD_MAX_US 5000

/* While streaming, the PSP sends every frame the game flips to, unless the
 * previous one is still on the wire: then that flip is skipped. If the PC
 * stops reading (a send times out), the PSP stops streaming by itself. */
typedef struct pspcap_stats {
    uint32_t flips;       /* sceDisplaySetFrameBuf calls since the hook went in */
    uint32_t frames_sent; /* frames streamed since the plugin started */
    uint32_t streaming;   /* 1 while streaming */
    uint32_t hooked;      /* PSPCAP_STATS_* bits */
    uint32_t audio_sent;    /* audio chunks sent since the plugin started */
    uint32_t audio_dropped; /* audio chunks lost (ring full, too big, unknown channel) */
    uint64_t psp_time_us;
} pspcap_stats;

#define PSPCAP_STATS_HOOKED 0x1u /* the flip hook is installed */
#define PSPCAP_STATS_GRAB   0x2u /* the full-frame grab buffer is allocated */
#define PSPCAP_STATS_AUDIO  0x4u /* the audio hooks are installed */

/* One block of sound as the game handed it to sceAudio: signed 16-bit
 * samples, interleaved L R when stereo. Sources are mixed on the PC. */
enum pspcap_audio_source {
    PSPCAP_AUDIO_SRC_CHANNEL0 = 0, /* .. 7: sceAudioOutput* channels */
    PSPCAP_AUDIO_SRC_SRC      = 8, /* sceAudioSRCOutputBlocking */
    PSPCAP_AUDIO_SRC_OUTPUT2  = 9  /* sceAudioOutput2OutputBlocking */
};

typedef struct pspcap_audio_info {
    uint32_t source;    /* enum pspcap_audio_source */
    uint32_t rate;      /* Hz */
    uint32_t channels;  /* 1 or 2 */
    uint32_t samples;   /* per channel */
    uint32_t bytes;     /* PCM that follows: samples * channels * 2 */
    uint16_t vol_left;  /* 0..0x8000 (= 1.0) */
    uint16_t vol_right;
    uint64_t psp_time_us;
} pspcap_audio_info;

/* Sound capture counters, to find where blocks go missing. Counters are
 * since the hooks went in and wrap; the PC works with differences. */
#define PSPCAP_AUDIO_FN_OUTPUT          0x01u /* sceAudioOutput */
#define PSPCAP_AUDIO_FN_OUTPUT_BLOCKING 0x02u
#define PSPCAP_AUDIO_FN_PANNED          0x04u /* sceAudioOutputPanned */
#define PSPCAP_AUDIO_FN_PANNED_BLOCKING 0x08u
#define PSPCAP_AUDIO_FN_SRC             0x10u /* sceAudioSRCOutputBlocking */
#define PSPCAP_AUDIO_FN_OUTPUT2         0x20u /* sceAudioOutput2OutputBlocking */

typedef struct pspcap_audio_source_diag {
    uint32_t calls;      /* output calls by the game, capture on or off */
    uint32_t failed;     /* calls where the original returned < 0 */
    uint32_t last_error; /* the latest such return value */
    uint32_t captured;   /* blocks put into the ring */
    uint32_t min_samples; /* shortest and longest block (per channel) since the previous */
    uint32_t max_samples; /* report, 0 = no call */
    int32_t  last_ret;   /* the latest non-negative return value */
    uint32_t funcs;      /* PSPCAP_AUDIO_FN_* used so far */
    uint32_t max_gap_us; /* longest time between two calls since the previous report */
} pspcap_audio_source_diag;

typedef struct pspcap_audio_diag {
    uint32_t lost;      /* blocks taken from the ring that could not be sent */
    uint32_t ring_used; /* bytes waiting in the ring */
    uint64_t psp_time_us;
    pspcap_audio_source_diag src[10]; /* enum pspcap_audio_source */
} pspcap_audio_diag;

typedef struct pspcap_frame_end {
    uint32_t status;     /* 0 = ok, otherwise enum pspcap_error_code */
    uint32_t detail;     /* on failure: sce error code or request retcode */
    uint32_t elapsed_us; /* PSP clock, vblank wait excluded: copy + send */
    uint32_t copy_us;    /* part of elapsed_us spent copying/converting pixels on the CPU */
} pspcap_frame_end;

enum pspcap_error_code {
    PSPCAP_ERR_BAD_PACKET  = 1, /* wrong magic or length */
    PSPCAP_ERR_BAD_VERSION = 2,
    PSPCAP_ERR_UNKNOWN_CMD = 3,
    PSPCAP_ERR_BAD_PARAM   = 4,
    PSPCAP_ERR_USB         = 5, /* transfer failed or timed out on the PSP */
    PSPCAP_ERR_NO_FRAME    = 6, /* no framebuffer to capture (detail: sce error) */
    PSPCAP_ERR_DMA         = 7  /* sceDmacMemcpy failed (detail: its error code) */
};

typedef struct pspcap_error {
    uint32_t code;   /* enum pspcap_error_code */
    uint32_t detail; /* e.g. the offending command type */
} pspcap_error;

#ifdef __cplusplus
#define PSPCAP_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define PSPCAP_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

PSPCAP_STATIC_ASSERT(sizeof(pspcap_header) == 24, "header layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_hello) == 32, "hello layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_ping) == 8, "ping layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_bench_request) == 20, "bench request layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_bench_result) == 16, "bench result layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_frame_info) == 40, "frame info layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_stats) == 32, "stats layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_audio_info) == 32, "audio info layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_audio_source_diag) == 36, "audio source diag layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_audio_diag) == 376, "audio diag layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_stream_request) == 12, "stream request layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_frame_end) == 16, "frame end layout");
PSPCAP_STATIC_ASSERT(sizeof(pspcap_error) == 8, "error layout");

#endif
