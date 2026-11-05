// Offline checks for the fresh-context role prompt (runSubagent's system
// prompt): role body, tool guidance flags, AGENTS.md, and the project skill
// advertisement (visible skills only, no authoring contract, dropped without
// the read tool).

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "agent/agents.h"
#include "tools/factories.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);               \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

namespace {

void writeFile(const std::string& path, const std::string& content) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    std::ofstream f(path);
    f << content;
}

} // namespace

int main() {
    const std::string proj = "/tmp/miniagent_role_prompt";
    std::filesystem::remove_all(proj);
    // An empty HOME keeps real ~/.config/miniagent/skills out of the
    // assertions (userSkillsDir() reads HOME on every call).
    std::filesystem::create_directories(proj + "/home");
    setenv("HOME", (proj + "/home").c_str(), 1);
    writeFile(proj + "/.miniagent/skills/do-thing/SKILL.md",
              "---\nname: do-thing\ndescription: How to do the thing here\n---\nStep 1.\n");
    writeFile(proj + "/.miniagent/skills/hidden/SKILL.md",
              "---\nname: hidden\ndescription: explicit only\ndisable-model-invocation: true\n---\n"
              "Step 1.\n");
    writeFile(proj + "/AGENTS.md", "Project rule: never run the deploy step.\n");

    AgentDef def;
    def.name         = "worker";
    def.description  = "does work";
    def.systemPrompt = "Role body: end with a bullet list.";

    const std::string withRead = roleSystemPrompt(proj, def, true, true, true, true);
    CHECK(withRead.find("You are a subagent in an isolated context") != std::string::npos);
    CHECK(withRead.find(proj) != std::string::npos);
    CHECK(withRead.find("Role body: end with a bullet list.") != std::string::npos);
    CHECK(withRead.find("- grep:") != std::string::npos);
    // Web tools are main-agent only: a role never gets their guidance (the
    // flag stays false in roleSystemPrompt) nor the tools (runSubagent drops
    // them even when the role's frontmatter tool list is empty = "all").
    CHECK(withRead.find("web_search") == std::string::npos);
    CHECK(withRead.find("web_fetch") == std::string::npos);
    CHECK(withRead.find("never run the deploy step") != std::string::npos);

    // Skills: advertised like for the parent agent, explicit-only ones hidden,
    // and no authoring contract (roles use skills, they do not invent them).
    CHECK(withRead.find("<available_skills>") != std::string::npos);
    CHECK(withRead.find("<name>do-thing</name>") != std::string::npos);
    CHECK(withRead.find(proj + "/.miniagent/skills/do-thing/SKILL.md") != std::string::npos);
    CHECK(withRead.find("<name>hidden</name>") == std::string::npos);
    CHECK(withRead.find("A skill is a directory holding a SKILL.md") == std::string::npos);

    // No read tool: no skill block (it instructs the model to read a file).
    const std::string noRead = roleSystemPrompt(proj, def, false, false, false, false);
    CHECK(noRead.find("<available_skills>") == std::string::npos);
    CHECK(noRead.find("The dedicated file tools are disabled") != std::string::npos);
    CHECK(noRead.find("Role body: end with a bullet list.") != std::string::npos);

    // A project without skills still gets a valid prompt.
    CHECK(roleSystemPrompt("/tmp/miniagent_role_prompt_none", def, true, false, false, false)
              .find("<available_skills>") == std::string::npos);

    if (failures) {
        printf("role_prompt_test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("role_prompt_test: passed\n");
    return 0;
}
