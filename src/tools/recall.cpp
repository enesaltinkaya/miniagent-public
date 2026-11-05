#include "tools/recall.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "agent/types.h"

namespace {

using nlohmann::json;

constexpr size_t kSearchPageSize = 5;
constexpr size_t kRangePageSize = 20;
constexpr size_t kRenderClip = 400;
constexpr size_t kExpandClip = 4000;
constexpr size_t kSnippetRadius = 60;

std::string toLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string clipText(const std::string& text, size_t max) {
    if (text.size() <= max)
        return text;
    return text.substr(0, max) + "...";
}

struct Entry {
    size_t index = 0;
    std::string kind;
    std::string render;
    std::string full;
    json raw = json::object();
};

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream f(path);
    if (!f)
        return lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string argsSummary(const json& args) {
    if (!args.is_object() || args.empty())
        return {};
    std::string out;
    for (auto it = args.begin(); it != args.end(); ++it) {
        if (!out.empty())
            out += " ";
        if (it.value().is_string())
            out += it.key() + "=" + it.value().get<std::string>();
        else
            out += it.key() + "=...";
    }
    return out;
}

std::vector<Entry> loadEntries(const std::string& path) {
    std::vector<Entry> entries;
    size_t index = 0;
    for (const std::string& line : readLines(path)) {
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded())
            continue;
        const std::string type = j.value("type", std::string{});
        if (type == "header")
            continue;
        ++index;
        Entry e;
        e.index = index;
        e.raw = j;
        if (type == "compaction") {
            e.kind = "compaction";
            e.full = j.value("summary", std::string{});
            e.render = "[compaction summary] " + clipText(e.full, kRenderClip);
            entries.push_back(std::move(e));
            continue;
        }
        if (type == "user") {
            e.kind = "user";
            e.full = j.value("text", std::string{});
            e.render = "user: " + clipText(e.full, kRenderClip);
            entries.push_back(std::move(e));
            continue;
        }
        if (type == "assistant") {
            e.kind = "assistant";
            std::ostringstream full;
            std::string text = j.value("text", std::string{});
            if (!text.empty())
                full << text << "\n";
            std::string thinking = j.value("thinking", std::string{});
            if (!thinking.empty())
                full << "[thinking] " << thinking << "\n";
            if (j.contains("toolCalls")) {
                for (const auto& tc : j["toolCalls"]) {
                    std::string name = tc.value("name", std::string{});
                    json arguments = tc.value("arguments", json::object());
                    std::string as = argsSummary(arguments);
                    full << "tool call " << name << (as.empty() ? "" : " " + as) << "\n";
                }
            }
            e.full = full.str();
            std::string render = text;
            if (render.empty() && j.contains("toolCalls")) {
                for (const auto& tc : j["toolCalls"]) {
                    if (!render.empty())
                        render += "; ";
                    render += tc.value("name", std::string{}) + "(" + argsSummary(tc.value("arguments", json::object())) + ")";
                }
            }
            e.render = "assistant: " + clipText(render, kRenderClip);
            entries.push_back(std::move(e));
            continue;
        }
        if (type == "tool") {
            e.kind = "tool_result";
            std::string name = j.value("toolName", std::string{});
            e.full = j.value("content", std::string{});
            std::string argsEcho = j.value("args", std::string{});
            if (!argsEcho.empty())
                e.full = "args: " + argsEcho + "\n" + e.full;
            e.render = "tool_result " + name + ": " + clipText(j.value("content", std::string{}), kRenderClip);
            entries.push_back(std::move(e));
            continue;
        }
        --index;
    }
    return entries;
}

std::vector<std::string> queryTerms(const std::string& query) {
    std::vector<std::string> terms;
    std::string current;
    for (char c : query) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                terms.push_back(toLower(current));
                current.clear();
            }
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty())
        terms.push_back(toLower(current));
    return terms;
}

std::string snippetAround(const std::string& text, const std::string& term) {
    if (term.empty())
        return {};
    const std::string hay = toLower(text);
    size_t idx = hay.find(term);
    if (idx == std::string::npos)
        return {};
    size_t start = idx > kSnippetRadius ? idx - kSnippetRadius : 0;
    size_t end = std::min(text.size(), idx + term.size() + kSnippetRadius);
    std::string out = (start > 0 ? "..." : "") + text.substr(start, end - start) + (end < text.size() ? "..." : "");
    for (char& c : out)
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    return out;
}

struct Hit {
    size_t index;
    int score;
    std::string snippet;
};

