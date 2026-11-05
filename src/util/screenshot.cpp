#include "screenshot.h"

#include <SDL3/SDL.h>

#include "stb_image_write.h"

#include <GL/gl.h>

#include <cstddef>
#include <vector>

bool saveGlFramebufferPng(const char* path, int width, int height) {
    if (width <= 0 || height <= 0) {
        SDL_Log("screenshot: invalid size %dx%d", width, height);
        return false;
    }

    std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    // glReadPixels returns bottom-up rows; stb expects top-down.
    size_t rowBytes = static_cast<size_t>(width) * 4;
    for (int y = 0; y < height / 2; ++y) {
        unsigned char* a = pixels.data() + static_cast<size_t>(y) * rowBytes;
        unsigned char* b = pixels.data() + static_cast<size_t>(height - 1 - y) * rowBytes;
        for (size_t x = 0; x < rowBytes; ++x) {
            unsigned char t = a[x];
            a[x] = b[x];
            b[x] = t;
        }
    }

    if (!stbi_write_png(path, width, height, 4, pixels.data(), static_cast<int>(rowBytes))) {
        SDL_Log("screenshot: stbi_write_png(%s) failed", path);
        return false;
    }
    SDL_Log("screenshot: saved %s (%dx%d)", path, width, height);
    return true;
}
