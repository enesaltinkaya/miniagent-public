#include "ui/rich_text.h"

#include "ui/fonts.h"

#include <algorithm>
#include <cctype>
#include <limits.h>
#include <string>
#include <utility>
#include <vector>

namespace {

ImVec2 MeasureText(ImFont* font, float size, const char* s, const char* end) {
    return font->CalcTextSizeA(size, FLT_MAX, 0.0f, s, end);
}

enum class RStyle {
    Plain,
    Bold,
    Code,
    ImageToken, // [image N]: a pasted image the prompt references
};

struct RRun {
    const char* s;
    size_t len;
    RStyle style;
};

struct RWord {
    const char* s;
    size_t len;
    RStyle style;
};

std::vector<RWord> parseLine(const std::string& line) {
    std::vector<RWord> words;
    size_t i = 0;
    const size_t n = line.size();
    auto pushWord = [&](size_t start, size_t len, RStyle style) {
        if (len > 0)
            words.push_back({line.c_str() + start, len, style});
    };
    while (i < n) {
        if (line[i] == '`') {
            size_t end = line.find('`', i + 1);
            if (end == std::string::npos) {
                i++;
                continue;
            }
            // inline code: treat whole thing as one word (spaces kept)
            size_t ws = i + 1;
            while (ws < end && line[ws] == ' ')
                ws++;
            size_t we = end;
            while (we > ws && line[we - 1] == ' ')
                we--;
            pushWord(ws, we - ws, RStyle::Code);
            i = end + 1;
        } else if (line.size() - i >= 2 && line[i] == '*' && line[i + 1] == '*') {
            size_t end = line.find("**", i + 2);
            if (end == std::string::npos) {
                i += 2;
                continue;
            }
            size_t ws = i + 2, we = end;
            while (ws < we && line[ws] == ' ')
                ws++;
            while (we > ws && line[we - 1] == ' ')
                we--;
            // Bold run may contain spaces: emit each word separately.
            size_t w = ws;
            while (w < we) {
                size_t sp = line.find(' ', w);
                if (sp == std::string::npos || sp >= we)
                    sp = we;
                pushWord(w, sp - w, RStyle::Bold);
                w = sp + 1;
            }
            i = end + 2;
        } else if (line.compare(i, 7, "[image ") == 0) {
            // Pasted-image token: "[image N]" as a single word, in its own color.
            size_t j = i + 7;
            while (j < n && isdigit((unsigned char)line[j]))
                ++j;
            if (j < n && line[j] == ']') {
                pushWord(i, j + 1 - i, RStyle::ImageToken);
                i = j + 1;
                continue;
            }
            i++;
        } else {
            size_t sp = line.find(' ', i);
            if (sp == std::string::npos)
                sp = n;
            pushWord(i, sp - i, RStyle::Plain);
            i = sp + 1;
        }
    }
    return words;
}

ImU32 ColorForStyle(RStyle style, ImU32 base) {
    switch (style) {
        case RStyle::Bold:
            return IM_COL32(255, 255, 255, 255);
        case RStyle::Code:
            return IM_COL32(130, 190, 255, 255);
        case RStyle::ImageToken:
            return IM_COL32(255, 196, 110, 255);
        default:
            return base;
    }
}

}  // namespace

