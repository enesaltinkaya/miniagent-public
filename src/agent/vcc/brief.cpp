#include "agent/vcc/brief.h"

#include <cctype>

#include "agent/vcc/constants.h"
#include "agent/vcc/text.h"

namespace vcc {
namespace {

std::string toLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trim(std::string s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string extractPath(const nlohmann::json& args) {
    if (!args.is_object())
        return {};
    for (const char* key : {"path", "file_path", "filePath", "file"}) {
        if (args.contains(key) && args[key].is_string())
            return args[key].get<std::string>();
    }
    return {};
}

bool isHeredocOpen(const std::string& line, std::string& term) {
    size_t pos = 0;
    while ((pos = line.find("<<", pos)) != std::string::npos) {
        if (pos > 0) {
            unsigned char prev = static_cast<unsigned char>(line[pos - 1]);
            if (std::isalnum(prev) || prev == ')' || prev == '_') {
                pos += 1;
                continue;
            }
        }
        size_t j = pos + 2;
        if (j < line.size() && line[j] == '-')
            ++j;
        while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j])))
            ++j;
        bool quoted = false;
        if (j < line.size() && (line[j] == '"' || line[j] == '\'')) {
            quoted = true;
            ++j;
        }
        if (j >= line.size() ||
            !(std::isalpha(static_cast<unsigned char>(line[j])) || line[j] == '_')) {
            pos += 1;
            continue;
        }
        size_t start = j;
        while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '_'))
            ++j;
        term = line.substr(start, j - start);
        return true;
    }
    return false;
}

bool isFilewriterHeredoc(const std::string& line) {
    size_t pos = 0;
    while (true) {
        size_t start = line.find_first_not_of(" \t", pos);
        if (start == std::string::npos)
            return false;
        size_t end = start;
        while (end < line.size() && !std::isspace(static_cast<unsigned char>(line[end])) &&
               line[end] != '|' && line[end] != '&' && line[end] != ';')
            ++end;
        std::string word = toLower(line.substr(start, end - start));
        if (word == "cat" || word == "tee" || word == "dd")
            return true;
        if (end >= line.size() || (line[end] != '|' && line[end] != '&' && line[end] != ';'))
            return false;
        pos = end + 1;
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos])))
            ++pos;
    }
}

bool isBodyNoise(const std::string& t) {
    static const std::vector<std::string> prefixes = {
        "import ", "from ", "require(", "const ", "#", "//", "\"\"\"", "'''",
        "\"use strict\"", "use strict", "<?php",
    };
    for (const auto& p : prefixes)
        if (t.rfind(p, 0) == 0)
            return true;
    return false;
}

std::string stripCdPrefix(const std::string& line) {
    if (line.rfind("cd ", 0) != 0)
        return trim(line);
    size_t sp = line.find(' ', 3);
    if (sp == std::string::npos)
        return trim(line);
    std::string rest = line.substr(sp + 1);
    size_t amp = rest.find("&&");
    if (amp == std::string::npos)
        return trim(line);
    return trim(rest.substr(amp + 2));
}

bool isPipeTailStart(const std::string& line, size_t pos) {
    if (pos == std::string::npos || pos + 1 >= line.size() || line[pos] != '|')
        return false;
    size_t j = pos + 1;
    while (j < line.size() && std::isspace(static_cast<unsigned char>(line[j])))
        ++j;
    static const std::vector<std::string> tails = {
        "head", "tail", "sort", "wc", "column", "tr", "cut", "awk", "uniq", "python3", "node", "bun",
    };
    for (const auto& t : tails) {
        if (line.compare(j, t.size(), t) == 0) {
            size_t after = j + t.size();
            if (after >= line.size() || !std::isalnum(static_cast<unsigned char>(line[after])))
                return true;
        }
    }
    return false;
}

std::string stripPipeTail(const std::string& line) {
    std::string c = trim(line);
    for (int round = 0; round < 3; ++round) {
        size_t pos = c.rfind('|');
        if (!isPipeTailStart(c, pos))
            return c;
        c = trim(c.substr(0, pos));
    }
    return c;
}

