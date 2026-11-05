#include "tui/tui_view.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include "agent/types.h"
#include "app.h"
#include "llm/backends.h"
#include "sessions/sessions.h"
#include "tui/rich_text_tui.h"
#include "ui/view_rows.h"
#include "util/image.h"

namespace tui {
namespace {

std::vector<TuiRow> RichRows(const std::string& text, int width, const std::string& prefix,
                             ftxui::Color base, bool dim = false, bool italic = false) {
    std::vector<TuiRow> out;
    for (const turich::Row& r : turich::rows(text, width, prefix)) {
        TuiRow row;
        row.reserve(r.spans.size());
        for (const turich::Span& s : r.spans) {
            TuiSpan t{s.text, base, false, dim, italic};
            switch (s.sp) {
            case turich::Sp::Plain:
                break;
            case turich::Sp::Bold:
                t.bold = true;
                break;
            case turich::Sp::Code:
                t.fg = ftxui::Color::Cyan;
                break;
            case turich::Sp::Image:
                t.fg = ftxui::Color::Yellow;
                break;
            case turich::Sp::Fence:
                t.fg = ftxui::Color::GrayDark;
                t.dim = true;
                break;
            }
            row.push_back(std::move(t));
        }
        out.push_back(std::move(row));
    }
    return out;
}

std::string FormatBusy(int64_t ms) {
    const int s = (int)(ms / 1000);
    char buf[32];
    if (s < 60)
        snprintf(buf, sizeof(buf), "%ds", s);
    else if (s < 3600)
        snprintf(buf, sizeof(buf), "%dm %02ds", s / 60, s % 60);
    else
        snprintf(buf, sizeof(buf), "%dh %02dm %02ds", s / 3600, (s % 3600) / 60, s % 60);
    return buf;
}

std::string FormatTokens(int n) {
    char buf[16];
    if (n >= 1000000)
        snprintf(buf, sizeof(buf), "%.2fM", n / 1000000.0);
    else if (n >= 1000)
        snprintf(buf, sizeof(buf), "%.2fK", n / 1000.0);
    else
        snprintf(buf, sizeof(buf), "%d", n);
    return buf;
}

std::string FormatCtx(int total, int window) {
    if (total <= 0) return "";
    char tok[12];
    if (total >= 1000)
        snprintf(tok, sizeof(tok), "%.2fk", total / 1000.0);
    else
        snprintf(tok, sizeof(tok), "%d", total);
    if (window > 0) {
        char win[12];
        if (window % 1000 == 0)
            snprintf(win, sizeof(win), "%dk", window / 1000);
        else
            snprintf(win, sizeof(win), "%.1fk", window / 1000.0);
        char out[28];
        snprintf(out, sizeof(out), "%s / %s", tok, win);
        return out;
    }
    return tok;
}

std::string SpeedText(App& app) {
    char buf[32] = "";
    if (app.agentRunning && app.liveTokPerS > 0.0)
        snprintf(buf, sizeof(buf), "%.1f tok/s", app.liveTokPerS);
    if (buf[0] == 0 && app.lastTokPerS > 0.0)
        snprintf(buf, sizeof(buf), "%.1f tok/s", app.lastTokPerS);
    return buf;
}

std::string CtxText(App& app) {
    const int live = app.streamedUsageOutput > 0
                         ? app.streamedUsageOutput
                         : (int)((app.streamedChars + 3) / 4);
    return FormatCtx(app.contextTokens + live, app.contextWindow);
}

} // namespace

std::string RowText(const TuiRow& row) {
    std::string out;
    for (const TuiSpan& s : row) out += s.text;
    return out;
}

ftxui::Element RenderRow(const TuiRow& row) {
    if (row.empty()) return ftxui::text("");
    ftxui::Elements els;
    els.reserve(row.size());
    for (const TuiSpan& s : row) {
        ftxui::Element e = ftxui::text(s.text);
        if (s.fg != ftxui::Color::Default) e = e | ftxui::color(s.fg);
        if (s.bold) e = e | ftxui::bold;
        if (s.dim) e = e | ftxui::dim;
        if (s.italic) e = e | ftxui::italic;
        els.push_back(std::move(e));
    }
    if (els.size() == 1) return std::move(els[0]);
    return ftxui::hbox(std::move(els));
}

std::string ShortCwd(const std::string& cwd, int max) {
    if ((int)cwd.size() <= max) return cwd;
    if (max <= 1) return "…";
    return "…" + cwd.substr(cwd.size() - (size_t)max + 1);
}

std::vector<TuiRow> StyledRows(App& app, int width) {
    return BuildLog(app, width).rows;
}

LogModel BuildLog(App& app, int width) {
    LogModel model;
    auto& rows = model.rows;
    auto& msgOf = model.msgIndex;
    auto push = [&](TuiRow row, int msg) {
        rows.push_back(std::move(row));
        msgOf.push_back(msg);
    };
    // Scaffold-run view (M5, desktop parity): while a dispatch is live the
    // log shows only that dispatch's rows - every dispatch that starts
    // gets a clean page. The pinned scaffoldViewDispatch swaps in an
    // earlier dispatch's transcript; the parent's own phases and idle
    // time show the full log. Rows carry the dispatch id, so repeated
    // roles (manager between worker rounds) are told apart.
    const int viewDispatch = app.scaffoldDisplayDispatch();
    const int64_t now = nowMs();
    for (size_t mi = 0; mi < app.uiMessages.size(); ++mi) {
        auto& m = app.uiMessages[mi];
        if (viewDispatch >= 0 && m.dispatch != viewDispatch) continue;
        const std::string ind = m.nestedAgent.empty() ? "  " : "    ";
        switch (m.kind) {
        case App::UiMessage::User: {
            push({}, -1);
            auto r = RichRows(m.text, width, "> ", ftxui::Color::White);
            for (auto& row : r) push(std::move(row), (int)mi);
            break;
        }
        case App::UiMessage::Agent:
            if (m.subagentHeader) {
                push({}, -1);
                push({{ind + m.text}}, (int)mi);
            } else if (!m.text.empty()) {
                auto r = RichRows(viewrows::TrimMessage(m.text), width, ind,
                                  ftxui::Color::Default);
                for (auto& row : r) push(std::move(row), (int)mi);
            }
            break;
        case App::UiMessage::Thinking:
            if (!app.showThinking) {
                push({{ind + "~ " +
                           (m.nestedAgent.empty() ? std::string("thinking")
                                                  : m.nestedAgent) + "…",
                       ftxui::Color::Default, false, true, true}},
                     (int)mi);
                break;
            }
            {
                auto r = RichRows(viewrows::TrimMessage(m.text), width, ind + "~ ",
                                  ftxui::Color::Default, true, true);
                for (auto& row : r) push(std::move(row), (int)mi);
            }
            break;
        case App::UiMessage::Tool: {
            const viewrows::ToolRowElapsed e = viewrows::toolRowElapsed(
                m.toolStreaming, m.toolDone, m.toolStartMs, m.toolElapsedMs, now);
            const std::string summary =
                viewrows::toolArgsSummary(m.toolName, m.toolArgs, m.toolExpanded,
                                          m.toolStreaming,
                                          {m.toolFile, m.toolCommand, m.toolFileResolved,
                                           m.toolFileScanSize});
            const std::string status = viewrows::toolRowStatusSuffix(
                m.toolStreaming, e, m.toolDone, m.toolIsError);
            const bool expandable = viewrows::toolRowExpandable(
                m.toolDone, m.toolStreaming, m.toolResult, m.toolName, m.toolCommand);
            TuiRow head;
            head.push_back({ind + "• ", ftxui::Color::Cyan});
            head.push_back({m.toolName, ftxui::Color::Cyan, true});
            if (!summary.empty()) head.push_back({" " + summary, ftxui::Color::Cyan});
            if (!status.empty()) {
                TuiSpan st{status, ftxui::Color::Default, false, !m.toolIsError};
                if (m.toolIsError) st.fg = ftxui::Color::Red;
                head.push_back(std::move(st));
            }
            if (expandable)
                head.push_back({std::string(" ") + (m.toolExpanded ? "−" : "+"),
                                ftxui::Color::GrayLight, false, true});
            push(std::move(head), (int)mi);
            if (e.line) {
                char buf[48];
                snprintf(buf, sizeof(buf), "%c %s", viewrows::toolSpinner(e.elapsedMs),
                         viewrows::FormatDurationMs(e.elapsedMs).c_str());
                push({{ind + "  " + buf, ftxui::Color::Default, false, true}}, (int)mi);
            }
            if (m.toolExpanded && !m.toolResult.empty()) {
                auto r = RichRows(m.toolResult, width, ind + "  ", ftxui::Color::Default);
                for (auto& row : r) push(std::move(row), (int)mi);
            }
            if (m.toolExpanded && !m.toolImagePath.empty()) {
                std::string line = "image: " + m.toolImagePath;
                int iw = 0, ih = 0;
                if (probeImageSize(m.toolImagePath, iw, ih)) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), " (%dx%d)", iw, ih);
                    line += buf;
                }
                push({{ind + "  " + line, ftxui::Color::Yellow, false, true}}, (int)mi);
            }
            break;
        }
        case App::UiMessage::Error: {
            auto r = RichRows(viewrows::TrimMessage(m.text), width, ind + "!! ",
                              ftxui::Color::Red);
            for (auto& row : r) push(std::move(row), (int)mi);
            break;
        }
        case App::UiMessage::System: {
            auto r = RichRows(viewrows::TrimMessage(m.text), width, ind + "· ",
                              ftxui::Color::Blue);
            for (auto& row : r) push(std::move(row), (int)mi);
            break;
        }
        case App::UiMessage::Warning: {
            auto r = RichRows(viewrows::TrimMessage(m.text), width, ind + "! ",
                              ftxui::Color::Yellow);
            for (auto& row : r) push(std::move(row), (int)mi);
            break;
        }
        }
    }
    if (rows.empty())
        push({{viewDispatch >= 0 ? "No messages from this run yet."
                                 : "no messages yet",
               ftxui::Color::Default, false, true}},
             -1);
    return model;
}

