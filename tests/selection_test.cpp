// Regression test for drag-selecting a message body (draw_message_log.cpp):
// a wordless (empty) layout line must anchor at its byte offset instead of
// indexing words[-1], and a selection in progress must survive a
// uiMessages reallocation (it is tracked by index, not pointer). Drives
// DrawMessageLog headless through a bare ImGui context.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <SDL3/SDL.h>

#include "imgui.h"
#include "imgui_internal.h"

#include "agent/types.h"
#include "app.h"
#include "ui/draw.h"
#include "ui/fonts.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1);  // never start the user's real MCP servers in tests
    setenv("MINIAGENT_NO_PROBE", "1", 1);  // no real network probe from a headless App
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_Init(SDL_INIT_VIDEO);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // No GL backend here: claim renderer-managed textures so ImGui builds
    // the font atlas itself instead of asserting on a missing upload.
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    SetupFonts(io, 1.0f);

    App app;
    app.showThinking = true;  // config may have persisted it off on this machine
    App::UiMessage th;
    th.kind = App::UiMessage::Thinking;
    th.text = "first line of thinking\n\nsecond line\nthird";
    app.uiMessages.push_back(th);

    auto frame = [&](auto&& body) {
        ImGui::NewFrame();
        // This ImGui build applies next-window settings only within the
        // frame scope (after NewFrame), and a call before NewFrame even
        // breaks the frame scope, so the order matters here.
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1280, 800), ImGuiCond_Always);
        ImGui::Begin("##main");
        body();
        ImGui::End();
        ImGui::EndFrame();
    };

    frame([&] { DrawMessageLog(app); });  // records the layout for this frame

    const RichTextLine& blank = app.uiMessages[0].richLayout.lines[1];
    check(app.uiMessages[0].richLayout.lines.size() == 4,
          "four visual lines (line 2 is the empty one)");
    check(blank.firstWord < 0, "empty line recorded as wordless");
    const RichTextLine& third = app.uiMessages[0].richLayout.lines[3];

    // Frame 2: click the empty line -> a selection starts there. Before the
    // words[-1] fix this indexed RichTextLayout::words out of bounds.
    io.AddMousePosEvent(blank.x0 + 4, (blank.y0 + blank.y1) * 0.5f);
    io.AddMouseButtonEvent(0, true);
    frame([&] { DrawMessageLog(app); });
    check(app.msgSel.active, "click on the empty line starts a selection");
    check(app.msgSel.msg == 0, "selection tracked by message index");
    check(app.msgSel.start == app.msgSel.end && app.msgSel.start == blank.byteStart,
          "empty-line hit anchors at the line's byte offset");

    // Frame 3: drag onto the last line.
    io.AddMousePosEvent(third.x0 + 1, (third.y0 + third.y1) * 0.5f);
    frame([&] { DrawMessageLog(app); });
    check(app.msgSel.end == third.byteStart,
          "drag end lands at the last line's byte offset");

    // Frame 4: release -> the selection is copied to the clipboard.
    io.AddMouseButtonEvent(0, false);
    frame([&] { DrawMessageLog(app); });
    check(!app.msgSel.active, "selection dropped after release");
    check(app.toast.text == "Text copied", "release armed the copied toast");
    check(app.msgSel.msg == std::size_t(-1) || app.msgSel.msg < app.uiMessages.size(),
          "index stays valid after release");

    // Frame 5: start a fresh drag on the "second line" (capture the
    // geometry by value: a push_back below reallocates the vector and
    // moves the messages, invalidating references into their layouts).
    const int secondByteStart = app.uiMessages[0].richLayout.lines[2].byteStart;
    const float secondY = (app.uiMessages[0].richLayout.lines[2].y0 +
                           app.uiMessages[0].richLayout.lines[2].y1) *
        0.5f;
    // x just inside the line's left edge: left of the first character's
    // midpoint, so the hit-test anchors at the line's byteStart.
    io.AddMousePosEvent(app.uiMessages[0].richLayout.lines[2].x0 + 1, secondY);
    io.AddMouseButtonEvent(0, true);
    frame([&] { DrawMessageLog(app); });
    check(app.msgSel.active && app.msgSel.msg == 0, "new drag started on line 3");
    check(app.msgSel.start == secondByteStart, "new drag anchored on line 3");

    // ...then the vector reallocates underneath it (streaming pushes rows).
    for (int i = 0; i < 64; ++i) {
        App::UiMessage row;
        row.kind = App::UiMessage::System;
        row.text = "x";
        app.uiMessages.push_back(std::move(row));
    }
    frame([&] { DrawMessageLog(app); });
    check(app.msgSel.active, "selection survives a uiMessages reallocation mid-drag");
    check(app.msgSel.msg == 0, "selection still points at the first message");
    check(app.msgSel.start == secondByteStart, "selection start unchanged by reallocation");

    // Regression: while a scaffold run is up, the floating task panel sits
    // over this log. Without the panel-rect guard, a press anywhere on the
    // panel (its scrollbar included) landed on the message text behind it
    // and started a selection that drag+release then copied (the stray
    // "Text copied" toast).
    //
    // The drag above still holds the button down in the shared ImGui io
    // state; release it first, or the block's press would be filtered as a
    // duplicate by AddMouseButtonEvent.
    io.AddMouseButtonEvent(0, false);
    frame([&] { DrawMessageLog(app); });
    {
        App sapp;
        sapp.showThinking = true;
        sapp.scaffoldRunActive = true;
        auto sev = [](AgentEventType t, const char* agent) {
            AgentEvent e{};
            e.type      = t;
            e.nested    = true;
            e.nestedAgent = agent;
            return e;
        };
        sapp.processEventForTest(sev(AgentEventType::AgentStart, "worker"));
        sapp.processEventForTest(sev(AgentEventType::TurnStart, "worker"));
        std::string body;
        for (int i = 0; i < 30; ++i)
            body += "worker transcript line " + std::to_string(i) + "\n";
        AgentEvent td = sev(AgentEventType::TextDelta, "worker");
        td.text       = body.c_str();
        sapp.processEventForTest(td);

        auto sframe = [&](auto&& b) {
            ImGui::NewFrame();
            ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
            ImGui::SetNextWindowSize(ImVec2(1280, 800), ImGuiCond_Always);
            ImGui::Begin("##main");
            b();
            ImGui::End();
            ImGui::EndFrame();
        };
        // Open the transcript at the top (the live view pins to the bottom,
        // which would scroll the body away from the panel). Scroll targets
        // apply one frame late, hence the settle frame.
        sapp.msgScrollToTop = true;
        ImRect tasklistRect;
        sframe([&] { DrawMessageLog(sapp); });
        sframe([&] {
            DrawMessageLog(sapp);
            for (ImGuiWindow* lw : ImGui::GetCurrentContext()->Windows)
                if (lw->Name && strstr(lw->Name, "##tasklist_"))
                    tasklistRect = lw->Rect();
        });
        const App::UiMessage* w = nullptr;
        for (const auto& m : sapp.uiMessages)
            if (m.kind == App::UiMessage::Agent && m.text == body)
                w = &m;
        check(w && !w->richLayout.lines.empty(), "worker body has a fresh layout");
        // The reported path: the task list's scrollbar. Its grab sits just
        // inside the list child's right edge; the message text behind the
        // panel runs the list's full height, so find a body line whose band
        // crosses the list.
        const RichTextLine* underSb = nullptr;
        if (w && tasklistRect.Min.x < tasklistRect.Max.x)
            for (const RichTextLine& l : w->richLayout.lines)
                if (l.y0 >= tasklistRect.Min.y && l.y1 <= tasklistRect.Max.y) {
                    underSb = &l;
                    break;
                }
        if (!underSb) {
            check(false, "a worker body line crosses the task list");
        } else {
            check(true, "a worker body line crosses the task list");
            const float midY = (underSb->y0 + underSb->y1) * 0.5f;
            const float sbX  = tasklistRect.Max.x - 8.0f;
            io.AddMousePosEvent(sbX, midY);
            io.AddMouseButtonEvent(0, true);
            sframe([&] { DrawMessageLog(sapp); });
            check(!sapp.msgSel.active, "press on the task list's scrollbar starts no selection");
            io.AddMouseButtonEvent(0, false);
            sframe([&] { DrawMessageLog(sapp); });
            check(sapp.toast.text != "Text copied", "release over the scrollbar copies nothing");

            // Control: the same line left of the panel still selects.
            io.AddMousePosEvent(underSb->x0 + 1.0f, midY);
            io.AddMouseButtonEvent(0, true);
            sframe([&] { DrawMessageLog(sapp); });
            check(sapp.msgSel.active, "the line itself still selects left of the panel");
        }
    }

    if (failures == 0)
        printf("selection_test: all passed\n");
    else
        printf("selection_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
