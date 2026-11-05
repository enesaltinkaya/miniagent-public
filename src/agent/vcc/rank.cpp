#include "agent/vcc/rank.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "agent/vcc/brief.h"
#include "agent/vcc/constants.h"

namespace vcc {
namespace {

std::string toLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

const std::set<std::string>& editTools() {
    static const std::set<std::string> tools = {
        "edit", "write", "multiedit", "quick_edit", "target_edit", "apply_patch",
    };
    return tools;
}

const std::set<std::string>& readTools() {
    static const std::set<std::string> tools = {
        "read",        "glob",      "grep",     "ls",            "find",
        "semantic_query", "semantic_grep", "semantic_show", "web_search", "web_fetch",
    };
    return tools;
}

bool hasWordBoundaryBefore(const std::string& s, size_t pos) {
    return pos == 0 || !std::isalnum(static_cast<unsigned char>(s[pos - 1])) &&
                           s[pos - 1] != '_' && s[pos - 1] != '-' && s[pos - 1] != '.';
}

size_t findWord(const std::string& haystack, const std::string& needle, bool wordBoundary = true) {
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        size_t after = pos + needle.size();
        bool boundaryAfter = after >= haystack.size() ||
                             (!std::isalnum(static_cast<unsigned char>(haystack[after])) &&
                              haystack[after] != '_' && haystack[after] != '-');
        if (!wordBoundary || (hasWordBoundaryBefore(haystack, pos) && boundaryAfter))
            return pos;
        ++pos;
    }
    return std::string::npos;
}

const std::vector<std::string>& testRunnerWords() {
    static const std::vector<std::string> words = {
        "bun", "npm", "pnpm", "yarn", "node", "pytest", "cargo", "go", "mvn", "gradle",
    };
    return words;
}

const std::vector<std::string>& testTargetWords() {
    static const std::vector<std::string> words = {
        "test", "spec", "check", "lint", "build", "tsc",
    };
    return words;
}

const std::vector<std::string>& workflowGitVerbs() {
    static const std::vector<std::string> verbs = {
        "commit", "push", "merge", "rebase", "revert", "cherry-pick", "tag", "reset", "checkout", "branch",
    };
    return verbs;
}

const std::vector<std::string>& workflowGhSubjects() {
    static const std::vector<std::string> subjects = {"pr", "issue"};
    return subjects;
}

bool isTrivialBashLine(const std::string& l) {
    if (l.empty())
        return false;
    if (l.rfind("set ", 0) == 0 && l.size() > 4 && (l[4] == '-' || l[4] == '+'))
        return true;
    if (l.rfind("cd ", 0) == 0) {
        std::string rest = l.substr(3);
        if (rest.find(' ') == std::string::npos && rest.find("&&") == std::string::npos)
            return true;
    }
    if (l.rfind("export ", 0) == 0 && l.find('=') != std::string::npos)
        return true;
    if (l.rfind("source ", 0) == 0 || (l.size() > 2 && l[0] == '.' && l[1] == ' '))
        return true;
    if (l == "pwd" || l == "true" || l == ":" || l[0] == '#')
        return true;
    if (l == "ls" || l.rfind("ls ", 0) == 0)
        return true;
    if (l.rfind("echo ", 0) == 0 || l == "echo")
        return true;
    if (l == "clear" || l.rfind("sleep ", 0) == 0)
        return true;
    return false;
}

struct Ranked {
    int index;
    int score = 0;
};

std::string extractPathFromArgs(const nlohmann::json& args) {
    if (!args.is_object())
        return {};
    for (const char* key : {"path", "file_path", "filePath", "file"}) {
        if (args.contains(key) && args[key].is_string())
            return args[key].get<std::string>();
    }
    return {};
}

std::string extractPathFromBashCommand(const std::string& command) {
    for (size_t i = 0; i < command.size(); ++i) {
        auto wordChar = [](char c) {
            unsigned char u = static_cast<unsigned char>(c);
            return std::isalnum(u) || c == '.' || c == '/' || c == '_' || c == '-';
        };
        if (!wordChar(command[i]))
            continue;
        size_t start = i;
        while (i < command.size() && wordChar(command[i]))
            ++i;
        std::string word = command.substr(start, i - start);
        size_t dot = word.rfind('.');
        if (dot == std::string::npos || dot == 0 || dot == word.size() - 1)
            continue;
        bool extOk = true;
        for (size_t j = dot + 1; j < word.size(); ++j)
            if (!std::isalnum(static_cast<unsigned char>(word[j])) && word[j] != '-')
                extOk = false;
        if (extOk)
            return word;
    }
    return {};
}

}  // namespace

