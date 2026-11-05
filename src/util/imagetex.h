#pragma once

#include <string>

// GL texture cache for images shown in the expanded "read" tool row.
//
// Decodes image files from disk (stb_image, same library the agent uses)
// and uploads them as RGBA8 OpenGL textures; the decoded pixels are then
// freed, so base64 is never involved. Textures are cached keyed by
// path + mtime (an edited file is reloaded), and all of them are dropped
// when the cwd changes (session switch), which may leave the cached paths
// relative to a different directory.

struct ImageTex {
    unsigned int gl = 0;
    int width = 0;
    int height = 0;
    long long mtimeMs = 0; // file mtime the pixels were decoded from
};

// Looks up or decodes+uploads the texture for `path` ("" or missing file:
// nullptr). Stale entries are evicted and reloaded when the file changed.
ImageTex* getImageTex(const std::string& path);

// Uploads `px` as a new texture and inserts it into the cache.
void cacheImageTex(const std::string& path, int w, int h, long long mtimeMs,
                   const unsigned char* px);

// Drops every cached texture (call when the cwd changes).
void clearImageTexCache();
