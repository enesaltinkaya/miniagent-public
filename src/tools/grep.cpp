#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tools/tools.h"
#include "util/path.h"
#include "util/utf8.h"

namespace {

// Longest line kept in the output. A minified bundle or a data blob can hold
// one enormous line; printing it whole would eat the whole result budget.
constexpr size_t kMaxLineChars = 500;

// Bytes read per file at most. A generated bundle beyond this is not source;
// reading it whole costs more than the rest of the tree put together.
constexpr size_t kMaxFileBytes = 8ull << 20;

// Directory tree depth limit (deep enough for any real source tree).
constexpr int kMaxDepth = 40;

// Wall-clock budget for one search. Without it a grep over a huge tree runs for
// minutes with no way to make progress; with it the tool always comes back with
// what it found plus a note on how to narrow the search.
constexpr int kDefaultBudgetSeconds = 20;

// Read granularity: one read syscall per 64KB instead of one per line.
constexpr size_t kChunkBytes = 1 << 16;

// Upper bound on worker threads (the walk is sequential, scanning is not).
constexpr size_t kMaxThreads = 8;

using Clock = std::chrono::steady_clock;

// VCS metadata never holds source worth searching, whatever .gitignore says.
bool isVcsDir(const std::string& name) {
    return name == ".git" || name == ".hg" || name == ".svn" || name == ".bzr" || name == ".jj";
}

std::vector<std::string> splitPath(const std::string& p) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= p.size()) {
        size_t slash = p.find('/', start);
        if (slash == std::string::npos) {
            if (start < p.size())
                out.push_back(p.substr(start));
            break;
        }
        if (slash > start)
            out.push_back(p.substr(start, slash - start));
        start = slash + 1;
    }
    return out;
}

// One path segment of a gitignore pattern against one path segment. Supports
// '*', '?', '[...]' and backslash escapes.
bool segGlob(const std::string& pat, size_t pi, const std::string& seg, size_t si) {
    while (pi < pat.size()) {
        char c = pat[pi];
        if (c == '*') {
            while (pi < pat.size() && pat[pi] == '*')
                ++pi;
            if (pi == pat.size())
                return true;
            for (size_t k = si; k <= seg.size(); ++k)
                if (segGlob(pat, pi, seg, k))
                    return true;
            return false;
        }
        if (si >= seg.size())
            return false;
        if (c == '?') {
            ++pi;
            ++si;
            continue;
        }
        if (c == '[') {
            size_t j = pi + 1;
            bool negate = false;
            if (j < pat.size() && (pat[j] == '!' || pat[j] == '^')) {
                negate = true;
                ++j;
            }
            bool hit = false;
            while (j < pat.size() && pat[j] != ']') {
                if (j + 2 < pat.size() && pat[j + 1] == '-' && pat[j + 2] != ']') {
                    if (seg[si] >= pat[j] && seg[si] <= pat[j + 2])
                        hit = true;
                    j += 3;
                } else {
                    if (seg[si] == pat[j])
                        hit = true;
                    ++j;
                }
            }
            if (j >= pat.size()) { // unterminated class: '[' was literal
                if (seg[si] != '[')
                    return false;
                ++pi;
                ++si;
                continue;
            }
            if (hit == negate)
                return false;
            pi = j + 1;
            ++si;
            continue;
        }
        if (c == '\\' && pi + 1 < pat.size()) {
            if (seg[si] != pat[pi + 1])
                return false;
            pi += 2;
            ++si;
            continue;
        }
        if (seg[si] != c)
            return false;
        ++pi;
        ++si;
    }
    return si == seg.size();
}

// Segment-list match where a "**" pattern segment spans any number of path
// segments (including none).
bool matchSegments(const std::vector<std::string>& pat, size_t pi,
                   const std::vector<std::string>& segs, size_t si) {
    if (pi == pat.size())
        return si == segs.size();
    if (pat[pi] == "**") {
        for (size_t k = si; k <= segs.size(); ++k)
            if (matchSegments(pat, pi + 1, segs, k))
                return true;
        return false;
    }
    if (si >= segs.size())
        return false;
    if (!segGlob(pat[pi], 0, segs[si], 0))
        return false;
    return matchSegments(pat, pi + 1, segs, si + 1);
}

// One .gitignore line: which entries under the file's directory it drops.
struct IgnoreRule {
    std::string base;                  // absolute dir the ignore file lives in
    std::vector<std::string> pattern;  // pattern split on '/'
    bool dirOnly = false;
    bool negated = false;
    bool anchored = false;             // pattern contained a separator, so it is rooted at base
};

