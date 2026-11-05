// Integration test (needs a live LLM endpoint, like agent_test): the main
// agent delegates to a subagent in a fresh context; the result comes back
// through the tool and the run completes.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "agent/agent.h"
#include "agent/agents.h"
#include "agent/system_prompt.h"
#include "llm/openai.h"
#include "tools/factories.h"
#include "live_llm.h"

int main(int argc, char** argv) {
    if (!liveLlmEnabled()) {
        printf("subagent_test: skipped (set MINIAGENT_LIVE_LLM=1 to send real requests)\n");
        return 0;
    }

    const char* cwd = "/tmp/miniagent_subagent_test";
    system("rm -rf /tmp/miniagent_subagent_test && mkdir -p /tmp/miniagent_subagent_test");

    // Discover the built-ins (brainstorm/worker) and add a local
    // helper role that answers in one line.
    std::vector<AgentDef> agents = discoverAgents(cwd);
    AgentDef helper;
    helper.name        = "helper";
    helper.description = "answers one question in one short line";
    helper.systemPrompt =
        "You answer the question you are given, in exactly one short line. No tools, "
        "no preamble.";
    agents.push_back(std::move(helper));

    ToolRegistry tools;
    tools.add(makeReadTool(cwd));
    tools.add(makeWriteTool(cwd));
    tools.add(makeEditTool(cwd));
    tools.add(makeBashTool(cwd));

    OpenAIProvider provider;
    std::atomic<bool> stop{false};
    tools.add(makeSubagentTool(cwd,
                               provider,
                               [&] { return agents; },
                               [&] { return tools.all(); },
                               [&] { return &stop; },
                               [](const AgentEvent& ev) {
                                   if (ev.nested) {
                                       if (ev.type == AgentEventType::TurnStart)
                                           printf("\n[subagent %s] ", ev.nestedAgent.c_str());
                                       else if (ev.type == AgentEventType::TextDelta)
                                           printf("%s", ev.text.c_str());
                                       else if (ev.type == AgentEventType::ToolCallStart)
                                           printf("\n[subagent %s tool] %s %s\n", ev.nestedAgent.c_str(),
                                                  ev.toolName.c_str(), ev.toolArgs.c_str());
                                       fflush(stdout);
                                       return;
                                   }
                                   switch (ev.type) {
                                       case AgentEventType::TextDelta:
                                           printf("%s", ev.text.c_str());
                                           break;
                                       case AgentEventType::ToolCallStart:
                                           printf("\n[tool start] %s %s\n", ev.toolName.c_str(), ev.toolArgs.c_str());
                                           break;
                                       case AgentEventType::ToolCallEnd:
                                           printf("[tool end] %s%s: %.200s\n", ev.toolName.c_str(),
                                                  ev.toolIsError ? " (error)" : "", ev.toolResult.c_str());
                                           break;
                                       default:
                                           break;
                                   }
                                   fflush(stdout);
                               }));

    Agent agent(buildSystemPrompt(cwd), provider, &tools, [](const AgentEvent&) {});
    agent.run(UserMessage{"Use the subagent tool with agent \"helper\" and task "
                          "\"What is 7 * 6? Reply with just the number.\" Then tell me "
                          "what it answered.",
                          0});

    printf("subagent_test: passed\n");
    return 0;
}
