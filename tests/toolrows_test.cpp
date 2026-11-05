// Regression test for tool-call rows in the message log.
//
// A response with several parallel tool calls streams one live row per call
// (ToolArgsDelta, each with its SSE tool_calls index). The calls'
// ToolCallStart/ToolCallEnd events must settle/finish exactly those rows --
// the old code tracked a single streaming row, so every non-last call got a
// duplicate row (and the results attached to the duplicates). The events are
// fed straight into App::eventQueue + pumpEvents(); no LLM involved.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

#include "app.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

static void feed(App& app, AgentEvent e) {
    app.eventQueue.push_back(std::move(e));
    app.pumpEvents();
}

static AgentEvent deltaEv(int toolIndex, const std::string& name, const std::string& fragment,
                          bool nested, const std::string& nestedAgent) {
    AgentEvent e;
    e.type        = AgentEventType::ToolArgsDelta;
    e.text        = fragment;
    e.toolName    = name;
    e.toolIndex   = toolIndex;
    e.nested      = nested;
    e.nestedAgent = nestedAgent;
    return e;
}

static AgentEvent startEv(int toolIndex, const std::string& name, const std::string& args,
                          bool nested, const std::string& nestedAgent) {
    AgentEvent e;
    e.type        = AgentEventType::ToolCallStart;
    e.toolName    = name;
    e.toolArgs    = args;
    e.toolIndex   = toolIndex;
    e.nested      = nested;
    e.nestedAgent = nestedAgent;
    return e;
}

static AgentEvent endEv(int toolIndex, const std::string& result, bool nested,
                        const std::string& nestedAgent) {
    AgentEvent e;
    e.type        = AgentEventType::ToolCallEnd;
    e.toolResult  = result;
    e.toolIndex   = toolIndex;
    e.nested      = nested;
    e.nestedAgent = nestedAgent;
    return e;
}

static int toolRowCount(App& app, const std::string& nested) {
    int n = 0;
    for (const auto& m : app.uiMessages)
        if (m.kind == App::UiMessage::Tool && m.nestedAgent == nested)
            ++n;
    return n;
}

static std::vector<std::string> toolResults(App& app, int toolIndex, const std::string& nested) {
    std::vector<std::string> out;
    for (const auto& m : app.uiMessages)
        if (m.kind == App::UiMessage::Tool && m.nestedAgent == nested && m.toolIndex == toolIndex)
            out.push_back(m.toolResult);
    return out;
}

static bool anyStreamingRow(App& app) {
    for (const auto& m : app.uiMessages)
        if (m.toolStreaming)
            return true;
    return false;
}

static bool anyPendingRow(App& app) {
    for (const auto& m : app.uiMessages)
        if (m.kind == App::UiMessage::Tool && !m.toolDone)
            return true;
    return false;
}

