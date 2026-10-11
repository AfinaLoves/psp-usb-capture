// Known-pixel checks for psp_to_rgba(), all four PSP formats.
// Run: ctest --test-dir pc/build -C Release

#include <cstdio>
#include <cstring>
#include <vector>

#include "pixel_format.h"
#include "pspcap_protocol.h"

namespace {

int g_failures = 0;

struct Rgb {
    uint8_t r, g, b;
};

// `raw` holds little-endian pixels as they come from the PSP.
void check(uint32_t format, std::vector<uint8_t> raw, const std::vector<Rgb> &want)
{
    std::vector<uint8_t> out(want.size() * 4);
    if (!psp_to_rgba(format, raw.data(), want.size(), out.data())) {
        std::printf("FAIL %s: conversion refused\n", psp_format_name(format));
        g_failures++;
        return;
    }
    for (size_t i = 0; i < want.size(); i++) {
        const uint8_t *p = &out[i * 4];
        if (p[0] != want[i].r || p[1] != want[i].g || p[2] != want[i].b || p[3] != 255) {
            std::printf("FAIL %s pixel %zu: got %u,%u,%u,%u want %u,%u,%u,255\n",
                        psp_format_name(format), i, p[0], p[1], p[2], p[3], want[i].r, want[i].g,
                        want[i].b);
            g_failures++;
        }
    }
}

std::vector<uint8_t> le16(std::initializer_list<unsigned> values)
{
    std::vector<uint8_t> out;
    for (unsigned v : values) {
        out.push_back(static_cast<uint8_t>(v & 0xFF));
        out.push_back(static_cast<uint8_t>(v >> 8));
    }
    return out;
}

} // namespace

int main()
{
    // Pure red, green, blue, white, black, and one mid value per format.
    check(PSPCAP_PIXEL_565, le16({0x001F, 0x07E0, 0xF800, 0xFFFF, 0x0000, 0x8410}),
          {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}, {0, 0, 0}, {132, 130, 132}});

    // Alpha bit set or not must not change the colour.
    check(PSPCAP_PIXEL_5551, le16({0x001F, 0x03E0, 0x7C00, 0xFFFF, 0x8000, 0x4210}),
          {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}, {0, 0, 0}, {132, 132, 132}});

    check(PSPCAP_PIXEL_4444, le16({0x000F, 0x00F0, 0x0F00, 0xFFFF, 0xF000, 0x0842}),
          {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}, {0, 0, 0}, {34, 68, 136}});

    check(PSPCAP_PIXEL_8888,
          {0xFF, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x80, 0x00, 0x00, 0xFF, 0xFF, 0x12, 0x34,
           0x56, 0x00},
          {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {0x12, 0x34, 0x56}});

    // Padded rows (DMA streams send the 512-pixel stride): 2x2 picture in
    // rows of 3 pixels; the third pixel of each row must be dropped.
    {
        std::vector<uint8_t> raw = le16({0x001F, 0x07E0, 0xFFFF, 0xF800, 0x0000, 0xFFFF});
        std::vector<uint8_t> out(2 * 2 * 4);
        const uint8_t want[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 0, 0, 0, 255};
        if (!psp_frame_to_rgba(PSPCAP_PIXEL_565, raw.data(), 2, 2, 3, out.data()) ||
            std::memcmp(out.data(), want, sizeof(want)) != 0) {
            std::printf("FAIL: padded rows not cropped correctly\n");
            g_failures++;
        }
    }

    uint8_t dummy[4];
    if (psp_to_rgba(7, dummy, 1, dummy)) {
        std::printf("FAIL: unknown format accepted\n");
        g_failures++;
    }
    if (psp_bytes_per_pixel(PSPCAP_PIXEL_565) != 2 || psp_bytes_per_pixel(PSPCAP_PIXEL_8888) != 4 ||
        psp_bytes_per_pixel(9) != 0) {
        std::printf("FAIL: bytes per pixel\n");
        g_failures++;
    }

    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("pixel_format_test: all formats ok\n");
    return 0;
}