std::vector<IgnoreRule> parseIgnoreFile(const std::string& path, const std::string& base) {
    std::vector<IgnoreRule> rules;
    std::ifstream in(path);
    if (!in)
        return rules;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r')
            line.pop_back();
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos)
            continue;
        if (line[b] == '#' && !(b + 1 < line.size() && line[b + 1] == '\\'))
            continue;
        IgnoreRule r;
        r.base = base;
        std::string pat = line.substr(b);
        if (!pat.empty() && pat[0] == '!') {
            r.negated = true;
            pat = pat.substr(1);
        }
        if (!pat.empty() && pat.back() == '/') {
            r.dirOnly = true;
            pat.pop_back();
        }
        if (!pat.empty() && pat[0] == '/') {
            r.anchored = true;
            pat = pat.substr(1);
        }
        if (pat.find('/') != std::string::npos)
            r.anchored = true;
        r.pattern = splitPath(pat);
        if (r.pattern.empty())
            continue;
        rules.push_back(std::move(r));
    }
    return rules;
}

// True when `abs` is dropped by the ignore rules collected so far. Last
// matching rule wins, matching git's own precedence.
bool isIgnored(const std::vector<IgnoreRule>& rules, const std::string& abs, bool isDir) {
    bool ignored = false;
    for (const auto& r : rules) {
        if (r.dirOnly && !isDir)
            continue;
        if (abs.size() <= r.base.size() || abs.compare(0, r.base.size(), r.base) != 0 ||
            abs[r.base.size()] != '/')
            continue;
        const std::string& rel = abs;
        size_t relStart = r.base.size() + 1;
        bool match;
        if (r.anchored) {
            match = matchSegments(r.pattern, 0, splitPath(rel.substr(relStart)), 0);
        } else {
            size_t slash = rel.find_last_of('/');
            const std::string& name = rel;
            size_t nameStart = slash == std::string::npos ? relStart : slash + 1;
            match = segGlob(r.pattern.back(), 0, name, nameStart);
        }
        if (match)
            ignored = !r.negated;
    }
    return ignored;
}

// The .git directory above `start` ("" when there is none).
std::string findGitDir(const std::string& start) {
    namespace fs = std::filesystem;
    std::error_code ec;
    std::string dir = start;
    for (int i = 0; i < kMaxDepth; ++i) {
        if (fs::is_directory(dir + "/.git", ec) || fs::is_regular_file(dir + "/.git", ec))
            return dir + "/.git";
        if (dir.empty() || dir == "/")
            break;
        size_t slash = dir.find_last_of('/');
        if (slash == std::string::npos)
            break;
        dir = slash == 0 ? std::string("/") : dir.substr(0, slash);
    }
    return {};
}

struct Walk {
    std::vector<std::string> files;
    std::vector<IgnoreRule> rules;
    Clock::time_point deadline;
    const std::atomic<bool>& abort;
    bool outOfBudget = false;

    explicit Walk(const std::atomic<bool>& a) : abort(a) {}

    bool expired() {
        if (abort.load(std::memory_order_relaxed))
            return true;
        if (Clock::now() > deadline) {
            outOfBudget = true;
            return true;
        }
        return false;
    }
};

// Collect candidate files, pruning VCS metadata and gitignored subtrees. The
// ignore rules of every visited directory are pushed on entry and popped on
// exit, so a nested .gitignore only affects its own subtree.
void walk(const std::string& dir, int depth, Walk& w) {
    size_t ruleBase = w.rules.size();
    for (const auto& r : parseIgnoreFile(dir + "/.gitignore", dir))
        w.rules.push_back(r);

    namespace fs = std::filesystem;
    std::error_code ec;
    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec)
            break;
        if (w.expired())
            break;
        std::string name = it->path().filename().string();
        std::string child = it->path().string();
        std::error_code tec;
        if (it->is_symlink(tec) || isVcsDir(name))
            continue;
        bool isDir = it->is_directory(tec);
        if (tec)
            continue;
        if (isDir) {
            if (isIgnored(w.rules, child, true))
                continue;
            if (depth + 1 < kMaxDepth)
                walk(child, depth + 1, w);
        } else if (it->is_regular_file(tec)) {
            if (!tec && !isIgnored(w.rules, child, false))
                w.files.push_back(std::move(child));
        }
    }
    w.rules.resize(ruleBase);
}

unsigned char lowerAscii(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c;
}

std::string lowerCopy(const std::string& s) {
    std::string out(s);
    for (auto& c : out)
        c = static_cast<char>(lowerAscii(static_cast<unsigned char>(c)));
    return out;
}

