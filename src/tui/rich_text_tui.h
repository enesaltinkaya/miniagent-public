#pragma once

#include <string>
#include <vector>

// Markdown-lite text model for the TUI (PLAN_TUI.md §4 "Rendering
// mapping"): ``` fences, `inline code`, **bold** and [image N] tokens,
// parsed and wrapped to terminal columns. Mirrors the desktop's
// ui/rich_text.cpp rules (fence markers toggle and are dropped, fence
// bodies stay unwrapped, inline code keeps inner spaces as one word,
// bold runs split into words) so the two frontends agree on where a body
// breaks and what gets a highlight. Pure data: no ftxui, no ImGui, so
// tui_render_test can assert on spans directly.

namespace turich {

enum class Sp {
    Plain,
    Code,
    Bold,
    Image,
    Fence,
};

struct Span {
    std::string text;
    Sp sp = Sp::Plain;
};

struct Row {
    std::vector<Span> spans;
};

// Parse `text` (markdown-lite) and wrap to `width` display columns. The
// first line is prefixed with `prefix`; continuation lines align under it
// (prefix.size() spaces). UTF-8 sequences are never split; tabs are
// pre-expanded to four spaces and CR dropped. Wide (CJK) glyphs count one
// column each — the same accepted boundary overflow as M1.
std::vector<Row> rows(const std::string& text, int width, const std::string& prefix);

// Concatenated span text of one row (test helper / debugging).
std::string rowText(const Row& row);

} // namespace turich
