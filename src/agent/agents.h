#pragma once

#include <string>
#include <vector>

// A subagent role definition: markdown file with a YAML frontmatter block
// (--- delimited) carrying name / description / tools, and a body that is the
// role's system prompt. Tools may be given as "a, b, c" or as a YAML list
// ("- a" lines). An empty tool list means "all tools".
struct AgentDef {
    std::string name;
    std::string description;
    std::vector<std::string> tools;
    std::string systemPrompt;
    std::string path;
};

// Parses agent file content. False when the frontmatter has no name and
// description (bad files are skipped, never fatal).
bool parseAgentContent(const std::string& nameHint, const std::string& content, AgentDef& out);

// Parses an agent file on disk (see parseAgentContent).
bool parseAgentFile(const std::string& path, AgentDef& out);

// ~/.config/miniagent/agents
std::string userAgentsDir();

// Built-in agents shipped in the binary (brainstorm, worker).
const char* builtinAgentContent(const std::string& name);

// Discovery: built-ins, then <cwd>/.miniagent/agents/*.md, then
// ~/.config/miniagent/agents/*.md; later sources override earlier ones by
// name.
std::vector<AgentDef> discoverAgents(const std::string& cwd);
