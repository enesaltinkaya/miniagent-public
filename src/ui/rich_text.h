#pragma once

#include <string>
#include <vector>

#include "imgui.h"

// One visual line of a RenderRichText pass, in screen coordinates.
// byteStart/byteEnd bound the body text this line's words cover; a
// wordless line (empty / blank) carries a zero-width anchor at byteStart.
// `lineIdx` in words points back here.
struct RichTextLine {
    float x0 = 0.0f;
    float y0 = 0.0f;
    float y1 = 0.0f;
    int byteStart = 0;
    int byteEnd = 0;
    int firstWord = -1; // indices into RichTextLayout::words; -1 (both)
    int lastWord = -1;  // means the line has no words
};

// A text run drawn with one font: a parsed word of a plain line, or a
// whole line inside a code fence. Byte offsets are relative to the body
// text passed to RenderRichText.
struct RichTextWord {
    int byteStart = 0;
    int byteLen = 0;
    float x = 0.0f;  // screen x where the run starts
    int lineIdx = 0; // visual line the run was drawn on
    ImFont* font = nullptr;
    float fontSize = 0.0f;
};

// Line/word geometry of one RenderRichText pass, rebuilt on every frame
// the text is actually drawn. Only usable while `frame` is the current
// ImGui frame count (messages outside the drawn band keep a stale layout).
struct RichTextLayout {
    std::vector<RichTextLine> lines;
    std::vector<RichTextWord> words;
    int frame = -1;

    void clear() {
        lines.clear();
        words.clear();
        frame = -1;
    }
};

// Renders markdown-ish text (``` fences, `inline code`, **bold**) starting
// at `origin`, wrapping at maxWidth. `mono` selects the monospace family
// (agent output); otherwise plain/bold text uses the default family.
// `italic` uses the italic mono face when available (thinking blocks).
// Returns the total height in pixels consumed. With `measureOnly` no
// pixels are drawn; `outWidth` receives the widest rendered line in
// pixels (useful for sizing a background that must be drawn before the
// text). With `outLayout` the per-line/per-word geometry is recorded (for
// hit-testing; e.g. drag-selecting text).
float RenderRichText(ImVec2 origin,
                     float maxWidth,
                     const std::string& text,
                     bool mono = true,
                     ImU32 base = IM_COL32(215, 215, 220, 255),
                     bool italic = false,
                     bool measureOnly = false,
                     float* outWidth = nullptr,
                     RichTextLayout* outLayout = nullptr);
