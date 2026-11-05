#include "tui/rich_text_tui.h"

#include <algorithm>
#include <cctype>

namespace turich {
namespace {

int GlyphLen(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

int Cols(const std::string& s) {
    int n = 0;
    size_t i = 0;
    while (i < s.size()) {
        int g = std::min(GlyphLen((unsigned char)s[i]), (int)(s.size() - i));
        ++n;
        i += (size_t)g;
    }
    return n;
}

struct Word {
    std::string text;
    Sp sp;
};

std::vector<Word> ParseLine(const std::string& line) {
    std::vector<Word> words;
    size_t i = 0;
    const size_t n = line.size();
    auto push = [&](size_t start, size_t len, Sp sp) {
        if (len > 0) words.push_back({line.substr(start, len), sp});
    };
    while (i < n) {
        if (line[i] == '`') {
            size_t end = line.find('`', i + 1);
            if (end == std::string::npos) {
                i++;
                continue;
            }
            size_t ws = i + 1;
            while (ws < end && line[ws] == ' ') ws++;
            size_t we = end;
            while (we > ws && line[we - 1] == ' ') we--;
            push(ws, we - ws, Sp::Code);
            i = end + 1;
        } else if (n - i >= 2 && line[i] == '*' && line[i + 1] == '*') {
            size_t end = line.find("**", i + 2);
            if (end == std::string::npos) {
                i += 2;
                continue;
            }
            size_t ws = i + 2, we = end;
            while (ws < we && line[ws] == ' ') ws++;
            while (we > ws && line[we - 1] == ' ') we--;
            size_t w = ws;
            while (w < we) {
                size_t sp = line.find(' ', w);
                if (sp == std::string::npos || sp >= we) sp = we;
                push(w, sp - w, Sp::Bold);
                w = sp + 1;
            }
            i = end + 2;
        } else if (line.compare(i, 7, "[image ") == 0) {
            size_t j = i + 7;
            while (j < n && isdigit((unsigned char)line[j])) ++j;
            if (j < n && line[j] == ']') {
                push(i, j + 1 - i, Sp::Image);
                i = j + 1;
                continue;
            }
            i++;
        } else {
            size_t sp = line.find(' ', i);
            if (sp == std::string::npos) sp = n;
            push(i, sp - i, Sp::Plain);
            i = sp + 1;
        }
    }
    return words;
}

std::string ExpandWhitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\t')
            out += "    ";
        else if (c != '\r')
            out += c;
    }
    return out;
}

} // namespace

std::string rowText(const Row& row) {
    std::string out;
    for (const Span& s : row.spans) out += s.text;
    return out;
}

std::vector<Row> rows(const std::string& text, int width, const std::string& prefix) {
    std::vector<Row> out;
    const int w = std::max(1, width);
    const std::string cont(prefix.size(), ' ');
    bool inFence = false;

    Row cur;
    int col = 0;

    auto pushSpan = [&](const std::string& t, Sp sp) {
        if (t.empty()) return;
        if (!cur.spans.empty() && cur.spans.back().sp == sp)
            cur.spans.back().text += t;
        else
            cur.spans.push_back({t, sp});
        col += Cols(t);
    };
    auto flush = [&] {
        out.push_back(std::move(cur));
        cur = Row{};
        col = 0;
        if (!cont.empty()) pushSpan(cont, Sp::Plain);
    };

    auto renderLine = [&](const std::string& line) {
        cur = Row{};
        col = 0;
        if (line.rfind("```", 0) == 0) {
            inFence = !inFence;
            return;
        }
        if (inFence) {
            pushSpan(prefix + line, Sp::Fence);
            flush();
            return;
        }
        if (!prefix.empty()) pushSpan(prefix, Sp::Plain);
        if (line.empty()) {
            flush();
            return;
        }
        const int first = Cols(prefix);
        for (const Word& word : ParseLine(line)) {
            if (col > first && col + 1 + Cols(word.text) > w) flush();
            if (col > first) pushSpan(" ", Sp::Plain);
            size_t i = 0;
            while (i < word.text.size()) {
                if (col >= w) flush();
                int g = std::min(GlyphLen((unsigned char)word.text[i]),
                                 (int)(word.text.size() - i));
                pushSpan(word.text.substr(i, (size_t)g), word.sp);
                i += (size_t)g;
            }
        }
        flush();
    };

    const std::string body = ExpandWhitespace(text);
    size_t start = 0;
    while (true) {
        size_t nl = body.find('\n', start);
        renderLine(body.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

} // namespace turich
