#include "ui/chords.h"

#include "imgui.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

bool ParseKeyChord(const char* name, App::KeyChord& out) {
    out = App::KeyChord{};
    std::string s(name);
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    std::string tok;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '+') {
            std::string part = s.substr(start, i - start);
            start = i + 1;
            if (part == "CTRL")
                out.ctrl = true;
            else if (part == "SHIFT")
                out.shift = true;
            else if (part == "ALT")
                out.alt = true;
            else if (part == "CMD" || part == "SUPER")
                out.ctrl = true;
            else if (!part.empty())
                tok = part;
        }
    }
    if (tok.size() == 1) {
        if (tok[0] >= 'A' && tok[0] <= 'Z') {
            out.key = (int)ImGuiKey_A + (tok[0] - 'A');
            return true;
        }
        if (tok[0] >= '0' && tok[0] <= '9') {
            out.key = (int)ImGuiKey_0 + (tok[0] - '0');
            return true;
        }
    }
    static const char* fnames[] = {"F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10", "F11", "F12"};
    for (int i = 0; i < 12; ++i)
        if (tok == fnames[i]) {
            out.key = (int)ImGuiKey_F1 + i;
            return true;
        }
    static const std::pair<const char*, int> named[] = {
        {"ENTER", (int)ImGuiKey_Enter},
        {"ESCAPE", (int)ImGuiKey_Escape},
        {"TAB", (int)ImGuiKey_Tab},
        {"SPACE", (int)ImGuiKey_Space},
        {"BACKSPACE", (int)ImGuiKey_Backspace},
        {"DELETE", (int)ImGuiKey_Delete},
        {"HOME", (int)ImGuiKey_Home},
        {"END", (int)ImGuiKey_End},
        {"PAGEUP", (int)ImGuiKey_PageUp},
        {"PAGEDOWN", (int)ImGuiKey_PageDown},
        {"LEFT", (int)ImGuiKey_LeftArrow},
        {"RIGHT", (int)ImGuiKey_RightArrow},
        {"UP", (int)ImGuiKey_UpArrow},
        {"DOWN", (int)ImGuiKey_DownArrow},
    };
    for (auto& p : named)
        if (tok == p.first) {
            out.key = p.second;
            return true;
        }
    return false;
}

bool ChordPressed(const App::KeyChord& c) {
    if (c.key < 0)
        return false;
    return ImGui::IsKeyPressed((ImGuiKey)c.key, true) &&
           c.ctrl == (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) &&
           c.shift == (ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift)) &&
           c.alt == (ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt));
}
