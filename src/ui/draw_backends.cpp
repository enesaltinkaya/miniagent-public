#include "ui/draw.h"

#include "util/settings.h"

// Centered modal listing the LLM backends (backends.json), opened by
// clicking the backend URL link in the status panel. Selecting a row
// switches to that backend and persists the choice in miniagent.settings.
// It takes effect for the next prompt: a running turn keeps the backend
// it was started on (its provider is a copy made at dispatch time).
// Each row also carries an "Image" button that designates (or undoes) the
// image model; while one is set, the analyze_image tool is registered and
// sends image questions to that backend.
void DrawBackendModal(App& app) {
    // BeginPopupModal() is called even when the modal is closed: setting
    // *p_open to false before calling it is Dear ImGui's documented way to
    // close a popup (Escape goes through App::onEscape).
    bool open = app.backendModalOpen;
    if (open && !ImGui::IsPopupOpen("Backends"))
        ImGui::OpenPopup("Backends");

    const ImVec2 vpSize   = ImGui::GetMainViewport()->Size;
    const ImVec2 vpCenter = ImVec2(vpSize.x * 0.5f, vpSize.y * 0.5f);

    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f * gFontScale, 440.0f * gFontScale),
                             ImGuiCond_Appearing);
    PushModalTitleStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(16.0f * gFontScale, 14.0f * gFontScale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.03f, 0.04f, 1.0f));
    if (ImGui::BeginPopupModal("Backends", &open,
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {
        // Footer (separator + hints + button) stays put; only the list
        // scrolls. Same height derivation as the MCP modal.
        const float itemSpacing = ImGui::GetStyle().ItemSpacing.y;
        const float fontH       = ImGui::GetFontSize();
        const float footerH = (1.0f + itemSpacing) +         // separator
                              2.0f * (fontH + itemSpacing) + // hint lines
                              (1.0f + itemSpacing) +          // spacing
                              (fontH + 2.0f * ImGui::GetStyle().FramePadding.y) + // button
                              itemSpacing;
        const float listH = std::max(ImGui::GetContentRegionAvail().y - footerH, 0.0f);
        ImGui::BeginChild("##backendlist", ImVec2(0.0f, listH));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        if (app.backends.empty()) {
            ImGui::TextDisabled("No backends configured. Add entries to:");
            ImGui::TextDisabled("%s", backendsPath().c_str());
            ImGui::TextDisabled("then restart miniagent to pick them up.");
        } else {
            // One full-width invisible button per backend; the row's text
            // (bold name, mono URL below it) is drawn inside the button
            // rect on the draw list so each row spans the full width.
            ImFont* nameFont = gFontBold ? gFontBold : gFontDefault;
            ImFont* monoFont = gFontMono ? gFontMono : gFontDefault;
            const float nameFs = FONT_SIZE_DEFAULT * gFontScale;
            const float monoFs = FONT_SIZE_MONO * gFontScale;
            const float nameLH =
                nameFont->CalcTextSizeA(nameFs, FLT_MAX, 0.0f, "Ag", "Ag" + 2).y;
            const float monoLH =
                monoFont->CalcTextSizeA(monoFs, FLT_MAX, 0.0f, "Ag", "Ag" + 2).y;
            const float padX = 10.0f * gFontScale, padY = 8.0f * gFontScale;
            const float rowGap = 4.0f * gFontScale;
            const float rowH   = padY * 2 + nameLH + rowGap + monoLH;
            const float framePadX = ImGui::GetStyle().FramePadding.x;
            const char* kImgLabelOn  = "Image model";
            const char* kImgLabelOff = "Image";
            const float imgBtnW = ImGui::CalcTextSize(kImgLabelOn).x + 2.0f * framePadX;
            const float imgGap  = ImGui::GetStyle().ItemSpacing.x;
            // The per-row Image button sits at the row's right edge; the row's
            // text (name, URL) gets the remaining width.
            const float textMaxW = ImGui::GetContentRegionAvail().x - padX * 2 -
                                   imgBtnW - imgGap;
            const float urlMaxW  = textMaxW;
            // Extra vertical gap between rows (like the MCP list).
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                                ImVec2(ImGui::GetStyle().ItemSpacing.x, 10.0f * gFontScale));
            for (size_t i = 0; i < app.backends.size(); ++i) {
                const LlmBackend& b = app.backends[i];
                const bool sel      = i == app.backendIndex;
                const bool isImg    = i == app.imageBackendIndex;
                if (ImGui::InvisibleButton(("##backend_" + std::to_string(i)).c_str(),
                                            ImVec2(ImGui::GetContentRegionAvail().x - imgBtnW - imgGap, rowH))) {
                    app.setBackend(i);
                    std::map<std::string, std::string> settings = loadSettings();
                    settings["backend_index"] = std::to_string(i);
                    saveSettings(settings);
                    open = false;
                }
                // The row is a plain InvisibleButton (no styling/hand cursor);
                // show the pointer so rows read as clickable, like HandButton.
                if (ImGui::IsItemHovered())
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                const ImVec2 rMin = ImGui::GetItemRectMin();
                // Right-edge toggle: designate this backend as the image
                // model (clicking the marked row undoes it). The modal stays
                // open so the marker can be verified / moved. PushID keeps
                // the button's ID unique per row (the label repeats).
                ImGui::SameLine(0.0f, imgGap);
                ImGui::PushID((int)i);
                if (isImg)
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.30f, 0.55f, 1.0f));
                else
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.10f, 0.10f, 0.13f, 1.0f));
                if (HandButton(isImg ? kImgLabelOn : kImgLabelOff, ImVec2(imgBtnW, 0)))
                    app.setImageBackend(i);
                ImGui::PopStyleColor();
                ImGui::PopID();
                ImDrawList* dl    = ImGui::GetWindowDrawList();
                float x = rMin.x + padX, y = rMin.y + padY;
                // Name: the link blue when selected, default text color
                // otherwise (bold either way).
                const ImU32 nameCol =
                    sel ? IM_COL32(90, 160, 255, 255) : ImGui::GetColorU32(ImGuiCol_Text);
                const std::string nameShown = SingleLineEllipsized(b.name, textMaxW);
                dl->AddText(nameFont, nameFs, ImVec2(x, y), nameCol,
                            nameShown.c_str(), nameShown.c_str() + nameShown.size());
                y += nameLH + rowGap;
                // URL, ellipsized to the row width.
                if (gFontMono)
                    ImGui::PushFont(gFontMono);
                const std::string urlShown =
                    SingleLineEllipsized(b.url, urlMaxW);
                dl->AddText(monoFont, monoFs, ImVec2(x, y),
                            ImGui::GetColorU32(ImGuiCol_TextDisabled),
                            urlShown.c_str(), urlShown.c_str() + urlShown.size());
                if (gFontMono)
                    ImGui::PopFont();
            }
            ImGui::PopStyleVar();
        }
        ImGui::PopStyleVar();  // WindowPadding
        ImGui::EndChild();

        ImGui::Separator();
        ImGui::TextDisabled("Picking a row selects the active backend; the Image button");
        ImGui::TextDisabled("designates the image model behind the analyze_image tool.");
        ImGui::Spacing();
        const float closeW =
            ImGui::CalcTextSize("Close").x + 2.0f * ImGui::GetStyle().FramePadding.x;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - closeW);
        if (HandButton("Close"))
            open = false;

        ImGui::EndPopup();
    }
    app.backendModalOpen = open;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    PopModalTitleStyle();
}
