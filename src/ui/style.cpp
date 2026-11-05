#include "ui/draw.h"

#include "ui/fonts.h"

// Push/pop the style that makes a modal's title bar look like the desktop
// window manager's own title bar (Breeze Dark) instead of Dear ImGui's
// blue active title bar: a neutral near-black bar (sampled from the native
// frame: srgb 17,17,17), rounded window corners (Breeze default 4px) and a
// subtle 1px frame border (srgb 31,39,39). Title stays left-aligned and the
// close button stays on the right, like the native frame. Push/Pop must be
// balanced, and the style must be active at BeginPopupModal().
void PushModalTitleStyle() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * gFontScale);
    ImGui::PushStyleColor(ImGuiCol_TitleBg, ImVec4(0.067f, 0.067f, 0.067f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_TitleBgActive, ImVec4(0.067f, 0.067f, 0.067f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.122f, 0.153f, 0.153f, 1.0f));
}

void PopModalTitleStyle() {
    ImGui::PopStyleColor();  // Border
    ImGui::PopStyleColor();  // TitleBgActive
    ImGui::PopStyleColor();  // TitleBg
    ImGui::PopStyleVar();  // WindowRounding
}
