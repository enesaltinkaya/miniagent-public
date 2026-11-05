#pragma once

#include <string>
#include <vector>

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

// Pure TUI row/element builders (PLAN_TUI.md §4): shared App state ->
// styled rows and FTXUI elements, with no event loop and no terminal
// state of their own. The fullscreen loop in src/tui/tui.cpp calls these
// every frame; tui_render_test renders the same builders off-screen
// (PLAN_TUI.md §6), so everything visible about the log is testable
// without a terminal.
//
// Row model: StyledRows() is the whole message log as one styled row per
// terminal line (M2): kind colors per UiMessage::Kind, markdown-lite via
// tui/rich_text_tui, tool rows through the shared viewrows summary. The
// log viewport slices it; tui_render_test asserts on the row data.

struct App;

namespace tui {

// Log-viewport scroll state, owned by the TUI loop (the desktop equivalent
// is App::stickToBottom + the ImGui scroll state): 0 = follow the newest
// line (auto-follow), otherwise the number of lines the view is pinned
// above the bottom.
struct ScrollState {
    int offsetLines = 0;
};

// One styled run of text on a terminal line. fg == Color::Default means
// the terminal's default color.
struct TuiSpan {
    std::string text;
    ftxui::Color fg = ftxui::Color::Default;
    bool bold = false;
    bool dim = false;
    bool italic = false;
};

using TuiRow = std::vector<TuiSpan>;

// The log as the viewport needs it: styled rows plus, per row, the index of
// the uiMessages entry it belongs to (-1 for pure separator rows). The
// mouse maps a clicked line back to its message through this (M4).
struct LogModel {
    std::vector<TuiRow> rows;
    std::vector<int> msgIndex;
};
LogModel BuildLog(App& app, int width);

// The whole message log, one entry per terminal line, wrapped at `width`.
// Takes App& (not const&) because the tool summary caches are updated in
// place through the message's own fields, exactly like the desktop
// renderer does.
std::vector<TuiRow> StyledRows(App& app, int width);

// Concatenated span text of one row (test helper).
std::string RowText(const TuiRow& row);

// One row as a single-line element (spans joined with ftxui::hbox).
ftxui::Element RenderRow(const TuiRow& row);

// Shortened cwd for the top/bottom bar: the full path while it fits, else
// an ellipsis + the last components.
std::string ShortCwd(const std::string& cwd, int max);

// Reflected hit boxes of the clickable chrome (M4). The renderer fills
// them every frame; the event handler compares mouse positions against
// them. A default-constructed Box has y_min > y_max (never hit).
struct ChromeHits {
    ftxui::Box sessions, mcp, backends, newBtn, help;
    ftxui::Box modeSimple, modeScaffold, compact, loop;
};

// One-line chrome pieces. `width` is the terminal width (screen.dimx()).
ftxui::Element TopBar(App& app, int width, ChromeHits* hits = nullptr);
// `height` is the viewport height (reflected box of the previous frame;
// <= 0 on the first frame). Slices the StyledRows to the visible window:
// the bottom `height` lines when following, `offsetLines` higher when
// scrolled. `rowMsg`/`firstRow` receive the visible window's message map
// and its first row index (for mouse hit-testing).
ftxui::Element LogView(App& app, int width, int height, const ScrollState& scroll,
                       std::vector<int>* rowMsg = nullptr, int* firstRow = nullptr);
// Status strip: state bullet (Working…/Idle/scaffold role), busy timer,
// tok/s, live context tokens; quit-confirmation hint right when armed.
ftxui::Element StatusLine(App& app, bool quitArmed);
// Bottom key hints / mode row: Send hint, Simple/Scaffold toggle buttons,
// the Loop checkbox, Compact (during a run, and while idle in Simple mode;
// all clickable via `hits`).
ftxui::Element ModeRow(App& app, ChromeHits* hits = nullptr);
// Bottom line: cwd left; model • session tokens • backend (link-colored)
// right, red backend error instead when the config was malformed. The
// backend name is the clickable link (box out-param, M4).
ftxui::Element BottomBar(App& app, int width, ftxui::Box* backendLink = nullptr);

// ---- overlays (M3) ------------------------------------------------------

// Which overlay is up (owned by the TUI loop; the desktop equivalents are
// App::sessionsModalOpen & friends). `sel` is the selected list row.
struct OverlayState {
    enum class Kind { None, Sessions, Backends, Mcp, Help, Tree, Roles };
    Kind kind = Kind::None;
    int sel = 0;
};

// Reflected boxes + list geometry of the overlay panel (M4 mouse).
struct OverlayHits {
    ftxui::Box panel;       // whole panel (clicks outside close it)
    int firstVisible = 0;   // row index of the first visible list row
    int visible = 0;        // number of visible rows
    std::vector<ftxui::Box> rowBoxes; // one box per visible row (filled at render)
};

// One row per cached session (current session marked "• "); index i lines
// up with app.sessionList[i]. Placeholder text when the list is empty.
std::vector<std::string> SessionsEntries(App& app);

// One row per backend: "• name · url · model [image]"; index i lines up
// with app.backends[i].
std::vector<std::string> BackendsEntries(App& app);

// One row per MCP server: "[x] name · state (N tools)"; index i lines up
// with app.mcpServers[i].
std::vector<std::string> McpEntries(App& app);

// Session-tree row: "HH:MM:SS  Tag  preview"; `user` marks user messages
// (rewinding them pastes the text back into the prompt). Index i lines up
// with the loaded message list.
struct TreeRow {
    std::string label;
    bool user = false;
};
std::vector<TreeRow> TreeRows(App& app);

// Key/chord reference shown in the Help overlay.
std::vector<std::string> HelpEntries();

// Scaffold dispatch-log entries for the Roles overlay (M5): one row per
// dispatch, the pinned one marked "•", the live one "▸"; index i lines up
// with app.scaffoldRoleLog[i].
std::vector<std::string> RolesEntries(App& app);

// Idle Scaffold-mode ledger affordance (M5, the desktop's resume row):
// saved task + [Continue] [New Session], or [Clear] for a finished/stale
// ledger. Empty element when nothing applies. Boxes are click targets.
ftxui::Element ResumeRow(App& app, ftxui::Box* cont = nullptr,
                         ftxui::Box* newSess = nullptr);

// The overlay panel (title, windowed list with the selected row inverted,
// dim footer hints) sized to `width`x`height`. Composited over the main
// layout by ftxui::Modal (clear_under + center are applied there).
// `hits` receives the panel/list boxes and list geometry when given.
ftxui::Element Overlay(App& app, const OverlayState& st, int width, int height,
                       OverlayHits* hits = nullptr);

} // namespace tui
