// Draws the PSP frame into the window (OpenGL 3.3 core, needs a current context).
#pragma once

#include <cstdint>
#include <string>

class Renderer {
public:
    bool init(std::string &err);
    void destroy();

    // Replaces the texture contents with an RGBA8888 image.
    void upload(const uint8_t *rgba, int width, int height);

    // Clears the window and draws the frame, letterboxed to keep its aspect
    // ratio, nearest-neighbour sampled. With `integer_scale` only whole
    // multiples of the frame size are used (sharpest pixels, wider borders).
    void draw(int window_width, int window_height, bool integer_scale = false);

private:
    unsigned program_ = 0;
    unsigned vao_ = 0;
    unsigned texture_ = 0;
    int scale_loc_ = -1;
    int tex_width_ = 0;
    int tex_height_ = 0;
};
