#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

#include "tools/tools.h"
#include "util/path.h"

namespace {

// Locate a unique occurrence of needle in haystack; returns start index or -1 if
// not found exactly once.
long long findUnique(const std::string& haystack, const std::string& needle, size_t& count) {
    count = 0;
    long long last = -1;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string::npos) {
        count++;
        last = (long long)pos;
        pos += needle.size();
    }
    return (count == 1) ? last : -1;
}

// 1-based line numbers of the first `cap` occurrences of needle in haystack.
std::vector<long long> matchLineNumbers(const std::string& haystack, const std::string& needle, size_t cap) {
    std::vector<long long> out;
    auto lineOf = [&](size_t off) {
        long long ln = 1;
        for (size_t k = 0; k < off; k++)
            if (haystack[k] == '\n')
                ln++;
        return ln;
    };
    size_t pos = 0;
    while (out.size() < cap && (pos = haystack.find(needle, pos)) != std::string::npos) {
        out.push_back(lineOf(pos));
        pos += needle.size();
    }
    return out;
}

// needle with one leading space stripped from every non-empty line (the artifact of copying
// lines from the read tool's "NNNNN\u2192 " prefixed output). Returns empty string if any
// non-empty line lacks a leading space.
std::string stripOneLeadingSpacePerLine(const std::string& s) {
    std::string out, line;
    auto flush = [&]() {
        if (line.empty())
            return true;
        if (line[0] != ' ')
            return false;
        out += line.substr(1);
        return true;
    };
    for (char c : s) {
        if (c == '\n') {
            if (!flush())
                return "";
            out += '\n';
            line.clear();
        } else
            line += c;
    }
    if (!flush())
        return "";
    return out;
}

// --- Fuzzy "closest region" search for oldText that doesn't match exactly ---

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    out.push_back(cur);
    return out;
}

// Similarity of two lines via character-bigram Jaccard (0..1). Cheap, and
// tolerant of small drift: one changed word, off-by-one indentation, a stray
// character. Exact match is 1.
double bigramJaccard(const std::string& a, const std::string& b) {
    if (a == b)
        return 1.0;
    if (a.empty() || b.empty())
        return 0.0;
    auto grams = [](const std::string& s) {
        std::vector<uint16_t> g;
        g.reserve(s.size() > 1 ? s.size() - 1 : 0);
        for (size_t i = 0; i + 1 < s.size(); i++)
            g.push_back((uint16_t)(((unsigned char)s[i]) << 8) | (unsigned char)s[i + 1]);
        return g;
    };
    auto ga = grams(a);
    auto gb = grams(b);
    if (ga.empty() || gb.empty())
        return 0.0;
    std::sort(ga.begin(), ga.end());
    std::sort(gb.begin(), gb.end());
    size_t i = 0, j = 0, inter = 0;
    while (i < ga.size() && j < gb.size()) {
        if (ga[i] == gb[j]) {
            inter++;
            i++;
            j++;
        } else if (ga[i] < gb[j])
            i++;
        else
            j++;
    }
    size_t uni = ga.size() + gb.size() - inter;
    return (double)inter / (double)uni;
}

size_t trimmedLength(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return 0;
    size_t e = s.find_last_not_of(" \t");
    return e - b + 1;
}

// Best-scoring window of `lines` that resembles `needle` (a retyped or stale
// copy of a file region). Anchors on the needle's most distinctive line, so
// only a few candidate windows are scored. Returns startLine = -1 when there
// is no candidate worth showing.
struct NearRegion {
    long long startLine = -1; // 0-based index of the window's first line
    size_t window = 0;        // number of file lines in the window
    double score = 0.0;       // mean line similarity (0..1)
    size_t diffLines = 0;     // lines with similarity < 0.999
};