// Case-insensitive substring test. The line is lowercased into a reused buffer
// and handed to string::find (memchr-backed) instead of being rescanned at
// every candidate offset, which is what made large files crawl.
bool lineMatches(const char* data, size_t size, const std::string& needle, std::string& scratch) {
    char first = needle[0];
    bool found = std::char_traits<char>::find(data, size, first) != nullptr;
    if (!found && first >= 'a' && first <= 'z')
        found = std::char_traits<char>::find(data, size, static_cast<char>(first - 32)) != nullptr;
    if (!found)
        return false;
    scratch.assign(data, size);
    for (auto& c : scratch)
        c = static_cast<char>(lowerAscii(static_cast<unsigned char>(c)));
    return scratch.find(needle) != std::string::npos;
}

struct ScanStats {
    std::atomic<size_t> scanned{0};
    std::atomic<size_t> binary{0};
    std::atomic<size_t> tooLarge{0};
    std::atomic<long long> matches{0};
    std::atomic<bool> stopped{false};
};

// One file's matches, formatted as "lineNo:text\n" and capped at maxLines.
std::string scanFile(const std::string& path, const std::string& needle, int maxLines,
                     ScanStats& stats, const std::atomic<bool>& abort, Clock::time_point deadline,
                     std::string& scratch) {
    std::string out;
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return out;
    ++stats.scanned;

    std::vector<char> chunk(kChunkBytes);
    std::string pending;  // tail of the file after the last newline
    long long lineNo = 0;
    int shown = 0;
    size_t bytes = 0;
    size_t sinceCheck = 0;
    bool first = true;

    auto handle = [&](const char* data, size_t size) {
        while (size && data[size - 1] == '\r')
            --size;
        if (!size || !lineMatches(data, size, needle, scratch))
            return;
        ++stats.matches;
        if (shown >= maxLines)
            return;
        std::string text = sanitizeUtf8(std::string(data, size));
        if (text.size() > kMaxLineChars)
            text = text.substr(0, kMaxLineChars) + "…";
        out += std::to_string(lineNo);
        out += ':';
        out += text;
        out += '\n';
        ++shown;
    };

    while (in) {
        if (abort.load(std::memory_order_relaxed)) {
            stats.stopped = true;
            return out;
        }
        if (sinceCheck >= (1 << 20)) {
            sinceCheck = 0;
            if (Clock::now() > deadline) {
                stats.stopped = true;
                return out;
            }
        }
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        std::streamsize n = in.gcount();
        if (n <= 0)
            break;
        bytes += static_cast<size_t>(n);
        sinceCheck += static_cast<size_t>(n);
        if (first) {
            first = false;
            if (std::char_traits<char>::find(chunk.data(), static_cast<size_t>(n), '\0')) {
                ++stats.binary;
                return std::string();
            }
        }

        size_t size = static_cast<size_t>(n);
        size_t start = 0;
        if (!pending.empty()) {
            const char* nl = std::char_traits<char>::find(chunk.data(), size, '\n');
            if (!nl) {
                pending.append(chunk.data(), size);
                continue;
            }
            size_t head = static_cast<size_t>(nl - chunk.data());
            pending.append(chunk.data(), head);
            ++lineNo;
            handle(pending.data(), pending.size());
            pending.clear();
            start = head + 1;
        }
        while (start < size) {
            const char* nl = std::char_traits<char>::find(chunk.data() + start, size - start, '\n');
            size_t end = nl ? static_cast<size_t>(nl - chunk.data()) : size;
            ++lineNo;
            handle(chunk.data() + start, end - start);
            if (!nl) {
                pending.assign(chunk.data() + start, size - start);
                break;
            }
            start = end + 1;
        }

        if (bytes > kMaxFileBytes) {
            ++stats.tooLarge;
            return out;
        }
    }
    if (!pending.empty()) {
        ++lineNo;
        handle(pending.data(), pending.size());
    }
    return out;
}

} // namespace

