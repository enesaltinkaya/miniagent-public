#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "stb_image.h"
#include "stb_image_write.h"

#include "util/image.h"

static int failures = 0;

static void check(bool ok, const char* what) {
    if (ok) {
        printf("ok   %s\n", what);
    } else {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static std::string makePng(const char* path, int w, int h) {
    std::vector<unsigned char> px((size_t)w * h * 4);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char* p = &px[(size_t)(y * w + x) * 4];
            p[0] = (unsigned char)(x * 255 / w);
            p[1] = (unsigned char)(y * 255 / h);
            p[2] = 128;
            p[3] = 255;
        }
    }
    stbi_write_png(path, w, h, 4, px.data(), w * 4);
    return path;
}

static std::string makeJpg(const char* path, int w, int h, int quality) {
    std::vector<unsigned char> px((size_t)w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char* p = &px[(size_t)(y * w + x) * 4];
            p[0] = (unsigned char)(255 - x * 255 / w);
            p[1] = (unsigned char)(y * 255 / h);
            p[2] = 64;
            p[3] = 255;
        }
    stbi_write_jpg(path, w, h, 4, px.data(), quality);
    return path;
}

int main() {
    // Sniffing
    check(detectImageFormat("\x89PNG\r\n\x1a\n") == ImageFormat::Png, "sniff png magic");
    check(detectImageFormat("\xff\xd8\xff\xe0") == ImageFormat::Jpeg, "sniff jpeg magic");
    check(detectImageFormat("hello world") == ImageFormat::None, "reject non-image");

    // Small PNG: pass-through untouched
    std::string smallPng = makePng("/tmp/miniagent_t_small.png", 640, 480);
    {
        auto img = processImageFile(smallPng);
        check(img.has_value(), "process small png");
        if (img) {
            check(img->mimeType == "image/png", "small png keeps png mime");
            check(img->width == 640 && img->height == 480, "small png keeps dimensions");
            check(!img->wasResized, "small png not resized");
            check(!img->base64.empty() && img->base64.size() == (img->base64.size() + 3) / 4 * 4, "base64 length valid");
        }
    }

    // Oversized PNG: resized to fit 2000x2000
    std::string bigPng = makePng("/tmp/miniagent_t_big.png", 4000, 3000);
    {
        auto img = processImageFile(bigPng);
        check(img.has_value(), "process big png");
        if (img) {
            check(img->width == 2000, "big png resized to width 2000");
            check(img->height == 1500, "big png resized to height 1500");
            check(img->wasResized, "big png marked resized");
            std::string hint = imageDimensionHint(*img);
            check(hint.find("4000x3000") != std::string::npos && hint.find("2000x1500") != std::string::npos,
                  "dimension hint mentions both sizes");
            check(hint.find("2.00") != std::string::npos, "dimension hint has scale factor");
            check(img->base64.size() < 4500 * 1024, "encoded size within limit");
        }
    }

    // Small JPEG: pass-through
    std::string smallJpg = makeJpg("/tmp/miniagent_t_small.jpg", 800, 600, 85);
    {
        auto img = processImageFile(smallJpg);
        check(img.has_value(), "process small jpeg");
        if (img) {
            check(img->mimeType == "image/jpeg", "small jpeg keeps jpeg mime");
            check(!img->wasResized, "small jpeg not resized");
        }
    }

    // Oversized JPEG: resized, prefers jpeg output
    std::string bigJpg = makeJpg("/tmp/miniagent_t_big.jpg", 4000, 2500, 90);
    {
        auto img = processImageFile(bigJpg);
        check(img.has_value(), "process big jpeg");
        if (img) {
            check(img->width == 2000, "big jpeg resized to width 2000");
            check(img->height == 1250, "big jpeg resized to height 1250");
            check(img->base64.size() < 4500 * 1024, "big jpeg within limit");
        }
    }

    // Garbage file: nullopt
    {
        std::FILE* f = std::fopen("/tmp/miniagent_t_garbage.bin", "wb");
        std::fputs("definitely not an image", f);
        std::fclose(f);
        check(!processImageFile("/tmp/miniagent_t_garbage.bin").has_value(), "reject garbage file");
        check(!processImageFile("/tmp/does-not-exist.png").has_value(), "reject missing file");
    }

    // Base64 output must decode back to valid image bytes
    {
        auto img = processImageFile(bigPng);
        check(img.has_value() && detectImageFormat(std::string("x")) == ImageFormat::None, "sanity: sniff works on non-image");
        // Decode base64
        // Decode base64
        auto rev = [](char c) -> int {
            if (c >= 'A' && c <= 'Z')
                return c - 'A';
            if (c >= 'a' && c <= 'z')
                return c - 'a' + 26;
            if (c >= '0' && c <= '9')
                return c - '0' + 52;
            if (c == '+')
                return 62;
            if (c == '/')
                return 63;
            return 0;
        };
        const std::string& b64 = img ? img->base64 : std::string{};
        std::vector<unsigned char> decoded;
        for (size_t i = 0; i + 3 < b64.size(); i += 4) {
            unsigned v = (rev(b64[i]) << 18) | (rev(b64[i + 1]) << 12) | (rev(b64[i + 2]) << 6) | rev(b64[i + 3]);
            decoded.push_back((v >> 16) & 0xff);
            if (b64[i + 2] != '=')
                decoded.push_back((v >> 8) & 0xff);
            if (b64[i + 3] != '=')
                decoded.push_back(v & 0xff);
        }
        if (img && !decoded.empty()) {
            std::string raw(decoded.begin(), decoded.end());
            check(detectImageFormat(raw) == ImageFormat::Png, "base64 decodes to png bytes");
            int w = 0, h = 0, c = 0;
            unsigned char* d = stbi_load_from_memory(decoded.data(), decoded.size(), &w, &h, &c, 4);
            check(d && w == 2000 && h == 1500, "decoded image has resized dimensions");
            // Pixel fidelity: bigPng is a horizontal R / vertical G gradient (B=128).
            // A wrong stbi_write_png stride produces a mangled image that fails this.
            if (d && w == 2000 && h == 1500) {
                int bad = 0;
                for (int y = 0; y < h; y += 100) {
                    for (int x = 0; x < w; x += 100) {
                        const unsigned char* p = d + (size_t)(y * w + x) * 4;
                        int er = std::abs((int)p[0] - x * 255 / w);
                        int eg = std::abs((int)p[1] - y * 255 / h);
                        int eb = std::abs((int)p[2] - 128);
                        if (er > 4 || eg > 4 || eb > 4)
                            bad++;
                    }
                }
                check(bad == 0, "decoded pixels match source gradient");
                // Dump for visual inspection.
                FILE* out = std::fopen("/tmp/miniagent_t_roundtrip.png", "wb");
                if (out) {
                    fwrite(decoded.data(), 1, decoded.size(), out);
                    fclose(out);
                }
            }
            free(d);
        }
    }

    if (failures) {
        printf("%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("all image tests passed\n");
    return 0;
}