NearRegion findNearestRegion(const std::vector<std::string>& lines,
                             const std::vector<std::string>& needle) {
    NearRegion best;
    const size_t n = lines.size();
    const size_t kMaxWindow = 40;    // max lines to score per window
    const size_t kMaxCandidates = 64; // max anchor-matched file lines to try
    const double kCandidateSim = 0.5; // min similarity of a candidate anchor line
    if (needle.empty() || n == 0)
        return best;

    // Anchor: the needle's longest line. Too short -> nothing distinctive to
    // search on (e.g. "} else {"); skip rather than guess.
    size_t anchor = 0;
    for (size_t k = 1; k < needle.size(); k++)
        if (trimmedLength(needle[k]) > trimmedLength(needle[anchor]))
            anchor = k;
    if (trimmedLength(needle[anchor]) < 6)
        return best;

    // Window: up to kMaxWindow needle lines, centered on the anchor.
    size_t m = std::min(needle.size(), kMaxWindow);
    size_t lo = (anchor + 1 > m) ? anchor + 1 - m : 0;
    if (lo + m > needle.size())
        lo = needle.size() - m;
    std::vector<std::string> sliceCopy;
    const std::vector<std::string>* slice = &needle;
    if (m < needle.size()) {
        sliceCopy.assign(needle.begin() + lo, needle.begin() + lo + m);
        slice = &sliceCopy;
    }
    const size_t anchorInSlice = anchor - lo;

    struct Cand {
        size_t line;
        double sim;
    };
    std::vector<Cand> cands;
    cands.reserve(64);
    for (size_t j = 0; j < n; j++) {
        double sim = bigramJaccard(lines[j], needle[anchor]);
        if (sim >= kCandidateSim)
            cands.push_back({j, sim});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.sim > b.sim; });
    if (cands.size() > kMaxCandidates)
        cands.resize(kMaxCandidates);

    for (const Cand& c : cands) {
        if (c.line < anchorInSlice)
            continue;
        size_t s = c.line - anchorInSlice;
        if (s + m > n)
            continue;
        double sum = 0;
        size_t diff = 0;
        for (size_t k = 0; k < m; k++) {
            double sim = bigramJaccard(lines[s + k], (*slice)[k]);
            sum += sim;
            if (sim < 0.999)
                diff++;
        }
        double score = sum / (double)m;
        if (score > best.score) {
            best.score = score;
            best.startLine = (long long)s;
            best.window = m;
            best.diffLines = diff;
        }
    }
    return best;
}

} // namespace

