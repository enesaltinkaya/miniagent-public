#include "agent/vcc/compile.h"

#include <algorithm>
#include <map>
#include <regex>
#include <set>

#include "agent/vcc/brief.h"
#include "agent/vcc/constants.h"
#include "agent/vcc/normalize.h"
#include "agent/vcc/rank.h"
#include "agent/vcc/sections.h"
#include "agent/vcc/text.h"
#include "agent/vcc/tokens.h"

namespace vcc {
namespace {

const char* const kHeaderNames[] = {
    "Session Goal", "Files And Changes", "Commits", "Outstanding Context", "User Preferences",
};

const std::string& separator() {
    static const std::string sep = "\n\n---\n\n";
    return sep;
}

std::string trim(std::string s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string trimEnd(std::string s) {
    size_t e = s.find_last_not_of(" \t\r\n");
    return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

size_t lineCount(const std::string& text) {
    if (text.empty())
        return 0;
    size_t n = 1;
    for (char c : text)
        if (c == '\n')
            ++n;
    return n;
}

std::vector<std::string> splitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        lines.push_back(text.substr(start, (nl == std::string::npos ? text.size() : nl) - start));
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0)
            out += '\n';
        out += lines[i];
    }
    return out;
}

std::string capItems(const std::vector<std::string>& items, size_t limit, const std::string& joinWith,
                     bool keepTail) {
    std::string out;
    if (keepTail) {
        out = "(+" + std::to_string(items.size() - limit) + " earlier) ";
        for (size_t i = items.size() - limit; i < items.size(); ++i) {
            if (i > items.size() - limit)
                out += joinWith;
            out += items[i];
        }
        return out;
    }
    for (size_t i = 0; i < limit; ++i) {
        if (i > 0)
            out += joinWith;
        out += items[i];
    }
    return out + " (+" + std::to_string(items.size() - limit) + " more)";
}

std::string capItemsJoined(const std::vector<std::string>& items, size_t limit, const std::string& joinWith,
                           bool keepTail) {
    if (items.size() <= limit) {
        std::string out;
        for (size_t i = 0; i < items.size(); ++i) {
            if (i > 0)
                out += joinWith;
            out += items[i];
        }
        return out;
    }
    return capItems(items, limit, joinWith, keepTail);
}

std::string stripCapMarker(std::string text) {
    static const std::regex head(R"(^\s*\(\+\d+ earlier\)\s*)");
    static const std::regex tail(R"(\s*\(\+\d+ (?:more|earlier)\)\s*$)");
    text = std::regex_replace(text, head, "");
    text = std::regex_replace(text, tail, "");
    return text;
}

std::string sectionOf(const std::string& text, const std::string& header) {
    const std::string tag = "[" + header + "]";
    size_t start = text.find(tag);
    if (start == std::string::npos)
        return {};
    std::string after = text.substr(start);
    size_t end = std::string::npos;
    for (const char* h : kHeaderNames) {
        if (h == header)
            continue;
        size_t pos = after.find("[" + std::string(h) + "]");
        if (pos > 0 && (end == std::string::npos || pos < end))
            end = pos;
    }
    size_t sep = after.find(separator());
    if (sep > 0 && (end == std::string::npos || sep < end))
        end = sep;
    if (end == std::string::npos)
        return trim(after);
    return trim(after.substr(0, end));
}

std::string briefOf(const std::string& text) {
    size_t idx = text.find(separator());
    if (idx == std::string::npos)
        return {};
    return trim(text.substr(idx + separator().size()));
}

std::vector<std::string> splitCategorized(const std::string& line, char on) {
    std::vector<std::string> out;
    std::string current;
    for (char c : line) {
        if (c == on) {
            std::string t = trim(current);
            if (!t.empty())
                out.push_back(t);
            current.clear();
        } else {
            current.push_back(c);
        }
    }
    std::string t = trim(current);
    if (!t.empty())
        out.push_back(t);
    return out;
}

std::map<std::string, std::vector<std::string>> mergeCategorized(const std::vector<std::string>& categories,
                                                                 const std::string& prev, const std::string& fresh,
                                                                 bool touchOnDup) {
    std::map<std::string, std::vector<std::string>> merged;
    for (const auto& cat : categories)
        merged[cat] = {};

    auto absorb = [&](const std::string& text) {
        for (const std::string& line : splitLines(text)) {
            for (const auto& cat : categories) {
                const std::string prefix = "- " + cat + ": ";
                if (line.rfind(prefix, 0) != 0)
                    continue;
                std::string rest = stripCapMarker(line.substr(prefix.size()));
                for (const std::string& item : splitCategorized(rest, ',')) {
                    auto& v = merged[cat];
                    auto it = std::find(v.begin(), v.end(), item);
                    if (it != v.end()) {
                        if (!touchOnDup)
                            continue;
                        v.erase(it);
                    }
                    v.push_back(item);
                }
            }
        }
    };
    absorb(prev);
    absorb(fresh);
    return merged;
}

std::string formatCategorized(const std::string& header, const std::map<std::string, std::vector<std::string>>& merged,
                              const std::vector<std::string>& categories, const std::string& joinWith,
                              size_t itemLimit, bool keepTail) {
    std::vector<std::string> lines;
    for (const auto& cat : categories) {
        auto it = merged.find(cat);
        if (it == merged.end() || it->second.empty())
            continue;
        lines.push_back("- " + cat + ": " + capItemsJoined(it->second, itemLimit, joinWith, keepTail));
    }
    if (lines.empty())
        return {};
    return "[" + header + "]\n" + joinLines(lines);
}

std::vector<std::string> discoverCategoryNames(const std::string& text) {
    std::vector<std::string> names;
    for (const std::string& line : splitLines(text)) {
        if (line.rfind("- ", 0) != 0)
            continue;
        size_t colon = line.find(": ", 2);
        if (colon == std::string::npos)
            continue;
        std::string name = line.substr(2, colon - 2);
        if (std::find(names.begin(), names.end(), name) == names.end())
            names.push_back(name);
    }
    return names;
}

std::string mergeHeaderSection(const std::string& header, const std::string& prev, const std::string& fresh) {
    if (header == "Outstanding Context")
        return fresh;
    if (prev.empty())
        return fresh;
    if (fresh.empty())
        return prev;

    if (header == "Files And Changes") {
        std::vector<std::string> cats = {"Modified", "Created", "Read"};
        auto merged = mergeCategorized(cats, prev, fresh, false);
        for (const auto& p : merged["Modified"]) {
            auto& created = merged["Created"];
            created.erase(std::remove(created.begin(), created.end(), p), created.end());
        }
        return formatCategorized(header, merged, cats, ", ", kFilesPerCategory, false);
    }

    auto isClean = [](const std::string& l) {
        return l.rfind("- ", 0) == 0 && l.find("<skill") == std::string::npos &&
               l.find("</skill") == std::string::npos;
    };
    std::vector<std::string> combined;
    auto pushUnique = [&](const std::string& l) {
        if (std::find(combined.begin(), combined.end(), l) == combined.end())
            combined.push_back(l);
    };
    for (const std::string& l : splitLines(prev))
        if (isClean(l))
            pushUnique(l);
    for (const std::string& l : splitLines(fresh))
        if (isClean(l))
            pushUnique(l);
    size_t cap = header == "Session Goal" || header == "Commits" ? kGoalCap : kMergedPreferenceCap;
    std::vector<std::string> capped;
    if (combined.size() > cap)
        capped.assign(combined.end() - cap, combined.end());
    else
        capped = combined;
    if (capped.empty())
        return {};
    return "[" + header + "]\n" + joinLines(capped);
}

std::string capBriefToLineBudget(const std::string& text, size_t maxLines) {
    if (text.empty() || maxLines == 0)
        return {};
    std::vector<std::string> lines = splitLines(text);
    if (lines.size() <= maxLines)
        return text;
    std::vector<std::string> kept(lines.end() - maxLines, lines.end());
    size_t firstHeader = 0;
    bool found = false;
    for (size_t i = 0; i < kept.size(); ++i) {
        if (!kept[i].empty() && kept[i][0] == '[' && kept[i].find(']') != std::string::npos) {
            firstHeader = i;
            found = true;
            break;
        }
    }
    std::vector<std::string> clean = found && firstHeader > 0
                                         ? std::vector<std::string>(kept.begin() + firstHeader, kept.end())
                                         : kept;
    size_t omitted = lines.size() - clean.size();
    return "...(" + std::to_string(omitted) + " earlier lines omitted)\n\n" + joinLines(clean);
}

std::string capBrief(const std::string& text) {
    if (lineCount(text) <= kBriefMaxLines)
        return text;
    return capBriefToLineBudget(text, kBriefMaxLines);
}

std::string mergeBriefTranscript(const std::string& prev, const std::string& fresh) {
    if (prev.empty())
        return fresh;
    if (fresh.empty())
        return prev;
    return prev + "\n\n" + fresh;
}

std::string mergeBriefWithFreshBudget(const std::string& prev, const std::string& fresh) {
    if (prev.empty())
        return fresh;
    if (fresh.empty())
        return capBrief(prev);
    size_t freshLines = lineCount(fresh);
    size_t remaining = kBriefMaxLines > freshLines ? kBriefMaxLines - freshLines : 0;
    std::string prevTail = capBriefToLineBudget(prev, remaining);
    return prevTail.empty() ? fresh : prevTail + "\n\n" + fresh;
}

std::string rejoinWrappedItems(const std::string& text) {
    static const std::regex cont(R"(\n[ \t]+(?=\S))");
    return std::regex_replace(text, cont, " ");
}

std::string mergePrevious(const std::string& prev, const std::string& fresh, bool preserveFreshBrief) {
    std::vector<std::string> headers;
    for (const char* h : kHeaderNames) {
        std::string merged = mergeHeaderSection(h, rejoinWrappedItems(sectionOf(prev, h)), sectionOf(fresh, h));
        if (!merged.empty())
            headers.push_back(merged);
    }

    std::string mergedBrief = preserveFreshBrief
                                  ? mergeBriefWithFreshBudget(briefOf(prev), briefOf(fresh))
                                  : capBrief(mergeBriefTranscript(briefOf(prev), briefOf(fresh)));

    std::vector<std::string> parts;
    if (!headers.empty())
        parts.push_back(joinLines(headers));
    if (!mergedBrief.empty())
        parts.push_back(mergedBrief);

    if (parts.empty())
        return {};
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            out += separator();
        out += parts[i];
    }
    return out;
}