ftxui::Element TopBar(App& app, int width, ChromeHits* hits) {
    // M4: bracketed labels are click targets (hit boxes reflected into
    // `hits`); the keyboard chords from Help remain as alternates.
    auto btn = [](const std::string& label, ftxui::Box* box) {
        ftxui::Element e = ftxui::text("[" + label + "]") | ftxui::dim;
        if (box) e = e | ftxui::reflect(*box);
        return e;
    };
    const std::string mode = app.scaffoldMode ? "Scaffold" : "Simple";
    const std::string right = "[Sessions] [MCP] [Backends] [New] [Help]";
    const int room = std::max(0, width - (int)right.size() - 3);
    std::string left = ShortCwd(app.cwd, std::max(0, room - (int)mode.size() - 3)) + " · " + mode;
    return ftxui::hbox({
        ftxui::text(left),
        ftxui::filler(),
        ftxui::hbox({
            btn("Sessions", hits ? &hits->sessions : nullptr),
            ftxui::text(" "),
            btn("MCP", hits ? &hits->mcp : nullptr),
            ftxui::text(" "),
            btn("Backends", hits ? &hits->backends : nullptr),
            ftxui::text(" "),
            btn("New", hits ? &hits->newBtn : nullptr),
            ftxui::text(" "),
            btn("Help", hits ? &hits->help : nullptr),
        }),
    });
}

