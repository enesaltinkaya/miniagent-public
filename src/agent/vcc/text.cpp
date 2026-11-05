#include "agent/vcc/text.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace vcc {
namespace {

struct WordSpan {
    size_t start;
    size_t end;
};

bool isWordByte(unsigned char c) {
    return std::isalnum(c) || c >= 0x80;
}

const std::vector<const char*>& stopWords() {
    static const std::vector<const char*> words = {
        "a", "an", "the", "is", "are", "was", "were", "be", "been", "being",
        "have", "has", "had", "do", "does", "did", "will", "would", "could",
        "should", "may", "might", "shall", "can", "need", "must",
        "to", "of", "in", "for", "on", "with", "at", "by", "from", "as",
        "into", "through", "during", "before", "after", "above", "below",
        "between", "under", "over",
        "and", "but", "or", "nor", "not", "so", "yet", "both", "either",
        "neither", "each", "every", "all", "any", "few", "more", "most",
        "other", "some", "such", "no",
        "that", "this", "these", "those", "it", "its",
        "i", "me", "my", "we", "our", "you", "your", "he", "him", "his",
        "she", "her", "they", "them", "their", "who", "which", "what",
        "if", "then", "than", "when", "where", "how", "just", "also",
    };
    return words;
}

std::string toLower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string normalizeForBudget(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool pendingSpace = false;
    int newlineRun = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                continue;
            c = '\n';
        }
        if (c == '\n') {
            pendingSpace = false;
            if (++newlineRun <= 2)
                out.push_back('\n');
            continue;
        }
        newlineRun = 0;
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!out.empty())
                pendingSpace = true;
            continue;
        }
        if (pendingSpace && out.back() != '\n')
            out.push_back(' ');
        pendingSpace = false;
        out.push_back(c);
    }
    size_t b = out.find_first_not_of(" \n");
    if (b == std::string::npos)
        return "";
    size_t e = out.find_last_not_of(" \n");
    return out.substr(b, e - b + 1);
}

std::vector<WordSpan> significantWordSpans(const std::string& flat) {
    std::vector<WordSpan> words;
    size_t i = 0;
    while (i < flat.size()) {
        if (!isWordByte(static_cast<unsigned char>(flat[i]))) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < flat.size() && isWordByte(static_cast<unsigned char>(flat[i])))
            ++i;
        if (!isStopWord(flat.substr(start, i - start)))
            words.push_back({start, i});
    }
    return words;
}

bool atUtf8Boundary(const std::string& text, size_t pos) {
    if (pos >= text.size())
        return true;
    return (static_cast<unsigned char>(text[pos]) & 0xC0) != 0x80;
}

}  // namespace

bool isStopWord(const std::string& word) {
    const std::string lower = toLower(word);
    for (const char* w : stopWords())
        if (lower == w)
            return true;
    return false;
}

std::string sanitize(std::string text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n')
                continue;
            out.push_back('\n');
            continue;
        }
        if (c == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            size_t j = i + 2;
            while (j < text.size() && (std::isdigit(static_cast<unsigned char>(text[j])) || text[j] == ';'))
                ++j;
            if (j < text.size() && std::isalpha(static_cast<unsigned char>(text[j])))
                i = j;
            continue;
        }
        if (static_cast<unsigned char>(c) < 0x20 && c != '\n' && c != '\t')
            continue;
        out.push_back(c);
    }
    return out;
}

std::string clip(const std::string& text, size_t max) {
    if (text.size() <= max)
        return text;
    size_t cut = text.rfind(' ', max);
    size_t end = (cut != std::string::npos && cut > max * 6 / 10) ? cut : max;
    while (end > 0 && !atUtf8Boundary(text, end))
        --end;
    return text.substr(0, end);
}

std::string clipSentence(const std::string& text, size_t max) {
    if (text.size() <= max)
        return text;
    const size_t half = max / 2;
    size_t end = 0;
    for (size_t i = 0; i + 1 < text.size() && i < max; ++i) {
        char c = text[i];
        if ((c == '.' || c == '!' || c == '?') &&
            (text[i + 1] == ' ' || text[i + 1] == '\n' || i + 1 == text.size())) {
            if (i + 1 >= half)
                end = std::max(end, i + 1);
        }
    }
    if (end > 0)
        return text.substr(0, end);
    return clip(text, max);
}

