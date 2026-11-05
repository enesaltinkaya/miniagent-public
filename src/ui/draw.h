#pragma once

#include "app.h"

#include "imgui.h"
#include "ui/fonts.h"
#include "ui/view_rows.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

// The duration formatter lives in the shared row model (view_rows); keep
// the unqualified name that the desktop draw code has always used.
using viewrows::FormatDurationMs;

// First non-empty line of `text`, ellipsized to fit `maxW` pixels in
// the current font (binary search over prefix widths). Shared by the
// rows that must stay on a single line: the session-tree rows and
// the scaffold task rows.
inline std::string SingleLineEllipsized(const std::string& text, float maxW) {
    size_t b = text.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return {};
    size_t e = text.find_first_of("\r\n", b);
    std::string s = text.substr(b, e == std::string::npos ? std::string::npos : e - b);
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW)
        return s;
    const float ellW = ImGui::CalcTextSize("\u2026").x;
    const float limit = std::max(maxW - ellW, 0.0f);
    int lo = 0, hi = (int)s.size();
    while (lo < hi) {
        const int mid = (lo + hi + 1) / 2;
        if (ImGui::CalcTextSize(s.c_str(), s.c_str() + mid).x <= limit)
            lo = mid;
        else
            hi = mid - 1;
    }
    return s.substr(0, lo) + "\u2026";
}

// Layout constants shared between the top bar and the pane placement in
// App::Frame().
constexpr float kTopBarH = 72.0f;
constexpr float kPaneGap = 8.0f;  // clear space between top bar and panes

// The dark-green action buttons' look (colors, 4px rounding, generous
// frame padding) is the app-wide style default; see main.cpp.

// The app's default button: ImGui::Button with the global dark-green theme
// plus the hand cursor while hovered (the SDL3 backend maps
// ImGuiMouseCursor_Hand to the OS "pointer" cursor). All buttons draw
// through this so hover feedback stays consistent app-wide.
inline bool HandButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    const bool hit = ImGui::Button(label, size);
    if (ImGui::IsItemHovered())
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return hit;
}

// Message kind -> accent color / label, shared by the chat log and the
// session tree.
ImVec4 ColorForKind(App::UiMessage::Kind kind);
const char* LabelForKind(App::UiMessage::Kind kind);

// Height of the prompt box / status strip for the current content.
float PromptBoxHeight(const char* buf);
float InputBarHeight(const char* buf);
float StatusStripHeight();

// Panel draw functions; each renders one region of the main window.
void DrawTopBar(App& app);
// Title-bar style shared by the modals (Sessions, Session Tree, Scaffold
// Tasks): makes their title bar read like the desktop window manager's
// own title bar. Push before BeginPopupModal(), Pop after EndPopup().
void PushModalTitleStyle();
void PopModalTitleStyle();
void DrawMessageLog(App& app);
void DrawInputBar(App& app);
void DrawStatusStrip(App& app);
void DrawStatusPanel(App& app);
// Transient toast pill ("Text copied" / "Message copied"): fading, floating
// just above the bottom of the message log; drawn on the foreground draw
// list so it stays above every window, modals included.
void DrawToast(App& app);
// Session-list modal (opened from the status strip's Sessions button),
// drawn outside the main window.
void DrawSessionsModal(App& app);
// MCP server-list modal (opened from the message log's MCP button): checkbox
// per server in ~/.config/miniagent/mcp.json to enable/disable it.
void DrawMcpModal(App& app);
// Backend list modal (opened from the status panel's backend URL link): one
// row per entry in ~/.config/miniagent/backends.json; picking a row
// switches the active backend.
void DrawBackendModal(App& app);
// Scaffold task-list modal (opened from the status strip's task button).
void DrawScaffoldTasksModal(App& app);
// Inline twin of the task modal: a floating panel in the top-right of the
// message log (under the New Session / Sessions buttons) showing the same
// live task list while a scaffold run is in progress. Hidden while the
// modal itself is open.
void DrawScaffoldTaskPanel(App& app, const ImVec2& pos, const ImVec2& size);
// True while the mouse is over the inline scaffold tasks panel (or its
// collapsed pill), as drawn this frame. The message log's drag-selection
// checks it so a press on the panel - its scrollbar included - is not
// taken as a selection on the message text underneath.
bool MouseOverScaffoldTasksPanel();
// Session tree popup (drawn outside the main window).
void DrawSessionTree(App& app);