bool isTestCommand(const std::string& command) {
    const std::string lower = toLower(command);
    for (const auto& runner : testRunnerWords()) {
        size_t pos = findWord(lower, runner);
        if (pos == std::string::npos)
            continue;
        size_t lineEnd = lower.find('\n', pos);
        std::string rest = lineEnd == std::string::npos ? lower.substr(pos + runner.size())
                                                        : lower.substr(pos + runner.size(), lineEnd - pos - runner.size());
        for (const auto& target : testTargetWords())
            if (rest.find(target) != std::string::npos)
                return true;
    }
    return false;
}

namespace {

size_t skipSpaces(const std::string& s, size_t pos) {
    while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos])))
        ++pos;
    return pos;
}

bool isVerbToken(const std::string& s, size_t pos, size_t* endOut = nullptr) {
    size_t end = pos;
    while (end < s.size() &&
           (std::islower(static_cast<unsigned char>(s[end])) || s[end] == '-'))
        ++end;
    if (endOut)
        *endOut = end;
    return end > pos;
}

}  // namespace

bool isWorkflowCommand(const std::string& command) {
    const std::string lower = toLower(command);
    for (size_t gh = lower.find("gh"); gh != std::string::npos; gh = lower.find("gh", gh + 1)) {
        if (!hasWordBoundaryBefore(lower, gh))
            continue;
        size_t subject = skipSpaces(lower, gh + 2);
        for (const auto& s : workflowGhSubjects()) {
            if (lower.compare(subject, s.size(), s) != 0)
                continue;
            size_t after = subject + s.size();
            if (after >= lower.size() || !std::isspace(static_cast<unsigned char>(lower[after])))
                continue;
            if (isVerbToken(lower, skipSpaces(lower, after)))
                return true;
        }
    }
    for (size_t git = lower.find("git"); git != std::string::npos; git = lower.find("git", git + 1)) {
        if (!hasWordBoundaryBefore(lower, git))
            continue;
        size_t verb = skipSpaces(lower, git + 3);
        for (const auto& v : workflowGitVerbs()) {
            if (lower.compare(verb, v.size(), v) != 0)
                continue;
            size_t after = verb + v.size();
            if (after >= lower.size() ||
                (!std::isalnum(static_cast<unsigned char>(lower[after])) && lower[after] != '_'))
                return true;
        }
    }
    return false;
}

