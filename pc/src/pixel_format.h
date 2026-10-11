// PSP framebuffer pixel formats -> RGBA8888 (see enum pspcap_pixel_format).
#pragma once

#include <cstddef>
#include <cstdint>

// Bytes per pixel for a PSP format, or 0 if the format is unknown.
unsigned psp_bytes_per_pixel(uint32_t format);

const char *psp_format_name(uint32_t format);

// Converts `pixels` pixels from `src` to RGBA bytes in `dst` (4 * pixels).
// Alpha is always 255: framebuffer alpha is not shown on screen.
// Returns false for an unknown format.
bool psp_to_rgba(uint32_t format, const uint8_t *src, size_t pixels, uint8_t *dst);

// Converts a frame of `height` rows, `row_pixels` pixels each (>= width), to a
// tightly packed width x height RGBA image, dropping the padding at the end
// of every row. Returns false for an unknown format.
bool psp_frame_to_rgba(uint32_t format, const uint8_t *src, unsigned width, unsigned height,
                       unsigned row_pixels, uint8_t *dst);