std::string formatSection(const std::string& title, const std::vector<std::string>& items) {
    if (items.empty())
        return {};
    std::string body;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0)
            body += '\n';
        body += "- " + items[i];
    }
    return "[" + title + "]\n" + body;
}

std::string formatSummary(const SectionData& data, bool capBriefTranscript) {
    std::vector<std::string> headerParts;
    for (const auto& [title, items] : std::initializer_list<std::pair<const char* const, const std::vector<std::string>*>>{
             {"Session Goal", &data.sessionGoal},
             {"Files And Changes", &data.filesAndChanges},
             {"Commits", &data.commits},
             {"Outstanding Context", &data.outstandingContext},
             {"User Preferences", &data.userPreferences},
         }) {
        std::string s = formatSection(title, *items);
        if (!s.empty())
            headerParts.push_back(s);
    }

    std::vector<std::string> parts;
    if (!headerParts.empty()) {
        std::string headers;
        for (size_t i = 0; i < headerParts.size(); ++i) {
            if (i > 0)
                headers += "\n\n";
            headers += headerParts[i];
        }
        parts.push_back(headers);
    }
    if (!data.briefTranscript.empty())
        parts.push_back(capBriefTranscript ? capBrief(data.briefTranscript) : data.briefTranscript);

    if (parts.empty())
        return {};

    std::string joined;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0)
            joined += separator();
        joined += parts[i];
    }
    return wrapLongLines(joined);
}