float RenderRichText(ImVec2 origin,
                     float maxWidth,
                     const std::string& text,
                     bool mono,
                     ImU32 base,
                     bool italic,
                     bool measureOnly,
                     float* outWidth,
                     RichTextLayout* outLayout) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!dl || !gFontDefault)
        return 0;
    const float sizeDefault = kSizeDefault * gFontScale;
    const float sizeMono = kSizeMono * gFontScale;
    const float sizeMain = mono ? sizeMono : sizeDefault;
    ImFont* mainFont = mono ? (gFontMono ? gFontMono : gFontDefault) : gFontDefault;
    const float lineH = mainFont->CalcTextSizeA(sizeMain, FLT_MAX, 0.0f, "Agj").y * 1.2f;
    const float spaceW = MeasureText(mainFont, sizeMain, " ", " " + 1).x;

    const bool record = !measureOnly && outLayout != nullptr;
    RichTextLayout* lay = record ? outLayout : nullptr;
    if (record)
        lay->clear();

    auto wordFontAndSize = [&](RStyle style) -> std::pair<ImFont*, float> {
        switch (style) {
            case RStyle::Bold:
                if (mono) {
                    ImFont* f =
                        gFontMonoBold ? gFontMonoBold : (gFontMono ? gFontMono : gFontDefault);
                    return {f, sizeMono};
                }
                if (gFontBold)
                    return {gFontBold, sizeDefault};
                return {gFontDefault, sizeDefault};
            case RStyle::Code:
                return {gFontMono ? gFontMono : gFontDefault, sizeMono};
            default:
                if (italic && gFontMonoItalic)
                    return {gFontMonoItalic, sizeMono};
                return {mainFont, sizeMain};
        }
    };

    float x = origin.x;
    float y = origin.y;
    bool inFence = false;
    float maxExtent = 0.0f;  // widest line, for outWidth

    // The caller owns `cur`'s position in `text` (byte offset of the
    // first byte of the line being accumulated), so the layout can record
    // byte offsets into the original text.
    std::string cur;
    auto renderLine = [&](const std::string& line, size_t byteStart) {
        if (line.rfind("```", 0) == 0) {
            inFence = !inFence;
            x = origin.x;
            y += lineH;
            return;
        }
        if (inFence) {
            ImFont* mfont = gFontMono ? gFontMono : gFontDefault;
            float msz = gFontMono ? sizeMono : sizeDefault;
            float tw = MeasureText(mfont, msz, line.c_str(), line.c_str() + line.size()).x;
            float w = std::min(maxWidth, tw + 8.0f);
            if (!measureOnly) {
                dl->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + w, y + lineH), IM_COL32(30, 34, 40, 255));
                ImU32 c = IM_COL32(190, 200, 210, 255);
                dl->AddText(mfont,
                            msz,
                            ImVec2(origin.x + 4, y + 2),
                            c,
                            line.c_str(),
                            line.c_str() + line.size());
            }
            maxExtent = std::max(maxExtent, w);
            // Fence lines are not wrapped: the whole line is one word, so a
            // layout pass can treat it like a single-run line.
            if (lay) {
                const int li = (int)lay->lines.size();
                int firstWord = -1;
                if (!line.empty()) {
                    firstWord = (int)lay->words.size();
                    lay->words.push_back(RichTextWord{(int)byteStart, (int)line.size(), origin.x + 4.0f,
                                                      li, mfont, msz});
                }
                lay->lines.push_back(RichTextLine{origin.x, y, y + lineH, (int)byteStart,
                                                  (int)byteStart + (int)line.size(), firstWord,
                                                  firstWord});
            }
            x = origin.x;
            y += lineH;
            return;
        }
        if (line.empty()) {
            if (lay)
                lay->lines.push_back(RichTextLine{origin.x, y, y + lineH * 0.8f, (int)byteStart,
                                                  (int)byteStart, -1, -1});
            x = origin.x;
            y += lineH * 0.8f;
            return;
        }
        x = origin.x;
        float lineW = 0.0f;
        const float yLineStart = y;
        int curLine = -1;  // visual line currently being written
        for (const RWord& w : parseLine(line)) {
            auto [font, ws] = wordFontAndSize(w.style);
            float ww = MeasureText(font, ws, w.s, w.s + w.len).x;
            const bool wrapped = x > origin.x + 1 && x + ww > origin.x + maxWidth;
            if (wrapped) {
                x = origin.x;
                y += lineH;
            }
            if (lay) {
                if (curLine < 0 || wrapped) {
                    curLine = (int)lay->lines.size();
                    const int b = (int)(byteStart + (size_t)(w.s - line.c_str()));
                    lay->lines.push_back(RichTextLine{origin.x, y, y + lineH, b, b + (int)w.len,
                                                      (int)lay->words.size(), -1});
                }
                const int wStart = (int)(byteStart + (size_t)(w.s - line.c_str()));
                lay->words.push_back(RichTextWord{wStart, (int)w.len, x, curLine, font, ws});
                lay->lines[curLine].byteEnd = wStart + (int)w.len;
                lay->lines[curLine].lastWord = (int)lay->words.size() - 1;
            }
            if (!measureOnly)
                dl->AddText(font, ws, ImVec2(x, y), ColorForStyle(w.style, base), w.s, w.s + w.len);
            x += ww + spaceW;
            lineW = std::max(lineW, x - spaceW);
        }
        // A wordless-but-nonempty line (spaces only) still consumes a full
        // line; record its band so the layout stays contiguous.
        if (lay && curLine < 0)
            lay->lines.push_back(RichTextLine{origin.x, yLineStart, yLineStart + lineH, (int)byteStart,
                                              (int)byteStart, -1, -1});
        maxExtent = std::max(maxExtent, lineW);
        y += lineH;
    };

    size_t curStart = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\n') {
            renderLine(cur, curStart);
            cur.clear();
            curStart = i + 1;
        } else {
            cur += text[i];
        }
    }
    renderLine(cur, curStart);

    if (record)
        lay->frame = ImGui::GetFrameCount();
    if (outWidth)
        *outWidth = maxExtent;
    return y - origin.y;
}
