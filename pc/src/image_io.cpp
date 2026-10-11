#include "image_io.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

bool write_png(const std::string &path, int width, int height, const uint8_t *rgba)
{
    return stbi_write_png(path.c_str(), width, height, 4, rgba, width * 4) != 0;
}
