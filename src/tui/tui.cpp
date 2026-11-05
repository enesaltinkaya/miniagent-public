#include "tui/tui.h"

#include "agent/types.h"
#include "app.h"
#include "tui/tui_view.h"
#include "ui/view_rows.h"
#include "util/image.h"

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_options.hpp"
#include "ftxui/component/event.hpp"
#include "ftxui/component/mouse.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace tui {
namespace {

// A Ctrl+Q / Ctrl+C press during an active run arms the quit instead of
// quitting; a second press inside this window goes through.
constexpr auto kQuitConfirmWindow = std::chrono::seconds(4);
constexpr auto kTickInterval = std::chrono::milliseconds(100);

// Everything the event handler and the renderer share. Owned by
// runFrontend; never touched from the ticker thread. Reflected boxes and
// the visible-row maps are written by the renderer each frame and read by
// the mouse handler on the next event (boxes finalize during Render).
struct TuiRuntime {
    App& app;
    ftxui::ScreenInteractive& screen;
    ftxui::Component input;      // the prompt editor
    std::string* editor;         // its content (InputOption::content)
    int* cursor;                 // its cursor position
    ScrollState scroll;          // log viewport scroll (0 = auto-follow)
    ftxui::Box logBox{};         // reflected box of the log viewport
    ftxui::Box inputBox{};       // reflected box of the prompt editor
    std::vector<int> rowMsg;     // visible log rows -> uiMessages index
    int logFirstRow = 0;         // visible window's first row index
    ChromeHits hits{};           // clickable chrome boxes
    ftxui::Box backendLink{};    // clickable backend name in the bottom line
    ftxui::Box resumeCont{};     // idle-scaffold Continue button
    ftxui::Box resumeNew{};      // idle-scaffold New Session / Clear button
    OverlayState overlay;        // open overlay (None = closed)
    bool overlayShown = false;   // flag ftxui::Modal reads at render time
    OverlayHits overlayHits{};
    int histIdx = -1;            // prompt history recall (-1 = not browsing)
    std::string draft;           // the in-progress prompt saved on recall
    std::chrono::steady_clock::time_point quitArmedAt{};
    ftxui::Closure quit;         // screen.ExitLoopClosure()

