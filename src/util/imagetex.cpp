#include "util/imagetex.h"

#include <GL/gl.h>

#include <algorithm>
#include <cstdlib>
#include <map>
#include <cstdio>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "stb_image.h" // implementation lives in util/image.cpp (always linked together)

namespace {

std::map<std::string, ImageTex> gCache;

long long FileMtimeMs(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return -1;
    return (long long)st.st_mtime * 1000;
}

bool FileReadable(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && access(path.c_str(), R_OK) == 0;
}

} // namespace

void cacheImageTex(const std::string& path, int w, int h, long long mtimeMs,
                   const unsigned char* px) {
    unsigned int gl = 0;
    glGenTextures(1, &gl);
    if (!gl)
        return;
    glBindTexture(GL_TEXTURE_2D, gl);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    ImageTex tex;
    tex.gl = gl;
    tex.width = w;
    tex.height = h;
    tex.mtimeMs = mtimeMs;
    gCache[path] = tex;
}

void clearImageTexCache() {
    for (auto& [path, tex] : gCache)
        if (tex.gl)
            glDeleteTextures(1, &tex.gl);
    gCache.clear();
}

ImageTex* getImageTex(const std::string& path) {
    if (path.empty() || !FileReadable(path))
        return nullptr;

    long long mtime = FileMtimeMs(path);

    auto it = gCache.find(path);
    if (it != gCache.end()) {
        if (it->second.mtimeMs == mtime)
            return &it->second;
        // File changed since the last decode: drop the old pixels, re-upload.
        if (it->second.gl)
            glDeleteTextures(1, &it->second.gl);
        it->second.gl = 0;
    }

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f)
        return nullptr;
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return nullptr;
    }
    std::vector<unsigned char> bytes((size_t)size);
    if (std::fread(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
        std::fclose(f);
        return nullptr;
    }
    std::fclose(f);

    int w = 0, h = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, nullptr, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px)
            free(px);
        return nullptr;
    }
    cacheImageTex(path, w, h, mtime, px);
    free(px);
    return &gCache[path];
}
