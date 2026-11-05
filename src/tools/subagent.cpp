#include <algorithm>
#include <string>

#include "agent/agents.h"
#include "agent/agent.h"
#include "agent/system_prompt.h"
#include "llm/openai.h"
#include "tools/tools.h"

#include <nlohmann/json.hpp>

// Cap on the text returned from a subagent (matches pi's per-task cap).
static const size_t kSubagentOutputCap = 50 * 1024;

// UTF-8-aware byte cap: back up over continuation bytes so the cut never
// splits a character.
static std::string capBytes(const std::string& s, size_t cap) {
    if (s.size() <= cap)
        return s;
    std::string out = s.substr(0, cap);
    while (!out.empty() && (out.back() & 0xC0) == 0x80)
        out.pop_back();
    return out;
}

// The fresh-context system prompt of one role: wrapper preamble + tool
// guidance + the role file's own prompt + AGENTS.md + the project's skill
// advertisement. Split out of runSubagent so the composition is testable
// without an LLM.
std::string roleSystemPrompt(const std::string& cwd,
                            const AgentDef& def,
                            bool read,
                            bool write,
                            bool edit,
                            bool grep,
                            bool applyPatch = false) {
    std::string system = "You are a subagent in an isolated context: you have no "
                         "conversation history, so do exactly the task you are given and nothing more. "
                         "Working directory: " + cwd +
                         ". Relative paths resolve against it. "
                         "End with the output format the role asks for.\n\n" +
                         toolGuidanceSection(read, write, edit, grep, /*web*/ false, applyPatch) +
                         def.systemPrompt;

    // The subagent's fresh context contains no project rules of its own, so
    // give it the same AGENTS.md the main agent gets (build/test conventions,
    // layout, do/don't lists).
    const std::string agentsMd = loadAgentsMd(cwd);
    if (!agentsMd.empty())
        system += "\n\nProject-specific instructions (AGENTS.md):\n" + agentsMd;

    // The same skill advertisement the parent agent gets, so a role can load a
    // project procedure itself instead of rediscovering it. Only with the read
    // tool: the block tells it to read the skill file, and a role without read
    // could not act on it. Roles are not told how to author skills
    // (skillsSection() stays parent-only).
    if (read)
        system += skillsBlockForCwd(cwd);
    return system;
}