    bool QuitArmed() const {
        return quitArmedAt.time_since_epoch().count() != 0 &&
               std::chrono::steady_clock::now() - quitArmedAt <= kQuitConfirmWindow;
    }
};

bool InBox(const ftxui::Box& b, int x, int y) {
    return x >= b.x_min && x <= b.x_max && y >= b.y_min && y <= b.y_max;
}

// FTXUI's parser uniformizes the terminal's \r (Enter) to \n, which makes
// Event::Return, Event::CtrlJ and every \n indistinguishable — a soft
// newline chord cannot exist on the legacy encoding. We therefore push an
// extended-keys mode around the loop (MINIAGENT_NO_KITTY_KEYS=1 skips it):
// outside tmux that is the kitty keyboard protocol's "disambiguate escape
// codes" flag; supporting terminals keep traditional bytes for
// Enter/Ctrl+letters but send CSI u forms for key combos that have no legacy
// encoding — Ctrl/Shift/Alt+Enter as CSI 13;<mods>u, a lone Esc as CSI 27u.
// Under tmux the kitty push is dropped by tmux's output parser, so we push
// xterm modifyOtherKeys mode 2 instead: tmux honors it (needs the server
// option extended-keys on) and re-encodes modified Enter for the pane in the
// same CSI u form (or CSI 27;<mods>;13~ when extended-keys-format is xterm).
// Terminals/multiplexers without the protocols ignore the pushes.
constexpr char kKittyKeysPush[] = "\x1b[>1u";
constexpr char kKittyKeysPop[] = "\x1b[<u";
constexpr char kModifyOtherKeysPush[] = "\x1b[>4;2m";
constexpr char kModifyOtherKeysPop[] = "\x1b[>4;0m";

bool KittyKeysEnabled() { return std::getenv("MINIAGENT_NO_KITTY_KEYS") == nullptr; }

bool InTmux() { return std::getenv("TMUX") != nullptr; }

bool IsEscape(const ftxui::Event& ev) {
    return ev == ftxui::Event::Escape || ev.input() == "\x1b[27u";
}

bool IsSoftNewline(const ftxui::Event& ev) {
    const std::string& in = ev.input();
    return in == "\x1b[13;2u" || in == "\x1b[13;3u" || in == "\x1b[13;5u" ||
           in == "\x1b[13;6u" || in == "\x1b[13;1;2u" || in == "\x1b[13;1;5u" ||
           in == "\x1b[27;2;13~" || in == "\x1b[27;3;13~" ||
           in == "\x1b[27;5;13~" || in == "\x1b[27;6;13~";
}

// The overlay's Enter action, shared by the key and the click path.
void OverlayActivate(TuiRuntime& rt, const std::vector<TreeRow>& tree) {
    App& app = rt.app;
    const int sel = rt.overlay.sel;
    switch (rt.overlay.kind) {
    case OverlayState::Kind::Sessions:
        if (!app.agentRunning && sel < (int)app.sessionList.size())
            app.openSession(app.sessionList[sel].path);
        break;
    case OverlayState::Kind::Backends:
        if (sel < (int)app.backends.size()) app.setBackend(sel);
        break;
    case OverlayState::Kind::Mcp:
        if (sel < (int)app.mcpServers.size())
            app.setMcpServerEnabled(sel, !app.mcpServers[sel].enabled);
        break;
    case OverlayState::Kind::Tree:
        if (!app.agentRunning && sel < (int)tree.size()) {
            if (tree[sel].user)
                app.rewindToUserMessage(sel);
            else
                app.rewindToMessage(sel);
        }
        break;
    case OverlayState::Kind::Roles:
        if (app.scaffoldRunActive && sel < (int)app.scaffoldRoleLog.size()) {
            app.scaffoldViewDispatch = (app.scaffoldViewDispatch == sel) ? -1 : sel;
            app.msgScrollToTop = app.scaffoldViewDispatch >= 0;
        }
        break;
    case OverlayState::Kind::Help:
        break;
    case OverlayState::Kind::None:
        break;
    }
}

// Mouse routing (M4): wheel scrolls the log (un-pins auto-follow); left
// press activates the top-bar / mode / compact / backend-link buttons,
// overlay rows (select + activate), and tool rows (expand toggle); clicks
// inside the prompt editor pass through so the Input can move the cursor.
// Middle/right buttons stay with the terminal (native selection etc.).
bool HandleMouse(TuiRuntime& rt, const ftxui::Event& ev) {
    App& app = rt.app;
    const ftxui::Mouse& m = const_cast<ftxui::Event&>(ev).mouse();

    auto openOverlay = [&](OverlayState::Kind kind) {
        rt.overlay = OverlayState{kind, 0};
        rt.overlayShown = true;
        if (kind == OverlayState::Kind::Sessions) app.refreshSessionList();
    };
    auto closeOverlay = [&]() {
        if (rt.overlay.kind == OverlayState::Kind::Tree) {
            app.treeOpen = false;
            app.treeSelection = -1;
        }
        rt.overlay = OverlayState{};
        rt.overlayShown = false;
    };

    if (m.button == ftxui::Mouse::WheelUp || m.button == ftxui::Mouse::WheelDown) {
        if (InBox(rt.logBox, m.x, m.y)) {
            if (m.button == ftxui::Mouse::WheelUp)
                rt.scroll.offsetLines += 3;
            else
                rt.scroll.offsetLines = std::max(0, rt.scroll.offsetLines - 3);
            return true;
        }
        return false;
    }
    if (m.motion != ftxui::Mouse::Pressed || m.button != ftxui::Mouse::Left)
        return false;

    if (rt.overlay.kind != OverlayState::Kind::None) {
        if (!InBox(rt.overlayHits.panel, m.x, m.y)) {
            closeOverlay();
            return true;
        }
        for (size_t i = 0; i < rt.overlayHits.rowBoxes.size(); ++i) {
            if (InBox(rt.overlayHits.rowBoxes[i], m.x, m.y)) {
                rt.overlay.sel = rt.overlayHits.firstVisible + (int)i;
                const OverlayState::Kind kind = rt.overlay.kind;
                OverlayActivate(rt, rt.overlay.kind == OverlayState::Kind::Tree
                                        ? TreeRows(app)
                                        : std::vector<TreeRow>{});
                if (kind == OverlayState::Kind::Sessions ||
                    kind == OverlayState::Kind::Tree || kind == OverlayState::Kind::Help)
                    closeOverlay();
                return true;
            }
        }
        return true;
    }

    if (InBox(rt.hits.sessions, m.x, m.y)) {
        openOverlay(OverlayState::Kind::Sessions);
        return true;
    }
    if (InBox(rt.hits.mcp, m.x, m.y)) {
        openOverlay(OverlayState::Kind::Mcp);
        return true;
    }
    if (InBox(rt.hits.backends, m.x, m.y)) {
        openOverlay(OverlayState::Kind::Backends);
        return true;
    }
    if (InBox(rt.hits.newBtn, m.x, m.y)) {
        app.newSession();
        return true;
    }
    if (InBox(rt.hits.help, m.x, m.y)) {
        openOverlay(OverlayState::Kind::Help);
        return true;
    }
    if (InBox(rt.hits.modeSimple, m.x, m.y)) {
        if (app.scaffoldMode && !app.promptSendBlocked()) app.setScaffoldMode(false);
        return true;
    }
    if (InBox(rt.hits.modeScaffold, m.x, m.y)) {
        if (!app.scaffoldMode && !app.promptSendBlocked()) app.setScaffoldMode(true);
        return true;
    }
    if (InBox(rt.hits.loop, m.x, m.y)) {
        // Clickable during a run: arming applies to that run's end, and a
        // click while the countdown is up drops the pending restart.
        if (app.loopArmed) app.cancelLoop();
        app.setLoopEnabled(!app.loopEnabled);
        return true;
    }
    if (InBox(rt.hits.compact, m.x, m.y)) {
        if (app.agentRunning || app.canCompactIdle())
            app.queueCompaction();
        return true;
    }
    if (InBox(rt.backendLink, m.x, m.y)) {
        openOverlay(OverlayState::Kind::Backends);
        return true;
    }
    if (InBox(rt.resumeCont, m.x, m.y)) {
        if (!app.agentRunning) {
            if (app.scaffoldResume.resumable)
                app.continueScaffold();
            else
                app.continueScaffoldLedger();
        }
        return true;
    }
    if (InBox(rt.resumeNew, m.x, m.y)) {
        if (!app.agentRunning) app.newScaffoldSession();
        return true;
    }

    if (InBox(rt.logBox, m.x, m.y) && !rt.rowMsg.empty()) {
        const int vis = m.y - rt.logBox.y_min;
        if (vis >= 0 && vis < (int)rt.rowMsg.size()) {
            const int mi = rt.rowMsg[vis];
            if (mi >= 0 && mi < (int)app.uiMessages.size()) {
                App::UiMessage& um = app.uiMessages[mi];
                if (um.kind == App::UiMessage::Tool &&
                    viewrows::toolRowExpandable(um.toolDone, um.toolStreaming, um.toolResult,
                                                um.toolName, um.toolCommand))
                    um.toolExpanded = !um.toolExpanded;
            }
        }
        return true;
    }

    if (InBox(rt.inputBox, m.x, m.y)) return false;
    return true;
}

// Global key handling, before events reach the focused input. Returns true
// when the event is consumed. All App calls happen here, i.e. on the FTXUI
// loop thread (thread lemma).
bool HandleEvent(TuiRuntime& rt, const ftxui::Event& ev) {
    App& app = rt.app;

    // Ticker wake-up (every 100 ms): pump agent events, refresh session
    // list / MCP / scaffold state. App::tick() is renderer-neutral.
    if (ev == ftxui::Event::Custom) {
        app.tick();
        return true;
    }

    if (ev.is_mouse()) return HandleMouse(rt, ev);

    auto openOverlay = [&](OverlayState::Kind kind) {
        rt.overlay = OverlayState{kind, 0};
        rt.overlayShown = true;
        if (kind == OverlayState::Kind::Sessions) app.refreshSessionList();
    };
    auto closeOverlay = [&]() {
        if (rt.overlay.kind == OverlayState::Kind::Tree) {
            app.treeOpen = false;
            app.treeSelection = -1;
        }
        rt.overlay = OverlayState{};
        rt.overlayShown = false;
    };

    // While an overlay is up it owns the keyboard: list navigation, its
    // per-kind actions, Esc closes. Nothing reaches the prompt editor.
    if (rt.overlay.kind != OverlayState::Kind::None) {
        const int nSessions = (int)app.sessionList.size();
        const int nBackends = (int)app.backends.size();
        const int nMcp = (int)app.mcpServers.size();
        const std::vector<TreeRow> tree = rt.overlay.kind == OverlayState::Kind::Tree
                                              ? TreeRows(app)
                                              : std::vector<TreeRow>{};
        const int nRows = [&] {
            switch (rt.overlay.kind) {
            case OverlayState::Kind::Sessions: return nSessions;
            case OverlayState::Kind::Backends: return nBackends;
            case OverlayState::Kind::Mcp: return nMcp;
            case OverlayState::Kind::Roles: return (int)app.scaffoldRoleLog.size();
            case OverlayState::Kind::Tree: return (int)tree.size();
            case OverlayState::Kind::Help: return 1;
            case OverlayState::Kind::None: return 0;
            }
            return 0;
        }();
        auto clampSel = [&] {
            if (nRows > 0) rt.overlay.sel = std::min(std::max(0, rt.overlay.sel), nRows - 1);
        };
        if (IsEscape(ev)) {
            closeOverlay();
            return true;
        }
        if (ev == ftxui::Event::Return) {
            const OverlayState::Kind kind = rt.overlay.kind;
            OverlayActivate(rt, tree);
            if (kind == OverlayState::Kind::Sessions || kind == OverlayState::Kind::Tree ||
                kind == OverlayState::Kind::Help)
                closeOverlay();
            clampSel();
            return true;
        }
        if (ev == ftxui::Event::ArrowUp) {
            rt.overlay.sel = std::max(0, rt.overlay.sel - 1);
            return true;
        }
        if (ev == ftxui::Event::ArrowDown) {
            if (nRows > 0) rt.overlay.sel = std::min(rt.overlay.sel + 1, nRows - 1);
            return true;
        }
        if (ev == ftxui::Event::PageUp) {
            rt.overlay.sel = std::max(0, rt.overlay.sel - 10);
            return true;
        }
        if (ev == ftxui::Event::PageDown) {
            if (nRows > 0) rt.overlay.sel = std::min(rt.overlay.sel + 10, nRows - 1);
            return true;
        }
        if (ev == ftxui::Event::Character("d") &&
            rt.overlay.kind == OverlayState::Kind::Sessions) {
            if (!app.agentRunning && rt.overlay.sel < nSessions) {
                app.deleteSession(app.sessionList[rt.overlay.sel].path);
                if (rt.overlay.sel >= (int)app.sessionList.size())
                    rt.overlay.sel = std::max(0, (int)app.sessionList.size() - 1);
            }
            return true;
        }
        if (ev == ftxui::Event::Character("i") &&
            rt.overlay.kind == OverlayState::Kind::Backends) {
            if (rt.overlay.sel < nBackends) app.setImageBackend(rt.overlay.sel);
            return true;
        }
        if (ev == ftxui::Event::Character(" ") &&
            rt.overlay.kind == OverlayState::Kind::Mcp) {
            if (rt.overlay.sel < nMcp)
                app.setMcpServerEnabled(rt.overlay.sel, !app.mcpServers[rt.overlay.sel].enabled);
            return true;
        }
        return true;
    }

    // Quit, with a confirm while a run is active (Ctrl+C follows the same
    // path; FTXUI defers to us because we consume the event).
    if (ev == ftxui::Event::CtrlQ || ev == ftxui::Event::CtrlC) {
        if (!app.agentRunning || rt.QuitArmed()) {
            rt.quit();
            return true;
        }
        rt.quitArmedAt = std::chrono::steady_clock::now();
        return true;
    }

    // Esc: shared semantics via App::onEscape() — stop a running agent,
    // double-Esc opens the session tree (treeOpen comes back true).
    if (IsEscape(ev)) {
        app.onEscape();
        if (app.treeOpen) {
            rt.overlay = OverlayState{OverlayState::Kind::Tree, 0};
            rt.overlayShown = true;
        }
        return true;
    }

    // Chords reused from the desktop: ChordPressed() reads ImGui IO state,
    // so the TUI maps its own events onto the same App toggles.
    if (ev == ftxui::Event::CtrlO) {
        app.toggleToolExpansion();
        return true;
    }
    if (ev == ftxui::Event::CtrlT) {
        app.toggleThinking();
        return true;
    }
    if (ev == ftxui::Event::CtrlN) {
        app.newSession();
        return true;
    }

    // Overlay chords (alternate to the clickable top-bar buttons).
    if (ev == ftxui::Event::CtrlS) {
        openOverlay(OverlayState::Kind::Sessions);
        return true;
    }
    if (ev == ftxui::Event::CtrlB) {
        openOverlay(OverlayState::Kind::Backends);
        return true;
    }
    if (ev == ftxui::Event::CtrlP) {
        openOverlay(OverlayState::Kind::Mcp);
        return true;
    }
    if (ev == ftxui::Event::CtrlG) {
        openOverlay(OverlayState::Kind::Help);
        return true;
    }
    if (ev == ftxui::Event::CtrlR) {
        openOverlay(OverlayState::Kind::Roles);
        return true;
    }

    // Log viewport scrolling: half a viewport per press; reaching 0
    // re-follows the bottom.
    if (ev == ftxui::Event::PageUp || ev == ftxui::Event::PageDown) {
        const int height = rt.logBox.y_max - rt.logBox.y_min + 1;
        const int step = std::max(1, height / 2);
        if (ev == ftxui::Event::PageUp)
            rt.scroll.offsetLines += step;
        else
            rt.scroll.offsetLines = std::max(0, rt.scroll.offsetLines - step);
        return true;
    }

    // /image PATH: attach the image to the prompt as an [image N] token
    // (the desktop's clipboard paste parity item). The token stays in the
    // editor so the user can add a question before sending.
    if (ev == ftxui::Event::Return && rt.editor->rfind("/image ", 0) == 0) {
        std::string path = rt.editor->substr(7);
        while (!path.empty() && (path.front() == ' ' || path.front() == '\t'))
            path.erase(path.begin());
        while (!path.empty() &&
               (path.back() == ' ' || path.back() == '\t' || path.back() == '"'))
            path.pop_back();
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
            path = path.substr(1, path.size() - 2);
        if (auto img = processImageFile(path)) {
            const int id = ++app.nextImageId;
            app.pendingImages.push_back({id, std::move(*img)});
            *rt.editor = "[image " + std::to_string(id) + "] ";
            *rt.cursor = (int)rt.editor->size();
            app.showToast("attached " + path);
        } else {
            app.showToast("cannot load image: " + path);
        }
        return true;
    }

    // Enter sends; a modified Enter (Ctrl/Shift/Alt+Enter under the kitty
    // protocol) inserts a newline instead. The plain Return never reaches
    // the input (it is consumed here); the newline is forwarded to it as a
    // Return event, which the multiline Input turns into a line break.
    if (ev == ftxui::Event::Return) {
        if (!app.promptSendBlocked() && !rt.editor->empty()) {
            app.sendPromptText(*rt.editor);
            rt.editor->clear();
            *rt.cursor = 0;
            rt.histIdx = -1;
            rt.draft.clear();
        }
        return true;
    }
    if (IsSoftNewline(ev)) {
        rt.input->OnEvent(ftxui::Event::Return);
        return true;
    }

    // Prompt history (Up from the first line / Down from the last, like
    // the desktop's input bar): history[0] is the oldest entry, so Up
    // starts at the back and walks older, Down walks back to the draft.
    const size_t firstNl = rt.editor->find('\n');
    const size_t lastNl = rt.editor->rfind('\n');
    const bool onFirstLine = firstNl == std::string::npos || *rt.cursor <= (int)firstNl;
    const bool onLastLine = lastNl == std::string::npos || *rt.cursor > (int)lastNl;
    if (ev == ftxui::Event::ArrowUp && onFirstLine) {
        if (!app.history.empty()) {
            if (rt.histIdx < 0) {
                rt.draft = *rt.editor;
                rt.histIdx = (int)app.history.size() - 1;
            } else if (rt.histIdx > 0) {
                rt.histIdx--;
            } else {
                return true;
            }
            *rt.editor = app.history[rt.histIdx];
            *rt.cursor = (int)rt.editor->size();
        }
        return true;
    }
    if (ev == ftxui::Event::ArrowDown && onLastLine) {
        if (rt.histIdx >= 0) {
            if (rt.histIdx + 1 >= (int)app.history.size()) {
                rt.histIdx = -1;
                *rt.editor = rt.draft;
            } else {
                rt.histIdx++;
                *rt.editor = app.history[rt.histIdx];
            }
            *rt.cursor = (int)rt.editor->size();
        }
        return true;
    }

    // While a scaffold run owns the app (promptSendBlocked), the editor is
    // fully inert (desktop parity: the box is disabled): consume everything
    // that would otherwise edit it.
    if (app.promptSendBlocked()) return true;

    return false;
}

} // namespace

