#include "ui/view_rows.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace viewrows {

// Bash summaries are capped so the per-frame text-measurement cost stays
// bounded; the cap must stay far larger than any realistic row width.
static const std::size_t kMaxBashSummaryChars = 4096;

std::string TrimMessage(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string BashCommandSummary(const std::string& cmd) {
    std::string out;
    out.reserve(cmd.size());
    for (char c : cmd)
        out += (c == '\n' || c == '\r') ? ' ' : c; // the row is one line
    if (out.size() > kMaxBashSummaryChars) {
        out = out.substr(0, kMaxBashSummaryChars);
        while (!out.empty() && (out.back() & 0xC0) == 0x80)
            out.pop_back(); // don't split a multi-byte UTF-8 character
    }
    return out;
}

// The args are JSON, so find the top-level `key` by scanning while tracking
// string state and brace depth. While the row is still streaming the
// fragment is a JSON prefix, so a still-open string value is returned as-is
// (it keeps growing); a complete value is returned decoded.
std::string JsonTopLevelString(const std::string& j, const char* key, std::size_t maxScan) {
    const size_t kLen = strlen(key);
    const size_t lim = std::min(j.size(), maxScan);
    int depth = 0;
    bool inStr = false, esc = false;
    for (size_t i = 0; i < lim; ++i) {
        const char c = j[i];
        if (inStr) {
            if (esc)
                esc = false;
            else if (c == '\\')
                esc = true;
            else if (c == '"')
                inStr = false;
            continue;
        }
        switch (c) {
            case '{':
            case '[':
                ++depth;
                break;
            case '}':
            case ']':
                --depth;
                break;
            case '"':
                inStr = true;
                if (depth == 1 && j.compare(i + 1, kLen, key) == 0) {
                    size_t p = i + 1 + kLen;
                    if (p < j.size() && j[p] == '"')
                        ++p; // key's closing quote
                    while (p < j.size() && isspace((unsigned char)j[p]))
                        ++p;
                    if (p < j.size() && j[p] == ':') {
                        ++p;
                        while (p < j.size() && isspace((unsigned char)j[p]))
                            ++p;
                        if (p < j.size() && j[p] == '"') {
                            std::string out;
                            ++p;
                            bool e2 = false, closed = false;
                            for (; p < lim; ++p) {
                                const char v = j[p];
                                if (v == '"' && !e2) {
                                    closed = true;
                                    break;
                                }
                                if (e2) {
                                    switch (v) {
                                        case 'n': out += '\n'; break;
                                        case 't': out += '\t'; break;
                                        case 'r': out += '\r'; break;
                                        case '"': out += '"'; break;
                                        case '\\': out += '\\'; break;
                                        case '/': out += '/'; break;
                                        case 'u': p += 4; out += '?'; break; // non-ASCII: not expected in paths
                                        default: out += v; break;
                                    }
                                    e2 = false; // v completed the escape
                                } else {
                                    out += (v == '\\') ? '\\' : v;
                                    e2 = (v == '\\');
                                }
                            }
                            if (!closed && e2 && !out.empty())
                                out.pop_back(); // dangling escape prefix of a still-growing value
                            return out;
                        }
                    }
                }
                break;
        }
    }
    return {};
}

ToolRowElapsed toolRowElapsed(bool streaming, bool done, int64_t startMs,
                              int64_t settledElapsedMs, int64_t nowEpochMs) {
    ToolRowElapsed e;
    e.running = !streaming && !done;
    if (e.running && startMs > 0)
        e.elapsedMs = std::max<int64_t>(0, nowEpochMs - startMs);
    else if (done && settledElapsedMs >= 0)
        e.elapsedMs = settledElapsedMs;
    e.slow = e.elapsedMs >= kToolElapsedLineMs;
    e.line = e.running && e.slow;
    return e;
}

std::string toolRowStatusSuffix(bool streaming, const ToolRowElapsed& e, bool done,
                                bool isError) {
    if (streaming)
        return "  [streaming...]";
    if (e.running)
        return "  [running...]";
    if (done && isError)
        return e.slow ? "  [error " + FormatDurationMs(e.elapsedMs) + "]" : "  [error]";
    if (e.slow)
        return "  [" + FormatDurationMs(e.elapsedMs) + "]";
    return {};
}

char toolSpinner(int64_t elapsedMs) {
    static const char kSpin[] = {'|', '/', '-', '\\'};
    return kSpin[(size_t)(elapsedMs / 120) % 4];
}

bool toolRowExpandable(bool done, bool streaming, const std::string& result,
                       const std::string& name, const std::string& command) {
    return (done && !result.empty()) ||
           (!done && !streaming && name == "bash" && !command.empty());
}

// apply_patch args are one giant 'patch' string: summarise them by the file
// markers inside it (the markers appear verbatim in the JSON text; a path
// ends at the next escape or closing quote).
std::string ApplyPatchPathSummary(const std::string& args, std::size_t maxScan) {
    const size_t lim = std::min(args.size(), maxScan);
    struct Marker {
        const char* text;
        size_t len;
    };
    const Marker markers[] = {
        {"*** Update File: ", 17}, {"*** Add File: ", 14}, {"*** Delete File: ", 17},
    };
    size_t best = std::string::npos;
    size_t bestLen = 0;
    for (const Marker& m : markers) {
        size_t at = args.find(m.text, 0);
        if (at != std::string::npos && at < lim && (best == std::string::npos || at < best)) {
            best = at;
            bestLen = m.len;
        }
    }
    if (best == std::string::npos)
        return {};
    std::string out;
    for (size_t p = best + bestLen; p < args.size() && p < lim; ++p) {
        const char ch = args[p];
        if (ch == '"' || ch == '\\' || ch == '\n')
            break;
        out += ch;
        if (out.size() >= 80)
            break;
    }
    return out;
}

std::string toolArgsSummary(const std::string& toolName, const std::string& toolArgs,
                            bool expanded, bool streaming, ToolSummaryCache c) {
    std::string args = toolArgs;
    if (streaming && !expanded) {
        // Still growing: never show the raw fragment -- a write/edit streams
        // the whole file content inside its args. Show only the target file
        // (bash: the partial command). If the model has not emitted the key
        // yet (e.g. it types content before path), or the tool name has not
        // arrived yet (then try both keys), show nothing but the name and
        // status.
        auto liveSummary = [&](std::size_t maxScan) -> std::string {
            if (toolName == "apply_patch")
                return ApplyPatchPathSummary(toolArgs, maxScan);
            std::string path = JsonTopLevelString(toolArgs, "path", maxScan);
            if (!path.empty())
                return path;
            std::string pattern = JsonTopLevelString(toolArgs, "pattern", maxScan);
            if (!pattern.empty())
                return pattern;
            for (const char* key : {"query", "url"}) {
                std::string v = JsonTopLevelString(toolArgs, key, maxScan);
                if (!v.empty())
                    return v;
            }
            return BashCommandSummary(JsonTopLevelString(toolArgs, "command", maxScan));
        };
        if (toolArgs.size() != c.fileScanSize) {
            c.fileScanSize = toolArgs.size();
            c.file = liveSummary(kStreamScanWindow);
        }
        args = c.file;
    } else if ((!expanded &&
                (toolName == "read" || toolName == "write" || toolName == "edit" ||
                 toolName == "apply_patch" || toolName == "grep" || toolName == "web_search" ||
                 toolName == "web_fetch")) ||
               toolName == "bash") {
        auto summarize = [&](std::size_t maxScan) -> std::string {
            if (toolName == "bash")
                return BashCommandSummary(JsonTopLevelString(toolArgs, "command", maxScan));
            if (toolName == "apply_patch")
                return ApplyPatchPathSummary(toolArgs, maxScan);
            if (toolName == "grep") {
                std::string pattern = JsonTopLevelString(toolArgs, "pattern", maxScan);
                if (!pattern.empty())
                    return pattern;
            }
            if (toolName == "web_search" || toolName == "web_fetch") {
                std::string q = JsonTopLevelString(toolArgs, "query", maxScan);
                if (!q.empty())
                    return q;
                return JsonTopLevelString(toolArgs, "url", maxScan);
            }
            return JsonTopLevelString(toolArgs, "path", maxScan);
        };
        // Settled args never change: cache the extraction (full scan: the
        // key may sit past the streaming window).
        if (!c.fileResolved) {
            if (toolName == "bash")
                c.command = JsonTopLevelString(toolArgs, "command");
            c.file = summarize(std::string::npos);
            c.fileResolved = true;
        }
        if (!c.file.empty())
            args = c.file;
    }
    return args;
}

} // namespace viewrows