std::vector<std::string> nonEmptyLines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string_view line(nl == std::string::npos ? text.data() + start : text.data() + start,
                              (nl == std::string::npos ? text.size() : nl) - start);
        size_t b = 0, e = line.size();
        while (b < e && std::isspace(static_cast<unsigned char>(line[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(line[e - 1])))
            --e;
        if (e > b)
            out.emplace_back(line.substr(b, e - b));
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return out;
}

std::string firstLine(const std::string& text, size_t max) {
    size_t nl = text.find('\n');
    return clip(nl == std::string::npos ? text : text.substr(0, nl), max);
}

std::string truncateWords(const std::string& text, size_t limit) {
    const std::string flat = normalizeForBudget(text);
    size_t count = 0;
    size_t lastEnd = 0;
    size_t i = 0;
    while (i < flat.size()) {
        if (!isWordByte(static_cast<unsigned char>(flat[i]))) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < flat.size() && isWordByte(static_cast<unsigned char>(flat[i])))
            ++i;
        if (!isStopWord(flat.substr(start, i - start))) {
            ++count;
            if (count > limit)
                return flat.substr(0, lastEnd) + "...(truncated)";
        }
        lastEnd = i;
    }
    return flat;
}

std::string truncateHeadTail(const std::string& text, size_t headWords, size_t tailWords) {
    const std::string flat = normalizeForBudget(text);
    if (headWords == 0 || tailWords == 0)
        return flat;
    const std::vector<WordSpan> words = significantWordSpans(flat);
    if (words.size() <= headWords + tailWords)
        return flat;
    std::string head = flat.substr(0, words[headWords - 1].end);
    while (!head.empty() && (head.back() == ' ' || head.back() == '\n'))
        head.pop_back();
    std::string tail = flat.substr(words[words.size() - tailWords].start);
    return head + "\n...(middle truncated)...\n" + tail;
}

namespace {

std::vector<std::string> wrapLine(const std::string& line, size_t maxChars) {
    if (line.size() <= maxChars)
        return {line};
    size_t indentLen = 0;
    while (indentLen < line.size() && (line[indentLen] == ' ' || line[indentLen] == '\t'))
        ++indentLen;
    size_t bullet = 0;
    if (indentLen < line.size() && (line[indentLen] == '-' || line[indentLen] == '*') &&
        indentLen + 1 < line.size() && line[indentLen + 1] == ' ')
        bullet = 2;
    else if (indentLen < line.size() && std::isdigit(static_cast<unsigned char>(line[indentLen]))) {
        size_t j = indentLen;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])))
            ++j;
        if (j < line.size() && line[j] == '.' && j + 1 < line.size() && line[j + 1] == ' ')
            bullet = j + 2 - indentLen;
    }
    const std::string indent = line.substr(0, indentLen + bullet);
    const std::string continuation(std::min(indent.size(), size_t{8}), ' ');
    std::vector<std::string> wrapped;
    std::string remaining = line;
    std::string prefix;
    while (prefix.size() + remaining.size() > maxChars) {
        size_t available = std::max(size_t{20}, maxChars - prefix.size());
        size_t splitAt = remaining.rfind(' ', available);
        if (splitAt == std::string::npos || splitAt < available / 2)
            splitAt = available;
        while (splitAt > 0 && splitAt < remaining.size() && !atUtf8Boundary(remaining, splitAt))
            --splitAt;
        std::string piece = prefix + remaining.substr(0, splitAt);
        while (!piece.empty() && piece.back() == ' ')
            piece.pop_back();
        wrapped.push_back(std::move(piece));
        remaining = remaining.substr(std::min(splitAt, remaining.size()));
        size_t b = 0;
        while (b < remaining.size() && remaining[b] == ' ')
            ++b;
        remaining = remaining.substr(b);
        prefix = continuation;
        if (remaining.empty())
            break;
    }
    if (!remaining.empty())
        wrapped.push_back(prefix + remaining);
    return wrapped;
}

}  // namespace

std::string wrapLongLines(const std::string& text, size_t maxChars) {
    std::string out;
    size_t start = 0;
    bool first = true;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string line = text.substr(start, (nl == std::string::npos ? text.size() : nl) - start);
        for (const std::string& w : wrapLine(line, maxChars)) {
            if (!first)
                out.push_back('\n');
            out += w;
            first = false;
        }
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return out;
}

}  // namespace vcc