std::string escapeRegexWord(const std::string& w) {
    static const std::regex special(R"([.*+?^${}()|[\]\\])");
    return std::regex_replace(w, special, "\\$&");
}

const std::regex& recallNoteRe() {
    static const std::regex re = [] {
        std::vector<std::string> words;
        {
            std::string note = kRecallNote;
            size_t start = 0;
            while (start <= note.size()) {
                size_t sp = note.find(' ', start);
                std::string word = note.substr(start, (sp == std::string::npos ? note.size() : sp) - start);
                if (!word.empty())
                    words.push_back(word);
                if (sp == std::string::npos)
                    break;
                start = sp + 1;
            }
        }
        std::string pattern = "\\n*(?:---\\n+)?";
        for (size_t i = 0; i < words.size(); ++i) {
            if (i > 0)
                pattern += "\\s+";
            pattern += escapeRegexWord(words[i]);
        }
        pattern += "\\n*";
        return std::regex(pattern, std::regex::ECMAScript);
    }();
    return re;
}

std::string stripRecallNote(const std::string& text) {
    std::string out = std::regex_replace(text, recallNoteRe(), "\n\n");
    return trimEnd(out);
}

}  // namespace

std::string compile(const CompileInput& input) {
    std::vector<Block> blocks = filterNoise(normalize(input.messages, input.sourceIndices));
    SectionData data = buildSections(blocks, blocks, input.fileOps);
    std::string fresh = formatSummary(data, true);
    std::string prev = input.previousSummary.empty() ? std::string() : stripRecallNote(input.previousSummary);
    std::string merged = prev.empty() ? fresh : mergePrevious(prev, fresh, false);
    if (merged.empty())
        return {};
    return wrapLongLines(merged + separator() + kRecallNote);
}

std::string compileRanked(const CompileInput& input) {
    std::vector<Block> blocks = filterNoise(normalize(input.messages, input.sourceIndices));

    size_t floorChars = static_cast<size_t>(input.briefFloorTokens * input.charsPerToken + 0.5);
    size_t ceilingChars = static_cast<size_t>(input.briefCeilingTokens * input.charsPerToken + 0.5);
    size_t slope = static_cast<size_t>(input.briefTokensPerBlock * input.charsPerToken + 0.5);
    size_t scaled = slope * blocks.size();
    size_t budget = std::min(ceilingChars, std::max(floorChars, scaled));

    RankOptions rankOpts;
    rankOpts.maxBriefChars = budget;
    rankOpts.fileOps = input.fileOps;
    std::vector<Block> briefBlocks = selectBriefBlocks(blocks, rankOpts);

    SectionData data = buildSections(blocks, briefBlocks, input.fileOps);
    std::string fresh = formatSummary(data, false);
    std::string prev = input.previousSummary.empty() ? std::string() : stripRecallNote(input.previousSummary);
    std::string merged = prev.empty() ? fresh : mergePrevious(prev, fresh, true);
    if (merged.empty())
        return {};
    return wrapLongLines(merged + separator() + kRecallNote);
}

}  // namespace vcc
