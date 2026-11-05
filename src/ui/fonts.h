#pragma once

#include "imgui.h"

// Font sizes (px, before display-scale); tweak here to play with the UI.
#define FONT_SIZE_DEFAULT 16.0f
#define FONT_SIZE_MONO 16.0f
#define FONT_SIZE_TITLE 17.0f

// Rich-text renderer font sizes follow the defines above.
constexpr float kSizeDefault = FONT_SIZE_DEFAULT;
constexpr float kSizeMono = FONT_SIZE_MONO;

// Live fonts, populated by SetupFonts() before the first frame; null means
// fall back to the ImGui default font.
extern ImFont* gFontDefault;
extern ImFont* gFontBold;
extern ImFont* gFontMono;
extern ImFont* gFontMonoBold;
extern ImFont* gFontMonoItalic;
extern float gFontScale;

// Adds the app's fonts (user-supplied TTFs under ~/.config/miniagent/fonts,
// falling back to the built-in default) to ImGui's font atlas.
void SetupFonts(ImGuiIO& io, float scale);