bool isTrivialLine(const std::string& l) {
    if (l.empty())
        return false;
    if (l.rfind("set ", 0) == 0 && l.size() > 4 && (l[4] == '-' || l[4] == '+'))
        return true;
    if (l.rfind("cd ", 0) == 0) {
        std::string rest = l.substr(3);
        if (rest.find(' ') == std::string::npos && rest.find("&&") == std::string::npos)
            return true;
    }
    if (l.rfind("export ", 0) == 0 && l.find('=') != std::string::npos) {
        std::string name = l.substr(7, l.find('=') - 7);
        bool ok = !name.empty();
        for (char c : name)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
                ok = false;
        if (ok)
            return true;
    }
    if (l.rfind("source ", 0) == 0 || (l.size() > 2 && l[0] == '.' && l[1] == ' '))
        return true;
    if (l == "pwd" || l == "true" || l == ":" || l[0] == '#')
        return true;
    return false;
}

bool isSelfTalkPrefix(const std::string& text) {
    static const std::vector<std::string> words = {"hmm", "wait", "actually", "oh", "okay", "ok", "well", "so"};
    size_t start = 0;
    while (start < text.size() && std::isspace(static_cast<unsigned char>(text[start])))
        ++start;
    for (const auto& w : words) {
        if (text.compare(start, w.size(), w) != 0)
            continue;
        size_t after = start + w.size();
        if (after >= text.size())
            return false;
        char c = text[after];
        if (c == ',' || c == '.' || c == '!' || c == ' ' || c == '\t' || c == '\n' || c == '-')
            return true;
    }
    return false;
}

std::string stripSelfTalk(std::string raw) {
    for (int i = 0; i < 2; ++i) {
        size_t start = 0;
        while (start < raw.size() && std::isspace(static_cast<unsigned char>(raw[start])))
            ++start;
        if (!isSelfTalkPrefix(raw.substr(start)))
            break;
        size_t after = start;
        while (after < raw.size() && (std::isspace(static_cast<unsigned char>(raw[after])) ||
                                      raw[after] == ',' || raw[after] == '.' || raw[after] == '!' ||
                                      raw[after] == '-'))
            ++after;
        raw = raw.substr(0, start) + raw.substr(after);
    }
    return raw;
}

const Block* nextRenderable(const std::vector<Block>& blocks, size_t index) {
    for (size_t i = index + 1; i < blocks.size(); ++i)
        if (kindOf(blocks[i]) != Kind::Result)
            return &blocks[i];
    return nullptr;
}

bool isSegmentClosingAssistant(const std::vector<Block>& blocks, size_t index) {
    if (kindOf(blocks[index]) != Kind::Assistant)
        return false;
    const Block* next = nextRenderable(blocks, index);
    return !next || kindOf(*next) == Kind::User;
}

bool parseTrailingRef(const std::string& line, std::string& base, std::string& ref) {
    static const std::string needle = " (#";
    size_t pos = line.rfind(needle);
    if (pos == std::string::npos)
        return false;
    size_t close = line.find(')', pos + needle.size());
    if (close != line.size() - 1)
        return false;
    std::string inner = line.substr(pos + needle.size(), close - (pos + needle.size()));
    if (inner.empty() || inner.find_first_not_of("#0123456789, ") != std::string::npos)
        return false;
    base = trim(line.substr(0, pos));
    ref = inner;
    return true;
}

}  // namespace

int heredocCloseIndex(const std::vector<std::string>& lines, size_t i) {
    std::string term;
    if (!isHeredocOpen(lines[i], term))
        return -1;
    for (size_t j = i + 1; j < lines.size(); ++j)
        if (trim(lines[j]) == term)
            return static_cast<int>(j);
    return -1;
}

