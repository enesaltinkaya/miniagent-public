#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

// Renderer-neutral row model for the message log, shared by the desktop
// (ImGui) renderer and the TUI renderer (PLAN_TUI.md §3 item 3). Everything
// the two frontends must agree on about a log row lives here: the collapsed
// tool summary (target file / command extracted from the JSON args, live
// while the args are still streaming), the trim rule for text bodies, and
// the elapsed / error / duration-tag formatting of tool rows.
//
// This header must stay free of ImGui (and app.h) includes: it speaks plain
// data only. Callers hand the corresponding App::UiMessage fields over, so
// the per-row caches keep living in the message and both frontends update
// them through the same code. The functions are cheap only because of those
// caches - construct them from the message's own fields, not temporaries.

namespace viewrows {

// ---- plain-text helpers --------------------------------------------------

// Leading/trailing whitespace trimmed; agent and thinking bodies render
// trimmed so a stray newline does not draw an empty line.
std::string TrimMessage(const std::string& s);

// Bash rows collapse to a one-line command summary: newlines become spaces
// (the row is a single line) and the result is char-capped. The cap only
// bounds the per-frame text-measurement cost; the drawing code truncates
// with an ellipsis at the row width anyway.
std::string BashCommandSummary(const std::string& cmd);

// Top-level `"key": "value"` string extraction from a JSON object,
// hand-rolled (not a parser) so a JSON *prefix* - tool args still
// streaming - yields the still-open value as-is (it keeps growing and the
// next scan re-reads it). A complete value is returned unescaped. `maxScan`
// caps how far the scan runs, bounding the per-frame cost on long in-flight
// args; a key beyond the window simply is not summarized yet.
std::string JsonTopLevelString(const std::string& j, const char* key,
                               std::size_t maxScan = std::string::npos);

// Milliseconds -> compact duration string ("9.9s" / "45s" / "12m 34s" /
// "1h 20m"), shared by the tool rows' [elapsed] suffix and the scaffold
// subagent-log column.
inline std::string FormatDurationMs(int64_t ms) {
    const double s = (double)ms / 1000.0;
    char buf[32];
    if (s < 10.0)
        snprintf(buf, sizeof(buf), "%.1fs", s);
    else if (s < 60.0)
        snprintf(buf, sizeof(buf), "%ds", (int)(s + 0.5));
    else if (s < 3600.0)
        snprintf(buf, sizeof(buf), "%dm %02ds", (int)(s / 60.0), (int)s % 60);
    else
        snprintf(buf, sizeof(buf), "%dh %02dm", (int)(s / 3600.0), (int)(s / 60.0) % 60);
    return buf;
}

// ---- tool rows ------------------------------------------------------------

// A tool row switches from the plain name+args line to the live-elapsed
// treatment once it has been running this long (3 s).
inline constexpr int64_t kToolElapsedLineMs = 3000;

// While the args stream, the summary is re-derived from the in-flight
// fragment only when it grew, and the scan is bounded by this window so a
// huge in-flight args string (a long write's content) keeps the per-frame
// cost constant; a key beyond the window is not summarized until the row
// settles.
inline constexpr std::size_t kStreamScanWindow = 8192;

// Resolved time state of one tool row at render time. The caller passes the
// wall clock in (`nowEpochMs`, epoch ms) so this stays UI-free.
struct ToolRowElapsed {
    bool running = false;   // started, neither streaming nor finished
    int64_t elapsedMs = -1; // live (startMs -> now) or settled value; -1 unknown
    bool slow = false;      // elapsedMs >= kToolElapsedLineMs
    bool line = false;      // running && slow: draw the spinner/elapsed line
};
ToolRowElapsed toolRowElapsed(bool streaming, bool done, int64_t startMs,
                              int64_t settledElapsedMs, int64_t nowEpochMs);

// Status suffix of a tool row: "  [streaming...]" / "  [running...]" /
// "  [error]" / "  [error 12.3s]" / "  [12.3s]"; "" when there is none. The
// error form carries the duration only when the tool turned out slow.
std::string toolRowStatusSuffix(bool streaming, const ToolRowElapsed& e, bool done,
                                bool isError);

// Spinner phase of a running slow tool row (advances every 120 ms).
char toolSpinner(int64_t elapsedMs);

// Whether a tool row offers the expand toggle: a finished row with result
// text, or a bash row whose command summary is known (expandable while it
// runs, so the command preview is available early).
bool toolRowExpandable(bool done, bool streaming, const std::string& result,
                       const std::string& name, const std::string& command);

// The per-row summary caches. These mirror App::UiMessage's toolFile /
// toolCommand / toolFileResolved / toolFileScanSize fields one to one; bind
// them to the message's own fields so the caching survives across frames
// (a copy would re-scan every frame, defeating the point).
struct ToolSummaryCache {
    std::string& file;
    std::string& command;
    bool& fileResolved;
    std::size_t& fileScanSize;
};

// The args text a tool row shows after the tool name:
//   - streaming + collapsed: the live summary derived from the partial args
//     (never the raw fragment - a write streams the whole file content);
//     re-scanned only when the fragment grew, within kStreamScanWindow;
//   - settled + collapsed read/write/edit/grep/web_search/web_fetch, and
//     bash rows in either state: the cached summary (file path / pattern /
//     query / url / command), extracted once from the settled args (the
//     settled args never change, so the cache is never invalidated);
//   - everything else: the full one-line JSON args.
// Returns the text without the leading space the renderer inserts.
std::string toolArgsSummary(const std::string& toolName, const std::string& toolArgs,
                            bool expanded, bool streaming, ToolSummaryCache cache);

} // namespace viewrows
