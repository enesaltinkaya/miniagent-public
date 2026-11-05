// Regression test for the scaffold-run message-log view (draw_message_log.cpp
// + App): while a scaffold dispatch is live the chat area shows only that
// dispatch's rows (each dispatch starts on a clean page), the pinned
// scaffoldViewDispatch swaps in an earlier dispatch's transcript, and the
// view resets when the parent itself talks (final report) or the run ends.
// Dispatches are identified by their subagent-log entry, not the role name:
// the same role runs several times in one run (manager between worker
// rounds), and pinning one manager entry must not show the other's rows.
// Drives processEventForTest + DrawMessageLog headless through a bare ImGui
// context.
#include <cstdio>
#include <cstdlib>
#include <string>

#include <SDL3/SDL.h>

#include "imgui.h"

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

static AgentEvent mkEv(AgentEventType type, const char* agent = nullptr) {
    AgentEvent e{};
    e.type = type;
    if (agent) {
        e.nested      = true;
        e.nestedAgent = agent;
    }
    return e;
}

static void dispatch(App& app, const char* role, const char* text) {
    app.processEventForTest(mkEv(AgentEventType::AgentStart, role));
    app.processEventForTest(mkEv(AgentEventType::TurnStart, role));
    AgentEvent td = mkEv(AgentEventType::TextDelta, role);
    td.text       = text;
    app.processEventForTest(td);
}

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1);  // never start the user's real MCP servers in tests
    setenv("MINIAGENT_NO_PROBE", "1", 1);  // no real network probe from a headless App
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_Init(SDL_INIT_VIDEO);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    io.DisplaySize = ImVec2(1280.0f, 800.0f);
    SetupFonts(io, 1.0f);

    App app;
    app.showThinking = true;

    App::UiMessage user;
    user.kind = App::UiMessage::User;
    user.text = "do the thing";
    app.uiMessages.push_back(user);

    // A scaffold run is up (App::startScaffold sets this on the UI thread).
    app.scaffoldRunActive = true;

    // Three dispatches: manager (0), brainstorm (1), manager again (2) --
    // the same role name runs twice, like the manager between worker rounds.
    dispatch(app, "manager", "manager plan");
    app.processEventForTest(mkEv(AgentEventType::AgentEnd, "manager"));
    dispatch(app, "brainstorm", "idea one, idea two");
    app.processEventForTest(mkEv(AgentEventType::AgentEnd, "brainstorm"));
    dispatch(app, "manager", "manager round two");

    check(app.scaffoldRoleLog.size() == 3, "one log entry per dispatch");
    check(app.scaffoldDisplayDispatch() == 2,
          "live view follows the newest (running) dispatch");

    auto rowOf = [&](const std::string& text) -> App::UiMessage* {
        for (auto& m : app.uiMessages)
            if (m.kind == App::UiMessage::Agent && !m.subagentHeader && m.text == text)
                return &m;
        return nullptr;
    };

    long frameNo = 0;
    auto frame = [&](auto&& body) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1280, 800), ImGuiCond_Always);
        ImGui::Begin("##main");
        body();
        frameNo = (long)ImGui::GetFrameCount();
        ImGui::End();
        ImGui::EndFrame();
    };

    // Live view: only the second manager dispatch's rows are drawn.
    frame([&] { DrawMessageLog(app); });
    {
        App::UiMessage* m1 = rowOf("manager plan");
        App::UiMessage* m2 = rowOf("manager round two");
        App::UiMessage* b  = rowOf("idea one, idea two");
        check(m1 && m2 && b, "every dispatch's rows exist in the full log");
        check(m2 && m2->dispatch == 2 && m2->richLayout.frame == frameNo,
              "live view draws the running dispatch's rows");
        check(m1 && m1->dispatch == 0 && m1->richLayout.frame != frameNo,
              "live view skips the first manager dispatch's rows");
        check(b && b->dispatch == 1 && b->richLayout.frame != frameNo,
              "live view skips the finished dispatch's rows");
        check(app.uiMessages[0].dispatch == -1 && app.uiMessages[0].richLayout.frame != frameNo,
              "live view skips the parent's user prompt");
    }

    // Pin the FIRST manager entry (index 0): its transcript shows, the second
    // manager dispatch's rows do not -- same role name, different dispatch.
    app.scaffoldViewDispatch = 0;
    app.msgScrollToTop       = true;
    check(app.scaffoldDisplayDispatch() == 0, "pinned view shows the chosen dispatch");
    frame([&] { DrawMessageLog(app); });
    {
        App::UiMessage* m1 = rowOf("manager plan");
        App::UiMessage* m2 = rowOf("manager round two");
        check(m1 && m1->richLayout.frame == frameNo,
              "pinned view draws the chosen dispatch's rows");
        check(m2 && m2->richLayout.frame != frameNo,
              "pinned view skips the other same-name dispatch's rows");
        check(!app.msgScrollToTop, "the scroll-to-top request is consumed once");
        check(!app.stickToBottom, "a pinned review does not stick to the bottom");
    }

    // Pin the SECOND manager entry (index 2): exactly its rows show.
    app.scaffoldViewDispatch = 2;
    frame([&] { DrawMessageLog(app); });
    {
        App::UiMessage* m1 = rowOf("manager plan");
        App::UiMessage* m2 = rowOf("manager round two");
        check(m2 && m2->richLayout.frame == frameNo,
              "pinning the second manager entry shows its rows");
        check(m1 && m1->richLayout.frame != frameNo,
              "the first manager entry's rows stay hidden");
    }

    // Unpin (click the live dispatch's entry): the live view returns.
    app.scaffoldViewDispatch = -1;
    app.stickToBottom = true;
    check(app.scaffoldDisplayDispatch() == 2, "unpinning returns to the live dispatch");

    // The harness gate (the plan's pinned build/test command) emits no model
    // output, so it has to be visible where the user is already looking: its
    // start row joins the live dispatch's page, and the verify state arms the
    // status strip's live "Verifying <elapsed>" readout.
    {
        const int live = app.scaffoldDisplayDispatch();
        AgentEvent gs  = mkEv(AgentEventType::Verification);
        gs.text          = "bash scripts/verify.sh";
        gs.verifyRunning = true;
        app.processEventForTest(gs);
        check(app.verifyRunning && app.verifyCmd == "bash scripts/verify.sh",
              "the gate's start event arms the live verify state");
        check(app.verifyElapsedMs() >= 0, "the gate exposes a readable elapsed time");
        bool onLivePage = false;
        for (const auto& m : app.uiMessages)
            if (m.text.find("bash scripts/verify.sh") != std::string::npos &&
                m.dispatch == live)
                onLivePage = true;
        check(onLivePage && live >= 0, "the gate's row lands on the page being viewed");

        AgentEvent ge  = mkEv(AgentEventType::Verification);
        ge.text        = "verification `bash scripts/verify.sh` -> exit 1 in 5s";
        ge.verifyExit  = 1;
        app.processEventForTest(ge);
        check(!app.verifyRunning && app.verifyCmd.empty(),
              "the gate's end event clears the live state");
        bool failRow = false;
        for (const auto& m : app.uiMessages)
            if (m.kind == App::UiMessage::Error && m.text.find("exit 1") != std::string::npos)
                failRow = true;
        check(failRow, "a failing gate reads as an error row");
    }

    // The run's final report is a parent turn: the view resets to the full
    // log so the answer (and everything else) is visible again.
    app.processEventForTest(mkEv(AgentEventType::AgentEnd, "manager"));
    app.scaffoldViewDispatch = 0;  // still pinned from an earlier review
    app.processEventForTest(mkEv(AgentEventType::TurnStart));
    check(app.scaffoldDisplayDispatch() == -1, "the parent turn (report) resets the view");
    AgentEvent rep = mkEv(AgentEventType::TextDelta);
    rep.text = "final report text";
    app.processEventForTest(rep);
    frame([&] { DrawMessageLog(app); });
    check(app.uiMessages[0].richLayout.frame == frameNo,
          "full log view draws the user prompt again");

    // Run end clears the pin and the role log.
    app.scaffoldViewDispatch = 0;
    app.processEventForTest(mkEv(AgentEventType::AgentEnd));
    check(!app.scaffoldRunActive && app.scaffoldRoleLog.empty() && app.scaffoldViewDispatch == -1,
          "run end clears the role log and the pinned view");

    if (failures == 0)
        printf("scaffold_view_test: all passed\n");
    else
        printf("scaffold_view_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
