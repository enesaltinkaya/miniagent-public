// Loop mode (the Loop checkbox): arming at the parent AgentEnd, the countdown
// landing in tick(), the error backoff, and every cancel path. No LLM, no
// desktop daemon, no speech: notifyRun/speakRun/speakLoop/loopDispatch are
// swapped for recorders.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "agent/types.h"
#include "app.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

struct Rec {
    std::vector<std::string> spoken;
    std::vector<std::string> dispatched;
};

static void install(App& app, Rec& r) {
    app.notifyRun = [](const std::string&, const std::string&, bool) {};
    app.speakRun  = [](bool) {};
    app.speakLoop = [&](const std::string& p) { r.spoken.push_back(p); };
    app.loopDispatch = [&](const std::string& p) { r.dispatched.push_back(p); };
}

static void endRun(App& app, const char* error = nullptr) {
    if (error) {
        AgentEvent err;
        err.type = AgentEventType::Error;
        err.text = error;
        app.processEventForTest(err);
    }
    AgentEvent end;
    end.type = AgentEventType::AgentEnd;
    app.processEventForTest(end);
}

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1);
    setenv("MINIAGENT_NO_PROBE", "1", 1);

    // Loop off: a finished run arms nothing.
    {
        App app;
        Rec r;
        install(app, r);
        app.loopPrompt = "continue working on vfx-editor";
        endRun(app);
        check(!app.loopArmed, "loop off: nothing armed");
        check(r.dispatched.empty(), "loop off: nothing re-sent");
    }
    // Loop on: the run settling arms the countdown, speaks, and the countdown
    // landing replays the exact prompt through the loop seam.
    {
        App app;
        Rec r;
        install(app, r);
        app.loopEnabled = true;
        app.loopDelayS  = 0.05;
        app.loopPrompt  = "continue working on vfx-editor";
        endRun(app);
        check(app.loopArmed, "loop on: countdown armed at AgentEnd");
        check(app.loopSecondsLeft() >= 0, "loop on: countdown readable");
        check(r.spoken.size() == 1 && r.spoken[0] == "loop in 0 seconds",
              "loop on: armed cue spoken");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        app.tick();
        check(r.dispatched.size() == 1 && r.dispatched[0] == "continue working on vfx-editor",
              "countdown expiry re-sends the exact prompt");
        check(!app.loopArmed && app.loopIteration == 1, "fired: countdown cleared, iteration counted");
        check(r.spoken.size() == 2 && r.spoken[1] == "loop 1", "fired: iteration cue spoken");
        // The replayed run settles -> the loop arms again (that is the point).
        endRun(app);
        check(app.loopArmed, "replayed run re-arms the loop");
    }
    // A draft typed during the pause survives the replay.
    {
        App app;
        Rec r;
        install(app, r);
        app.loopEnabled = true;
        app.loopDelayS  = 0.05;
        app.loopPrompt  = "task";
        snprintf(app.inputBuf, sizeof(app.inputBuf), "next task queued by hand");
        endRun(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        app.tick();
        check(r.dispatched.size() == 1, "fire: prompt dispatched with a draft up");
        check(std::string(app.inputBuf) == "next task queued by hand",
              "fire: the typed draft is put back");
    }
    // Each iteration starts a FRESH session: the previous conversation, log
    // and context counters are cleared before the replay is dispatched.
    {
        App app;
        Rec r;
        install(app, r);
        app.loopEnabled = true;
        app.loopDelayS  = 0.05;
        app.loopPrompt  = "task";
        app.contextTokens = 4321;
        app.sessionUsage.totalTokens = 99;
        App::UiMessage old;
        old.kind = App::UiMessage::User;
        old.text = "previous iteration's transcript";
        app.uiMessages.push_back(old);
        endRun(app);
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        app.tick();
        check(r.dispatched.size() == 1, "fire: prompt dispatched");
        check(app.contextTokens == 0, "fire: context estimate cleared");
        check(app.sessionUsage.totalTokens == 0, "fire: session usage cleared");
        check(app.uiMessages.empty(), "fire: previous transcript cleared");
    }
    // An abort never restarts (the user said stop).
    {
        App app;
        Rec r;
        install(app, r);
        app.loopEnabled = true;
        app.loopPrompt  = "task";
        app.runAbort    = true;
        endRun(app);
        check(!app.loopArmed, "aborted run: nothing armed");
    }
    // Error backoff: kLoopErrorLimit error runs in a row stop the loop, and a
    // clean run resets the streak.
    {
        App app;
        Rec r;
        install(app, r);
        app.loopEnabled = true;
        app.loopPrompt  = "task";
        endRun(app, "HTTP 500: boom");
        check(app.loopArmed, "error 1: still armed");
        app.cancelLoop(true);
        endRun(app, "HTTP 500: boom");
        check(app.loopArmed, "error 2: still armed");
        app.cancelLoop(true);
        endRun(app);  // clean run resets the streak
        check(app.loopArmed, "clean run resets the error streak");
        app.cancelLoop(true);
        endRun(app, "boom");
        endRun(app, "boom");
        app.cancelLoop(true);
        endRun(app, "boom");
        check(!app.loopArmed, "error limit: loop stops");
        check(!app.loopEnabled, "error limit: the checkbox unchecks itself");
        bool stopped = false;
        for (const auto& s : r.spoken)
            if (s == "loop stopped after errors") stopped = true;
        check(stopped, "error limit: stop announced");
        endRun(app);
        check(!app.loopArmed, "error limit: a later run does not restart the cycle");
    }
    // Cancel paths: unchecking, Esc (onEscape), newSession.
    {
        App app;
        Rec r;
        install(app, r);
        app.loopEnabled = true;
        app.loopDelayS  = 30.0;
        app.loopPrompt  = "task";
        endRun(app);
        check(app.loopArmed, "cancel: armed before uncheck");
        app.setLoopEnabled(false);
        check(!app.loopArmed, "unchecking cancels the pending restart");

        app.loopEnabled = true;
        endRun(app);
        app.onEscape();
        check(!app.loopArmed, "Esc cancels the pending restart");

        endRun(app);
        app.newSession();
        check(!app.loopArmed, "new session cancels the pending restart");

        endRun(app);
        app.stopAgent();
        check(!app.loopArmed, "stop cancels the pending restart");
    }
    // Unchecking mid-countdown must not re-arm on the next run end.
    {
        App app;
        Rec r;
        install(app, r);
        app.loopEnabled = true;
        app.loopPrompt  = "task";
        endRun(app);
        app.setLoopEnabled(false);
        check(!app.loopArmed, "loop off again: the pending pause is dropped");
        endRun(app);
        check(!app.loopArmed, "loop off again: no re-arm");
        // A loop switched off by any other path still cannot fire: tick()
        // drops the pause instead of replaying.
        app.loopEnabled = true;
        endRun(app);
        app.loopEnabled = false;
        app.tick();
        check(!app.loopArmed && r.dispatched.empty(), "loop off during the pause: nothing fires");
    }
    return failures ? 1 : 0;
}