ftxui::Element LogView(App& app, int width, int height, const ScrollState& scroll,
                       std::vector<int>* rowMsg, int* firstRow) {
    LogModel model = BuildLog(app, width);
    const int n = (int)model.rows.size();
    const int h = height > 0 ? height : std::min(n, 20);
    int start = std::max(0, n - h - std::max(0, scroll.offsetLines));
    const int end = std::min(n, start + h);
    ftxui::Elements els;
    for (int i = start; i < end; ++i) els.push_back(RenderRow(model.rows[i]));
    if (els.empty()) els.push_back(ftxui::text(""));
    if (rowMsg) {
        rowMsg->clear();
        rowMsg->insert(rowMsg->end(), model.msgIndex.begin() + start,
                       model.msgIndex.begin() + end);
    }
    if (firstRow) *firstRow = start;
    return ftxui::vbox(std::move(els)) | ftxui::flex;
}

ftxui::Element StatusLine(App& app, bool quitArmed) {
    std::string label;
    std::string bullet = "○";
    ftxui::Color stateCol = ftxui::Color::GrayLight;
    if (app.scaffoldRunActive && !app.scaffoldRole.empty()) {
        bullet = "●";
        label = "scaffold: " + app.scaffoldRole;
        stateCol = ftxui::Color::GreenLight;
    } else if (app.agentRunning) {
        bullet = "●";
        static const char* dots[] = {"", ".", "..", "..."};
        label = std::string("Working") + dots[(nowMs() / 500) % 4];
        stateCol = ftxui::Color::GreenLight;
    } else {
        label = "Idle";
    }
    // The scaffold harness gate is a build/test run with no model output, so it
    // owns the label with its own elapsed time (the command is in its log row).
    if (app.agentRunning && app.verifyRunning) {
        bullet   = "●";
        label    = "verification " + FormatBusy(app.verifyElapsedMs());
        stateCol = ftxui::Color::Yellow;
    }
    int64_t busy = app.busyMsTotal;
    if (app.busyActive)
        busy += std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - app.busyStart)
                    .count();
    std::vector<std::string> fields;
    if (busy > 0) fields.push_back(FormatBusy(busy));
    const std::string speed = SpeedText(app);
    if (!speed.empty()) fields.push_back(speed);
    const std::string ctx = CtxText(app);
    if (!ctx.empty()) fields.push_back(ctx);
    std::string stats;
    for (size_t i = 0; i < fields.size(); ++i) {
        if (i) stats += " · ";
        stats += fields[i];
    }
    ftxui::Elements left;
    left.push_back(ftxui::text(bullet + " " + label) | ftxui::color(stateCol));
    if (!stats.empty()) left.push_back(ftxui::text("  " + stats) | ftxui::dim);
    if (app.agentRunning) left.push_back(ftxui::text("  · Esc stops") | ftxui::dim);
    const bool toastUp = !app.toast.text.empty() &&
                         app.toast.until > std::chrono::steady_clock::now();
    ftxui::Element right = ftxui::filler();
    if (quitArmed)
        right = ftxui::text("run active · Ctrl+Q again to quit") | ftxui::dim;
    else if (toastUp)
        right = ftxui::text(app.toast.text) | ftxui::color(ftxui::Color::Yellow);
    return ftxui::hbox({
        ftxui::hbox(std::move(left)),
        ftxui::filler(),
        std::move(right),
    });
}

