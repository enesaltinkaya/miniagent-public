// Off-screen render test for the TUI frontend (PLAN_TUI.md §6): drives the
// same builders the fullscreen loop uses (tui::StyledRows / LogView /
// StatusLine / BottomBar) through an ftxui::Screen created with fixed
// dimensions — no terminal is ever attached. The App is seeded directly
// through uiMessages (public, like toolrows_test seeds the event queue).
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>
#include <vector>

#include "app.h"
#include "tui/rich_text_tui.h"
#include "tui/tui_view.h"

#include "ftxui/dom/elements.hpp"
#include "ftxui/dom/node.hpp"
#include "ftxui/screen/screen.hpp"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

static std::string renderText(ftxui::Element el, int w, int h) {
    auto screen =
        ftxui::Screen::Create(ftxui::Dimension::Fixed(w), ftxui::Dimension::Fixed(h));
    ftxui::Render(screen, el);
    std::string out;
    for (int y = 0; y < h; ++y) {
        std::string line;
        for (int x = 0; x < w; ++x) {
            const auto& c = screen.PixelAt(x, y);
            line += c.character.empty() ? " " : c.character;
        }
        while (!line.empty() && line.back() == ' ')
            line.pop_back();
        out += line;
        out += '\n';
    }
    return out;
}

static bool renderHas(ftxui::Element el, int w, int h, const std::string& needle) {
    return renderText(el, w, h).find(needle) != std::string::npos;
}

static std::vector<std::string> texts(const std::vector<tui::TuiRow>& rows) {
    std::vector<std::string> out;
    out.reserve(rows.size());
    for (const auto& r : rows) out.push_back(tui::RowText(r));
    return out;
}

static bool hasText(const std::vector<std::string>& v, const std::string& needle) {
    for (const auto& s : v)
        if (s.find(needle) != std::string::npos) return true;
    return false;
}

static std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const auto& s : v) {
        out += s;
        out += '\n';
    }
    return out;
}

