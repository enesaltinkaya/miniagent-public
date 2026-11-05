// Integration test (needs a live LLM endpoint, like agent_test): the
// deterministic scaffold loop sets up the ledger, dispatches the roles and
// finishes; the worker's change lands in the repo.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include "agent/scaffold.h"
#include "llm/openai.h"
#include "tools/factories.h"
#include "live_llm.h"

int main(int argc, char** argv) {
    setenv("MINIAGENT_NO_SPEECH", "1", 1);
    if (!liveLlmEnabled()) {
        printf("scaffold_test: skipped (set MINIAGENT_LIVE_LLM=1 to send real requests)\n");
        return 0;
    }

    const char* cwd = "/tmp/miniagent_scaffold_test";
    system("rm -rf /tmp/miniagent_scaffold_test && mkdir -p /tmp/miniagent_scaffold_test");
    {
        std::ofstream(std::string(cwd) + "/hello.txt") << "line one\n";
    }

    ToolRegistry tools;
    tools.add(makeReadTool(cwd));
    tools.add(makeWriteTool(cwd));
    tools.add(makeEditTool(cwd));
    tools.add(makeBashTool(cwd));

    OpenAIProvider provider;
    std::atomic<bool> stop{false};
    Scaffold scaffold(cwd, provider, &tools, [](const AgentEvent& ev) {
        if (ev.type == AgentEventType::TurnStart && ev.nested) {
            printf("\n[role %s] ", ev.nestedAgent.c_str());
            fflush(stdout);
        } else if (ev.type == AgentEventType::TextDelta && ev.nested) {
            printf("%s", ev.text.c_str());
            fflush(stdout);
        } else if (ev.type == AgentEventType::ToolCallStart && ev.nested) {
            printf("\n[role %s tool] %s %s\n", ev.nestedAgent.c_str(),
                   ev.toolName.c_str(), ev.toolArgs.c_str());
            fflush(stdout);
        } else if (ev.type == AgentEventType::TextDelta && !ev.nested) {
            printf("\n[report] %s\n", ev.text.c_str());
            fflush(stdout);
        }
    });

    scaffold.run("Add the line 'line two' to hello.txt. Do it with a single edit or "
                 "write; do not change anything else.",
                 stop);

    // The worker's change must be in the repo.
    FILE* f = fopen("/tmp/miniagent_scaffold_test/hello.txt", "r");
    char buf[4096] = {0};
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f)
        fclose(f);
    std::string content(buf, n);
    if (content.find("line two") == std::string::npos) {
        printf("scaffold_test: FAILED (file not updated)\n%s\n", content.c_str());
        return 1;
    }

    // The ledger must exist with the core files.
    for (const char* name : {"task.md", "notes.md", "tasks.json", "plan.md"}) {
        struct stat st;
        if (stat((std::string(cwd) + "/.miniagent/ledger/" + name).c_str(), &st) != 0 || st.st_size == 0) {
            printf("scaffold_test: FAILED (missing ledger file %s)\n", name);
            return 1;
        }
    }
    printf("scaffold_test: passed\n");
    return 0;
}