ftxui::Element ModeRow(App& app, ChromeHits* hits) {
    auto toggle = [](bool active, const std::string& label, ftxui::Box* box) {
        ftxui::Element e = active ? ftxui::text(" " + label + " ") | ftxui::inverted
                                  : ftxui::text("[" + label + "]") | ftxui::dim;
        if (box) e = e | ftxui::reflect(*box);
        return e;
    };
    ftxui::Elements els;
    els.push_back(ftxui::text(" [Enter] Send") | ftxui::dim);
    els.push_back(toggle(!app.scaffoldMode, "Simple", hits ? &hits->modeSimple : nullptr));
    els.push_back(ftxui::text(" "));
    els.push_back(toggle(app.scaffoldMode, "Scaffold", hits ? &hits->modeScaffold : nullptr));
    // Loop: re-send the same prompt after a pause once a run settles. The
    // label stays fixed so the hit box does not jump while the countdown
    // ticks; the remaining seconds follow it.
    els.push_back(ftxui::text(" "));
    els.push_back(toggle(app.loopEnabled, "Loop", hits ? &hits->loop : nullptr));
    if (app.loopArmed)
        els.push_back(ftxui::text(" " + std::to_string(app.loopSecondsLeft()) + "s") |
                      ftxui::dim);
    if (app.agentRunning || app.canCompactIdle()) {
        ftxui::Element c = ftxui::text("[Compact]") | ftxui::dim;
        if (hits) c = c | ftxui::reflect(hits->compact);
        els.push_back(ftxui::text(" "));
        els.push_back(std::move(c));
    }
    els.push_back(ftxui::filler());
    els.push_back(ftxui::text("Ctrl+G keys · PgUp/PgDn scroll · Ctrl+Q quit") | ftxui::dim);
    return ftxui::hbox(std::move(els));
}

