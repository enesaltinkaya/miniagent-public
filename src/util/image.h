#pragma once

#include <optional>
#include <string>

struct ImageAttachment {
    std::string base64;
    std::string mimeType; // "image/png" | "image/jpeg"
    int width = 0;
    int height = 0;
    int originalWidth = 0;
    int originalHeight = 0;
    bool wasResized = false;
};

enum class ImageFormat { None, Png, Jpeg };

// Sniff PNG/JPEG magic bytes. Returns None for everything else.
ImageFormat detectImageFormat(const std::string& data);

// Decode, resize (max 2000x2000) and re-encode image bytes as base64.
// Returns nullopt if the data is not a supported image or cannot be
// encoded within the size limit.
std::optional<ImageAttachment> processImageBytes(const std::string& bytes);

// Convenience wrapper: read file from disk and process it.
std::optional<ImageAttachment> processImageFile(const std::string& path);

// Header-only image size probe (PNG/JPEG/etc via stbi_info): no decode,
// no resize. False when the file is missing or not a supported image.
// Used by the TUI's expanded image-tool rows ("path (WxH)", PLAN_TUI.md §4).
bool probeImageSize(const std::string& path, int& w, int& h);

// "[Image: original WxH, displayed at WxH. Multiply coordinates by S to map to original image.]"
// Empty when the image was not resized.
std::string imageDimensionHint(const ImageAttachment& img);
