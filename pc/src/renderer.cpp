#include "renderer.h"

#include <glad/glad.h>

#include <algorithm>

namespace {

// A full-screen quad from gl_VertexID, scaled to the letterbox.
const char *kVertexShader = R"(#version 330 core
uniform vec2 u_scale;
out vec2 v_uv;
void main()
{
    vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));
    v_uv = vec2(corner.x, 1.0 - corner.y);
    gl_Position = vec4((corner * 2.0 - 1.0) * u_scale, 0.0, 1.0);
}
)";

const char *kFragmentShader = R"(#version 330 core
uniform sampler2D u_frame;
in vec2 v_uv;
out vec4 o_color;
void main()
{
    o_color = vec4(texture(u_frame, v_uv).rgb, 1.0);
}
)";

GLuint compile(GLenum type, const char *source, std::string &err)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        err = std::string("shader compile failed: ") + log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

bool Renderer::init(std::string &err)
{
    GLuint vs = compile(GL_VERTEX_SHADER, kVertexShader, err);
    if (!vs)
        return false;
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFragmentShader, err);
    if (!fs) {
        glDeleteShader(vs);
        return false;
    }
    program_ = glCreateProgram();
    glAttachShader(program_, vs);
    glAttachShader(program_, fs);
    glLinkProgram(program_);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(program_, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetProgramInfoLog(program_, sizeof(log), nullptr, log);
        err = std::string("shader link failed: ") + log;
        return false;
    }
    scale_loc_ = glGetUniformLocation(program_, "u_scale");
    glUseProgram(program_);
    glUniform1i(glGetUniformLocation(program_, "u_frame"), 0);

    glGenVertexArrays(1, &vao_); // core profile needs one bound, even with no attributes

    glGenTextures(1, &texture_);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    return true;
}

void Renderer::destroy()
{
    if (texture_)
        glDeleteTextures(1, &texture_);
    if (vao_)
        glDeleteVertexArrays(1, &vao_);
    if (program_)
        glDeleteProgram(program_);
    texture_ = vao_ = program_ = 0;
}

void Renderer::upload(const uint8_t *rgba, int width, int height)
{
    glBindTexture(GL_TEXTURE_2D, texture_);
    if (width != tex_width_ || height != tex_height_) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     rgba);
        tex_width_ = width;
        tex_height_ = height;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    }
}

void Renderer::draw(int window_width, int window_height, bool integer_scale)
{
    glViewport(0, 0, window_width, window_height);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!tex_width_ || window_width <= 0 || window_height <= 0)
        return;

    float frame_aspect = static_cast<float>(tex_width_) / static_cast<float>(tex_height_);
    float window_aspect = static_cast<float>(window_width) / static_cast<float>(window_height);
    float sx = std::min(1.0f, frame_aspect / window_aspect);
    float sy = std::min(1.0f, window_aspect / frame_aspect);
    if (integer_scale) {
        int k = std::max(1, std::min(window_width / tex_width_, window_height / tex_height_));
        sx = std::min(1.0f, static_cast<float>(k * tex_width_) / window_width);
        sy = std::min(1.0f, static_cast<float>(k * tex_height_) / window_height);
    }

    glUseProgram(program_);
    glUniform2f(scale_loc_, sx, sy);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture_);
    glBindVertexArray(vao_);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