ftxui::Element BottomBar(App& app, int width, ftxui::Box* backendLink) {
    const bool err = !app.backendError.empty();
    std::string prefixText;
    if (err) {
        prefixText = app.backendError;
    } else {
        const std::string tok = FormatTokens(app.sessionUsage.totalTokens);
        if (app.model == app.backendName)
            prefixText = tok + " tokens · ";
        else
            prefixText = app.model + " · " + tok + " tokens · ";
    }
    const std::string rightText = prefixText + (err ? "" : app.backendName);
    const int room = std::max(0, width - (int)rightText.size() - 2);
    ftxui::Element right = err
        ? ftxui::text(rightText) | ftxui::color(ftxui::Color::Red)
        : ftxui::hbox({
              ftxui::text(prefixText) | ftxui::dim,
              [&] {
                  ftxui::Element e =
                      ftxui::text(app.backendName) | ftxui::color(ftxui::Color::Cyan);
                  if (backendLink) e = e | ftxui::reflect(*backendLink);
                  return e;
              }(),
          });
    return ftxui::hbox({
        ftxui::text(ShortCwd(app.cwd, room)) | ftxui::dim,
        ftxui::filler(),
        std::move(right),
    });
}

// Scaffold dispatch log entries (the subagent-log column parity): one row
// per dispatch, "• " marks the pinned review dispatch, "▸ " the live one;
// running roles show the live elapsed time.
std::string OneLine(const std::string& s, size_t maxChars);
std::vector<std::string> RolesEntries(App& app) {
    std::vector<std::string> out;
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < app.scaffoldRoleLog.size(); ++i) {
        const App::ScaffoldRoleEntry& e = app.scaffoldRoleLog[i];
        int64_t ms = e.elapsedMs;
        if (ms < 0)
            ms = std::max<int64_t>(
                0, std::chrono::duration_cast<std::chrono::milliseconds>(now - e.start).count());
        const char* mark = "  ";
        if ((int)i == app.scaffoldViewDispatch)
            mark = "\u2022 ";
        else if (e.elapsedMs < 0 && (int)i == app.liveDispatchIndex())
            mark = "\u25b8 ";
        out.push_back(std::string("#") + std::to_string(i) + " " + mark + e.role + "  " +
                      viewrows::FormatDurationMs(ms));
    }
    if (out.empty()) out.push_back("No scaffold run in this session yet.");
    return out;
}

// Idle Scaffold-mode ledger affordance (the desktop's resume row): the
// saved task and Continue / New Session buttons, or a Clear button for a
// finished/stale ledger. Returns an empty element when nothing applies.
ftxui::Element ResumeRow(App& app, ftxui::Box* cont, ftxui::Box* newSess) {
    if (!app.scaffoldMode || app.agentRunning) return ftxui::text("");
    const auto& r = app.scaffoldResume;
    if (!r.exists) return ftxui::text("");
    const bool resumable = r.resumable;
    const bool continuable = r.continuable;
    if (!resumable && !continuable) {
        ftxui::Element clear = ftxui::text("[Clear]") | ftxui::dim;
        if (newSess) clear = clear | ftxui::reflect(*newSess);
        return ftxui::hbox({
            ftxui::text(" " + std::string(r.finished ? "finished" : "stale ledger")) | ftxui::dim,
            ftxui::text(" "),
            std::move(clear),
        });
    }
    ftxui::Element c = ftxui::text("[Continue]") | ftxui::bold;
    if (cont) c = c | ftxui::reflect(*cont);
    ftxui::Element n = ftxui::text("[New Session]") | ftxui::dim;
    if (newSess) n = n | ftxui::reflect(*newSess);
    std::string task = OneLine(r.task, 48);
    if (task.empty()) task = "(saved phase: " + r.phase + ")";
    return ftxui::hbox({
        ftxui::text(" " + task) | ftxui::dim,
        ftxui::text("  "),
        std::move(c),
        ftxui::text(" "),
        std::move(n),
    });
}

