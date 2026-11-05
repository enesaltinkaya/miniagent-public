// Tool-steering tests: the agent must be pushed toward the dedicated file
// tools (read, grep) and away from bash for reading/searching file contents.
//
// Three levers are covered:
//   - the bash tool's own description (the model reads it in the tool list)
//   - the shared tool-guidance block in the system prompt (main agent and
//     subagent both get it)
//   - the read tool's truncation note, which must point at the next offset
//     instead of leaving the model to reach for `sed -n`
//   - the web tools' descriptions, and the web guidance block that only appears
//     when those tools are registered (the main agent; never a role)
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "agent/system_prompt.h"
#include "tools/factories.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

namespace {
std::atomic<bool> noAbort{false};
}

int main() {
    // --- bash: the description must not advertise file operations ---
    Tool bash = makeBashTool("/tmp");
    check(bash.description.find("file operations") == std::string::npos,
          "bash: description does not invite file operations");
    check(bash.description.find("use the read tool") != std::string::npos,
          "bash: description points reading at the read tool");
    check(bash.description.find("use the grep tool") != std::string::npos,
          "bash: description points searching at the grep tool");
    check(bash.description.find("cat") != std::string::npos &&
              bash.description.find("sed -n") != std::string::npos &&
              bash.description.find("head") != std::string::npos &&
              bash.description.find("tail") != std::string::npos,
          "bash: description names the file-reading commands it forbids");

    // --- grep: the tool exists and describes itself as the search tool ---
    Tool grep = makeGrepTool("/tmp");
    check(grep.name == "grep", "grep: tool is named grep");
    check(grep.description.find("instead of running grep/rg through bash") != std::string::npos,
          "grep: description steers away from bash grep/rg");
    check(grep.description.find("read tool") != std::string::npos,
          "grep: description points reading at the read tool");
    check(grep.paramsSchema.contains("properties") &&
              grep.paramsSchema["properties"].contains("pattern") &&
              grep.paramsSchema["properties"].contains("path") &&
              grep.paramsSchema["properties"].contains("limit"),
          "grep: schema exposes pattern/path/limit");
    check(grep.paramsSchema["required"].size() == 1 &&
              grep.paramsSchema["required"][0] == "pattern",
          "grep: only pattern is required");

    // --- web tools: their own descriptions steer away from bash curl ---
    Tool webSearch = makeWebSearchTool("http://localhost:7070");
    check(webSearch.name == "web_search", "web_search: tool is named web_search");
    check(webSearch.description.find("curl or wget through bash") != std::string::npos,
          "web_search: description steers away from bash curl");
    check(webSearch.description.find("web_fetch") != std::string::npos,
          "web_search: description points at web_fetch for reading a page");
    check(webSearch.paramsSchema["required"].size() == 1 &&
              webSearch.paramsSchema["required"][0] == "query",
          "web_search: only query is required");
    Tool webFetch = makeWebFetchTool();
    check(webFetch.name == "web_fetch", "web_fetch: tool is named web_fetch");
    check(webFetch.description.find("curl or wget through bash") != std::string::npos,
          "web_fetch: description steers away from bash curl");
    check(webFetch.description.find("readable text") != std::string::npos,
          "web_fetch: description says it returns text, not raw HTML");

    // --- system prompt: the guidance names grep and forbids bash for reads ---
    const std::string guidance = toolGuidanceSection(true, true, true, true);
    check(guidance.find("- grep:") != std::string::npos, "prompt: grep tool is listed");
    check(guidance.find("never cat, sed -n, head, or tail") != std::string::npos,
          "prompt: bash reading is forbidden");
    check(guidance.find("Do not search file contents with bash grep/rg - use the grep tool") !=
              std::string::npos,
          "prompt: bash searching is forbidden");
    check(guidance.find("offset/limit") != std::string::npos,
          "prompt: read is told about offset/limit");

    // Without grep registered (file tools off) the guidance must not mention it.
    const std::string noGrep = toolGuidanceSection(false, false, false, false);
    check(noGrep.find("- grep:") == std::string::npos, "prompt: no grep line when it is not registered");
    check(noGrep.find("The dedicated file tools are disabled") != std::string::npos,
          "prompt: the no-file-tools case still explains what to do");

    // Web guidance only when the tools are registered (main agent with a
    // configured SearXNG): the default prompt and every role prompt omit it.
    const std::string withWeb = toolGuidanceSection(true, true, true, true, true);
    check(withWeb.find("- web_search:") != std::string::npos,
          "prompt: web_search is listed when registered");
    check(withWeb.find("Do not search the web or download pages with bash curl/wget") !=
              std::string::npos,
          "prompt: bash curl/wget is forbidden when the web tools exist");
    const std::string noWeb = toolGuidanceSection(true, true, true, true, false);
    check(noWeb.find("web_search") == std::string::npos,
          "prompt: no web line when the tools are not registered");
    check(buildSystemPrompt("/tmp").find("- web_search:") == std::string::npos,
          "prompt: default main agent prompt omits web guidance");
    check(buildSystemPrompt("/tmp", true, nullptr, true).find("- web_search:") != std::string::npos,
          "prompt: the web flag reaches the main agent prompt");

    // The main agent's prompt carries the same guidance.
    const std::string sys = buildSystemPrompt("/tmp");
    check(sys.find("- grep:") != std::string::npos, "prompt: main agent prompt lists grep");
    check(sys.find("never cat, sed -n, head, or tail") != std::string::npos,
          "prompt: main agent prompt forbids bash reads");
    // The skill contract is part of the built-in prompt, so a project with no
    // skills loaded yet still learns the format.
    check(sys.find("A skill is a directory holding a SKILL.md") != std::string::npos,
          "prompt: main agent prompt states the skill contract");

    // --- read: a truncated read names the next offset ---
    system("rm -rf /tmp/tool_steer && mkdir -p /tmp/tool_steer");
    {
        FILE* f = fopen("/tmp/tool_steer/big.txt", "w");
        for (int i = 0; i < 4000; ++i)
            fprintf(f, "line %d\n", i);
        fclose(f);
    }
    Tool read = makeReadTool("/tmp/tool_steer");
    nlohmann::json args = nlohmann::json::object();
    args["path"] = "/tmp/tool_steer/big.txt";
    auto r = read.execute(args, noAbort);
    check(!r.isError, "read: big file ok");
    check(r.content.find("Re-read with offset=") != std::string::npos,
          "read: truncation names the next offset");
    check(r.content.find("sed") == std::string::npos,
          "read: truncation note does not suggest a shell command");

    system("rm -rf /tmp/tool_steer");

    if (failures == 0)
        printf("tool_steer_test: all passed\n");
    else
        printf("tool_steer_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
