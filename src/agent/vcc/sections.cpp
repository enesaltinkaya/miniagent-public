#include "agent/vcc/sections.h"

#include <algorithm>
#include <cctype>
#include <regex>

#include "agent/vcc/brief.h"
#include "agent/vcc/constants.h"
#include "agent/vcc/text.h"

namespace vcc {
namespace {

std::regex icase(const char* pattern) {
    return std::regex(pattern, std::regex::ECMAScript | std::regex::icase);
}

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

std::string extractPathFromArgs(const nlohmann::json& args) {
    if (!args.is_object())
        return {};
    for (const char* key : {"path", "file_path", "filePath", "file"}) {
        if (args.contains(key) && args[key].is_string())
            return args[key].get<std::string>();
    }
    return {};
}

const std::set<std::string>& fileReadTools() {
    static const std::set<std::string> tools = {"read", "read_file", "view"};
    return tools;
}

const std::set<std::string>& fileWriteTools() {
    static const std::set<std::string> tools = {
        "edit", "write", "edit_file", "write_file", "multiedit", "quick_edit", "target_edit", "apply_patch",
    };
    return tools;
}

const std::set<std::string>& fileCreateTools() {
    static const std::set<std::string> tools = {"write", "write_file"};
    return tools;
}

const std::regex& scopeChangeRe() {
    static const std::regex re =
        icase(R"(\b(instead|actually|change of plan|forget that|new task|switch to|now I want|pivot|let'?s do|stop .* and)\b)");
    return re;
}

const std::regex& taskRe() {
    static const std::regex re =
        icase(R"(\b(fix|implement|add|create|build|refactor|debug|investigate|update|remove|delete|migrate|deploy|test|write|set up)\b)");
    return re;
}

const std::regex& noiseShortRe() {
    static const std::regex re = icase(R"(^(ok|yes|no|sure|yeah|yep|go|hi|hey|thx|thanks|ok\b.*|y|n|k)\s*[.!?]*$)");
    return re;
}

const std::regex& nonGoalRe() {
    static const std::regex re = icase(
        R"RE(^\s*[\[│├└─╭╰]|```|^\s*(=[A-Z]+\(|function |const |let |var |import |export |class )|^(https?:|file:|\/[A-Za-z])|\\n|^\s*For each\b|\bin full\b[^\n]*\b(comments|issue|issues|PRs?|linked)\b)RE");
    return re;
}

const std::regex& templateSignalRe() {
    static const std::regex re = icase(R"(^\s*(For each\b|Do NOT implement\b|Analyze and propose\b|If Task\/context\b|Output:\s*$))");
    return re;
}

std::string stripLeadingBullet(const std::string& line) {
    size_t b = 0;
    while (b < line.size() && std::isspace(static_cast<unsigned char>(line[b])))
        ++b;
    size_t after = b;
    if (after < line.size() && (line[after] == '-' || line[after] == '*' || line[after] == '+')) {
        ++after;
    } else if (after < line.size() && std::isdigit(static_cast<unsigned char>(line[after]))) {
        size_t j = after;
        while (j < line.size() && std::isdigit(static_cast<unsigned char>(line[j])))
            ++j;
        if (j < line.size() && line[j] == '.')
            after = j + 1;
    }
    if (after > b && after < line.size() && std::isspace(static_cast<unsigned char>(line[after])))
        ++after;
    return trim(line.substr(after));
}

const std::vector<std::regex>& prefPatternList() {
    static const std::vector<std::regex> patterns = {
        icase(R"(\bprefer(?:s|red|ring)?\s+\w)"),
        icase(R"(\bdon'?t want\b)"),
        icase(R"(\balways (?:use|do|run|prefer|keep|make|format|write|add|set|put|prefix|start|include|append)\b)"),
        icase(R"(\bnever (?:use|do|run|push|commit|write|ignore|add|set|put|remove|delete|include|deploy)\b)"),
        icase(R"(\bplease (?:use|avoid|keep|make|don'?t|do not|format|write)\b)"),
        icase(R"(\b(?:style|format|language|naming)\s*[:=]\s*\S)"),
    };
    return patterns;
}

const std::regex& blockerRe() {
    static const std::regex re = icase(
        R"(\b(fail(ed|s|ure|ing)?|broken|cannot|can't|won't work|does not work|doesn't work|still (broken|failing|wrong)|blocked|blocker|not (fixed|resolved|working)|crash(es|ed|ing)?)\b)");
    return re;
}

std::string longestCommonDirPrefix(const std::set<std::string>& all) {
    std::vector<std::string> abs;
    for (const auto& p : all)
        if (!p.empty() && p[0] == '/')
            abs.push_back(p);
    if (abs.size() < 2)
        return {};
    std::vector<std::vector<std::string>> split;
    for (const auto& p : abs) {
        std::vector<std::string> parts;
        size_t start = 1;
        while (start <= p.size()) {
            size_t slash = p.find('/', start);
            parts.push_back(p.substr(start, (slash == std::string::npos ? p.size() : slash) - start));
            if (slash == std::string::npos)
                break;
            start = slash + 1;
        }
        split.push_back(std::move(parts));
    }
    size_t minParts = SIZE_MAX;
    for (const auto& s : split)
        minParts = std::min(minParts, s.size());
    size_t i = 0;
    while (i + 1 < minParts) {
        const std::string& seg = split[0][i];
        bool same = true;
        for (const auto& s : split)
            if (s[i] != seg) {
                same = false;
                break;
            }
        if (!same)
            break;
        ++i;
    }
    if (i < 2)
        return {};
    std::string prefix;
    for (size_t j = 0; j < i; ++j)
        prefix += "/" + split[0][j];
    prefix += "/";
    return prefix;
}

std::set<std::string> trimPaths(const std::set<std::string>& set, const std::string& prefix) {
    std::set<std::string> out;
    for (const auto& p : set)
        out.insert(p.rfind(prefix, 0) == 0 ? p.substr(prefix.size()) : p);
    return out;
}

const std::regex& commitMsgRe() {
    static const std::regex re = std::regex(
        R"RE(git\s+commit[^\n]*?-m\s+(?:"((?:[^"\\]|\\.)*)"|'((?:[^'\\]|\\.)*)'|\$?'((?:[^'\\]|\\.)*)'))RE",
        std::regex::ECMAScript);
    return re;
}

const std::regex& heredocCommitRe() {
    static const std::regex re = std::regex(
        R"RE(^git\s+commit[^\n"'&;|]*?-m\s+"\$\(cat\s+<<-?\s*(?:'([^']+)'|"([^"]+)"|([A-Za-z_]\w*))(?:\r?\n|\\n))RE",
        std::regex::ECMAScript);
    return re;
}

std::string cleanCommitMessage(std::string msg) {
    std::string out;
    for (size_t i = 0; i < msg.size(); ++i) {
        if (msg[i] == '\\' && i + 1 < msg.size() && (msg[i + 1] == '"' || msg[i + 1] == '\'')) {
            out.push_back(msg[i + 1]);
            ++i;
            continue;
        }
        out.push_back(msg[i]);
    }
    return trim(out);
}

std::string firstLineOf(const std::string& text) {
    size_t nl = text.find('\n');
    std::string first = nl == std::string::npos ? text : text.substr(0, nl);
    size_t esc = first.find("\\n");
    if (esc != std::string::npos)
        first = first.substr(0, esc);
    return trim(first);
}

std::string subjectFromHeredocCmd(const std::string& cmd) {
    std::smatch m;
    if (!std::regex_search(cmd, m, heredocCommitRe()))
        return {};
    std::string delim = m[1].matched ? m[1].str() : m[2].matched ? m[2].str() : m[3].str();
    std::string rest = cmd.substr(m.position() + m.length());
    size_t pos = 0;
    while (true) {
        size_t nl = rest.find('\n', pos);
        std::string line = nl == std::string::npos ? rest.substr(pos) : rest.substr(pos, nl - pos);
        size_t esc = line.find("\\n");
        if (esc != std::string::npos)
            line = line.substr(0, esc);
        size_t t = 0;
        while (t < line.size() && line[t] == '\t')
            ++t;
        line = trim(line.substr(t));
        if (line == delim)
            return {};
        if (!line.empty())
            return cleanCommitMessage(line);
        if (nl == std::string::npos)
            return {};
        pos = nl + 1;
    }
}

std::string bashCommandFromBlock(const Block& b) {
    if (auto* sh = std::get_if<BashBlock>(&b))
        return sh->command;
    if (auto* c = std::get_if<CallBlock>(&b))
        if (toLower(c->name) == "bash" && c->args.is_object() && c->args.contains("command") &&
            c->args["command"].is_string())
            return c->args["command"].get<std::string>();
    return {};
}

}  // namespace

std::vector<std::string> extractGoals(const std::vector<Block>& blocks) {
    std::vector<std::string> goals;
    std::vector<std::string> latestScopeChange;

    for (const Block& b : blocks) {
        const auto* u = std::get_if<UserBlock>(&b);
        if (!u)
            continue;
        std::vector<std::string> rawLines = nonEmptyLines(u->text);
        size_t cut = rawLines.size();
        for (size_t i = 0; i < rawLines.size(); ++i) {
            if (std::regex_search(rawLines[i], templateSignalRe())) {
                cut = i;
                break;
            }
        }
        rawLines.resize(cut);
        std::vector<std::string> lines;
        for (const auto& l : rawLines) {
            std::string t = trim(l);
            if (t.size() <= 5 || t.size() > kMaxGoalChars)
                continue;
            if (std::regex_search(t, noiseShortRe()) || std::regex_search(t, nonGoalRe()))
                continue;
            std::string stripped = stripLeadingBullet(t);
            if (stripped.size() > 5)
                lines.push_back(stripped);
        }
        if (lines.empty())
            continue;

        if (goals.empty()) {
            size_t n = std::min(lines.size(), kGoalFirstLines);
            for (size_t i = 0; i < n; ++i)
                goals.push_back(lines[i]);
            continue;
        }

        std::string leading = u->text.substr(0, std::min(u->text.size(), size_t{200}));
        if (std::regex_search(leading, scopeChangeRe())) {
            latestScopeChange.clear();
            size_t n = std::min(lines.size(), kScopeChangeLines);
            for (size_t i = 0; i < n; ++i)
                latestScopeChange.push_back(clip(lines[i], kMaxGoalChars));
        } else if (std::regex_search(leading, taskRe()) && lines[0].size() > 15) {
            latestScopeChange.clear();
            size_t n = std::min(lines.size(), kScopeChangeTaskLines);
            for (size_t i = 0; i < n; ++i)
                latestScopeChange.push_back(clip(lines[i], kMaxGoalChars));
        }
    }

    if (!latestScopeChange.empty()) {
        goals.push_back("[Scope change]");
        for (const auto& l : latestScopeChange)
            goals.push_back(l);
    }

    if (goals.size() > kGoalCap)
        goals.resize(kGoalCap);
    return goals;
}

FileActivity extractFiles(const std::vector<Block>& blocks, const FileOps& fileOps) {
    FileActivity act;
    act.read.insert(fileOps.readFiles.begin(), fileOps.readFiles.end());
    act.modified.insert(fileOps.modifiedFiles.begin(), fileOps.modifiedFiles.end());
    act.created.insert(fileOps.createdFiles.begin(), fileOps.createdFiles.end());

    for (const Block& b : blocks) {
        const auto* c = std::get_if<CallBlock>(&b);
        if (!c)
            continue;
        std::string p = extractPathFromArgs(c->args);
        if (p.empty())
            continue;
        std::string lower = toLower(c->name);
        if (fileReadTools().count(lower))
            act.read.insert(p);
        if (fileWriteTools().count(lower))
            act.modified.insert(p);
        if (fileCreateTools().count(lower))
            act.created.insert(p);
    }

    std::set<std::string> all;
    for (const auto* group : {&act.read, &act.modified, &act.created})
        all.insert(group->begin(), group->end());
    std::string prefix = longestCommonDirPrefix(all);
    if (!prefix.empty()) {
        act.read = trimPaths(act.read, prefix);
        act.modified = trimPaths(act.modified, prefix);
        act.created = trimPaths(act.created, prefix);
    }
    return act;
}

std::vector<std::string> extractCommits(const std::vector<Block>& blocks) {
    std::vector<std::pair<std::string, std::string>> commits;

    for (size_t i = 0; i < blocks.size(); ++i) {
        const Block& b = blocks[i];
        if (kindOf(b) != Kind::Call && kindOf(b) != Kind::Bash)
            continue;
        std::string cmd = bashCommandFromBlock(b);
        if (cmd.empty() || cmd.find("git commit") == std::string::npos)
            continue;
        std::smatch m;
        if (!std::regex_search(cmd, m, commitMsgRe()))
            continue;
        std::string quoted = m[1].matched ? m[1].str() : m[2].matched ? m[2].str() : m[3].str();
        std::string message;
        if (m[1].matched && quoted.rfind("$(cat <<", 0) == 0) {
            message = subjectFromHeredocCmd(cmd.substr(m.position()));
        } else {
            message = firstLineOf(cleanCommitMessage(quoted));
        }
        if (message.empty() || message.rfind("$(cat <<", 0) == 0)
            continue;

        std::string hash;
        std::string resultText;
        for (size_t j = i + 1; j < std::min(blocks.size(), i + 3); ++j) {
            if (const auto* r = std::get_if<ResultBlock>(&blocks[j]))
                resultText = r->text;
            else if (const auto* sh = std::get_if<BashBlock>(&blocks[j]))
                resultText = sh->output;
            else
                continue;
            break;
        }
        if (!resultText.empty()) {
            static const std::regex bracket(R"(\[\S+\s+([0-9a-f]{7,12})\])");
            static const std::regex rangeRe(R"(\b([0-9a-f]{7,12})\.\.([0-9a-f]{7,12})\b)");
            static const std::regex plain(R"(\b([0-9a-f]{7,12})\b)");
            std::smatch hm;
            if (std::regex_search(resultText, hm, bracket))
                hash = hm[1].str();
            else if (std::regex_search(resultText, hm, rangeRe))
                hash = hm[2].str();
            else if (std::regex_search(resultText, hm, plain))
                hash = hm[1].str();
        }

        std::string key = hash + "::" + message;
        bool dup = false;
        for (const auto& c : commits)
            if (c.first + "::" + c.second == key) {
                dup = true;
                break;
            }
        if (!dup)
            commits.emplace_back(hash, message);
    }

    std::vector<std::string> out;
    for (const auto& c : commits)
        out.push_back(c.first.empty() ? c.second : c.first + ": " + c.second);
    return out;
}

std::vector<std::string> formatCommits(const std::vector<std::string>& commits, size_t limit) {
    std::vector<std::string> out;
    size_t start = commits.size() > limit ? commits.size() - limit : 0;
    for (size_t i = start; i < commits.size(); ++i)
        out.push_back(commits[i]);
    return out;
}

std::vector<std::string> extractPreferences(const std::vector<Block>& blocks) {
    std::vector<std::string> prefs;
    std::set<std::string> seen;

    for (const Block& b : blocks) {
        const auto* u = std::get_if<UserBlock>(&b);
        if (!u)
            continue;
        int perBlock = 0;
        for (const std::string& line : nonEmptyLines(u->text)) {
            std::string t = trim(line);
            if (t.size() < 5 || t.size() > 200)
                continue;
            if (t.back() == '?' || t.find("?...") != std::string::npos)
                continue;
            bool matched = false;
            for (const auto& p : prefPatternList())
                if (std::regex_search(t, p)) {
                    matched = true;
                    break;
                }
            if (!matched)
                continue;
            std::string clipped = clip(t, 200);
            std::string key = toLower(clipped);
            if (seen.count(key))
                continue;
            seen.insert(key);
            prefs.push_back(clipped);
            if (++perBlock >= 1)
                break;
        }
    }

    if (prefs.size() > kPreferenceCap)
        prefs.resize(kPreferenceCap);
    return prefs;
}

std::vector<std::string> dedupPreferencesAgainstGoals(const std::vector<std::string>& prefs,
                                                      const std::vector<std::string>& goals) {
    std::set<std::string> goalSet;
    for (const auto& g : goals)
        goalSet.insert(toLower(trim(g)));
    std::vector<std::string> out;
    for (const auto& p : prefs)
        if (!goalSet.count(toLower(trim(p))))
            out.push_back(p);
    return out;
}

std::vector<std::string> extractOutstandingContext(const std::vector<Block>& blocks) {
    std::vector<std::string> items;
    size_t start = blocks.size() > kOutstandingTailBlocks ? blocks.size() - kOutstandingTailBlocks : 0;
    static const std::regex contFragment(R"(^\s*[-*+>]\s)");
    static const std::regex parenStart(R"(^\s*\()");
    static const std::regex sentenceStart(R"(^\s*["'`*_]?[A-Z`])");

    for (size_t bi = start; bi < blocks.size(); ++bi) {
        Kind k = kindOf(blocks[bi]);
        if (k != Kind::Assistant && k != Kind::User)
            continue;
        const std::string& text = k == Kind::User ? std::get<UserBlock>(blocks[bi]).text
                                                  : std::get<AssistantBlock>(blocks[bi]).text;
        for (const std::string& line : nonEmptyLines(text)) {
            if (!std::regex_search(line, blockerRe()))
                continue;
            if (line.size() < 15)
                continue;
            if (std::regex_search(line, contFragment) || std::regex_search(line, parenStart))
                continue;
            if (!std::regex_search(line, sentenceStart))
                continue;
            std::string clipped = k == Kind::User ? "[user] " + clipSentence(line, 150) : clipSentence(line, 150);
            if (std::find(items.begin(), items.end(), clipped) == items.end())
                items.push_back(clipped);
            break;
        }
        if (items.size() >= kOutstandingCap)
            break;
    }
    return items;
}

std::vector<std::string> formatFileActivity(const FileActivity& act) {
    FileActivity a = act;
    for (const auto& p : a.modified)
        a.created.erase(p);

    std::vector<std::string> lines;
    auto cap = [](const std::set<std::string>& s, size_t limit) {
        std::vector<std::string> arr(s.begin(), s.end());
        if (arr.size() <= limit) {
            std::string out;
            for (size_t i = 0; i < arr.size(); ++i) {
                if (i > 0)
                    out += ", ";
                out += arr[i];
            }
            return out;
        }
        std::string out;
        for (size_t i = 0; i < limit; ++i) {
            if (i > 0)
                out += ", ";
            out += arr[i];
        }
        return out + " (+" + std::to_string(arr.size() - limit) + " more)";
    };
    if (!a.modified.empty())
        lines.push_back("Modified: " + cap(a.modified, kFilesPerCategory));
    if (!a.created.empty())
        lines.push_back("Created: " + cap(a.created, kFilesPerCategory));
    if (!a.read.empty())
        lines.push_back("Read: " + cap(a.read, kFilesPerCategory));
    return lines;
}

SectionData buildSections(const std::vector<Block>& blocks, const std::vector<Block>& briefBlocks,
                          const FileOps& fileOps) {
    SectionData data;
    data.briefTranscript = compileBrief(briefBlocks);
    data.sessionGoal = extractGoals(blocks);
    data.userPreferences = dedupPreferencesAgainstGoals(extractPreferences(blocks), data.sessionGoal);
    data.outstandingContext = extractOutstandingContext(blocks);
    data.filesAndChanges = formatFileActivity(extractFiles(blocks, fileOps));
    data.commits = formatCommits(extractCommits(blocks));
    return data;
}

}  // namespace vcc