std::string formatHits(const std::vector<Hit>& hits, const std::vector<Entry>& entries, size_t page) {
    const size_t pages = std::max<size_t>(1, (hits.size() + kSearchPageSize - 1) / kSearchPageSize);
    if (page > pages)
        return "No results on page " + std::to_string(page) + ". Use a page between 1 and " +
               std::to_string(pages) + ".";
    size_t begin = (page - 1) * kSearchPageSize;
    size_t end = std::min(hits.size(), begin + kSearchPageSize);
    std::string out = "Matches " + std::to_string(hits.size()) + " (page " + std::to_string(page) + "/" +
                      std::to_string(pages) + "):\n";
    for (size_t i = begin; i < end; ++i) {
        const Hit& h = hits[i];
        out += "#" + std::to_string(h.index) + " " + entries[h.index - 1].render + "\n";
        if (!h.snippet.empty())
            out += "    " + h.snippet + "\n";
    }
    if (page < pages)
        out += "\n--- Use page:" + std::to_string(page + 1) + " for more results ---";
    else if (!hits.empty())
        out += "\n--- Read around a hit: range:[<n-3>, <n+3>]; full text: expand:[<n>] ---";
    return out;
}

std::string formatRange(const std::vector<Entry>& entries, long a, long b, size_t page) {
    if (entries.empty())
        return "Session is empty.";
    const long last = static_cast<long>(entries.size());
    long from = std::max(1L, a);
    long to = std::min(last, b);
    if (to < from)
        return "Range is empty (asked " + std::to_string(a) + ".." + std::to_string(b) +
               ", session has entries #1..#" + std::to_string(last) + ").";
    const size_t pages = static_cast<size_t>((to - from) / kRangePageSize + 1);
    if (page > pages)
        return "No entries on page " + std::to_string(page) + ". Use a page between 1 and " +
               std::to_string(pages) + ".";
    long begin = from + static_cast<long>((page - 1) * kRangePageSize);
    long end = std::min(to, begin + static_cast<long>(kRangePageSize) - 1);
    std::string out = "Entries #" + std::to_string(begin) + "..#" + std::to_string(end) + " of #" +
                      std::to_string(from) + "..#" + std::to_string(to) + " (page " + std::to_string(page) +
                      "/" + std::to_string(pages) + "):\n";
    for (long i = begin; i <= end; ++i)
        out += "#" + std::to_string(i) + " " + entries[i - 1].render + "\n";
    if (page < pages)
        out += "\n--- Use page:" + std::to_string(page + 1) + " for more entries ---";
    return out;
}

std::string formatExpand(const std::vector<size_t>& wanted, const std::vector<Entry>& entries) {
    std::string out;
    int missing = 0;
    for (size_t n : wanted) {
        if (n == 0 || n > entries.size()) {
            ++missing;
            continue;
        }
        const Entry& e = entries[n - 1];
        out += "# " + std::to_string(n) + " " + e.kind + "\n";
        std::string body = clipText(e.full, kExpandClip);
        out += body.empty() ? "(empty)\n" : body + "\n";
        if (e.kind == "assistant") {
            for (size_t k = n; k < entries.size(); ++k) {
                if (entries[k].kind != "tool_result")
                    break;
                out += "  result #" + std::to_string(k + 1) + ": " + clipText(entries[k].full, 1000) + "\n";
            }
        }
        out += "\n";
    }
    if (out.empty())
        return missing > 0 ? std::to_string(missing) + " index(es) out of range; session has #1..#" +
                                 std::to_string(entries.size()) + "."
                           : "Nothing to expand.";
    if (missing > 0)
        out += "(" + std::to_string(missing) + " index(es) out of range; session has #1..#" +
               std::to_string(entries.size()) + ".)\n";
    return out;
}

struct Touch {
    size_t index;
    std::string tool;
};

std::string formatTouched(const std::vector<Entry>& entries, size_t page) {
    std::map<std::string, std::vector<Touch>> byPath;
    for (const Entry& e : entries) {
        if (e.kind != "assistant")
            continue;
        const json& j = e.raw;
        if (!j.contains("toolCalls"))
            continue;
        for (const auto& tc : j["toolCalls"]) {
            const std::string name = toLower(tc.value("name", std::string{}));
            json args = tc.value("arguments", json::object());
            std::string path;
            if (args.is_object()) {
                for (const char* key : {"path", "file_path", "filePath", "file"})
                    if (args.contains(key) && args[key].is_string()) {
                        path = args[key].get<std::string>();
                        break;
                    }
            }
            if (path.empty())
                continue;
            if (name != "read" && name != "write" && name != "edit")
                continue;
            byPath[path].push_back({e.index, name});
        }
    }
    if (byPath.empty())
        return "No files were read or modified in this session.";

    std::vector<std::string> paths;
    for (const auto& [p, _] : byPath)
        paths.push_back(p);
    const size_t pages = (paths.size() + kRangePageSize - 1) / kRangePageSize;
    if (page > pages)
        return "No paths on page " + std::to_string(page) + ". Use a page between 1 and " +
               std::to_string(pages) + ".";
    size_t begin = (page - 1) * kRangePageSize;
    size_t end = std::min(paths.size(), begin + kRangePageSize);
    std::string out = "Files touched (page " + std::to_string(page) + "/" + std::to_string(pages) + "):\n";
    for (size_t i = begin; i < end; ++i) {
        const auto& touches = byPath[paths[i]];
        out += "- " + paths[i] + ": ";
        size_t shown = 0;
        for (const auto& t : touches) {
            if (shown >= 8) {
                out += " (+" + std::to_string(touches.size() - shown) + " more)";
                break;
            }
            if (shown > 0)
                out += " ";
            out += "#" + std::to_string(t.index) + " " + t.tool;
            ++shown;
        }
        out += "\n";
    }
    out += "\n--- Full text of an entry: expand:[N] ---";
    return out;
}

}  // namespace

