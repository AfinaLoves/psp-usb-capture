#include "pixel_format.h"

#include "pspcap_protocol.h"

namespace {

// Scale an n-bit channel to 8 bits so that the maximum maps to 255.
inline uint8_t expand4(unsigned v) { return static_cast<uint8_t>(v * 17); }
inline uint8_t expand5(unsigned v) { return static_cast<uint8_t>((v << 3) | (v >> 2)); }
inline uint8_t expand6(unsigned v) { return static_cast<uint8_t>((v << 2) | (v >> 4)); }

inline unsigned load16(const uint8_t *p) { return p[0] | (p[1] << 8); }

} // namespace

unsigned psp_bytes_per_pixel(uint32_t format)
{
    switch (format) {
    case PSPCAP_PIXEL_565:
    case PSPCAP_PIXEL_5551:
    case PSPCAP_PIXEL_4444:
        return 2;
    case PSPCAP_PIXEL_8888:
        return 4;
    default:
        return 0;
    }
}

const char *psp_format_name(uint32_t format)
{
    switch (format) {
    case PSPCAP_PIXEL_565: return "565";
    case PSPCAP_PIXEL_5551: return "5551";
    case PSPCAP_PIXEL_4444: return "4444";
    case PSPCAP_PIXEL_8888: return "8888";
    default: return "unknown";
    }
}

bool psp_to_rgba(uint32_t format, const uint8_t *src, size_t pixels, uint8_t *dst)
{
    switch (format) {
    case PSPCAP_PIXEL_565:
        for (size_t i = 0; i < pixels; i++, src += 2, dst += 4) {
            unsigned v = load16(src);
            dst[0] = expand5(v & 0x1F);
            dst[1] = expand6((v >> 5) & 0x3F);
            dst[2] = expand5((v >> 11) & 0x1F);
            dst[3] = 255;
        }
        return true;
    case PSPCAP_PIXEL_5551:
        for (size_t i = 0; i < pixels; i++, src += 2, dst += 4) {
            unsigned v = load16(src);
            dst[0] = expand5(v & 0x1F);
            dst[1] = expand5((v >> 5) & 0x1F);
            dst[2] = expand5((v >> 10) & 0x1F);
            dst[3] = 255;
        }
        return true;
    case PSPCAP_PIXEL_4444:
        for (size_t i = 0; i < pixels; i++, src += 2, dst += 4) {
            unsigned v = load16(src);
            dst[0] = expand4(v & 0xF);
            dst[1] = expand4((v >> 4) & 0xF);
            dst[2] = expand4((v >> 8) & 0xF);
            dst[3] = 255;
        }
        return true;
    case PSPCAP_PIXEL_8888:
        for (size_t i = 0; i < pixels; i++, src += 4, dst += 4) {
            dst[0] = src[0];
            dst[1] = src[1];
            dst[2] = src[2];
            dst[3] = 255;
        }
        return true;
    default:
        return false;
    }
}

bool psp_frame_to_rgba(uint32_t format, const uint8_t *src, unsigned width, unsigned height,
                       unsigned row_pixels, uint8_t *dst)
{
    const unsigned bpp = psp_bytes_per_pixel(format);
    if (!bpp || row_pixels < width)
        return false;
    if (row_pixels == width)
        return psp_to_rgba(format, src, static_cast<size_t>(width) * height, dst);
    for (unsigned y = 0; y < height; y++) {
        psp_to_rgba(format, src + static_cast<size_t>(y) * row_pixels * bpp, width,
                    dst + static_cast<size_t>(y) * width * 4);
    }
    return true;
}