// ---- overlays ------------------------------------------------------------

std::string OneLine(const std::string& s, size_t maxChars) {
    std::string out;
    out.reserve(std::min(s.size(), maxChars));
    for (char c : s) {
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        if (out.size() >= maxChars) {
            out += "…";
            return out;
        }
        out += c;
    }
    return out;
}

std::vector<std::string> SessionsEntries(App& app) {
    std::vector<std::string> out;
    const std::string& cur = app.sessions ? app.sessions->currentPath() : std::string();
    for (const SessionInfo& s : app.sessionList) {
        std::string t = s.title.empty() ? "(untitled)" : s.title;
        if (s.path == cur) t = "• " + t;
        out.push_back(OneLine(t, 72));
    }
    if (out.empty()) out.push_back("No sessions yet. Sending a prompt starts one.");
    return out;
}

std::vector<std::string> BackendsEntries(App& app) {
    std::vector<std::string> out;
    for (size_t i = 0; i < app.backends.size(); ++i) {
        const LlmBackend& b = app.backends[i];
        std::string mark = (i == app.backendIndex) ? "• " : "  ";
        if (i == app.imageBackendIndex) mark += "[image] ";
        out.push_back(OneLine(mark + b.name + " · " + b.url + " · " + b.model, 72));
    }
    if (out.empty()) {
        out.push_back("No backends configured. Add entries to:");
        out.push_back(backendsPath());
        out.push_back("then restart miniagent to pick them up.");
    }
    return out;
}

std::vector<std::string> McpEntries(App& app) {
    std::vector<std::string> out;
    for (const McpServer& s : app.mcpServers) {
        const char* state =
            s.running ? "running" : (s.connecting && *s.connecting ? "connecting" : "stopped");
        std::string tools =
            s.running && !s.tools.empty() ? "  (" + std::to_string(s.tools.size()) + " tools)" : "";
        std::string status = s.status.empty() ? "" : "  · " + OneLine(s.status, 40);
        out.push_back(OneLine(std::string(s.enabled ? "[x] " : "[ ] ") + s.name + " · " +
                               state + tools + status,
                               72));
    }
    if (out.empty()) {
        out.push_back("No MCP servers configured. Add entries to:");
        out.push_back("~/.config/miniagent/mcp.json");
        out.push_back("then restart miniagent to pick them up.");
    }
    return out;
}

