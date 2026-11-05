#include "ui/fonts.h"

#include "util/settings.h"

#include <sys/stat.h>

ImFont* gFontDefault = nullptr;
ImFont* gFontBold = nullptr;
ImFont* gFontMono = nullptr;
ImFont* gFontMonoBold = nullptr;
ImFont* gFontMonoItalic = nullptr;
float gFontScale = 1.0f;

namespace {

// True if path names an existing regular file.
bool FileExists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

}  // namespace

void SetupFonts(ImGuiIO& io, float scale) {
    gFontScale = scale;
    // User-supplied fonts live in ~/.config/miniagent/fonts; any file that is
    // missing (or the whole dir) falls back to the built-in default font.
    const std::string fontDir = miniagentFontsDir() + "/";
    const std::string montserrat =
        FileExists(fontDir + "montserrat-Regular.ttf") ? fontDir + "montserrat-Regular.ttf" : std::string{};
    const std::string montserratBold =
        FileExists(fontDir + "montserrat-Bold.ttf") ? fontDir + "montserrat-Bold.ttf" : std::string{};
    const std::string jbMono =
        FileExists(fontDir + "jetbrainsMono.ttf") ? fontDir + "jetbrainsMono.ttf" : std::string{};
    const std::string jbMonoBold =
        FileExists(fontDir + "jetbrainsMono-Bold.ttf") ? fontDir + "jetbrainsMono-Bold.ttf" : std::string{};
    const std::string jbMonoItalic =
        FileExists(fontDir + "jetbrainsMono-Italic.ttf") ? fontDir + "jetbrainsMono-Italic.ttf" : std::string{};
    // Default range is ASCII only; add U+2000..U+206F so punctuation like the
    // session-list bullet (U+2022) and the ellipsis (U+2026) render.
    static const ImWchar extraRanges[] = {0x0020, 0x007E, 0x2000, 0x206F, 0};

    if (!montserrat.empty()) {
        ImFontConfig cfg;
        gFontDefault =
            io.Fonts->AddFontFromFileTTF(montserrat.c_str(), FONT_SIZE_DEFAULT * scale, &cfg, extraRanges);
        if (gFontDefault && !montserratBold.empty())
            gFontBold = io.Fonts->AddFontFromFileTTF(montserratBold.c_str(),
                                                     FONT_SIZE_DEFAULT * scale,
                                                     &cfg, extraRanges);

        if (!jbMono.empty()) {
            ImFontConfig mcfg;
            mcfg.PixelSnapH = true;
            gFontMono = io.Fonts->AddFontFromFileTTF(jbMono.c_str(), FONT_SIZE_MONO * scale, &mcfg, extraRanges);
            if (gFontMono && !jbMonoBold.empty())
                gFontMonoBold =
                    io.Fonts->AddFontFromFileTTF(jbMonoBold.c_str(), FONT_SIZE_MONO * scale, &mcfg, extraRanges);
            if (gFontMono && !jbMonoItalic.empty())
                gFontMonoItalic =
                    io.Fonts->AddFontFromFileTTF(jbMonoItalic.c_str(),
                                                 FONT_SIZE_MONO * scale,
                                                 &mcfg, extraRanges);
        }
    }
    if (!gFontDefault) {
        ImFontConfig dcfg;
        dcfg.SizePixels = FONT_SIZE_DEFAULT * scale;
        gFontDefault = io.Fonts->AddFontDefault(&dcfg);
        gFontBold = nullptr;
        gFontMono = nullptr;
        gFontMonoBold = nullptr;
        gFontMonoItalic = nullptr;
    }
}
