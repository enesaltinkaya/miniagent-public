#include "ui/draw.h"

#include "ui/fonts.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <time.h>
#include <vector>

// Centered modal listing the project's sessions, opened from the status
// strip's Sessions button (the list is no longer a permanent pane). Click a
// row to open that session (and close the list), middle-click to delete it,
// right-click to copy its first user message.
void DrawSessionsModal(App& app) {
    // BeginPopupModal() is called even when the modal is closed: setting
    // *p_open to false before calling it is Dear ImGui's documented way to
    // close a popup, and it tears the popup state down (ClosePopupToLevel)
    // on the frame after Escape/X/Close closed it (onEscape does the same).
    bool open = app.sessionsModalOpen;
    if (open && !ImGui::IsPopupOpen("Sessions"))
        ImGui::OpenPopup("Sessions");

    const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    const ImVec2 vpCenter = ImVec2(vpSize.x * 0.5f, vpSize.y * 0.5f);

    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f * gFontScale, 460.0f * gFontScale), ImGuiCond_Appearing);
    PushModalTitleStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * gFontScale, 14.0f * gFontScale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.03f, 0.04f, 1.0f));
    if (ImGui::BeginPopupModal("Sessions", &open,
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {
        const std::vector<SessionInfo>& sessions = app.sessionList;
        const std::string& cur = app.sessions->currentPath();

        // Footer (separator + hints + button) stays put; only the list
        // scrolls. Its height is derived from the current style so the list
        // and footer exactly fill the window content region.
        const float itemSpacing = ImGui::GetStyle().ItemSpacing.y;
        const float fontH = ImGui::GetFontSize();
        const float footerH = (1.0f + itemSpacing) +         // separator
                              2.0f * (fontH + itemSpacing) + // hint lines
                              (1.0f + itemSpacing) +          // spacing
                              (fontH + 2.0f * ImGui::GetStyle().FramePadding.y) + // button
                              itemSpacing;
        const float listH = std::max(ImGui::GetContentRegionAvail().y - footerH, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 16.0f * gFontScale);
        ImGui::BeginChild("##sessionlist", ImVec2(0.0f, listH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        if (gFontMono)
            ImGui::PushFont(gFontMono);

        if (sessions.empty())
            ImGui::TextDisabled("No sessions yet. Sending a prompt starts one.");

        // Switching sessions (or deleting/copying them) mid-run would race the
        // running agent's session file, so the list is inert while it works
        // (the App methods guard it too; this only greys the rows out).
        ImGui::BeginDisabled(app.agentRunning);
        // Extra vertical gap between list items, and no hover highlight.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(ImGui::GetStyle().ItemSpacing.x, 10.0f * gFontScale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        for (size_t i = 0; i < sessions.size(); ++i) {
            const SessionInfo& s = sessions[i];
            const bool current = (s.path == cur);
            // Keep each session on a single line: truncate to the panel width
            // and finish with an ellipsis if it had to be shortened.
            const std::string prefix = current ? "\u2022 " : "";
            const std::string full =
                prefix + (s.title.empty() ? std::string("(untitled)") : s.title);
            const float availW = std::max(ImGui::GetContentRegionAvail().x,
                                           1.0f);
            std::string label = full;
            if (ImGui::CalcTextSize(full.c_str(), nullptr).x > availW) {
                const float ellipsisW = ImGui::CalcTextSize("\u2026", nullptr).x;
                const float maxW = std::max(availW - ellipsisW, 0.0f);
                int lo = 0, hi = (int)full.size();
                while (lo < hi) {
                    const int mid = (lo + hi + 1) / 2;
                    if (ImGui::CalcTextSize(full.c_str(), full.c_str() + mid).x <= maxW)
                        lo = mid;
                    else
                        hi = mid - 1;
                }
                label = full.substr(0, lo) + "\u2026";
            }
            // Unique, stable ID per row: duplicate titles (e.g. two "hi how
            // are you?") would otherwise collide in the ID stack.
            ImGui::PushID(s.path.c_str());
            const bool selected = ImGui::Selectable(label.c_str(), current);
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (selected && !app.agentRunning) {
                app.openSession(s.path);
                open = false;  // the pick is done; back to the chat
            }
            // Middle-click deletes the session (ImGui button indices: 0=left, 1=right, 2=middle).
            if (hovered && ImGui::IsMouseClicked(2))
                app.deleteSession(s.path);
            // Right-click copies the session's first user message to the
            // clipboard (falling back to the title if the session never had
            // a user turn).
            if (hovered && ImGui::IsMouseClicked(1))
                app.copyFirstUserMessage(s.path, s.title);
        }
        ImGui::EndDisabled();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        if (gFontMono)
            ImGui::PopFont();
        ImGui::PopStyleVar();  // WindowPadding
        ImGui::EndChild();
        ImGui::PopStyleVar();  // ScrollbarSize

        ImGui::Separator();
        ImGui::TextDisabled("Click a session to open it (the list closes). /new or Ctrl+N starts a new one.");
        ImGui::TextDisabled("Middle-click deletes a session; right-click copies its first message.");
        ImGui::Spacing();
        const float closeW =
            ImGui::CalcTextSize("Close").x + 2.0f * ImGui::GetStyle().FramePadding.x;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - closeW);
        if (HandButton("Close"))
            open = false;

        ImGui::EndPopup();
    }
    app.sessionsModalOpen = open;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    PopModalTitleStyle();
}

// Centered popup listing the open session's messages; click one to rewind
// the conversation to it (pi-style session tree).
void DrawSessionTree(App& app) {
    // BeginPopupModal() is called even when the tree is closed: setting
    // *p_open to false before calling it is Dear ImGui's documented way to
    // close a popup, and it tears the popup state down (ClosePopupToLevel)
    // on the frame after Escape/X/Close closed the tree.
    bool open = app.treeOpen;
    if (open && !ImGui::IsPopupOpen("Session Tree"))
        ImGui::OpenPopup("Session Tree");

    std::vector<AgentMessage> messages;
    if (open && app.sessions->hasOpenSession())
        messages = SessionStore::loadMessages(app.sessions->currentPath());
    if (open && app.treeSelection >= (int)messages.size())
        app.treeSelection = -1;

    const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    const ImVec2 vpCenter = ImVec2(vpSize.x * 0.5f, vpSize.y * 0.5f);

    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f * gFontScale, 460.0f * gFontScale), ImGuiCond_Appearing);
    PushModalTitleStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * gFontScale, 14.0f * gFontScale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.03f, 0.04f, 1.0f));
    if (ImGui::BeginPopupModal("Session Tree", &open,
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {

        // Footer (separator + hints + buttons) stays put; only the list
        // scrolls. Its height is derived from the current style so the list
        // and footer exactly fill the window content region.
        const float itemSpacing = ImGui::GetStyle().ItemSpacing.y;
        const float fontH = ImGui::GetFontSize();
        const float footerH = (1.0f + itemSpacing) +         // separator
                              2.0f * (fontH + itemSpacing) + // hint lines
                              (1.0f + itemSpacing) +          // spacing
                              (fontH + 2.0f * ImGui::GetStyle().FramePadding.y) + // buttons
                              itemSpacing;
        const float listH = std::max(ImGui::GetContentRegionAvail().y - footerH, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 16.0f * gFontScale);
        ImGui::BeginChild("##treelist", ImVec2(0.0f, listH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        if (messages.empty()) {
            ImGui::TextDisabled("No messages in this session.");
        } else {
            const float rowW = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
            if (gFontMono)
                ImGui::PushFont(gFontMono);
            for (size_t i = 0; i < messages.size(); ++i) {
                const AgentMessage& m = messages[i];
                const App::UiMessage::Kind kind =
                    std::holds_alternative<UserMessage>(m)
                        ? App::UiMessage::User
                        : std::holds_alternative<AssistantMessage>(m) ? App::UiMessage::Agent
                                                                      : App::UiMessage::Tool;
                const char* tag = "?";
                std::string preview;
                if (const auto* u = std::get_if<UserMessage>(&m)) {
                    tag = "User";
                    preview = u->text;
                } else if (const auto* a = std::get_if<AssistantMessage>(&m)) {
                    tag = a->toolCalls.empty() ? "Agent" : "Agent\u2192tools";
                    preview = a->text.empty() ? "\u2026" : a->text;
                } else if (const auto* t = std::get_if<ToolResultMessage>(&m)) {
                    tag = "Tool";
                    preview = t->toolName + (t->args.empty() ? std::string{} : " " + t->args);
                }
                char ts[16];
                {
                    const int64_t ms =
                        std::visit([](const auto& v) -> int64_t { return v.timestamp; }, m);
                    time_t sec = (time_t)(ms / 1000);
                    struct tm tmv;
                    localtime_r(&sec, &tmv);
                    snprintf(ts, sizeof(ts), "%02d:%02d:%02d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
                }
                const std::string prefix = std::string(ts) + "  " + tag + "  ";
                const float maxW = rowW - ImGui::CalcTextSize(prefix.c_str()).x - 8.0f;
                const std::string label = prefix + SingleLineEllipsized(preview, std::max(maxW, 0.0f));
                ImGui::PushID((int)i);
                const bool sel = app.treeSelection == (int)i;
                // Row text colored by kind, same palette as the chat log.
                ImGui::PushStyleColor(ImGuiCol_Text, (ImU32)ImColor(ColorForKind(kind)));
                if (ImGui::Selectable(label.c_str(), sel)) {
                    app.treeSelection = (int)i;
                    ImGui::PopStyleColor();
                    ImGui::PopID();
                    if (!app.agentRunning) {
                        if (std::holds_alternative<UserMessage>(m))
                            app.rewindToUserMessage(i);
                        else
                            app.rewindToMessage(i);
                        open = false;
                    }
                    continue;
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
            }
            if (gFontMono)
                ImGui::PopFont();
            // First frame the list appears: start scrolled to the latest
            // message (the bottom of the list).
            if (ImGui::IsWindowAppearing())
                ImGui::SetScrollHereY(1.0f);
        }
        ImGui::PopStyleVar();  // WindowPadding
        ImGui::EndChild();
        ImGui::PopStyleVar();  // ScrollbarSize

        ImGui::Separator();
        ImGui::TextDisabled("Click a message to rewind; it stays as the last one.");
        ImGui::TextDisabled("Rewinding a user message removes it and pastes it into the prompt.");
        ImGui::Spacing();
        ImGui::BeginDisabled(app.agentRunning || app.treeSelection < 0 || messages.empty());
        if (HandButton("Rewind to selected")) {
            if (std::holds_alternative<UserMessage>(messages[app.treeSelection]))
                app.rewindToUserMessage(app.treeSelection);
            else
                app.rewindToMessage(app.treeSelection);
            open = false;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) {
            if (app.agentRunning)
                ImGui::SetTooltip("Wait for the agent to finish");
            else if (app.treeSelection < 0)
                ImGui::SetTooltip("Pick a message above first");
        }
        ImGui::SameLine();
        if (HandButton("Close"))
            open = false;

        ImGui::EndPopup();
    }
    app.treeOpen = open;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    PopModalTitleStyle();
}