static App::UiMessage msg(App::UiMessage::Kind k, const std::string& text) {
    App::UiMessage m;
    m.kind = k;
    m.text = text;
    return m;
}

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1);
    setenv("MINIAGENT_NO_PROBE", "1", 1);
    system("rm -rf /tmp/miniagent_tui_render_test && mkdir -p /tmp/miniagent_tui_render_test");
    chdir("/tmp/miniagent_tui_render_test");

    // ---- markdown-lite model -------------------------------------------
    {
        auto r = turich::rows("hello world foo", 10, "> ");
        check(r.size() == 3 && turich::rowText(r[0]) == "> hello" &&
                  turich::rowText(r[1]) == "  world" && turich::rowText(r[2]) == "  foo",
              "wrap at width with aligned continuation");
    }
    {
        auto r = turich::rows("abcdefghij", 4, "");
        check(r.size() == 3 && turich::rowText(r[0]) == "abcd" &&
                  turich::rowText(r[1]) == "efgh" && turich::rowText(r[2]) == "ij",
              "word longer than width hard-splits on glyph boundaries");
    }
    {
        auto r = turich::rows("use `foo bar` now", 40, "");
        check(r.size() == 1 && r[0].spans.size() == 3 &&
                  r[0].spans[1].sp == turich::Sp::Code &&
                  r[0].spans[1].text == "foo bar",
              "inline code is one word, spaces kept");
    }
    {
        auto r = turich::rows("**a b** c", 40, "");
        check(r[0].spans.size() == 4 && r[0].spans[0].sp == turich::Sp::Bold &&
                  r[0].spans[0].text == "a" && r[0].spans[2].sp == turich::Sp::Bold &&
                  r[0].spans[2].text == "b" && r[0].spans[3].sp == turich::Sp::Plain,
              "bold run splits into words");
    }
    {
        auto r = turich::rows("see [image 3] here", 40, "");
        bool ok = false;
        for (const auto& s : r[0].spans)
            if (s.sp == turich::Sp::Image && s.text == "[image 3]") ok = true;
        check(ok, "[image N] token parsed");
    }
    {
        auto r = turich::rows("```cpp\nint x;\n```\nafter", 40, "  ");
        check(r.size() == 2 && turich::rowText(r[0]) == "  int x;" &&
                  r[0].spans[0].sp == turich::Sp::Fence && turich::rowText(r[1]) == "  after",
              "fence markers dropped, fence body dim block, prefix kept");
    }
    {
        auto r = turich::rows("a\n\nb", 40, "");
        check(r.size() == 3 && turich::rowText(r[1]).empty(), "empty logical line stays a row");
    }

    // ---- StyledRows: kinds, tool rows, expand ---------------------------
    {
        App app;
        app.showThinking = true;
        app.uiMessages.push_back(msg(App::UiMessage::User, "hello there"));
        app.uiMessages.push_back(msg(App::UiMessage::Agent, "**bold** and `code`"));
        app.uiMessages.push_back(msg(App::UiMessage::Thinking, "pondering"));
        app.uiMessages.push_back(msg(App::UiMessage::Error, "boom"));
        app.uiMessages.push_back(msg(App::UiMessage::System, "loaded"));
        app.uiMessages.push_back(msg(App::UiMessage::Warning, "careful"));

        App::UiMessage t;
        t.kind = App::UiMessage::Tool;
        t.toolName = "read";
        t.toolArgs = "{\"path\": \"PLAN_TUI.md\"}";
        t.toolResult = "file contents\nsecond line";
        t.toolDone = true;
        t.toolElapsedMs = 9200;
        app.uiMessages.push_back(t);

        auto rows = texts(tui::StyledRows(app, 60));
        check(hasText(rows, "> hello there"), "user row keeps > prefix");
        check(hasText(rows, "  bold and code"), "agent markdown body rendered");
        check(hasText(rows, "  ~ pondering"), "thinking row carries ~ prefix");
        check(hasText(rows, "  !! boom"), "error row carries !! prefix");
        check(hasText(rows, "  · loaded"), "system row carries · prefix");
        check(hasText(rows, "  ! careful"), "warning row carries ! prefix");
        check(hasText(rows, "  • read PLAN_TUI.md  [9.2s] +"),
              "settled tool row: summary, duration, expand mark");
        check(!hasText(rows, "file contents"),
              "collapsed tool row shows no result body");

        auto styled = tui::StyledRows(app, 60);
        bool userWhite = false, errRed = false, sysBlue = false, warnYellow = false,
             toolCyan = false, thinkDim = false, codeCyan = false;
        for (const auto& row : styled)
            for (const auto& s : row) {
                if (s.text.find("hello there") != std::string::npos &&
                    s.fg == ftxui::Color::White)
                    userWhite = true;
                if (s.text.find("boom") != std::string::npos && s.fg == ftxui::Color::Red)
                    errRed = true;
                if (s.text.find("loaded") != std::string::npos && s.fg == ftxui::Color::Blue)
                    sysBlue = true;
                if (s.text.find("careful") != std::string::npos &&
                    s.fg == ftxui::Color::Yellow)
                    warnYellow = true;
                if (s.text == "read" && s.fg == ftxui::Color::Cyan && s.bold) toolCyan = true;
                if (s.text.find("pondering") != std::string::npos && s.dim && s.italic)
                    thinkDim = true;
                if (s.text == "code" && s.fg == ftxui::Color::Cyan) codeCyan = true;
            }
        check(userWhite, "user spans are white");
        check(errRed, "error spans are red");
        check(sysBlue, "system spans are blue");
        check(warnYellow, "warning spans are yellow");
        check(toolCyan, "tool name is bold cyan");
        check(thinkDim, "thinking spans are dim italic");
        check(codeCyan, "inline code spans are cyan");

        app.uiMessages.back().toolExpanded = true;
        rows = texts(tui::StyledRows(app, 60));
        check(hasText(rows, "−"), "expanded tool row carries the − mark");
        check(hasText(rows, "    file contents") && hasText(rows, "    second line"),
              "expanded tool row shows the body");
    }

    // ---- streaming tool row: live summary, never raw JSON ---------------
    {
        App app;
        App::UiMessage t;
        t.kind = App::UiMessage::Tool;
        t.toolName = "write";
        t.toolArgs = "{\"path\": \"src/ma";
        t.toolStreaming = true;
        t.toolStartMs = nowMs() - 100;
        app.uiMessages.push_back(t);
        auto rows = texts(tui::StyledRows(app, 60));
        check(hasText(rows, "  • write src/ma"), "streaming row shows live file summary");
        bool rawJson = false;
        for (const auto& s : rows)
            if (s.find("{\"path\"") != std::string::npos) rawJson = true;
        check(!rawJson, "streaming row never shows raw JSON");
    }

    // ---- off-screen LogView --------------------------------------------
    {
        App app;
        app.uiMessages.push_back(msg(App::UiMessage::User, "alpha"));
        app.uiMessages.push_back(msg(App::UiMessage::Agent, "beta"));

        auto screen =
            ftxui::Screen::Create(ftxui::Dimension::Fixed(40), ftxui::Dimension::Fixed(30));
        ftxui::Render(screen, tui::LogView(app, 40, 30, tui::ScrollState{}));
        std::string l1, l2;
        for (int x = 0; x < 40; ++x) {
            const auto& c1 = screen.PixelAt(x, 1);
            l1 += c1.character.empty() ? " " : c1.character;
            const auto& c2 = screen.PixelAt(x, 2);
            l2 += c2.character.empty() ? " " : c2.character;
        }
        check(l1.rfind("> alpha", 0) == 0, "LogView paints the user row");
        check(l2.rfind("  beta", 0) == 0, "LogView paints the agent row");
        check(screen.PixelAt(2, 1).foreground_color == ftxui::Color::White,
              "user row painted white");
    }

    // ---- StatusLine -----------------------------------------------------
    {
        App app;
        check(renderHas(tui::StatusLine(app, false), 60, 1, "Idle"), "idle state label");
        app.agentRunning = true;
        check(renderHas(tui::StatusLine(app, false), 60, 1, "Working"), "working state label");
        app.agentRunning = false;
        app.busyMsTotal = 65000;
        app.busyActive = false;
        app.lastTokPerS = 12.5;
        app.contextTokens = 45200;
        app.contextWindow = 131000;
        const std::string s = renderText(tui::StatusLine(app, false), 60, 1);
        check(s.find("1m 05s") != std::string::npos, "busy timer format");
        check(s.find("12.5 tok/s") != std::string::npos, "tok/s shown");
        check(s.find("45.20k / 131k") != std::string::npos, "context tokens vs window");
        app.scaffoldRunActive = true;
        app.scaffoldRole = "manager";
        check(renderHas(tui::StatusLine(app, false), 60, 1, "scaffold: manager"),
              "scaffold role shown while a run is up");
    }

    // ---- BottomBar ------------------------------------------------------
    {
        App app;
        app.model = "qwen3";
        app.backendName = "local";
        app.sessionUsage.totalTokens = 12345;
        auto rows = tui::StyledRows(app, 60); // app row model still works with empty log
        check(!rows.empty(), "empty log falls back to a placeholder row");
        check(renderHas(tui::BottomBar(app, 70), 70, 1, "qwen3 · 12.35K tokens · local"),
              "bottom line: model, tokens, backend");
        app.backendError = "backends.json corrupt";
        check(renderHas(tui::BottomBar(app, 70), 70, 1, "backends.json corrupt"),
              "backend error replaces the link text");
    }

    // ---- ShortCwd -------------------------------------------------------
    check(tui::ShortCwd("/a/b/c", 10) == "/a/b/c", "ShortCwd keeps short paths");
    check(tui::ShortCwd("/some/long/path", 8) == "…ng/path", "ShortCwd ellipsizes");

    // ---- overlays -------------------------------------------------
    {
        App app;
        SessionInfo s1;
        s1.path = "/tmp/s1.jsonl";
        s1.title = "first session";
        SessionInfo s2;
        s2.path = "/tmp/s2.jsonl";
        s2.title = "second session";
        app.sessionList = {s1, s2};
        auto entries = tui::SessionsEntries(app);
        check(entries.size() == 2 && entries[0].find("first session") != std::string::npos,
              "SessionsEntries lists cached sessions");

        tui::OverlayState st;
        st.kind = tui::OverlayState::Kind::Sessions;
        st.sel = 1;
        auto screen =
            ftxui::Screen::Create(ftxui::Dimension::Fixed(60), ftxui::Dimension::Fixed(24));
        ftxui::Render(screen, tui::Overlay(app, st, 60, 24));
        std::string all;
        bool selInverted = false;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 60; ++x) {
                const auto& c = screen.PixelAt(x, y);
                all += c.character.empty() ? " " : c.character;
                if (c.inverted) selInverted = true;
            }
        check(all.find("Sessions") != std::string::npos &&
                  all.find("second session") != std::string::npos,
              "Sessions overlay renders title and rows");
        check(selInverted, "selected overlay row is inverted");

        check(tui::TreeRows(app).empty(), "tree rows empty without an open session");

        app.backends.clear();
        app.backends.push_back({"local", "http://localhost:1234/v1", "k", "qwen"});
        app.backends.push_back({"remote", "https://x/v1", "k", "gpt"});
        app.backendIndex = 1;
        app.imageBackendIndex = 0;
        auto be = tui::BackendsEntries(app);
        check(be.size() == 2 && be[1].rfind("• ", 0) == 0 &&
                  be[0].find("[image]") != std::string::npos,
              "BackendsEntries marks selection and image model");

        app.mcpServers.resize(1);
        app.mcpServers[0].name = "fetch";
        app.mcpServers[0].enabled = true;
        app.mcpServers[0].running = false;
        auto me = tui::McpEntries(app);
        check(me.size() == 1 && me[0].find("[x] fetch") != std::string::npos &&
                  me[0].find("stopped") != std::string::npos,
              "McpEntries shows checkbox and state");

        auto help = tui::HelpEntries();
        bool hasQuit = false, hasNewline = false;
        for (const auto& h : help) {
            if (h.find("Ctrl+Q") != std::string::npos) hasQuit = true;
            if (h.find("Ctrl/Shift+Enter") != std::string::npos) hasNewline = true;
        }
        check(hasQuit, "HelpEntries documents quit");
        check(hasNewline, "HelpEntries documents the modified-Enter newline");
    }

    // ---- toast + blocked prompt -----------------------------------------
    {
        App app;
        app.showToast("Text copied");
        check(renderHas(tui::StatusLine(app, false), 60, 1, "Text copied"),
              "toast shows in the status strip");
        app.agentRunning = true;
        check(app.promptSendBlocked(), "prompt blocked while a run owns the app");
        check(renderHas(tui::StatusLine(app, false), 60, 1, "Esc stops"),
              "working state hints Esc");
    }

    // ---- M4: log model mapping + mode row ------------------------------
    {
        App app;
        app.uiMessages.push_back(msg(App::UiMessage::User, "alpha"));
        App::UiMessage t;
        t.kind = App::UiMessage::Tool;
        t.toolName = "read";
        t.toolArgs = "{\"path\": \"f\"}";
        t.toolDone = true;
        t.toolResult = "body";
        app.uiMessages.push_back(t);
        auto model = tui::BuildLog(app, 60);
        check(model.rows.size() == model.msgIndex.size(), "msgIndex parallel to rows");
        int userRow = -1, toolHead = -1;
        for (size_t i = 0; i < model.msgIndex.size(); ++i) {
            if (model.msgIndex[i] == 0 && tui::RowText(model.rows[i]).find("> alpha") == 0)
                userRow = (int)i;
            if (model.msgIndex[i] == 1 &&
                tui::RowText(model.rows[i]).find("• read") != std::string::npos)
                toolHead = (int)i;
        }
        check(userRow >= 0 && toolHead > userRow, "rows map back to their messages");

        check(renderHas(tui::ModeRow(app, nullptr), 90, 1, "[Scaffold]"),
              "mode row shows the inactive toggle bracketed");
        check(renderHas(tui::ModeRow(app, nullptr), 90, 1, "[Loop]"),
              "mode row shows the loop checkbox off");
        app.loopEnabled = true;
        check(renderHas(tui::ModeRow(app, nullptr), 90, 1, "Loop"),
              "mode row shows the loop checkbox on");
        app.loopEnabled = false;
        app.agentRunning = true;
        check(renderHas(tui::ModeRow(app, nullptr), 90, 1, "[Compact]"),
              "mode row shows Compact while a run is up");
        app.agentRunning = false;
        tui::ChromeHits hits;
        check(renderHas(tui::TopBar(app, 70, &hits), 70, 1, "[Sessions]"),
              "top bar buttons are bracketed labels");
    }

    // ---- M5: scaffold dispatch view + roles + resume -------------------
    {
        App app;
        app.scaffoldRunActive = true;
        app.scaffoldRole = "worker";
        const auto now = std::chrono::steady_clock::now();
        app.scaffoldRoleLog.push_back({"manager", now, 1500});
        app.scaffoldRoleLog.push_back({"worker", now, -1});
        auto mk = [&](int d, const char* who, const char* text) {
            App::UiMessage m;
            m.kind = App::UiMessage::Agent;
            m.nestedAgent = who;
            m.dispatch = d;
            m.text = text;
            app.uiMessages.push_back(m);
        };
        mk(0, "manager", "planning phase text");
        mk(1, "worker", "doing the work text");

        auto rows = texts(tui::StyledRows(app, 60));
        check(hasText(rows, "doing the work text") && !hasText(rows, "planning phase text"),
              "live dispatch shows a clean page");

        app.scaffoldViewDispatch = 0;
        rows = texts(tui::StyledRows(app, 60));
        check(hasText(rows, "planning phase text") && !hasText(rows, "doing the work text"),
              "pinned dispatch swaps in its transcript");

        app.scaffoldViewDispatch = -1;
        app.uiMessages.clear();
        rows = texts(tui::StyledRows(app, 60));
        check(hasText(rows, "No messages from this run yet."),
              "empty dispatch view carries its placeholder");
        auto roles = tui::RolesEntries(app);
        check(roles.size() == 2 && roles[1].find("worker") != std::string::npos &&
                  roles[1].find("▸") != std::string::npos,
              "RolesEntries marks the live dispatch");
        app.scaffoldRunActive = false;

        app.scaffoldMode = true;
        app.scaffoldResume.exists = true;
        app.scaffoldResume.resumable = true;
        app.scaffoldResume.task = "fix the build";
        check(renderHas(tui::ResumeRow(app), 60, 1, "[Continue]"),
              "resume row shows Continue for an interrupted ledger");
        app.scaffoldResume.resumable = false;
        app.scaffoldResume.continuable = false;
        app.scaffoldResume.finished = true;
        check(renderHas(tui::ResumeRow(app), 60, 1, "[Clear]"),
              "finished ledger shows Clear");
    }

    printf(failures ? "tui_render_test: %d FAILURES\n" : "tui_render_test: all ok\n", failures);
    return failures ? 1 : 0;
}