Tool makeGrepTool(const std::string& cwd) {
    Tool t;
    t.name = "grep";
    t.description =
        "Search file contents for a case-insensitive substring. Searches the whole working "
        "directory by default, or one directory/file when 'path' is given. Returns matching "
        "lines as 'path:line:text'. VCS metadata, gitignored directories, oversized and binary "
        "files are skipped. Output is truncated to 2000 lines or 50KB. This is the dedicated "
        "search tool - use it instead of running grep/rg through bash. To read a file's contents "
        "use the read tool (offset/limit for ranges); do not use bash to cat, sed -n, head, or "
        "tail a file.";
    t.paramsSchema = {
        {"type", "object"},
        {"properties", {
            {"pattern", {{"type", "string"}, {"description", "Substring to search for (case-insensitive)"}}},
            {"path", {{"type", "string"}, {"description",
                "File or directory to search, relative to the working directory or absolute "
                "(default: the working directory)"}}},
            {"limit", {{"type", "integer"}, {"description",
                "Maximum number of matching lines to return (default 100)"}}},
            {"timeout", {{"type", "integer"}, {"description",
                "Search budget in seconds; when it runs out the matches found so far are "
                "reported (default 20)"}}},
        }},
        {"required", {"pattern"}},
    };
    t.execute = [cwd](nlohmann::json args, const std::atomic<bool>& abort) -> ToolResult {
        if (!args.contains("pattern") || !args["pattern"].is_string() ||
            args["pattern"].get<std::string>().empty())
            return {"Missing 'pattern' argument", true};

        const std::string pattern = args["pattern"].get<std::string>();
        std::string root = cwd;
        if (args.contains("path") && args["path"].is_string() && !args["path"].get<std::string>().empty())
            root = resolvePath(cwd, args["path"].get<std::string>());

        int maxLines = 100;
        if (args.contains("limit") && args["limit"].is_number())
            maxLines = args["limit"].get<int>();
        if (maxLines < 1)
            maxLines = 1;

        int budgetSeconds = kDefaultBudgetSeconds;
        if (args.contains("timeout") && args["timeout"].is_number() && args["timeout"].get<int>() > 0)
            budgetSeconds = args["timeout"].get<int>();

        const std::string needle = lowerCopy(pattern);
        const Clock::time_point deadline = Clock::now() + std::chrono::seconds(budgetSeconds);

        std::vector<std::string> files;
        bool walkRanOut = false;
        if (isDir(root)) {
            Walk w(abort);
            w.deadline = deadline;
            std::string gitDir = findGitDir(root);
            if (!gitDir.empty()) {
                std::string repo = gitDir.size() > 5 ? gitDir.substr(0, gitDir.size() - 5) : gitDir;
                for (const auto& r : parseIgnoreFile(gitDir + "/info/exclude", repo))
                    w.rules.push_back(r);
            }
            walk(root, 0, w);
            files = std::move(w.files);
            walkRanOut = w.outOfBudget;
        } else if (isFile(root)) {
            files.push_back(root);
        } else {
            return {"Path not found: " + root, true};
        }
        std::sort(files.begin(), files.end());

        if (walkRanOut) {
            return {"grep: walking " + root + " hit the " + std::to_string(budgetSeconds) +
                        "s budget after finding " + std::to_string(files.size()) +
                        " file(s). Narrow 'path' or raise 'timeout'.",
                    true};
        }

        ScanStats stats;
        std::vector<std::string> hits(files.size());
        {
            size_t threads = 1;
            if (files.size() > 1) {
                threads = std::min<size_t>(std::thread::hardware_concurrency(), kMaxThreads);
                if (threads < 1)
                    threads = 1;
            }
            std::atomic<size_t> next{0};
            auto worker = [&] {
                std::string scratch;
                for (size_t i = next.fetch_add(1); i < files.size(); i = next.fetch_add(1)) {
                    if (abort.load(std::memory_order_relaxed) || Clock::now() > deadline) {
                        stats.stopped = true;
                        break;
                    }
                    hits[i] = scanFile(files[i], needle, maxLines, stats, abort, deadline, scratch);
                }
            };
            std::vector<std::thread> pool;
            pool.reserve(threads - 1);
            for (size_t i = 1; i < threads; ++i)
                pool.emplace_back(worker);
            worker();
            for (auto& th : pool)
                th.join();
        }

        std::ostringstream out;
        int shown = 0;
        for (size_t i = 0; i < files.size() && shown < maxLines; ++i) {
            std::istringstream lines(hits[i]);
            std::string line;
            while (shown < maxLines && std::getline(lines, line)) {
                out << files[i] << ":" << line << "\n";
                ++shown;
            }
        }

        std::string result = out.str();
        std::string note;
        auto append = [&](const std::string& part) {
            if (note.empty())
                note = "\n[" + part;
            else
                note += "; " + part;
        };
        if (shown > 0)
            append(std::to_string(shown) + " match" + (shown == 1 ? "" : "es") + " shown (limit " +
                   std::to_string(maxLines) + ")");
        if (stats.matches.load() > shown)
            append("more matches exist - narrow the pattern or the path");
        if (stats.binary > 0)
            append(std::to_string(stats.binary.load()) + " binary file(s) skipped");
        if (stats.tooLarge > 0)
            append(std::to_string(stats.tooLarge.load()) + " file(s) over 8MB read only partially");
        if (stats.stopped)
            append(std::to_string(stats.scanned.load()) + " of " + std::to_string(files.size()) +
                   " file(s) searched before the " + std::to_string(budgetSeconds) +
                   "s budget ran out - narrow 'path' or raise 'timeout'");
        if (!note.empty())
            result += note + "]";
        if (shown == 0)
            result = "No matches for \"" + pattern + "\" in " + root + "." + result;
        return {truncateToLimit(result, 2000, 50 * 1024), false};
    };
    return t;
}
