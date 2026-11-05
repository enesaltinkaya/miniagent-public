#include "util/image.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <unistd.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"

namespace {

constexpr int kMaxDim = 2000;
// base64 payload limit; headroom below the 5MB per-image API limit
constexpr size_t kMaxBase64Bytes = 4500 * 1024;

std::string toBase64(const unsigned char* data, size_t len) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = data[i] << 16;
        if (i + 1 < len)
            v |= data[i + 1] << 8;
        if (i + 2 < len)
            v |= data[i + 2];
        out.push_back(table[v >> 18]);
        out.push_back(table[(v >> 12) & 0x3f]);
        out.push_back(i + 1 < len ? table[(v >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < len ? table[v & 0x3f] : '=');
    }
    return out;
}

// Encode pixels, writing to a temp file (stb_image_write has no
// in-memory API and no strides), then read the bytes back.
std::optional<std::string> encodeRgba(const unsigned char* px, int w, int h,
                                      int channels, int quality) {
    // stbi_write_jpg wants tightly packed RGB; convert from RGBA.
    std::vector<unsigned char> rgb;
    const unsigned char* usePx = px;
    if (channels == 3) {
        rgb.resize((size_t)w * h * 3);
        for (int i = 0; i < w * h; i++) {
            rgb[i * 3] = px[i * 4];
            rgb[i * 3 + 1] = px[i * 4 + 1];
            rgb[i * 3 + 2] = px[i * 4 + 2];
        }
        usePx = rgb.data();
    }

    char tmpl[] = "/tmp/miniagent_img_XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0)
        return std::nullopt;
    std::string path = tmpl;
    close(fd);

    bool ok = false;
    if (channels == 3)
        ok = stbi_write_jpg(path.c_str(), w, h, 3, usePx, quality) != 0;
    else
        ok = stbi_write_png(path.c_str(), w, h, 4, usePx, w * 4) != 0;
    std::string out;
    if (ok) {
        std::ifstream f(path, std::ios::binary);
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    std::remove(path.c_str());
    if (!ok || out.empty())
        return std::nullopt;
    return out;
}

struct EncodedImage {
    std::string base64;
    std::string mime;
};

// Encode the pixels, preferring the original format's encoder first, then
// the other. JPEG quality is stepped down. Returns the first candidate
// whose base64 size fits under kMaxBase64Bytes, else nullopt.
std::optional<EncodedImage> encodeFit(const unsigned char* px, int w, int h, bool preferJpeg) {
    auto encode = [&](int channels, int quality, const std::string& mime) {
        EncodedImage e;
        auto bytes = encodeRgba(px, w, h, channels, quality);
        if (bytes) {
            e.base64 = toBase64(reinterpret_cast<const unsigned char*>(bytes->data()), bytes->size());
            e.mime = mime;
        }
        return e;
    };

    auto fits = [&](const EncodedImage& e) {
        return !e.base64.empty() && e.base64.size() < kMaxBase64Bytes;
    };

    const std::vector<int> jpegQualities{85, 70, 55};
    if (preferJpeg) {
        for (int q : jpegQualities) {
            auto e = encode(3, q, "image/jpeg");
            if (fits(e))
                return e;
        }
        auto e = encode(4, 0, "image/png");
        if (fits(e))
            return e;
    } else {
        auto e = encode(4, 0, "image/png");
        if (fits(e))
            return e;
        for (int q : jpegQualities) {
            e = encode(3, q, "image/jpeg");
            if (fits(e))
                return e;
        }
    }
    return std::nullopt;
}

} // namespace

ImageFormat detectImageFormat(const std::string& data) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(data.data());
    size_t n = data.size();
    if (n >= 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G' &&
        b[4] == 0x0d && b[5] == 0x0a && b[6] == 0x1a && b[7] == 0x0a)
        return ImageFormat::Png;
    if (n >= 3 && b[0] == 0xff && b[1] == 0xd8 && b[2] == 0xff)
        return ImageFormat::Jpeg;
    return ImageFormat::None;
}

std::optional<ImageAttachment> processImageBytes(const std::string& bytes) {
    ImageFormat fmt = detectImageFormat(bytes);
    if (fmt == ImageFormat::None)
        return std::nullopt;
    bool isJpeg = (fmt == ImageFormat::Jpeg);

    int w = 0, h = 0, ch = 0;
    unsigned char* px =
        stbi_load_from_memory(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), &w, &h, &ch, 4);
    if (!px || w <= 0 || h <= 0) {
        free(px);
        return std::nullopt;
    }

    ImageAttachment img;
    img.originalWidth = w;
    img.originalHeight = h;

    // Already within dimension and size limits: pass the original file
    // bytes through untouched (preserves the exact on-disk encoding).
    size_t inBase64 = (bytes.size() + 2) / 3 * 4;
    if (w <= kMaxDim && h <= kMaxDim && inBase64 < kMaxBase64Bytes) {
        img.base64 = toBase64(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
        img.mimeType = isJpeg ? "image/jpeg" : "image/png";
        img.width = w;
        img.height = h;
        free(px);
        return img;
    }

    int tw = w, th = h;
    if (tw > kMaxDim) {
        th = (int)((long long)th * kMaxDim / tw);
        tw = kMaxDim;
    }
    if (th > kMaxDim) {
        tw = (int)((long long)tw * kMaxDim / th);
        th = kMaxDim;
    }
    tw = std::max(tw, 1);
    th = std::max(th, 1);

    bool resized = (tw != w || th != h);
    std::vector<unsigned char> pixelBuf;
    unsigned char* srcPx = px;
    if (resized) {
        pixelBuf.resize((size_t)tw * th * 4);
        stbir_resize_uint8_srgb(px, w, h, (int)((size_t)w * 4), pixelBuf.data(), tw, th,
                                (int)((size_t)tw * 4), STBIR_RGBA);
        srcPx = pixelBuf.data();
    }

    auto encoded = encodeFit(srcPx, tw, th, isJpeg);
    free(px);
    if (!encoded)
        return std::nullopt;

    img.base64 = std::move(encoded->base64);
    img.mimeType = std::move(encoded->mime);
    img.width = tw;
    img.height = th;
    img.wasResized = (tw != w) || (th != h);
    return img;
}

std::optional<ImageAttachment> processImageFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return std::nullopt;
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return processImageBytes(bytes);
}

bool probeImageSize(const std::string& path, int& w, int& h) {
    int comp = 0;
    return stbi_info(path.c_str(), &w, &h, &comp) != 0;
}

std::string imageDimensionHint(const ImageAttachment& img) {
    if (!img.wasResized)
        return "";
    char buf[256];
    snprintf(buf, sizeof(buf),
             "[Image: original %dx%d, displayed at %dx%d. Multiply coordinates by %.2f to map to original image.]",
             img.originalWidth, img.originalHeight, img.width, img.height,
             (double)img.originalWidth / img.width);
    return buf;
}