int runFrontend(App& app) {
    ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();

    // The prompt editor: unbounded multiline buffer (the 4096-byte
    // inputBuf truncation inside sendPromptText is a documented accepted
    // loss, PLAN_TUI.md §8). Plain Enter never reaches the input
    // (HandleEvent sends instead); a modified Enter inserts the newline.
    std::string editor;
    int cursor = 0;
    ftxui::InputOption inputOpt;
    inputOpt.content = &editor;
    inputOpt.cursor_position = &cursor;
    inputOpt.multiline = true;
    inputOpt.placeholder = "Type a prompt… (Enter to send, Shift/Ctrl+Enter for a newline)";
    inputOpt.transform = [](ftxui::InputState s) {
        if (s.is_placeholder) s.element |= ftxui::dim;
        return s.element;
    };
    ftxui::Component input = ftxui::Input(inputOpt);

    TuiRuntime rt{
        .app = app,
        .screen = screen,
        .input = input,
        .editor = &editor,
        .cursor = &cursor,
        .quit = screen.ExitLoopClosure(),
    };

    ftxui::Component modal =
        ftxui::Renderer([&] { return Overlay(app, rt.overlay, screen.dimx(), screen.dimy(),
                                             &rt.overlayHits); });

    ftxui::Component root = ftxui::Renderer(input, [&] {
        const int width = screen.dimx();
        ftxui::Element promptArea =
            ftxui::hbox({
                ftxui::text(" > "),
                input->Render() | ftxui::xflex | ftxui::reflect(rt.inputBox),
            });
        if (app.promptSendBlocked())
            promptArea = ftxui::text(" > (run active — editor disabled)") | ftxui::dim;
        return ftxui::vbox({
            TopBar(app, width, &rt.hits),
            ftxui::separator(),
            // flex fills the leftover height; reflect reports the final
            // viewport box back for the next frame's scroll math.
            LogView(app, width, rt.logBox.y_max - rt.logBox.y_min + 1, rt.scroll,
                    &rt.rowMsg, &rt.logFirstRow) |
                ftxui::flex | ftxui::reflect(rt.logBox),
            ftxui::separator(),
            StatusLine(app, rt.QuitArmed()),
            ResumeRow(app, &rt.resumeCont, &rt.resumeNew),
            std::move(promptArea),
            ModeRow(app, &rt.hits),
            BottomBar(app, width, &rt.backendLink),
        });
    }) | ftxui::CatchEvent([&](const ftxui::Event& ev) { return HandleEvent(rt, ev); }) |
        ftxui::Modal(modal, &rt.overlayShown);

    // The ticker: posts a wake-up event every 100 ms and nothing else. All
    // App access stays on the loop thread; the stop token guards posts
    // racing the shutdown below.
    std::jthread ticker([&screen](std::stop_token st) {
        while (!st.stop_requested()) {
            std::this_thread::sleep_for(kTickInterval);
            if (st.stop_requested()) break;
            screen.PostEvent(ftxui::Event::Custom);
        }
    });

    // Enable the extended-keys mode before the loop takes over the terminal
    // (modified-Enter chords; see IsSoftNewline) and pop it again after the
    // loop restored the terminal state: kitty protocol outside tmux,
    // modifyOtherKeys under tmux (which re-encodes the chords for us).
    const bool extKeys = KittyKeysEnabled();
    const char* push = InTmux() ? kModifyOtherKeysPush : kKittyKeysPush;
    const char* pop = InTmux() ? kModifyOtherKeysPop : kKittyKeysPop;
    if (extKeys) (void)!write(STDOUT_FILENO, push, strlen(push));

    screen.Loop(root);

    if (extKeys) (void)!write(STDOUT_FILENO, pop, strlen(pop));

    // Shutdown ordering (see the ledger): the loop has exited, so stop and
    // join the ticker before the caller runs app.shutdown().
    ticker.request_stop();
    ticker.join();
    return 0;
}

} // namespace tui
