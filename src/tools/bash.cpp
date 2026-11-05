#include "tools/tools.h"
#include "util/path.h"
#include "util/process.h"

Tool makeBashTool(const std::string& cwd) {
    Tool t;
    t.name = "bash";
    t.description = "Run a shell command in the working directory and return combined stdout/stderr plus the exit code. "
                    "Use for builds, tests, git, scripts, and anything with no dedicated tool. "
                    "Do NOT use bash to read file contents: never cat, sed -n, head, or tail a file - "
                    "use the read tool (offset/limit for line ranges) instead. Do NOT use bash to search "
                    "file contents: use the grep tool instead. "
                    "Output is truncated to 2000 lines or 50KB. Default timeout is 120 seconds.";
    t.paramsSchema = {
        {"type", "object"},
        {"properties", {
            {"command", {{"type", "string"}, {"description", "The command to run (bash -c)"}}},
            {"timeout", {{"type", "integer"}, {"description", "Timeout in seconds (default 120; values <= 0 use the default)"}}},
        }},
        {"required", {"command"}},
    };
    t.execute = [cwd](nlohmann::json args, const std::atomic<bool>& abort) -> ToolResult {
        if (!args.contains("command") || !args["command"].is_string())
            return {"Missing 'command' argument", true};

        std::string command = args["command"].get<std::string>();
        int timeoutSec = args.value("timeout", 120);
        if (timeoutSec <= 0)
            timeoutSec = 120; // 0 must not mean "no timeout"

        ProcessOutput proc = runShellCommand(command, cwd, timeoutSec * 1000, abort);

        std::string result;
        result += "$ " + command + "\n";
        result += "[exit code: " + std::to_string(proc.exitCode) + "]";
        if (proc.timedOut)
            result += " [timed out]";
        if (proc.killed)
            result += " [killed]";
        if (!proc.output.empty())
            result += "\n" + proc.output;
        result = truncateToLimit(result, 2000, 50 * 1024);

        bool isError = proc.exitCode != 0;
        return {result, isError};
    };
    return t;
}
