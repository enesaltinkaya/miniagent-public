#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

#include "app.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1);  // never start the user's real MCP servers in tests
    setenv("MINIAGENT_NO_PROBE", "1", 1);  // no real network probe from a headless App
    system("rm -rf /tmp/miniagent_app_test && mkdir -p /tmp/miniagent_app_test");
    FILE* f = fopen("/tmp/miniagent_app_test/f.txt", "w");
    fputs("the answer is forty-two\n", f);
    fclose(f);

    chdir("/tmp/miniagent_app_test");
    App app;
    check(app.cwd == "/tmp/miniagent_app_test", "app uses process working directory");

    snprintf(app.inputBuf, sizeof(app.inputBuf),
             "Read the file f.txt and tell me what word comes after 'is' in one short sentence.");
    app.sendPrompt();
    check(app.agentRunning, "agent started");

    // Queue a second prompt while the first run is in flight (pi-style
    // steering: it is injected mid-run, between LLM calls).
    snprintf(app.inputBuf, sizeof(app.inputBuf), "Second, queued question: reply with the single word forty-two.");
    app.sendPrompt();
    check(app.agentRunning, "agent still running after queuing");
    check(app.agent && app.agent->pendingMessageCount() == 1, "prompt queued in agent");
    bool sawQueued = false;
    for (const auto& m : app.uiMessages)
        if (m.kind == App::UiMessage::User && m.queued)
            sawQueued = true;
    check(sawQueued, "queued user message shown in log");
    bool sawUnqueued = true; // the first prompt must not be marked queued
    for (const auto& m : app.uiMessages)
        if (m.kind == App::UiMessage::User && m.text.find("Read the file") == 0 && m.queued)
            sawUnqueued = false;
    check(sawUnqueued, "initial prompt not marked queued");

    int frames = 0;
    while (app.agentRunning && frames++ < 60000) {
        app.pumpEvents();
        usleep(100 * 1000); // 100ms; up to ~10 min
    }
    app.pumpEvents();

    bool sawReadTool = false;
    bool sawAgentText = false;
    bool sawError = false;
    std::string agentText;
    for (const auto& m : app.uiMessages) {
        if (m.kind == App::UiMessage::User)
            check(!m.text.empty(), "user message recorded");
        if (m.kind == App::UiMessage::Tool && m.toolName == "read" && m.toolDone) {
            sawReadTool = true;
            check(m.toolResult.find("forty-two") != std::string::npos, "read tool result contains file content");
        }
        if (m.kind == App::UiMessage::Agent && !m.text.empty()) {
            sawAgentText = true;
            agentText += m.text + "\n";
        }
        if (m.kind == App::UiMessage::Error)
            sawError = true;
    }

    check(sawReadTool, "agent called read tool");
    check(sawAgentText, "agent produced text");
    check(!sawError, "no error events");
    check(agentText.find("forty-two") != std::string::npos, "final answer mentions file content");
    check(app.sessionUsage.totalTokens > 0, "token usage tracked");
    check(!app.agentRunning, "agent finished");
    {
        int userMsgs = 0;
        for (const auto& m : SessionStore::loadMessages(app.sessions->currentPath()))
            if (std::holds_alternative<UserMessage>(m))
                userMsgs++;
        check(userMsgs == 2, "queued prompt delivered mid-run");
    }

    printf("\n--- agent said ---\n%s\n", agentText.c_str());

    if (failures == 0)
        printf("app_test: all passed\n");
    else
        printf("app_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
