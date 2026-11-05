// Regression test for interleaved reasoning/content deltas: a provider may
// stream reasoning_content and content deltas alternately ("case cheap",
// "Quick", "ly.", " end-to-end"). Each delta must rejoin its own run's
// thinking/text row instead of matching the newest message, or the log
// fragments one logical message into several rows and words appear split
// across lines.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "app.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

static AgentEvent deltaOf(AgentEventType type, std::string text) {
    AgentEvent ev;
    ev.type = type;
    ev.text = std::move(text);
    return ev;
}

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1);  // never start the user's real MCP servers in tests
    setenv("MINIAGENT_NO_PROBE", "1", 1);  // no real network probe from a headless App
    system("rm -rf /tmp/miniagent_stream_rows && mkdir -p /tmp/miniagent_stream_rows");
    chdir("/tmp/miniagent_stream_rows");
    App app;

    AgentEvent ts;
    ts.type = AgentEventType::TurnStart;
    app.processEventForTest(ts);  // pushes the turn's empty placeholder row

    // Interleaved: reasoning and content alternate mid-sentence.
    app.processEventForTest(deltaOf(AgentEventType::ThinkingDelta, "case cheap"));
    app.processEventForTest(deltaOf(AgentEventType::TextDelta, "Quick"));
    app.processEventForTest(deltaOf(AgentEventType::ThinkingDelta, "ly."));
    app.processEventForTest(deltaOf(AgentEventType::TextDelta, " end-to-end check"));

    check(app.uiMessages.size() == 3, "placeholder + one thinking row + one text row");
    if (app.uiMessages.size() == 3) {
        check(app.uiMessages[1].kind == App::UiMessage::Thinking &&
                  app.uiMessages[1].text == "case cheaply.",
              "interleaved thinking deltas rejoin one row");
        check(app.uiMessages[2].kind == App::UiMessage::Agent &&
                  app.uiMessages[2].text == "Quick end-to-end check",
              "interleaved text deltas rejoin one row");
    }

    // A new turn starts fresh rows (it must not grow the previous turn's).
    app.processEventForTest(ts);
    app.processEventForTest(deltaOf(AgentEventType::ThinkingDelta, "more thinking"));
    app.processEventForTest(deltaOf(AgentEventType::TextDelta, "more text"));
    check(app.uiMessages.size() == 6, "second turn gets its own rows");
    if (app.uiMessages.size() == 6) {
        check(app.uiMessages[4].kind == App::UiMessage::Thinking &&
                  app.uiMessages[4].text == "more thinking",
              "turn 2 thinking row is fresh");
        check(app.uiMessages[5].kind == App::UiMessage::Agent && app.uiMessages[5].text == "more text",
              "turn 2 text row is fresh");
    }

    // A subagent run interleaves the same way; rows stay tagged by role.
    AgentEvent ns, nts;
    ns.type        = AgentEventType::AgentStart;
    ns.nested      = true;
    ns.nestedAgent = "worker";
    nts.type        = AgentEventType::TurnStart;
    nts.nested      = true;
    nts.nestedAgent = "worker";
    app.processEventForTest(ns);
    app.processEventForTest(nts);
    auto nth = deltaOf(AgentEventType::ThinkingDelta, "reason");
    auto ntx = deltaOf(AgentEventType::TextDelta, "answer");
    auto nth2 = deltaOf(AgentEventType::ThinkingDelta, "ing");
    nth.nested = nth2.nested = ntx.nested = true;
    nth.nestedAgent = nth2.nestedAgent = ntx.nestedAgent = "worker";
    app.processEventForTest(nth);
    app.processEventForTest(ntx);
    app.processEventForTest(nth2);
    check(app.uiMessages.size() == 9, "nested run: header + one thinking row + one text row");
    if (app.uiMessages.size() == 9) {
        check(app.uiMessages[7].kind == App::UiMessage::Thinking &&
                  app.uiMessages[7].text == "reasoning" && app.uiMessages[7].nestedAgent == "worker",
              "nested thinking deltas rejoin one tagged row");
        check(app.uiMessages[8].kind == App::UiMessage::Agent && app.uiMessages[8].text == "answer",
              "nested text row is separate from thinking");
    }

    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}
