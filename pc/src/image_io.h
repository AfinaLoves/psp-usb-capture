#pragma once

#include <cstdint>
#include <string>

// Writes an RGBA8888 image as PNG. Returns false on failure.
bool write_png(const std::string &path, int width, int height, const uint8_t *rgba);
