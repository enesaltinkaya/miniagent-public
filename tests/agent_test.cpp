#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "agent/agent.h"
#include "agent/system_prompt.h"
#include "llm/openai.h"
#include "tools/factories.h"
#include "live_llm.h"

int main(int argc, char** argv) {
    if (!liveLlmEnabled()) {
        printf("agent_test: skipped (set MINIAGENT_LIVE_LLM=1 to send real requests)\n");
        return 0;
    }

    const char* cwd = "/tmp/miniagent_agent_test";
    system("rm -rf /tmp/miniagent_agent_test && mkdir -p /tmp/miniagent_agent_test");

    std::ofstream("/tmp/miniagent_agent_test/hello.txt") << "line one\nline two\nline three\n";

    ToolRegistry tools;
    tools.add(makeReadTool(cwd));
    tools.add(makeWriteTool(cwd));
    tools.add(makeEditTool(cwd));
    tools.add(makeBashTool(cwd));

    Agent agent(buildSystemPrompt(cwd), OpenAIProvider{}, &tools, [](const AgentEvent& ev) {
        switch (ev.type) {
            case AgentEventType::AgentStart:
                printf("[agent start]\n");
                break;
            case AgentEventType::TurnStart:
                printf("[turn %d]\n", ev.turnIndex);
                break;
            case AgentEventType::TextDelta:
                printf("%s", ev.text.c_str());
                fflush(stdout);
                break;
            case AgentEventType::ToolCallStart:
                printf("\n[tool start] %s %s\n", ev.toolName.c_str(), ev.toolArgs.c_str());
                break;
            case AgentEventType::ToolCallEnd:
                printf("[tool end] %s%s: %.120s\n", ev.toolName.c_str(),
                       ev.toolIsError ? " (error)" : "", ev.toolResult.c_str());
                break;
            case AgentEventType::TurnEnd:
                printf("[turn %d end]\n", ev.turnIndex);
                break;
            case AgentEventType::AgentEnd:
                printf("[agent end]\n");
                break;
            case AgentEventType::Error:
                printf("[error] %s\n", ev.text.c_str());
                break;
            case AgentEventType::Retry:
            case AgentEventType::Compaction:
            case AgentEventType::Warning:
                printf("[note] %s\n", ev.text.c_str());
                break;
            default:
                break;
        }
    });

    agent.run(UserMessage{"Append the line 'line four' to /tmp/miniagent_agent_test/hello.txt. First read the file to see its contents, then edit it. Then verify with a second read.", 0});

    // Verify the file was modified
    FILE* f = fopen("/tmp/miniagent_agent_test/hello.txt", "r");
    char buf[4096] = {0};
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f)
        fclose(f);
    std::string content(buf, n);
    if (content.find("line four") == std::string::npos) {
        printf("agent_test: FAILED (file not updated)\n%s\n", content.c_str());
        return 1;
    }
    printf("agent_test: passed\nfinal file:\n%s\n", content.c_str());
    return 0;
}