// Runs one role agent in a fresh context and returns its final reply (with a
// status prefix when the run did not end cleanly). The nested agent shares
// the parent's abort flag, so stopping the parent run stops subagents too.
ToolResult runSubagent(const std::string& cwd,
                       OpenAIProvider provider,
                       const std::vector<AgentDef>& agents,
                       const std::string& agentName,
                       const std::string& task,
                       const std::vector<Tool>& parentTools,
                       const std::atomic<bool>& abort,
                       const std::function<void(const AgentEvent&)>& onEvent,
                       const std::string& roleLabel,
                       std::atomic<bool>* manualCompact) {
    const AgentDef* def = nullptr;
    for (const auto& a : agents)
        if (a.name == agentName)
            def = &a;
    if (!def) {
        std::string avail;
        for (const auto& a : agents)
            avail += (avail.empty() ? "" : ", ") + a.name;
        return ToolResult{"Unknown agent: " + agentName + (avail.empty() ? "" : ". Available agents: " + avail), true};
    }

    // Tool filtering: the agent's frontmatter names the tools it may use;
    // unknown names are dropped, empty means all. The subagent tool itself
    // is never re-registered (no nested dispatching).
    ToolRegistry subTools;
    for (const auto& t : parentTools) {
        if (t.name == "subagent")
            continue;
        // Web tools are main-agent only: a role works a narrow task in a fresh
        // context, and its frontmatter cannot opt in (an empty tool list means
        // "all tools", which would otherwise let them through).
        if (t.name == "web_search" || t.name == "web_fetch")
            continue;
        if (!def->tools.empty() &&
            std::find(def->tools.begin(), def->tools.end(), t.name) == def->tools.end())
            continue;
        subTools.add(t);
    }
    if (subTools.all().empty())
        return ToolResult{"Agent '" + def->name + "' has no compatible tools.", true};

    auto hasTool = [&subTools](const char* name) {
        for (const auto& t : subTools.all())
            if (t.name == name)
                return true;
        return false;
    };

    const std::string system = roleSystemPrompt(cwd,
                                               *def,
                                               hasTool("read"),
                                               hasTool("write"),
                                               hasTool("edit"),
                                               hasTool("grep"),
                                               hasTool("apply_patch"));

    // Forward a reduced set of the nested events to the parent UI: turn
    // boundaries, text, thinking and tool rows, plus the argument fragments
    // of in-flight tool calls (a scaffold role's write/edit streams into a
    // live row just like the parent agent's). Usage-delta events are
    // skipped so the parent's streaming/speed state stays untouched.
    auto forward = [name = roleLabel.empty() ? def->name : roleLabel, onEvent](const AgentEvent& ev) {
        switch (ev.type) {
            case AgentEventType::AgentStart:
            case AgentEventType::AgentEnd:
            case AgentEventType::TurnStart:
            case AgentEventType::TextDelta:
            case AgentEventType::ThinkingDelta:
            case AgentEventType::ToolCallStart:
            case AgentEventType::ToolArgsDelta:
            case AgentEventType::ToolCallEnd:
            case AgentEventType::TurnEnd:
            case AgentEventType::Error:
            case AgentEventType::Retry:
            case AgentEventType::Compaction:
            case AgentEventType::Warning:
                break;
            default:
                return;
        }
        AgentEvent out = ev;
        out.nested      = true;
        out.nestedAgent = name;
        if (onEvent)
            onEvent(out);
    };

    // Capture the last assistant message (its stopReason tells whether the
    // run finished cleanly) and the accumulated usage.
    AssistantMessage last;
    Usage total;
    auto onMessage = [&](const AgentMessage& m) {
        if (auto* a = std::get_if<AssistantMessage>(&m)) {
            last = *a;
            total.inputTokens  += a->usage.inputTokens;
            total.outputTokens  += a->usage.outputTokens;
            total.totalTokens   += a->usage.totalTokens;
        }
    };

    Agent nested(system, provider, &subTools, forward, onMessage);
    nested.shareAbort(abort);
    if (manualCompact)
        nested.shareCompactionRequest(*manualCompact);
    UserMessage um;
    um.text      = task;
    um.timestamp = nowMs();
    nested.run(std::move(um));

    std::string result = capBytes(last.text, kSubagentOutputCap);
    bool isError       = false;
    if (last.stopReason == "error") {
        isError = true;
        result  = "[subagent " + def->name + " error] " + last.errorMessage +
                    (result.empty() ? "" : "\n" + result);
    } else if (last.stopReason == "aborted") {
        isError = true;
        result  = "[subagent " + def->name + " aborted]" + (result.empty() ? "" : "\n" + result);
    } else if (last.stopReason == "length") {
        result = (result.empty() ? "" : result + "\n") +
                 "[subagent " + def->name + " stopped: output limit reached]";
    }
    if (!result.empty() && result.size() > kSubagentOutputCap + 256)
        result = capBytes(result, kSubagentOutputCap) +
                 "\n[output truncated to " + std::to_string(kSubagentOutputCap / 1024) + " KB]";
    result += "\n[usage: \u2191" + std::to_string(total.inputTokens) +
              " \u2193" + std::to_string(total.outputTokens) + " tokens]";
    return {result, isError};
}

Tool makeSubagentTool(const std::string& cwd,
                      OpenAIProvider provider,
                      const std::function<std::vector<AgentDef>()>& agentLookup,
                      const std::function<std::vector<Tool>()>& parentTools,
                      const std::function<const std::atomic<bool>*()>& parentAbort,
                      const std::function<void(const AgentEvent&)>& onEvent) {
    Tool tool;
    tool.name = "subagent";
    tool.description = "Delegate a task to a specialized agent that runs in a fresh, "
                       "isolated context (no conversation history). The agent's final "
                       "reply is returned as the result. Roles available for the "
                       "manager-worker scaffold: brainstorm (ideation pass), worker "
                       "(executes one task).";
    tool.paramsSchema = nlohmann::json{{
        "type", "object",
    },
                                      {"properties",
                                       {
                                           {"agent",
                                            {
                                                {"type", "string"},
                                                {"description", "Name of the agent to run: brainstorm or worker"},
                                            }},
                                           {"task",
                                            {
                                                {"type", "string"},
                                                {"description",
                                                 "The task to delegate. Keep it short; the agent "
                                                 "reads its context (e.g. the ledger) from disk."},
                                            }},
                                       }},
                                      {"required", {"agent", "task"}}};
    tool.execute = [cwd, provider, agentLookup, parentTools, parentAbort, onEvent](
                       const nlohmann::json& args, const std::atomic<bool>& abort) {
        std::string agentName = args.value("agent", "");
        std::string task      = args.value("task", "");
        if (agentName.empty() || task.empty())
            return ToolResult{"subagent requires both 'agent' and 'task' arguments.", true};
        auto agents = agentLookup();
        const std::atomic<bool>* flag = parentAbort ? parentAbort() : nullptr;
        if (!flag)
            return ToolResult{"subagent cannot run: no active agent.", true};
        const std::vector<Tool> tools = parentTools ? parentTools() : std::vector<Tool>{};
        // manualCompact stays null: a plain run's Compact button targets the
        // parent agent's conversation, not its subagent calls.
        return runSubagent(cwd, provider, agents, agentName, task, tools, *flag, onEvent, "",
                           nullptr);
    };
    return tool;
}