Tool makeEditTool(const std::string& cwd) {
    Tool t;
    t.name = "edit";
    t.description = "Apply exact text replacements to a file. Each edit's oldText must match the file "
                    "exactly and occur exactly once. All edits are matched against the original file "
                    "content (not incrementally) and must not overlap. The file is only rewritten if "
                    "every edit is valid. Whitespace is significant: indentation in oldText must "
                    "match the file exactly; the read tool's 'NNNNN→' line prefix is display-only "
                    "and not part of the file content.";
    t.paramsSchema = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "File path, relative to the working directory or absolute"}}},
            {"edits", {
                {"type", "array"},
                {"description", "List of replacements"},
                {"items", {
                    {"type", "object"},
                    {"properties", {
                        {"oldText", {{"type", "string"}, {"description", "Exact text to find (must be unique)"}}},
                        {"newText", {{"type", "string"}, {"description", "Replacement text"}}},
                    }},
                    {"required", {"oldText", "newText"}},
                }},
            }},
        }},
        {"required", {"path", "edits"}},
    };
    t.execute = [cwd](nlohmann::json args, const std::atomic<bool>& /*abort*/) -> ToolResult {
        if (!args.contains("path") || !args["path"].is_string())
            return {"Missing 'path' argument", true};
        if (!args.contains("edits") || !args["edits"].is_array() || args["edits"].empty())
            return {"Missing or empty 'edits' array", true};

        std::string resolved = resolvePath(cwd, args["path"].get<std::string>());
        if (!isFile(resolved))
            return {"File not found: " + resolved, true};

        std::ifstream f(resolved, std::ios::binary);
        if (!f)
            return {"Cannot open file: " + resolved, true};
        std::stringstream ss;
        ss << f.rdbuf();
        std::string original = ss.str();

        struct Match {
            long long start;
            long long end; // exclusive
            int editIndex;
        };
        std::vector<Match> matches;
        std::vector<std::string> newtexts;

        for (size_t i = 0; i < args["edits"].size(); i++) {
            const auto& e = args["edits"][i];
            std::string oldText = e.value("oldText", std::string());
            std::string newText = e.value("newText", std::string());
            if (oldText.empty())
                return {"Edit " + std::to_string(i + 1) + ": oldText must not be empty", true};
            size_t count = 0;
            long long pos = findUnique(original, oldText, count);
            if (count == 0) {
                std::string msg = "Edit " + std::to_string(i + 1) + ": oldText not found in file";
                std::string cand = stripOneLeadingSpacePerLine(oldText);
                bool artifactHint = false;
                if (!cand.empty()) {
                    size_t c2 = 0;
                    if (findUnique(original, cand, c2) != -1) {
                        msg += "; but the same text matches exactly once after removing one leading space from "
                               "each line. The read tool's line prefix ('NNNNN\u2192') is display-only and not "
                               "part of the file \u2014 retry with the file's actual indentation.";
                        artifactHint = true;
                    } else
                        msg += "; verify exact indentation/whitespace (the read tool's line prefix "
                               "'NNNNN\u2192' is display-only, not file content).";
                }
                if (!artifactHint) {
                    // Show the closest actual region so the model can retry in
                    // one step instead of re-reading / cat -A forensics.
                    NearRegion near = findNearestRegion(splitLines(original), splitLines(oldText));
                    if (near.startLine >= 0 && near.score >= 0.6) {
                        char head[160];
                        snprintf(head, sizeof(head),
                                 "\nClosest region in the file (lines %lld-%lld, ~%d%% similar, "
                                 "%zu of %zu lines differ) \u2014 actual file content:\n",
                                 near.startLine + 1, near.startLine + (long long)near.window,
                                 (int)(near.score * 100), near.diffLines, near.window);
                        msg += head;
                        auto fileLines = splitLines(original);
                        for (size_t k = 0; k < near.window; k++) {
                            char num[16];
                            snprintf(num, sizeof(num), "%5zu\u2192", (size_t)near.startLine + k + 1);
                            msg += num;
                            msg += fileLines[(size_t)near.startLine + k];
                            msg += '\n';
                        }
                        msg += "Copy oldText from the content above (or re-read the file); do not retype it from memory.";
                    }
                }
                return {msg, true};
            }
            if (count > 1) {
                std::string loc;
                auto lns = matchLineNumbers(original, oldText, 5);
                for (size_t k = 0; k < lns.size(); k++)
                    loc += (k ? ", " : "") + std::to_string(lns[k]);
                if (count > lns.size())
                    loc += ", \u2026";
                return {"Edit " + std::to_string(i + 1) + ": oldText matches " + std::to_string(count) +
                            " times (lines " + loc + "); include more surrounding context to make it "
                            "unique",
                        true};
            }
            matches.push_back({pos, pos + (long long)oldText.size(), (int)i});
            newtexts.push_back(newText);
        }

        // Check for overlaps
        std::sort(matches.begin(), matches.end(), [](const Match& a, const Match& b) { return a.start < b.start; });
        for (size_t i = 1; i < matches.size(); i++) {
            if (matches[i].start < matches[i - 1].end)
                return {"Edits " + std::to_string(matches[i - 1].editIndex + 1) + " and " +
                            std::to_string(matches[i].editIndex + 1) + " overlap",
                        true};
        }

        // Apply from last to first
        std::string modified = original;
        for (int i = (int)matches.size() - 1; i >= 0; i--) {
            const Match& m = matches[i];
            modified.replace((size_t)m.start, (size_t)(m.end - m.start), newtexts[m.editIndex]);
        }

        std::ofstream out(resolved, std::ios::binary | std::ios::trunc);
        if (!out)
            return {"Cannot open file for writing: " + resolved, true};
        out.write(modified.data(), modified.size());
        out.close();
        if (!out)
            return {"Write failed: " + resolved, true};

        return {"Applied " + std::to_string(matches.size()) + " edit(s) to " + resolved, false};
    };
    return t;
}