Tool makeRecallTool(std::function<std::string()> sessionFile) {
    Tool t;
    t.name = "recall";
    t.description =
        "Recall earlier parts of this session, including anything dropped by compaction.\n"
        "Use it before saying the context is gone.\n"
        "\n"
        "Pick one action per call:\n"
        "- query: keyword search; entries containing more of the words rank first, 5 per page.\n"
        "- range: [from, to]: entries #from..#to in order, 20 per page.\n"
        "- expand: [N, ...]: full untruncated text of those entries (tool results capped at 4000 chars).\n"
        "- mode \"touched\": files read/modified, each with the entries that touched it.\n"
        "\n"
        "Add page: N for the next page of query, range or touched results.\n"
        "#N is the number shown in summary refs like (#12) and in recall results.";
    t.paramsSchema = {
        {"type", "object"},
        {"properties",
         {
             {"query", {{"type", "string"}, {"description", "Keywords to search for. Entries matching more terms rank first. Case-insensitive."}}},
             {"range", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 2}, {"maxItems", 2}, {"description", "[from, to]: entries #from..#to inclusive, in order."}}},
             {"expand", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"minItems", 1}, {"description", "Entry indices to return in full."}}},
             {"mode", {{"type", "string"}, {"enum", json::array({"touched"})}, {"description", "\"touched\": files worked on."}}},
             {"page", {{"type", "integer"}, {"description", "1-based page for query, range or touched results. Default 1."}}},
         }},
        {"additionalProperties", false},
    };
    t.execute = [sessionFile = std::move(sessionFile)](const json& args, const std::atomic<bool>&) -> ToolResult {
        const std::string file = sessionFile ? sessionFile() : std::string();
        if (file.empty())
            return {"No session file available.", true};
        std::vector<Entry> entries = loadEntries(file);
        if (entries.empty())
            return {"Session is empty."};

        size_t page = 1;
        if (args.contains("page") && args["page"].is_number())
            page = static_cast<size_t>(std::max(1L, args["page"].get<long>()));

        std::vector<std::string> ignored;
        if (args.contains("query") && !args["query"].is_string())
            ignored.push_back("query");
        if (args.contains("mode") && !(args["mode"].is_string() && args["mode"] == "touched"))
            ignored.push_back("mode");

        std::string prefix;
        if (!ignored.empty()) {
            prefix = "Ignored: " + ignored[0] + " (unusable value).\n\n";
        }

        if (args.contains("query") && args["query"].is_string()) {
            const std::string query = args["query"].get<std::string>();
            std::vector<std::string> terms = queryTerms(query);
            if (terms.empty())
                return {prefix + "Query is empty."};
            std::vector<Hit> hits;
            for (const Entry& e : entries) {
                const std::string hay = toLower(e.render + "\n" + e.full);
                int score = 0;
                std::string firstSnippet;
                for (const auto& term : terms) {
                    if (hay.find(term) != std::string::npos) {
                        ++score;
                        if (firstSnippet.empty())
                            firstSnippet = snippetAround(e.full.empty() ? e.render : e.full, term);
                    }
                }
                if (score > 0)
                    hits.push_back({e.index, score, firstSnippet});
            }
            if (hits.empty())
                return {prefix + "No matches for \"" + query + "\" in " + std::to_string(entries.size()) +
                        " entries. Try fewer or different words."};
            std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
                if (a.score != b.score)
                    return a.score > b.score;
                return a.index > b.index;
            });
            return {prefix + formatHits(hits, entries, page)};
        }

        if (args.contains("range") && args["range"].is_array() && args["range"].size() == 2 &&
            args["range"][0].is_number() && args["range"][1].is_number()) {
            long a = args["range"][0].get<long>();
            long b = args["range"][1].get<long>();
            if (a > b)
                std::swap(a, b);
            return {prefix + formatRange(entries, a, b, page)};
        }

        if (args.contains("expand") && args["expand"].is_array() && !args["expand"].empty()) {
            std::vector<size_t> wanted;
            bool ok = true;
            for (const auto& v : args["expand"]) {
                if (!v.is_number()) {
                    ok = false;
                    break;
                }
                wanted.push_back(std::max(0L, v.get<long>()));
            }
            if (ok)
                return {prefix + formatExpand(wanted, entries)};
        }

        if (args.contains("mode") && args["mode"] == "touched")
            return {prefix + formatTouched(entries, page)};

        if (args.contains("expand") || args.contains("range"))
            return {prefix + "Unusable expand/range value: use numbers, e.g. expand:[12] or range:[5, 20]."};

        return {prefix + "Nothing to do: pass query, range, expand or mode:\"touched\"."};
    };
    return t;
}
