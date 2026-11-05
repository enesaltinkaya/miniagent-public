// Live end-to-end test: run the real agent loop against the local LLM server
// (http://localhost:1234/v1, same as the app) and have it edit a real
// indented C++ file with the edit tool. Regression test for the "failed edit
// tool calls" issue: before the read-prefix fix, the model routinely produced
// oldText with a phantom extra leading space per line.
//
// Skips (exit 0) unless MINIAGENT_LIVE_LLM=1, and also when the local LLM
// server is unreachable.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "agent/agent.h"
#include "agent/system_prompt.h"
#include "llm/openai.h"
#include "tools/factories.h"
#include "live_llm.h"

namespace {
const char* kCwd = "/tmp/miniagent_edit_test";
const char* kFile = "/tmp/miniagent_edit_test/src/util.cpp";

const char* kOriginal =
    "#include <string>\n"
    "\n"
    "namespace util {\n"
    "\n"
    "int clampValue(int v, int lo, int hi) {\n"
    "    if (v < lo)\n"
    "        return lo;\n"
    "    if (v > hi)\n"
    "        return hi;\n"
    "    return v;\n"
    "}\n"
    "\n"
    "std::string makeGreeting(const std::string& name) {\n"
    "    return \"hello \" + name;\n"
    "}\n"
    "\n"
    "} // namespace util\n";

const char* kExpected =
    "#include <string>\n"
    "\n"
    "namespace util {\n"
    "\n"
    "int clampToRange(int v, int lo, int hi) {\n"
    "    if (v < lo)\n"
    "        return lo;\n"
    "    if (v > hi)\n"
    "        return hi;\n"
    "    return v;\n"
    "}\n"
    "\n"
    "std::string makeGreeting(const std::string& name) {\n"
    "    return \"hey \" + name;\n"
    "}\n"
    "\n"
    "} // namespace util\n";

int editCalls = 0;
int editErrors = 0;

std::string readFileText(const std::string& path) {
    std::ifstream f(path);
    std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return all;
}

} // namespace

int main() {
    // Send real requests only when explicitly opted in.
    if (!liveLlmEnabled()) {
        std::printf("agent_edit_test: skipped (set MINIAGENT_LIVE_LLM=1 to send real requests)\n");
        return 0;
    }

    // Skip when the local LLM server is not running.
    if (std::system("curl -s -m 3 -o /dev/null http://localhost:1234/v1/models") != 0) {
        std::printf("agent_edit_test: skipped (local LLM server unreachable)\n");
        return 0;
    }

    std::system((std::string("rm -rf ") + kCwd + " && mkdir -p " + kCwd + "/src").c_str());
    std::ofstream(kFile) << kOriginal;

    ToolRegistry tools;
    tools.add(makeReadTool(kCwd));
    tools.add(makeWriteTool(kCwd));
    tools.add(makeEditTool(kCwd));
    tools.add(makeBashTool(kCwd));

    OpenAIProvider provider;
    Agent agent(buildSystemPrompt(kCwd), provider, &tools, [](const AgentEvent& ev) {
        if (ev.type == AgentEventType::ToolCallEnd) {
            if (ev.toolName == "edit") {
                ++editCalls;
                if (ev.toolIsError)
                    ++editErrors;
            }
            std::printf("[tool] %s%s: %.100s\n", ev.toolName.c_str(),
                        ev.toolIsError ? " (error)" : "", ev.toolResult.c_str());
        } else if (ev.type == AgentEventType::Error) {
            std::printf("[error] %s\n", ev.text.c_str());
        }
    });

    agent.run(UserMessage{
        "In src/util.cpp: (1) rename the function clampValue to clampToRange, and "
        "(2) change the greeting string literal \"hello \" to \"hey \". "
        "Read the file first, then apply both changes with the edit tool. "
        "Do not change anything else in the file.",
        0});

    std::string final = readFileText(kFile);
    std::printf("edit calls: %d, edit errors: %d\n", editCalls, editErrors);

    if (final != kExpected) {
        std::printf("agent_edit_test: FAILED (file content)\n--- expected ---\n%s--- actual ---\n%s\n",
                    kExpected, final.c_str());
        return 1;
    }
    // Before the read-prefix fix the model failed most edit attempts; allow a
    // single retry (models hiccup), fail on repeated failures.
    if (editErrors >= 2) {
        std::printf("agent_edit_test: FAILED (%d failed edit calls)\n", editErrors);
        return 1;
    }
    std::printf("agent_edit_test: passed (%d edit calls, %d errors)\n", editCalls, editErrors);
    std::system((std::string("rm -rf ") + kCwd).c_str());
    return 0;
}
