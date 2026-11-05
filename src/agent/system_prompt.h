#pragma once

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "agent/skills.h"
#include "util/settings.h"

// The user can override the system prompt by placing a file at
// ~/.config/miniagent/SYSTEM.md. If it exists and is non-empty, its contents
// are used verbatim; otherwise the built-in default below is returned.
inline std::string loadCustomSystemPrompt() {
    const std::string path = miniagentConfigDir() + "/SYSTEM.md";
    std::ifstream f(path);
    if (!f)
        return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string out = ss.str();
    // Trim a single trailing newline so the prompt reads cleanly.
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// Reads AGENTS.md from the working directory, if present.
inline std::string loadAgentsMd(const std::string& cwd) {
    std::ifstream f(cwd + "/AGENTS.md");
    if (!f)
        return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string out = ss.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return out;
}

// Tool-steering block shared by the main agent's system prompt and the
// subagent wrapper prompt (runSubagent), so fresh-context roles get the
// same guidance about preferring the dedicated file tools over bash.
// Adapted to the tools actually registered for the caller.
inline std::string toolGuidanceSection(bool read, bool write, bool edit, bool grep = false,
                                      bool web = false, bool applyPatch = false) {
    std::string s = "Tools:\n";
    if (read)
        s += "- read: inspect files before editing them. Lines are prefixed with 'NNNNN→' for display "
             "only; that prefix is not part of the file content. A directory path lists its entries. "
             "For large files use offset/limit to read specific line ranges.\n";
    if (grep)
        s += "- grep: search file contents recursively under a directory for a case-insensitive "
             "substring. Use it instead of running grep/rg through bash. It returns file:line "
             "matches; read the interesting ones in full with read.\n";
    if (write)
        s += "- write: create or overwrite a file.\n";
    if (edit)
        s += "- edit: precise text replacements. Each oldText must match the file exactly "
             "(whitespace included) and occur exactly once.\n";
    if (applyPatch)
        s += "- apply_patch: edit files with a patch instead of the edit tool. Format:\n"
             "  *** Begin Patch\n"
             "  *** Update File: <path>\n"
             "  @@ <optional anchor line the chunk follows>\n"
             "   <context line> (leading space: unchanged, must match the file)\n"
             "  -<line to remove>\n"
             "  +<line to add>\n"
             "  *** End Patch\n"
             "  Several chunks (@@ sections) per file and several files per patch; '*** Add File:' "
             "creates, '*** Delete File:' removes, '*** Move to:' renames inside an update hunk. "
             "Copy context and '-' lines from the file's current content; matching tolerates "
             "whitespace and unicode differences, but a chunk that cannot be located fails the "
             "whole patch.\n";
    if (web)
        s += "- web_search: search the web for documentation, library behaviour, error messages and "
             "external examples; it returns titles, URLs and snippets. Then web_fetch on the URL "
             "that matters to read its content as text. Verify version-specific claims against the "
             "fetched page rather than your priors, and cite the URL you used.\n";
    if (!read && !write && !edit)
        s += "- The dedicated file tools are disabled: read, create, and edit files with the bash "
             "tool (e.g. cat, sed, awk, python).\n";
    s += "- bash: run shell commands - builds, tests, scripts, and anything with no dedicated tool. "
         "Do not read files with bash: never cat, sed -n, head, or tail a file to view it - use read "
         "(offset/limit for ranges).";
    if (grep)
        s += " Do not search file contents with bash grep/rg - use the grep tool.";
    if (web)
        s += " Do not search the web or download pages with bash curl/wget - use web_search and "
             "web_fetch.";
    s += "\n";
    s += "- codegraph: if codegraph tools are available, use them directly before grep/read for "
         "structural questions - where a symbol lives, who calls it, call flow, architecture, the "
         "impact of a change. One codegraph call answers what grep plus repeated file reads takes "
         "many round trips to find.\n\n";
    return s;
}

// The skill contract: where skills live, what SKILL.md must contain, and how
// to distil one out of a repository. Static, so a project that has no skills
// yet still learns the format from the prompt (the <available_skills> block
// alone only describes what already exists).
inline std::string skillsSection() {
    return "\n\nSkills:\n"
           "A skill is a directory holding a SKILL.md, discovered from "
           "<cwd>/.miniagent/skills/<name>/SKILL.md and "
           "~/.config/miniagent/skills/<name>/SKILL.md. Its YAML frontmatter carries `name` "
           "(lowercase letters, digits and hyphens; keep it equal to the directory name) and "
           "`description` (what the skill does AND when to use it), and its markdown body is the "
           "instructions. Only name, description and file path are advertised to you; the body is "
           "read with the read tool when a task matches the description, so the description is the "
           "sole routing signal - never write one like 'Helps with PDFs'.\n"
           "Supporting files sit next to SKILL.md and are referenced by paths relative to the skill "
           "directory; a directory that contains a SKILL.md is never scanned deeper, so put extras "
           "under references/ instead of nesting another SKILL.md.\n"
           "When asked to create or find skills for a project, turn recurring *procedures* into "
           "them (build and test commands, verification workflows, release steps, environment "
           "quirks, external tool locations). Mine AGENTS.md, scripts/, CI config, Makefiles and "
           "package.json scripts, CONTRIBUTING.md and docs - not the source tree, which yields "
           "skills that merely restate the file layout. Keep facts that are always needed in "
           "AGENTS.md and move the how-to detail into the skill, so it is paid for only when used. "
           "One skill per workflow, each in its own directory.\n"
           "The app lists the skills it loaded, with any load warnings, under its /skills command. "
           "You cannot type that command, so to check what actually loaded, run the app with "
           "MINIAGENT_PROMPT=/skills and MINIAGENT_SCREENSHOT=/tmp/skills.png and read that image "
           "(needs a display).\n";
}

// The <available_skills> block for a directory, discovered on the spot. Used
// by fresh-context roles (runSubagent, and the scaffold roles that go through
// it): they get the same advertisement the parent agent sees - and so can read
// a skill body when their task matches - but not skillsSection(), which is the
// authoring contract only the top-level agent needs.
inline std::string skillsBlockForCwd(const std::string& cwd) {
    return formatSkillsForPrompt(loadSkills(cwd).skills);
}

// Builds the system prompt: identity + working directory + tool guidance +
// execution/validation policy + AGENTS.md. The execution and verification
// sections are adapted from Codex's base instructions (only the parts that
// apply to this harness's tools).
// fileTools=false when the read/write/edit tools are not registered. `skills`
// is the discovered skill list; only name/description/path are advertised, and
// the section is skipped entirely without the read tool (the model could not
// load a skill body it cannot read).
inline std::string buildSystemPrompt(const std::string& cwd,
                                    bool fileTools = true,
                                    const std::vector<Skill>* skills = nullptr,
                                    bool web = false,
                                    bool applyPatch = false) {
    std::string prompt;
    const std::string custom = loadCustomSystemPrompt();
    if (!custom.empty())
        prompt = custom;
    else {
        prompt = "You are a coding agent that completes tasks in a local directory by using tools.\n"
                 "Current working directory: " +
                 cwd +
                 ".\n"
                 "Relative paths in tool calls are resolved against the working directory.\n\n";
        prompt += toolGuidanceSection(fileTools, fileTools, fileTools && !applyPatch,
                                      fileTools, web, fileTools && applyPatch);
        prompt +=
            "Task execution:\n"
            "- Keep working until the task is completely resolved before ending your turn. Only stop "
            "when you are confident the problem is solved; do not hand the task back half-done.\n"
            "- Do not guess or make up results. Verify assumptions with tools.\n"
            "- Proceed step by step, using each tool's result to decide the next step.\n"
            "- Do not re-read a file after a successful edit or write; those tools report an error "
            "when they fail.\n\n"
            "Verifying your work:\n"
            "- If the project can be built, tested, or run, verify your changes before finishing.\n"
            "- Start with the narrowest check that covers your change (the file or module you "
            "touched), then broaden to the full build or test suite as you gain confidence.\n"
            "- If a check fails, fix the cause and re-check. After roughly three failed fix attempts, "
            "stop and report exactly what fails and what you tried.\n"
            "- Never claim something works unless you verified it with a tool.\n\n"
            "Coding guidelines:\n"
            "- Fix root causes rather than applying surface-level patches.\n"
            "- Keep changes minimal and focused; follow the existing code style. Do not refactor or "
            "rename unrelated code.\n"
            "- Do not fix unrelated bugs or broken tests; mention them in your final message instead.\n"
            "- Do not run git commit or create branches unless explicitly asked.\n\n"
            "Keep responses concise. Finish with a short summary of what changed and how it was "
            "verified.";
    }
    const std::string agentsMd = loadAgentsMd(cwd);
    if (!agentsMd.empty())
        prompt += "\n\nProject-specific instructions (AGENTS.md):\n" + agentsMd;
    if (fileTools) {
        prompt += skillsSection();
        if (skills)
            prompt += formatSkillsForPrompt(*skills);
    }
    return prompt;
}