std::vector<TreeRow> TreeRows(App& app) {
    std::vector<TreeRow> out;
    if (!app.sessions || !app.sessions->hasOpenSession()) return out;
    const std::vector<AgentMessage> msgs =
        SessionStore::loadMessages(app.sessions->currentPath());
    for (const AgentMessage& m : msgs) {
        const char* tag = "?";
        std::string preview;
        bool user = false;
        if (const auto* u = std::get_if<UserMessage>(&m)) {
            tag = "User";
            preview = u->text;
            user = true;
        } else if (const auto* a = std::get_if<AssistantMessage>(&m)) {
            tag = a->toolCalls.empty() ? "Agent" : "Agent→tools";
            preview = a->text.empty() ? "…" : a->text;
        } else if (const auto* t = std::get_if<ToolResultMessage>(&m)) {
            tag = "Tool";
            preview = t->toolName + (t->args.empty() ? std::string{} : " " + t->args);
        }
        const int64_t ms = std::visit([](const auto& v) -> int64_t { return v.timestamp; }, m);
        char ts[16];
        time_t sec = (time_t)(ms / 1000);
        struct tm tmv;
        localtime_r(&sec, &tmv);
        snprintf(ts, sizeof(ts), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
        out.push_back({std::string(ts) + "  " + tag + "  " + OneLine(preview, 64), user});
    }
    return out;
}

std::vector<std::string> HelpEntries() {
    return {
        "Enter        send the prompt (Ctrl/Shift+Enter inserts a newline;",
        "             kitty-protocol terminal, or tmux with extended-keys on)",
        "Up/Down      prompt history (from an empty line / last line)",
        "PgUp/PgDn    scroll the message log (0 = follow the bottom)",
        "Ctrl+O       collapse/expand tool outputs",
        "Ctrl+T       show/hide thinking blocks",
        "Ctrl+N       new session",
        "Ctrl+S       sessions overlay (Enter open, d delete)",
        "Ctrl+B       backends overlay (Enter select, i image model)",
        "Ctrl+P       MCP servers overlay (Space toggle)",
        "Ctrl+R       scaffold roles overlay (pin a dispatch for review)",
        "Ctrl+G       this help",
        "Esc          close overlay · stop the run · double-Esc session tree",
        "Ctrl+Q       quit (press twice while a run is active)",
        "/image PATH  attach an image to the prompt as [image N]",
        "/new, /skills, /skill:name …   slash commands, sent as prompts",
    };
}

ftxui::Element Overlay(App& app, const OverlayState& st, int width, int height,
                       OverlayHits* hits) {
    std::string title;
    std::vector<std::string> rows;
    std::vector<std::string> footer;
    switch (st.kind) {
    case OverlayState::Kind::Sessions:
        title = "Sessions";
        rows = SessionsEntries(app);
        footer = {"Enter open · d delete · Esc close"};
        break;
    case OverlayState::Kind::Backends:
        title = "Backends";
        rows = BackendsEntries(app);
        footer = {"Enter select · i designates the image model · Esc close"};
        break;
    case OverlayState::Kind::Mcp:
        title = "MCP servers";
        rows = McpEntries(app);
        footer = {"Space/Enter toggle · new mcp.json entries need a restart"};
        break;
    case OverlayState::Kind::Help:
        title = "Help";
        rows = HelpEntries();
        footer = {"Esc close"};
        break;
    case OverlayState::Kind::Roles:
        title = "Scaffold roles";
        rows = RolesEntries(app);
        footer = {"Enter pins a dispatch for review · pin the live one again to "
                  "unpin · Esc close"};
        break;
    case OverlayState::Kind::Tree: {
        title = "Session Tree";
        for (const TreeRow& r : TreeRows(app)) rows.push_back(r.label);
        if (rows.empty()) rows.push_back("No messages in this session.");
        footer = {"Enter rewinds (user messages return to the prompt) · Esc close"};
        break;
    }
    case OverlayState::Kind::None:
        return ftxui::text("");
    }

    const int n = (int)rows.size();
    const int maxVis = std::max(4, height / 2 - 3);
    int sel = std::min(std::max(0, st.sel), std::max(0, n - 1));
    int first = std::max(0, std::min(sel - maxVis + 1, n - maxVis));
    if (n <= maxVis) first = 0;
    const int last = std::min(n, first + maxVis);

    ftxui::Elements list;
    if (hits) hits->rowBoxes.assign(std::max(0, last - first), ftxui::Box{});
    for (int i = first; i < last; ++i) {
        ftxui::Element e = ftxui::text(rows[i]);
        if (i == sel) e = e | ftxui::inverted;
        if (hits) e = e | ftxui::reflect(hits->rowBoxes[i - first]);
        list.push_back(std::move(e));
    }
    if (list.empty()) list.push_back(ftxui::text("") | ftxui::dim);
    if (hits) {
        hits->firstVisible = first;
        hits->visible = last - first;
    }

    ftxui::Elements top;
    top.push_back(ftxui::text(title) | ftxui::bold);
    if (n > maxVis && last < n) {
        top.push_back(ftxui::filler());
        top.push_back(ftxui::text("↓ more") | ftxui::dim);
    }

    ftxui::Elements body;
    body.push_back(ftxui::hbox(std::move(top)));
    body.push_back(ftxui::separator());
    for (auto& e : list) body.push_back(std::move(e));
    body.push_back(ftxui::separator());
    for (const std::string& f : footer)
        body.push_back(ftxui::text(f) | ftxui::dim);

    ftxui::Element panel = ftxui::vbox(std::move(body)) | ftxui::border |
                           ftxui::size(ftxui::WIDTH, ftxui::LESS_THAN,
                                       std::max(24, width - 4));
    if (hits) panel = std::move(panel) | ftxui::reflect(hits->panel);
    return panel;
}

} // namespace tui