bool isTrivialOnlyBash(const std::string& raw) {
    std::vector<std::string> lines;
    {
        size_t start = 0;
        while (start <= raw.size()) {
            size_t nl = raw.find('\n', start);
            lines.push_back(raw.substr(start, (nl == std::string::npos ? raw.size() : nl) - start));
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
    }
    std::vector<std::string> kept;
    for (size_t i = 0; i < lines.size(); ++i) {
        kept.push_back(lines[i]);
        int close = heredocCloseIndex(lines, i);
        if (close != -1)
            i = static_cast<size_t>(close);
    }
    for (const std::string& l : kept) {
        std::string t = l;
        size_t b = 0, e = t.size();
        while (b < e && std::isspace(static_cast<unsigned char>(t[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(t[e - 1])))
            --e;
        if (e > b && !isTrivialBashLine(t.substr(b, e - b)))
            return false;
    }
    return true;
}

std::optional<std::string> bashCommandFromBlock(const Block& b) {
    if (auto* sh = std::get_if<BashBlock>(&b))
        return sh->command;
    if (auto* c = std::get_if<CallBlock>(&b)) {
        if (toLower(c->name) == "bash" && c->args.is_object() && c->args.contains("command") &&
            c->args["command"].is_string())
            return c->args["command"].get<std::string>();
    }
    return std::nullopt;
}

std::optional<std::string> pathFromBlock(const Block& b) {
    if (auto* c = std::get_if<CallBlock>(&b)) {
        std::string p = extractPathFromArgs(c->args);
        if (!p.empty())
            return p;
    }
    if (auto* sh = std::get_if<BashBlock>(&b)) {
        std::string p = extractPathFromBashCommand(sh->command);
        if (!p.empty())
            return p;
    }
    return std::nullopt;
}

namespace {

std::optional<std::string> dedupKey(const Block& b) {
    auto command = bashCommandFromBlock(b);
    if (command) {
        size_t pos = 0;
        const std::string lower = toLower(*command);
        while ((pos = lower.find("gh pr ", pos)) != std::string::npos) {
            if (!hasWordBoundaryBefore(lower, pos)) {
                ++pos;
                continue;
            }
            size_t rest = pos + 6;
            if ((lower.compare(rest, 5, "view ") == 0) || (lower.compare(rest, 7, "checks ") == 0)) {
                size_t numStart = rest + (lower.compare(rest, 5, "view ") == 0 ? 5 : 7);
                size_t numEnd = numStart;
                while (numEnd < lower.size() && std::isdigit(static_cast<unsigned char>(lower[numEnd])))
                    ++numEnd;
                if (numEnd > numStart)
                    return std::string("gh-pr-poll:") + lower.substr(numStart, numEnd - numStart);
            }
            ++pos;
        }
        std::string normalized;
        bool inSpace = true;
        for (char c : *command) {
            if (std::isspace(static_cast<unsigned char>(c))) {
                if (!inSpace)
                    normalized.push_back(' ');
                inSpace = true;
            } else {
                normalized.push_back(c);
                inSpace = false;
            }
        }
        while (!normalized.empty() && normalized.back() == ' ')
            normalized.pop_back();
        if (!normalized.empty())
            return "bash:" + normalized;
        return std::nullopt;
    }
    if (auto* c = std::get_if<CallBlock>(&b)) {
        auto path = pathFromBlock(b);
        if (path)
            return "tool:" + toLower(c->name) + ":" + *path;
    }
    return std::nullopt;
}

Ranked scoreBlock(const Block& b, size_t index, size_t total, const std::set<std::string>& modifiedFiles,
                  const std::set<std::string>& readFiles) {
    Ranked r{static_cast<int>(index)};
    auto add = [&](int points) { r.score += points; };

    if (total > 1) {
        double t = static_cast<double>(index) / static_cast<double>(total - 1);
        add(static_cast<int>(std::lround(t * kScoreRecencyMax)));
    }

    switch (kindOf(b)) {
        case Kind::User:
            add(kScoreUserTurn);
            break;
        case Kind::Assistant:
            add(kScoreAssistant);
            break;
        case Kind::Result:
            add(kScoreToolResult);
            break;
        case Kind::Call: {
            const auto& c = std::get<CallBlock>(b);
            std::string lower = toLower(c.name);
            if (editTools().count(lower))
                add(kScoreEditTool);
            else if (bashCommandFromBlock(b) && isTestCommand(*bashCommandFromBlock(b)))
                add(kScoreTestCommand);
            else if (readTools().count(lower))
                add(kScoreReadTool);
            else
                add(kScoreToolCall);
            auto command = bashCommandFromBlock(b);
            if (command && isWorkflowCommand(*command))
                add(kScoreWorkflowCommand);
            if (command && isTrivialOnlyBash(*command))
                add(kScoreTrivialBash);
            break;
        }
        case Kind::Bash: {
            const auto& sh = std::get<BashBlock>(b);
            add(kScoreBash);
            if (sh.hasExitCode && sh.exitCode != 0)
                add(kScoreNonzeroExit);
            if (isTestCommand(sh.command))
                add(kScoreTestCommand);
            if (isWorkflowCommand(sh.command))
                add(kScoreWorkflowCommand);
            if (isTrivialOnlyBash(sh.command) && !(sh.hasExitCode && sh.exitCode != 0))
                add(kScoreTrivialBash);
            break;
        }
    }

    auto path = pathFromBlock(b);
    if (path) {
        if (modifiedFiles.count(*path))
            add(kScoreHookModifiedFile);
        if (readFiles.count(*path))
            add(kScoreHookReadFile);
    }

    if (auto* r2 = std::get_if<ResultBlock>(&b))
        if (r2->text.size() > kLongToolResultChars)
            add(kScoreLongToolResult);
    return r;
}

bool isImportant(const Ranked& r, const std::vector<Block>& blocks) {
    if (r.score >= kImportantScoreThreshold)
        return true;
    const Block& b = blocks[r.index];
    if (auto* c = std::get_if<CallBlock>(&b))
        if (editTools().count(toLower(c->name)))
            return true;
    if (auto command = bashCommandFromBlock(b))
        if (isTestCommand(*command))
            return true;
    if (auto* sh = std::get_if<BashBlock>(&b))
        if (sh->hasExitCode && sh->exitCode != 0)
            return true;
    return false;
}

}  // namespace

std::vector<Block> selectBriefBlocks(const std::vector<Block>& blocks, const RankOptions& options) {
    const size_t maxBlocks = options.maxBlocks;
    const size_t preserveRecent = std::min(options.preserveRecentBlocks, maxBlocks);
    if (blocks.size() <= maxBlocks && !options.maxBriefChars)
        return blocks;

    std::set<std::string> modifiedFiles(options.fileOps.modifiedFiles.begin(), options.fileOps.modifiedFiles.end());
    std::set<std::string> readFiles(options.fileOps.readFiles.begin(), options.fileOps.readFiles.end());

    std::vector<Ranked> ranked;
    ranked.reserve(blocks.size());
    for (size_t i = 0; i < blocks.size(); ++i)
        ranked.push_back(scoreBlock(blocks[i], i, blocks.size(), modifiedFiles, readFiles));

    std::vector<bool> isImportantFlag(ranked.size(), false);
    for (auto& r : ranked)
        isImportantFlag[r.index] = isImportant(r, blocks);
    for (const auto& r : ranked) {
        if (!isImportantFlag[r.index])
            continue;
        for (int i = r.index - 1; i >= std::max(0, r.index - static_cast<int>(kAdjacencyUserBack)); --i) {
            if (kindOf(blocks[i]) == Kind::User) {
                ranked[i].score += kScoreNearImportantUser;
                break;
            }
        }
        for (int i = r.index - 1; i >= std::max(0, r.index - static_cast<int>(kAdjacencyAssistantBack)); --i) {
            if (kindOf(blocks[i]) == Kind::Assistant) {
                ranked[i].score += kScoreNearImportantAssistant;
                break;
            }
        }
        for (size_t i = r.index + 1; i <= std::min(blocks.size() - 1, r.index + kAdjacencyForward); ++i) {
            if (kindOf(blocks[i]) == Kind::Assistant || kindOf(blocks[i]) == Kind::Bash) {
                ranked[i].score += kScoreAfterImportant;
                break;
            }
        }
    }
    for (size_t i = 0; i < ranked.size(); ++i) {
        if (kindOf(blocks[i]) != Kind::Assistant)
            continue;
        const auto& a = std::get<AssistantBlock>(blocks[i]);
        std::string trimmed = a.text;
        size_t b = 0, e = trimmed.size();
        while (b < e && std::isspace(static_cast<unsigned char>(trimmed[b])))
            ++b;
        while (e > b && std::isspace(static_cast<unsigned char>(trimmed[e - 1])))
            --e;
        if (e - b < kMinSegmentClosingAssistantChars)
            continue;
        const Block* next = nullptr;
        for (size_t j = i + 1; j < blocks.size(); ++j)
            if (kindOf(blocks[j]) != Kind::Result) {
                next = &blocks[j];
                break;
            }
        if (!next || kindOf(*next) == Kind::User)
            ranked[i].score += kScoreSegmentClosingAssistant;
    }

    std::vector<size_t> costs;
    if (options.maxBriefChars) {
        costs.resize(blocks.size(), 0);
        for (size_t i = 0; i < blocks.size(); ++i) {
            if (kindOf(blocks[i]) == Kind::Result)
                continue;
            costs[i] = compileBrief({blocks[i]}).size() + 1;
        }
    }
    const size_t budget = options.maxBriefChars.value_or(SIZE_MAX);

    std::set<size_t> selected;
    std::set<std::string> seenKeys;
    size_t usedChars = 0;

    for (size_t i = blocks.size(); i-- > std::max(size_t{0}, blocks.size() - preserveRecent);) {
        if (kindOf(blocks[i]) == Kind::Result)
            continue;
        if (costs.size() == blocks.size() && usedChars + costs[i] > budget)
            continue;
        selected.insert(i);
        if (costs.size() == blocks.size())
            usedChars += costs[i];
        auto key = dedupKey(blocks[i]);
        if (key)
            seenKeys.insert(*key);
    }

    std::vector<const Ranked*> ordered;
    for (const auto& r : ranked)
        ordered.push_back(&r);
    std::stable_sort(ordered.begin(), ordered.end(), [](const Ranked* a, const Ranked* b) {
        if (a->score != b->score)
            return a->score > b->score;
        return a->index > b->index;
    });
    for (const Ranked* item : ordered) {
        if (selected.size() >= maxBlocks)
            break;
        if (selected.count(item->index))
            continue;
        if (kindOf(blocks[item->index]) == Kind::Result)
            continue;
        auto key = dedupKey(blocks[item->index]);
        if (key && seenKeys.count(*key))
            continue;
        if (costs.size() == blocks.size()) {
            if (usedChars + costs[item->index] > budget)
                continue;
            usedChars += costs[item->index];
        }
        selected.insert(item->index);
        if (key)
            seenKeys.insert(*key);
    }

    std::vector<Block> out;
    out.reserve(selected.size());
    for (size_t i : selected)
        out.push_back(blocks[i]);
    return out;
}

}  // namespace vcc
