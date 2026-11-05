#pragma once

#include <string>
#include <vector>

// A skill: a directory holding a SKILL.md whose YAML frontmatter carries a
// name and a description, and whose body is the instruction text. Only the
// name, description and file path are advertised in the system prompt; the
// body reaches the model only when it reads the file (progressive disclosure)
// or when the user forces it with "/skill:name". Supporting files (scripts,
// references) sit next to SKILL.md and are referenced by relative path.
// Modelled on pi's core/skills.ts.
struct Skill {
    std::string name;
    std::string description;
    std::string filePath; // absolute path of the SKILL.md
    std::string baseDir;  // dirname of filePath; relative paths resolve here
    std::string body;     // instruction text after the frontmatter
    bool disableModelInvocation = false; // hidden from the prompt, /skill:name only
};

struct SkillDiagnostic {
    std::string message;
    std::string path;
};

struct SkillsResult {
    std::vector<Skill> skills;
    std::vector<SkillDiagnostic> diagnostics;
};

// ~/.config/miniagent/skills
std::string userSkillsDir();

// Parses SKILL.md content. nameHint is the skill directory name (or the file
// stem for a standalone .md), used when the frontmatter has no name. Returns
// false when the file cannot be used (no frontmatter, no description); either
// way `warnings` collects what was wrong so callers can report diagnostics.
// Name/description rule violations only warn - the skill still loads.
bool parseSkillContent(const std::string& nameHint,
                       const std::string& content,
                       Skill& out,
                       std::vector<std::string>& warnings);

// Scans one directory: a directory that contains SKILL.md is a skill root and
// is not descended into any further (so nested SKILL.md files are ignored);
// otherwise its direct .md children are loaded as standalone skills and its
// subdirectories are scanned recursively for SKILL.md. Dot-directories and
// node_modules are skipped. Appends to `out`.
void loadSkillsFromDir(const std::string& dir, SkillsResult& out);

// Discovery: <cwd>/.miniagent/skills and <userDir> (default
// ~/.config/miniagent/skills). The first skill with a given name wins; later
// duplicates are dropped and reported as collision diagnostics.
SkillsResult loadSkills(const std::string& cwd, const std::string& userDir = userSkillsDir());

// The <available_skills> block appended to the system prompt: one entry per
// skill with name, description and location. Empty string when there is
// nothing to advertise; skills with disableModelInvocation are left out.
std::string formatSkillsForPrompt(const std::vector<Skill>& skills);

// Expands "/skill:name extra args" into the skill body wrapped in a <skill>
// block, with the args appended as the user request. Returns an empty string
// when no skill matched the name (or the text is not a skill command).
std::string expandSkillCommand(const std::vector<Skill>& skills, const std::string& text);
