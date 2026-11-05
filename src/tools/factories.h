#pragma once

#include <string>

#include "agent/agents.h"
#include "llm/backends.h"
#include "llm/openai.h"
#include "tools/tools.h"

// visionHint: optional sentence appended when an image file is read while an
// image model is designated in the Backends modal — the main backend then
// cannot view image bytes, so the tool reports the image without attaching
// it and steers the model to analyze_image.
Tool makeReadTool(const std::string& cwd, const std::string& visionHint = {});

// The image-analysis tool: only registered when the Backends modal designates
// an image model. Sends the (decoded, resized) images as image_url parts plus
// the question straight to that backend and returns the model's text answer.
Tool makeAnalyzeImageTool(const std::string& cwd, LlmBackend imageBackend);
Tool makeWriteTool(const std::string& cwd);
Tool makeEditTool(const std::string& cwd);
// codex-style apply_patch (port of codex-rs/apply-patch): a single patch
// argument in the '*** Begin Patch ... *** End Patch' format that adds,
// deletes, moves, or updates files, with codex's fuzzy line matching
// (whitespace/unicode tolerant). Replaces the edit tool when
// apply_patch=1 is set in ~/.config/miniagent/miniagent.settings.
Tool makeApplyPatchTool(const std::string& cwd);
Tool makeBashTool(const std::string& cwd);
// Recursive substring search (case-insensitive) over file contents. The
// dedicated search tool - use it instead of running grep/rg through bash.
Tool makeGrepTool(const std::string& cwd);

// Web tools (main agent only - runSubagent never hands them to a role):
// web_search queries a local SearXNG instance over its JSON API (baseUrl from
// the 'searxng_url' setting), web_fetch downloads one URL and reduces it to
// readable text. Both go through util/http's httpGetEx, so they honour the
// agent's abort flag.
Tool makeWebSearchTool(const std::string& baseUrl);
Tool makeWebFetchTool();

// The subagent tool: dispatches a role agent (see agent/agents.h) in a fresh
// context and returns its final reply. agentLookup() re-discovers agents per
// dispatch (project and user dirs may change); parentTools() hands over the
// current tool set to filter per role; parentAbort() the live abort flag of
// the running agent so Stop propagates into subagents; onEvent forwards the
// nested agent's activity to the UI (marked nested).
Tool makeSubagentTool(const std::string& cwd,
                      OpenAIProvider provider,
                      const std::function<std::vector<AgentDef>()>& agentLookup,
                      const std::function<std::vector<Tool>()>& parentTools,
                      const std::function<const std::atomic<bool>*()>& parentAbort,
                      const std::function<void(const AgentEvent&)>& onEvent);

// Runs one role agent (by name from `agents`) in a fresh context; returns
// its final reply. The nested agent shares `abort`. onEvent (optional)
// receives the nested events marked nested/nestedAgent. manualCompact
// (optional) is a shared manual-compaction request flag (the UI's Compact
// button during a scaffold run): the role agent consumes it at its next
// turn boundary and compacts its own conversation.
ToolResult runSubagent(const std::string& cwd,
                       OpenAIProvider provider,
                       const std::vector<AgentDef>& agents,
                       const std::string& agentName,
                       const std::string& task,
                       const std::vector<Tool>& parentTools,
                       const std::atomic<bool>& abort,
                       const std::function<void(const AgentEvent&)>& onEvent,
                       const std::string& roleLabel = "",
                       std::atomic<bool>* manualCompact = nullptr);

// The system prompt runSubagent gives the role (wrapper preamble + tool
// guidance + the role's own prompt + AGENTS.md + the project's skill
// advertisement), built from the role definition and the tools it was
// allowed. Exposed for tests; the flags mirror the role's tool filter.
std::string roleSystemPrompt(const std::string& cwd,
                            const AgentDef& def,
                            bool read,
                            bool write,
                            bool edit,
                            bool grep,
                            bool applyPatch = false);