std::string compressBash(const std::string& raw) {
    std::vector<std::string> rawLines;
    {
        size_t start = 0;
        while (start <= raw.size()) {
            size_t nl = raw.find('\n', start);
            rawLines.push_back(raw.substr(start, (nl == std::string::npos ? raw.size() : nl) - start));
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
    }

    std::vector<std::string> withoutBodies;
    for (size_t i = 0; i < rawLines.size(); ++i) {
        withoutBodies.push_back(rawLines[i]);
        int close = heredocCloseIndex(rawLines, i);
        if (close == -1)
            continue;
        bool wantPreview = !isFilewriterHeredoc(rawLines[i]);
        std::string preview;
        for (size_t j = i + 1; wantPreview && preview.empty() && j < static_cast<size_t>(close); ++j) {
            std::string t = trim(rawLines[j]);
            if (!t.empty() && !isBodyNoise(t))
                preview = t;
        }
        if (!preview.empty()) {
            std::string clipped = preview.size() > kHeredocBodyCap
                                      ? preview.substr(0, kHeredocBodyCap - 1) + "\xe2\x80\xa6"
                                      : preview;
            withoutBodies.back() = trim(rawLines[i]) + " " + clipped;
        }
        i = static_cast<size_t>(close);
    }

    std::vector<std::string> lines;
    for (auto& l : withoutBodies) {
        std::string t = trim(l);
        if (!t.empty())
            lines.push_back(std::move(t));
    }
    if (lines.empty())
        return trim(raw);

    std::vector<std::string> meaningful;
    for (auto& l : lines)
        if (!isTrivialLine(l))
            meaningful.push_back(stripPipeTail(stripCdPrefix(l)));
    std::vector<std::string> chosen;
    for (auto& m : meaningful)
        if (!m.empty())
            chosen.push_back(std::move(m));
    if (chosen.empty()) {
        std::string fallback = stripPipeTail(stripCdPrefix(lines[0]));
        if (!fallback.empty())
            chosen.push_back(std::move(fallback));
    }
    if (chosen.empty())
        return trim(raw);

    std::string cmd;
    for (size_t i = 0; i < chosen.size(); ++i) {
        if (i > 0)
            cmd += "; ";
        cmd += chosen[i];
    }
    if (cmd.size() > kBashCapChars)
        return cmd.substr(0, kBashCapChars - 3) + "...";
    return cmd;
}

std::string toolOneLiner(const std::string& name, const nlohmann::json& args) {
    std::string path = extractPath(args);
    if (!path.empty())
        return "* " + name + " \"" + path + "\"";
    if (toLower(name) == "bash") {
        std::string raw = args.value("command", args.value("description", std::string()));
        return "* " + name + " \"" + compressBash(raw) + "\"";
    }
    if (args.is_object() && args.contains("query") && args["query"].is_string())
        return "* " + name + " \"" + clip(args["query"].get<std::string>(), 60) + "\"";
    if (args.is_object() && args.contains("url") && args["url"].is_string())
        return "* " + name + " \"" + clip(args["url"].get<std::string>(), 60) + "\"";
    return "* " + name;
}

std::vector<BriefSection> buildBriefSections(const std::vector<Block>& blocks) {
    std::vector<BriefSection> sections;
    std::string lastHeader;

    auto push = [&](const std::string& header, const std::string& line) {
        if (header == lastHeader && !sections.empty()) {
            sections.back().lines.push_back(line);
            return;
        }
        sections.push_back({header, {line}});
        lastHeader = header;
    };
    auto pushText = [&](const std::string& header, const std::string& text, const std::string& ref) {
        std::vector<std::string> lines;
        {
            size_t start = 0;
            while (start <= text.size()) {
                size_t nl = text.find('\n', start);
                lines.push_back(text.substr(start, (nl == std::string::npos ? text.size() : nl) - start));
                if (nl == std::string::npos)
                    break;
                start = nl + 1;
            }
        }
        if (!ref.empty() && !lines.empty())
            lines.back() += ref;
        for (auto& l : lines)
            push(header, l);
    };
    auto refOf = [](const std::optional<int>& src) {
        return src ? " (#" + std::to_string(*src) + ")" : std::string();
    };

    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        switch (kindOf(b)) {
            case Kind::User: {
                const auto& u = std::get<UserBlock>(b);
                if (!trim(u.text).empty()) {
                    std::string text = truncateWords(u.text, kTruncateUserWords);
                    if (!trim(text).empty())
                        pushText("[user]", text, refOf(u.src));
                }
                lastHeader = "[user]";
                break;
            }
            case Kind::Bash: {
                const auto& sh = std::get<BashBlock>(b);
                std::string cmd = compressBash(sh.command);
                if (!cmd.empty())
                    push("[user]", "$ " + cmd + refOf(sh.src));
                lastHeader = "[user]";
                break;
            }
            case Kind::Assistant: {
                const auto& a = std::get<AssistantBlock>(b);
                std::string raw = stripSelfTalk(a.text);
                std::string text = isSegmentClosingAssistant(blocks, i)
                                       ? truncateHeadTail(raw, kSegmentClosingHeadWords, kSegmentClosingTailWords)
                                       : truncateHeadTail(raw, kAssistantHeadWords, kAssistantTailWords);
                if (!trim(text).empty())
                    pushText("[assistant]", text, refOf(a.src));
                break;
            }
            case Kind::Call: {
                const auto& c = std::get<CallBlock>(b);
                if (trim(c.name).empty())
                    break;
                if (toLower(c.name) == "bash" && c.args.is_object() && c.args.contains("command") &&
                    c.args["command"].is_string())
                    break;
                push("[assistant]", toolOneLiner(c.name, c.args) + refOf(c.src));
                break;
            }
            case Kind::Result:
                break;
        }
    }

    for (auto& sec : sections) {
        if (sec.header != "[assistant]")
            continue;
        std::vector<std::string> out;
        for (const std::string& line : sec.lines) {
            if (line.rfind("* ", 0) != 0) {
                out.push_back(line);
                continue;
            }
            std::string base, ref;
            if (!parseTrailingRef(line, base, ref)) {
                out.push_back(line);
                continue;
            }
            if (!out.empty()) {
                const std::string& last = out.back();
                size_t x = last.rfind(" x");
                size_t openParen = last.rfind(" (");
                if (x != std::string::npos && openParen != std::string::npos && x > openParen) {
                    size_t countStart = x + 2;
                    size_t countEnd = last.find(' ', countStart);
                    if (countEnd == std::string::npos) {
                        std::string countStr = last.substr(countStart);
                        bool digits = !countStr.empty();
                        for (char c : countStr)
                            if (!std::isdigit(static_cast<unsigned char>(c)))
                                digits = false;
                        if (digits && last.compare(0, openParen, base) == 0) {
                            std::string refs = last.substr(openParen + 2, x - openParen - 2);
                            long count = std::stol(countStr);
                            out.back() = base + " (#" + refs + ", #" + ref + ") x" + std::to_string(count + 1);
                            continue;
                        }
                    }
                }
                std::string prevBase, prevRef;
                if (parseTrailingRef(last, prevBase, prevRef) && prevBase == base &&
                    !prevRef.empty() && prevRef.find(',') == std::string::npos &&
                    prevRef.find_first_not_of("0123456789 ") == std::string::npos) {
                    out.back() = base + " (#" + prevRef + ", #" + ref + ") x2";
                    continue;
                }
            }
            out.push_back(line);
        }
        sec.lines = std::move(out);
    }

    for (auto& sec : sections) {
        if (sec.header != "[assistant]")
            continue;
        std::vector<size_t> toolIdxs;
        for (size_t i = 0; i < sec.lines.size(); ++i)
            if (sec.lines[i].rfind("* ", 0) == 0)
                toolIdxs.push_back(i);
        if (toolIdxs.size() <= kToolCallsPerTurn)
            continue;
        size_t dropCount = toolIdxs.size() - kToolCallsPerTurn;
        std::vector<bool> drop(sec.lines.size(), false);
        for (size_t i = 0; i < dropCount; ++i)
            drop[toolIdxs[i]] = true;
        size_t firstKept = toolIdxs[dropCount];
        std::vector<std::string> next;
        bool inserted = false;
        for (size_t i = 0; i < sec.lines.size(); ++i) {
            if (drop[i])
                continue;
            if (!inserted && i == firstKept) {
                next.push_back("* (" + std::to_string(dropCount) + " earlier tool-call entries omitted)");
                inserted = true;
            }
            next.push_back(sec.lines[i]);
        }
        sec.lines = std::move(next);
    }

    return sections;
}

std::string stringifyBrief(const std::vector<BriefSection>& sections) {
    std::vector<std::string> out;
    auto allTools = [](const BriefSection& s) {
        if (s.header != "[assistant]")
            return false;
        for (const auto& l : s.lines)
            if (l.rfind("* ", 0) != 0)
                return false;
        return !s.lines.empty();
    };
    for (size_t i = 0; i < sections.size(); ++i) {
        if (i > 0 && !(allTools(sections[i - 1]) && allTools(sections[i])))
            out.push_back("");
        out.push_back(sections[i].header);
        for (const auto& l : sections[i].lines)
            out.push_back(l);
    }
    std::string text;
    for (size_t i = 0; i < out.size(); ++i) {
        if (i > 0)
            text += '\n';
        text += out[i];
    }
    return text;
}

std::string compileBrief(const std::vector<Block>& blocks) {
    return stringifyBrief(buildBriefSections(blocks));
}

}  // namespace vcc
