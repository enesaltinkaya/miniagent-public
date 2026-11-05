#include "ui/draw.h"

#include "mcp/mcp.h"
#include "ui/fonts.h"

// Centered modal listing the user-level MCP servers (mcp.json). A checkbox
// per server enables/disables it (persisted in miniagent.settings): enabling
// spawns the server process and adds its tools to the agent, disabling stops
// it. Only new entries in mcp.json are picked up at restart.
void DrawMcpModal(App& app) {
    // BeginPopupModal() is called even when the modal is closed: setting
    // *p_open to false before calling it is Dear ImGui's documented way to
    // close a popup (Escape goes through App::onEscape).
    bool open = app.mcpModalOpen;
    if (open && !ImGui::IsPopupOpen("MCP Servers"))
        ImGui::OpenPopup("MCP Servers");

    const ImVec2 vpSize   = ImGui::GetMainViewport()->Size;
    const ImVec2 vpCenter = ImVec2(vpSize.x * 0.5f, vpSize.y * 0.5f);

    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f * gFontScale, 440.0f * gFontScale),
                             ImGuiCond_Appearing);
    PushModalTitleStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(16.0f * gFontScale, 14.0f * gFontScale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.03f, 0.04f, 1.0f));
    if (ImGui::BeginPopupModal("MCP Servers", &open,
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {
        // Footer (separator + hints + button) stays put; only the list
        // scrolls. Same height derivation as the Sessions modal.
        const float itemSpacing = ImGui::GetStyle().ItemSpacing.y;
        const float fontH       = ImGui::GetFontSize();
        const float footerH = (1.0f + itemSpacing) +         // separator
                              2.0f * (fontH + itemSpacing) + // hint lines
                              (1.0f + itemSpacing) +          // spacing
                              (fontH + 2.0f * ImGui::GetStyle().FramePadding.y) + // button
                              itemSpacing;
        const float listH = std::max(ImGui::GetContentRegionAvail().y - footerH, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 16.0f * gFontScale);
        ImGui::BeginChild("##mcplist", ImVec2(0.0f, listH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        if (app.mcpServers.empty()) {
            ImGui::TextDisabled("No MCP servers configured. Add entries to:");
            ImGui::TextDisabled("%s", mcpConfigPath().c_str());
            ImGui::TextDisabled("then restart miniagent to pick them up.");
        } else {
            if (gFontMono)
                ImGui::PushFont(gFontMono);
            // Toggling mid-run would rewrite the registry the running agent
            // reads from, so the checkboxes are inert then (App guards too).
            ImGui::BeginDisabled(app.agentRunning);
            // Extra vertical gap between rows, no hover highlight (Sessions).
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(ImGui::GetStyle().ItemSpacing.x, 10.0f * gFontScale));
            for (size_t i = 0; i < app.mcpServers.size(); ++i) {
                McpServer& s = app.mcpServers[i];
                if (ImGui::Checkbox(("##mcp_" + s.name).c_str(), &s.enabled))
                    app.setMcpServerEnabled(i, s.enabled);
                ImGui::SameLine();
                // Status on the row's right: green when connected, grey when
                // disabled, red on a start-up failure.
                const char* state;
                ImU32 color;
                if (!s.enabled) {
                    state = "disabled";
                    color = IM_COL32(120, 120, 128, 255);
                } else if (s.running) {
                    state = "connected";
                    color = IM_COL32(90, 200, 120, 255);
                } else if (s.connecting->load()) {
                    // First uvx/npx runs download a package: this can take
                    // a while (it runs in the background, the UI stays live).
                    state = s.status.empty() ? "starting\u2026" : s.status.c_str();
                    color = IM_COL32(220, 190, 100, 255);
                } else if (s.status.empty()) {
                    state = "not connected";
                    color = IM_COL32(160, 160, 168, 255);
                } else {
                    state = s.status.c_str();
                    color = IM_COL32(230, 100, 90, 255);
                }
                ImGui::Text("%s", s.name.c_str());
                ImGui::SameLine();
                ImGui::TextColored(ImColor(color), "%s", state);
                if (s.running) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%zu tools)", s.tools.size());
                }
            }
            ImGui::PopStyleVar();
            ImGui::EndDisabled();
            if (gFontMono)
                ImGui::PopFont();
        }
        ImGui::PopStyleVar();  // WindowPadding
        ImGui::EndChild();
        ImGui::PopStyleVar();  // ScrollbarSize

        ImGui::Separator();
        ImGui::TextDisabled("Enabling starts the server now; disabling stops it. Tools are");
        ImGui::TextDisabled("named mcp_<server>_<tool>; new mcp.json entries need a restart.");
        ImGui::Spacing();
        const float closeW =
            ImGui::CalcTextSize("Close").x + 2.0f * ImGui::GetStyle().FramePadding.x;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - closeW);
        if (HandButton("Close"))
            open = false;

        ImGui::EndPopup();
    }
    app.mcpModalOpen = open;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    PopModalTitleStyle();
}
