#include "debug_hooks.h"

#include "app.h"

#include <chrono>
#include <cstdlib>
#include <string>

// Startup debug hooks, read once before the first frame (see
// .miniagent/skills/verify-ui/references/env-hooks.md). They describe the
// state of the first frame and exist so screenshots and render tests can
// photograph specific UI states without a live LLM.

void seedDebugHooks(App& app) {
    // Debug hook: open the session tree popup on startup (used with MINIAGENT_SCREENSHOT).
    if (std::getenv("MINIAGENT_TREE")) app.treeOpen = true;
    // Debug hook: open the session-list modal on startup (used with MINIAGENT_SCREENSHOT).
    if (std::getenv("MINIAGENT_SESSIONS")) app.sessionsModalOpen = true;
    // Debug hook: open the Backends modal on startup (used with MINIAGENT_SCREENSHOT).
    if (std::getenv("MINIAGENT_BACKENDS")) app.backendModalOpen = true;
    // Debug hook: open the scaffold task-list modal on startup (needs
    // MINIAGENT_FAKE_SCAFFOLD too, which keeps the modal open).
    if (std::getenv("MINIAGENT_TASKS")) app.scaffoldTasksOpen = true;
    // Debug hook: start in Scaffold mode (used with MINIAGENT_SCREENSHOT to
    // capture the interrupted-ledger Continue / New Session row).
    if (std::getenv("MINIAGENT_SCAFFOLD")) app.setScaffoldMode(true);
    // Debug hook: open a specific session file on startup.
    if (const char* sess = std::getenv("MINIAGENT_SESSION")) app.openSession(sess);
    // Debug hook: send a prompt on startup (after any opened session, so it
    // continues that session instead of starting a fresh one).
    if (const char* pre = std::getenv("MINIAGENT_PROMPT")) app.sendPromptText(pre);
    // Debug hook: expand every tool result row on startup.
    if (std::getenv("MINIAGENT_EXPAND"))
        for (auto& m : app.uiMessages)
            if (m.kind == App::UiMessage::Tool) m.toolExpanded = true;
    // Debug hook: arm the "Text copied" toast on startup (used with
    // MINIAGENT_SCREENSHOT and a small MINIAGENT_SCREENSHOT_AFTER).
    if (std::getenv("MINIAGENT_TOAST")) app.showToast("Text copied");
    // Debug hook: seed fake in-flight tool rows (partial streamed args) so a
    // screenshot shows the streaming bubble state without a live LLM.
    if (std::getenv("MINIAGENT_FAKE_STREAMING")) {
        App::UiMessage a;
        a.kind          = App::UiMessage::Tool;
        a.toolName      = "read";
        a.toolArgs      = "{\"path\": \"src/agent/agent.cpp\"";
        a.toolStreaming = true;
        a.toolIndex     = 0;
        app.uiMessages.push_back(a);
        App::UiMessage b;
        b.kind          = App::UiMessage::Tool;
        b.toolName      = "bash";
        b.toolArgs      = "{\"command\": \"grep -rn 'streaming' src --include=*.cpp | head -30\"";
        b.toolStreaming = true;
        b.toolIndex     = 1;
        app.uiMessages.push_back(b);
        App::UiMessage c;
        c.kind          = App::UiMessage::Tool;
        c.toolName      = "bash";
        c.toolArgs      =
            "{\"command\": \"find ~/Projects -name '*.cpp' -type f "
            "-exec grep -l streaming {} \\; | head -40 && echo done\"}";
        c.toolStreaming = true;
        c.toolIndex     = 2;
        app.uiMessages.push_back(c);
        // A write whose args stream content *before* the path key: the
        // bubble must not show the raw in-flight content.
        App::UiMessage w;
        w.kind          = App::UiMessage::Tool;
        w.toolName      = "write";
        w.toolArgs      = std::string("{\"content\": \"") + std::string(12000, 'x') +
                          "\", \"path\": \"src/main.cpp\"}";
        w.toolStreaming = true;
        w.toolIndex     = 3;
        app.uiMessages.push_back(w);
    }

    // Debug hook: seed a long-running tool row (started 5s ago) plus a slow
    // finished one, so a screenshot shows the live elapsed line and the
    // duration tag without a live LLM.
    if (std::getenv("MINIAGENT_FAKE_RUNNING")) {
        App::UiMessage a;
        a.kind         = App::UiMessage::Tool;
        a.toolName     = "bash";
        a.toolArgs     = "{\"command\": \"cmake --build build 2>&1 | tail -20 && ctest --"
                        "test-dir build --output-on-failure\"}";
        a.toolStartMs  = nowMs() - 5200;
        a.toolExpanded = true;
        app.uiMessages.push_back(a);
        App::UiMessage b;
        b.kind          = App::UiMessage::Tool;
        b.toolName      = "bash";
        b.toolArgs      = "{\"command\": \"scripts/run.sh --all --verbose\"}";
        b.toolDone      = true;
        b.toolResult    = "$ scripts/run.sh --all --verbose\n[exit code: 0]\n"
                       "sse_test: ok\ntoolrows_test: ok\n3 passed";
        b.toolElapsedMs = 14200;
        b.toolExpanded  = true;
        app.uiMessages.push_back(b);
        app.agentRunning = true;
    }

    // Debug hook: fake a running scaffold (Agent is null, so the prompt box,
    // mode toggle and session list are disabled) so a screenshot shows that
    // input-bar state without a live scaffold run. =gap fakes the between-
    // roles state (no live role) where the Compact button waits.
    if (std::getenv("MINIAGENT_FAKE_SCAFFOLD")) {
        app.agentRunning      = true;
        app.scaffoldRunActive = true;
        if (std::string(std::getenv("MINIAGENT_FAKE_SCAFFOLD")) != "gap")
            app.scaffoldRole = "worker";
        app.busyActive        = true;
        app.busyStart         = std::chrono::steady_clock::now();
    }

    // Debug hook: arm the harness-gate state (pair it with
    // MINIAGENT_FAKE_SCAFFOLD) so a screenshot shows the "Verifying <elapsed>"
    // status strip and its log row - the gate emits no model output, so this is
    // the only way to see that state offline.
    if (const char* gate = std::getenv("MINIAGENT_FAKE_VERIFY")) {
        app.agentRunning      = true;
        app.scaffoldRunActive = true;
        app.busyActive        = true;
        app.busyStart         = std::chrono::steady_clock::now() - std::chrono::seconds(96);
        app.verifyRunning     = true;
        app.verifyCmd         = *gate ? gate : "bash scripts/verify.sh";
        app.verifyStart       = std::chrono::steady_clock::now() - std::chrono::seconds(42);
        App::UiMessage v;
        v.kind = App::UiMessage::System;
        v.text = "harness verification: `" + app.verifyCmd + "` running (no model call)";
        app.uiMessages.push_back(v);
    }

    // Debug hook: arm the queued manual-compaction state (with
    // MINIAGENT_FAKE_RUNNING or MINIAGENT_FAKE_SCAFFOLD) so a screenshot
    // shows the disabled "Compacting..." button.
    if (std::getenv("MINIAGENT_FAKE_COMPACT")) app.compactQueued = true;

    // Debug hook: seed the loop state (Loop checkbox on, a prompt to replay,
    // the countdown running) so a screenshot shows the armed input bar and its
    // log row without a live run. The long delay keeps it from firing during
    // the capture.
    if (std::getenv("MINIAGENT_FAKE_LOOP")) {
        app.loopEnabled = true;
        app.loopPrompt  = "continue working on vfx-editor tool. polish it if done.";
        app.loopArmed   = true;
        app.loopFireAt  = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        App::UiMessage u;
        u.kind = App::UiMessage::User;
        u.text = app.loopPrompt;
        app.uiMessages.push_back(std::move(u));
        App::UiMessage s;
        s.kind = App::UiMessage::System;
        s.text = "Loop: re-running \"" + app.loopPrompt + "\" in 30s (Esc or uncheck Loop to stop).";
        app.uiMessages.push_back(std::move(s));
    }

    // Debug hook: seed retry/compaction system rows so a screenshot shows
    // the transient event rows without a failing provider.
    if (std::getenv("MINIAGENT_FAKE_SYSROWS")) {
        App::UiMessage u;
        u.kind = App::UiMessage::User;
        u.text = "Refactor the build scripts.";
        app.uiMessages.push_back(u);
        App::UiMessage r;
        r.kind = App::UiMessage::System;
        r.text =
            "Retrying (1/3) in 2s after: HTTP 503: {\"error\":{\"message\":\"overloaded_error\"}}";
        app.uiMessages.push_back(r);
        App::UiMessage r2;
        r2.kind = App::UiMessage::System;
        r2.text =
            "Retrying (2/3) in 3s after: HTTP 503: {\"error\":{\"message\":\"overloaded_error\"}}";
        app.uiMessages.push_back(r2);
        App::UiMessage c;
        c.kind = App::UiMessage::System;
        c.text = "Context compacted: 45231 -> 9102 tokens";
        app.uiMessages.push_back(c);
        App::UiMessage w;
        w.kind = App::UiMessage::Warning;
        w.text = "Output limit reached (16000 tokens) mid-reply, continuing where it stopped (1/8)";
        app.uiMessages.push_back(w);
        App::UiMessage a;
        a.kind = App::UiMessage::Agent;
        a.text = "Continuing after compaction: the retry landed and the context shrank.";
        app.uiMessages.push_back(a);
        App::UiMessage t;
        t.kind           = App::UiMessage::Thinking;
        t.text           = "Let me think about this carefully and then decide on an approach.";
        app.showThinking = true;
        app.uiMessages.push_back(t);
    }
}