// One parent turn in which the model streams four parallel read calls (the
// ledger-reading shape that used to render twice): four argument-delta rows
// appear before any ToolCallStart, in index order 0..3.
static void parallelParentCalls(App& app) {
    const char* args[4] = {"{\"path\": \"task.md\"}",
                           "{\"path\": \"plan.md\"}",
                           "{\"path\": \"tasks.json\"}",
                           "{\"path\": \"notes.md\"}"};
    feed(app, [] { AgentEvent e; e.type = AgentEventType::AgentStart; return e; }());
    AgentEvent turn;
    turn.type = AgentEventType::TurnStart;
    feed(app, turn);
    for (int i = 0; i < 4; ++i)
        feed(app, deltaEv(i, "read", args[i], false, ""));
    for (int i = 0; i < 4; ++i)
        feed(app, startEv(i, "read", args[i], false, ""));
    check(toolRowCount(app, "") == 4, "four parallel calls -> exactly four tool rows");
    for (int i = 0; i < 4; ++i)
        feed(app, endEv(i, "result " + std::to_string(i), false, ""));
    for (int i = 0; i < 4; ++i) {
        auto rs = toolResults(app, i, "");
        check(rs.size() == 1 && rs[0] == "result " + std::to_string(i),
              "result attached to the streamed row, no duplicate");
    }
    AgentEvent end;
    end.type = AgentEventType::TurnEnd;
    feed(app, end);
    check(!anyStreamingRow(app), "no row left in the streaming state");
    check(!anyPendingRow(app), "no row left without a result");
}

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1);  // never start the user's real MCP servers in tests
    setenv("MINIAGENT_NO_PROBE", "1", 1);  // no real network probe from a headless App
    system("rm -rf /tmp/miniagent_toolrows_test && mkdir -p /tmp/miniagent_toolrows_test");
    chdir("/tmp/miniagent_toolrows_test");

    // 1) Parent: parallel streamed calls, then a second turn reusing index 0.
    {
        App app;
        parallelParentCalls(app);

        AgentEvent turn;
        turn.type = AgentEventType::TurnStart;
        feed(app, turn);
        feed(app, deltaEv(0, "read", "{\"path\": \"task.md\"}", false, ""));
        feed(app, deltaEv(1, "read", "{\"path\": \"plan.md\"}", false, ""));
        feed(app, startEv(0, "read", "{\"path\": \"task.md\"}", false, ""));
        feed(app, startEv(1, "read", "{\"path\": \"plan.md\"}", false, ""));
        feed(app, endEv(0, "turn2-r0", false, ""));
        feed(app, endEv(1, "turn2-r1", false, ""));
        AgentEvent end;
        end.type = AgentEventType::TurnEnd;
        feed(app, end);
        check(toolRowCount(app, "") == 6, "second turn reuses indices without duplicating rows");
        check(toolResults(app, 0, "").size() == 2 &&
                  toolResults(app, 0, "").back() == "turn2-r0",
              "second-turn row carries the second result");
    }

    // 2) Scaffold role (nested events): streamed calls plus one call that
    //    never streamed fragments must not duplicate either.
    {
        App app;
        app.scaffoldRunActive = true;
        AgentEvent start;
        start.type        = AgentEventType::AgentStart;
        start.nested      = true;
        start.nestedAgent = "manager";
        feed(app, start);
        AgentEvent turn;
        turn.type        = AgentEventType::TurnStart;
        turn.nested      = true;
        turn.nestedAgent = "manager";
        feed(app, turn);
        feed(app, deltaEv(0, "read", "{\"path\": \"task.md\"}", true, "manager"));
        feed(app, deltaEv(1, "read", "{\"path\": \"plan.md\"}", true, "manager"));
        feed(app, startEv(0, "read", "{\"path\": \"task.md\"}", true, "manager"));
        feed(app, startEv(1, "read", "{\"path\": \"plan.md\"}", true, "manager"));
        feed(app, startEv(2, "bash", "{\"command\": \"cmake --build build\"}", true, "manager"));
        check(toolRowCount(app, "manager") == 3, "role: three calls -> exactly three rows");
        feed(app, endEv(0, "r0", true, "manager"));
        feed(app, endEv(1, "r1", true, "manager"));
        feed(app, endEv(2, "r2", true, "manager"));
        for (int i = 0; i < 3; ++i) {
            auto rs = toolResults(app, i, "manager");
            check(rs.size() == 1 && rs[0] == "r" + std::to_string(i), "role result on the right row");
        }
        AgentEvent end;
        end.type        = AgentEventType::TurnEnd;
        end.nested      = true;
        end.nestedAgent = "manager";
        feed(app, end);
        check(!anyStreamingRow(app), "role: no row left streaming");
        check(!anyPendingRow(app), "role: no row left pending");
        app.scaffoldRunActive = false;
    }

    // 3) Simple-mode subagent: the worker's calls reuse index 0..n of the
    //    parent's own calls; its ToolCallEnd must not steal the parent row's
    //    result (and the parent's must not land on the worker's row).
    {
        App app;
        AgentEvent start;
        start.type = AgentEventType::AgentStart;
        feed(app, start);
        AgentEvent turn;
        turn.type = AgentEventType::TurnStart;
        feed(app, turn);
        feed(app, deltaEv(0, "read", "{\"path\": \"src/main.cpp\"}", false, ""));
        feed(app, startEv(0, "read", "{\"path\": \"src/main.cpp\"}", false, ""));
        // While the parent's call runs, the subagent tool dispatches a
        // worker whose own first call is index 0 too.
        feed(app, startEv(0, "read", "{\"path\": \"src/app.cpp\"}", true, "worker"));
        check(toolRowCount(app, "") == 1, "worker row is separate from the parent's");
        check(toolRowCount(app, "worker") == 1, "parent row not consumed by the worker's start");
        feed(app, endEv(0, "parent result", false, ""));
        auto pr = toolResults(app, 0, "");
        check(pr.size() == 1 && pr[0] == "parent result",
              "parent result stays on the parent's row");
        feed(app, endEv(0, "worker result", true, "worker"));
        auto wr = toolResults(app, 0, "worker");
        check(wr.size() == 1 && wr[0] == "worker result", "worker result on the worker's row");
    }

    // 4) An argument stream that never becomes a call (orphaned live row):
    //    TurnEnd lands it without leaving it in the streaming state.
    {
        App app;
        AgentEvent start;
        start.type = AgentEventType::AgentStart;
        feed(app, start);
        AgentEvent turn;
        turn.type = AgentEventType::TurnStart;
        feed(app, turn);
        feed(app, deltaEv(0, "write", "{\"path\": \"x.md\"}", false, ""));
        feed(app, startEv(1, "read", "{\"path\": \"y.md\"}", false, ""));
        feed(app, endEv(1, "ok", false, ""));
        AgentEvent end;
        end.type = AgentEventType::TurnEnd;
        feed(app, end);
        check(toolRowCount(app, "") == 2, "orphaned stream + real call -> two rows");
        check(!anyStreamingRow(app), "orphaned row landed at turn end");
        check(!anyPendingRow(app), "orphaned row marked done at turn end");
        check(toolResults(app, 1, "").back() == "ok", "real call's result intact");
    }

    // 5) Run duration bookkeeping: ToolCallStart stamps the row with the
    //    event timestamp (both the settled live row and a fresh row);
    //    ToolCallEnd computes the elapsed time from the end event.
    {
        App app;
        AgentEvent start;
        start.type = AgentEventType::AgentStart;
        feed(app, start);
        AgentEvent turn;
        turn.type = AgentEventType::TurnStart;
        feed(app, turn);
        feed(app, deltaEv(0, "bash", "{\"command\": \"sleep 5\"}", false, ""));
        AgentEvent s = startEv(0, "bash", "{\"command\": \"sleep 5\"}", false, "");
        s.timestamp = 1000;
        feed(app, s);
        const App::UiMessage* row = nullptr;
        for (const auto& m : app.uiMessages)
            if (m.kind == App::UiMessage::Tool)
                row = &m;
        check(row && row->toolStartMs == 1000, "settled live row stamped with the start timestamp");
        AgentEvent e = endEv(0, "done", false, "");
        e.timestamp = 6200;
        feed(app, e);
        row = nullptr;
        for (const auto& m : app.uiMessages)
            if (m.kind == App::UiMessage::Tool)
                row = &m;
        check(row && row->toolElapsedMs == 5200, "elapsed computed from the end event");
        AgentEvent s2 = startEv(1, "read", "{\"path\": \"a.md\"}", false, "");
        s2.timestamp = 7000;
        feed(app, s2);
        AgentEvent e2 = endEv(1, "ok", false, "");
        e2.timestamp = 7400;
        feed(app, e2);
        const App::UiMessage* row2 = nullptr;
        for (const auto& m : app.uiMessages)
            if (m.kind == App::UiMessage::Tool && m.toolIndex == 1)
                row2 = &m;
        check(row2 && row2->toolStartMs == 7000 && row2->toolElapsedMs == 400,
              "fresh row start/duration stamped too");
    }

    if (failures == 0)
        printf("toolrows_test: all passed\n");
    else
        printf("toolrows_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
