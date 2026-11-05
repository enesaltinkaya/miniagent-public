// Desktop-notification branch on run end: feed synthetic events through
// processEvent with a recording notifyRun (no LLM, no desktop daemon).
#include <cstdio>
#include <cstdlib>
#include <string>

#include "agent/compaction.h"
#include "agent/types.h"
#include "app.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

struct Rec {
    bool called = false;
    std::string title;
    std::string body;
    bool isError = false;
    bool spoke = false;
    bool spokeError = false;
};

static void install(App& app, Rec& r) {
    app.notifyRun = [&](const std::string& t, const std::string& b, bool e) {
        r.called = true;
        r.title  = t;
        r.body   = b;
        r.isError = e;
    };
    app.speakRun = [&](bool e) {
        r.spoke = true;
        r.spokeError = e;
    };
}

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1);  // never start the user's real MCP servers in tests
    setenv("MINIAGENT_NO_PROBE", "1", 1);  // no real network probe from a headless App
    // Clean finish: done toast, not an error.
    {
        App app;
        Rec r;
        install(app, r);
        AgentEvent end;
        end.type = AgentEventType::AgentEnd;
        app.processEventForTest(end);
        check(r.called, "done: notification fired");
        check(r.title == "miniagent" && !r.isError, "done: title/flag");
        check(r.body == "Done \u2014 waiting for input", "done: body");
        check(r.spoke && !r.spokeError, "done: speech fired");
        check(app.runLastError.empty(), "done: error state cleared");
    }
    // Parent error: error toast with the error text.
    {
        App app;
        Rec r;
        install(app, r);
        AgentEvent err;
        err.type = AgentEventType::Error;
        err.text = "HTTP 500: boom";
        app.processEventForTest(err);
        AgentEvent end;
        end.type = AgentEventType::AgentEnd;
        app.processEventForTest(end);
        check(r.called, "error: notification fired");
        check(r.title == "miniagent \u2014 error" && r.isError, "error: title/flag");
        check(r.body == "HTTP 500: boom", "error: body is the error text");
        check(r.spoke && r.spokeError, "error: speech fired");
    }
    // Aborted plain run (Error "Aborted" + runAbort): neutral stopped toast.
    {
        App app;
        Rec r;
        install(app, r);
        app.runAbort = true;
        AgentEvent err;
        err.type = AgentEventType::Error;
        err.text = "Aborted";
        app.processEventForTest(err);
        AgentEvent end;
        end.type = AgentEventType::AgentEnd;
        app.processEventForTest(end);
        check(r.called, "aborted: notification fired");
        check(!r.isError, "aborted: not an error");
        check(r.body == "Stopped (aborted by user)", "aborted: body");
        check(!r.spoke, "aborted: silent");
    }
    // Scaffold abort (runAbort without a parent Error event).
    {
        App app;
        Rec r;
        install(app, r);
        app.runAbort = true;
        AgentEvent end;
        end.type = AgentEventType::AgentEnd;
        app.processEventForTest(end);
        check(r.called, "scaffold abort: notification fired");
        check(!r.isError && r.body == "Stopped (aborted by user)", "scaffold abort: body");
        check(!r.spoke, "scaffold abort: silent");
    }
    // Nested (subagent) errors must not turn the final toast into an error.
    {
        App app;
        Rec r;
        install(app, r);
        AgentEvent err;
        err.type        = AgentEventType::Error;
        err.text        = "nested boom";
        err.nested      = true;
        err.nestedAgent = "worker";
        app.processEventForTest(err);
        AgentEvent end;
        end.type = AgentEventType::AgentEnd;
        app.processEventForTest(end);
        check(r.called, "nested error: notification fired");
        check(!r.isError && r.body == "Done \u2014 waiting for input", "nested error: done body");
        check(r.spoke && !r.spokeError, "nested error: done speech");
    }

    // Manual-compaction queue (the input bar's Compact button): armed only
    // while a run is up, cleared by the compaction note or the run's end.
    {
        App app;
        app.queueCompaction();
        check(!app.compactQueued.load(), "compact: idle run queues nothing");
        app.agentRunning = true;
        app.queueCompaction();
        check(!app.compactQueued.load(),
              "compact: a scaffold gap (no live role) queues nothing");
        app.scaffoldRunActive = true;
        app.scaffoldRole = "worker";
        app.queueCompaction();
        check(app.compactQueued.load() && app.manualCompactRequest.load(),
              "compact: scaffold queue arms the shared role request");
        app.queueCompaction();
        check(app.compactQueued.load(), "compact: second click is a no-op");
        AgentEvent c;
        c.type = AgentEventType::Compaction;
        c.text = "Manual compaction: 45231 -> 9102 tokens";
        c.nested = true;  // the role's own note arrives nested
        c.nestedAgent = "worker";
        app.processEventForTest(c);
        check(!app.compactQueued.load(), "compact: the role's note re-arms the button");
        check(!app.uiMessages.empty() &&
                  app.uiMessages.back().kind == App::UiMessage::System &&
                  app.uiMessages.back().text == "worker: " + c.text,
              "compact: the note renders as a role-tagged system row");
        app.queueCompaction();
        AgentEvent end;
        end.type = AgentEventType::AgentEnd;
        app.processEventForTest(end);
        check(!app.compactQueued.load() && !app.manualCompactRequest.load(),
              "compact: run end clears an unconsumed queue and the shared flag");
    }
    // Simple mode: the request goes to the live parent agent, and a nested
    // (subagent) automatic compaction must not re-arm the button.
    {
        App app;
        OpenAIProvider p;
        app.agent = std::make_unique<Agent>("s", p, nullptr, nullptr);
        app.agentRunning = true;
        app.queueCompaction();
        check(app.compactQueued.load() && !app.manualCompactRequest.load(),
              "compact: plain run arms the agent only");
        check(app.agent->pendingMessageCount() == 0, "compact: live agent constructed");
        AgentEvent nested;
        nested.type = AgentEventType::Compaction;
        nested.text = "Context compacted: 1 -> 1 tokens";
        nested.nested = true;
        nested.nestedAgent = "worker";
        app.processEventForTest(nested);
        check(app.compactQueued.load(),
              "compact: a simple-mode subagent's auto-compaction keeps the queue armed");
        AgentEvent c;
        c.type = AgentEventType::Compaction;
        c.text = "Manual compaction: 100 -> 50 tokens";
        app.processEventForTest(c);
        check(!app.compactQueued.load(), "compact: the parent's note re-arms the button");
    }

    // Idle compaction (Simple mode, no run up): the Compact button rewrites
    // the conversation the next prompt continues from, on the click.
    {
        system("rm -rf /tmp/miniagent_idle_compact && mkdir -p /tmp/miniagent_idle_compact");
        auto makeApp = [] {
            auto a = std::make_unique<App>();
            a->sessions = std::make_unique<SessionStore>("/tmp/miniagent_idle_compact",
                                                         "/tmp/miniagent_idle_compact/root");
            a->sessions->startSession("idle compact");
            for (int i = 1; i <= 3; ++i) {
                UserMessage u;
                u.text = "question " + std::to_string(i) + " " + std::string(400, 'x');
                AssistantMessage as;
                as.text = "answer " + std::to_string(i) + " " + std::string(400, 'y');
                a->sessions->appendMessage(u);
                a->sessions->appendMessage(as);
            }
            return a;
        };
        // The live agent still owns this session's history: it is compacted
        // in place, so the next prompt seeds from the smaller conversation.
        {
            const std::unique_ptr<App> owned = makeApp();
            App& app = *owned;
            const std::vector<AgentMessage> msgs =
                SessionStore::loadProjected(app.sessions->currentPath()).messages;
            std::vector<AgentEvent> emitted;
            OpenAIProvider p;
            app.agent = std::make_unique<Agent>(
                "sys", p, nullptr, [&emitted](const AgentEvent& ev) { emitted.push_back(ev); },
                nullptr,
                [&app](const CompactionRecord& r) { app.sessions->appendCompaction(r); });
            app.agent->setHistory(msgs, 1, true);
            app.liveHistoryPath = app.sessions->currentPath();
            check(app.canCompactIdle(), "idle compact: the button is up with no run in Simple mode");
            app.scaffoldMode = true;
            check(!app.canCompactIdle(), "idle compact: Scaffold mode has no idle button");
            app.scaffoldMode = false;
            app.contextTokens = 123456;
            app.queueCompaction();
            check(app.compactQueued.load(), "idle compact: the click arms the button");
            check(!emitted.empty() && emitted.back().type == AgentEventType::Compaction,
                  "idle compact: the note is emitted");
            app.processEventForTest(emitted.back());
            check(!app.compactQueued.load(), "idle compact: the note disarms the button");
            check(app.agent->history().size() < msgs.size() &&
                      isCompactionSummaryMessage(app.agent->history().front()),
                  "idle compact: the live conversation is summary + tail");
            check(app.contextTokens == emitted.back().tokensAfter,
                  "idle compact: the context figure lands with the note");
            const ProjectedSession proj =
                SessionStore::loadProjected(app.sessions->currentPath());
            check(proj.compacted, "idle compact: the record reached the session file");
        }
        // No live agent (restart, session switch, scaffold run): the session
        // file is the source of truth, so its projected view is compacted and
        // only the record is kept.
        {
            const std::unique_ptr<App> owned = makeApp();
            App& app = *owned;
            const size_t before =
                SessionStore::loadProjected(app.sessions->currentPath()).messages.size();
            check(app.canCompactIdle() && !app.agent,
                  "idle compact (file): a session with no live agent is compactable");
            app.queueCompaction();
            check(app.compactQueued.load(), "idle compact (file): the click arms the button");
            app.pumpEvents();  // the note arrives through the normal event queue
            check(!app.compactQueued.load(), "idle compact (file): the note disarms the button");
            check(!app.uiMessages.empty() &&
                      app.uiMessages.back().kind == App::UiMessage::System &&
                      app.uiMessages.back().text.find("Manual compaction") == 0,
                  "idle compact (file): the note renders as a system row");
            check(!app.agent && app.liveHistoryPath.empty(),
                  "idle compact (file): no live agent is left behind");
            const ProjectedSession proj =
                SessionStore::loadProjected(app.sessions->currentPath());
            check(proj.compacted && proj.messages.size() < before,
                  "idle compact (file): the next prompt seeds the compacted view");
            check(app.contextTokens > 0,
                  "idle compact (file): the context figure updated");
        }
        // Nothing to compact (no session open) leaves the button inert.
        {
            App app;
            check(!app.canCompactIdle(), "idle compact: no open session means no button");
            app.queueCompaction();
            check(!app.compactQueued.load(), "idle compact: no open session queues nothing");
        }
    }

    if (failures) {
        printf("notify_app_test: FAILURES: %d\n", failures);
        return 1;
    }
    printf("notify_app_test: ok\n");
    return 0;
}
