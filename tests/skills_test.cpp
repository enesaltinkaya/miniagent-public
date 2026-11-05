// Pure unit tests for skill discovery and prompt/expansion formatting.
// No network: frontmatter rules, one-skill-per-directory scanning, name
// collisions between the user and project dirs, the <available_skills> block,
// and /skill:name expansion.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "agent/skills.h"
#include "agent/system_prompt.h"

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

const Skill* find(const std::vector<Skill>& skills, const std::string& name) {
    for (const auto& s : skills)
        if (s.name == name)
            return &s;
    return nullptr;
}

std::string messageOf(const SkillDiagnostic& d) { return d.message; }
std::string messageOf(const std::string& s) { return s; }

template <typename T>
bool hasDiagnostic(const std::vector<T>& ds, const std::string& needle) {
    for (const auto& d : ds)
        if (messageOf(d).find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

static void testFrontmatter() {
    printf("frontmatter\n");
    Skill sk;
    std::vector<std::string> w;

    w.clear();
    std::string s = "---\nname: pdf-tools\ndescription: Extract text from PDFs.\n---\n"
                    "# PDF tools\n\nRead references/formats.md first.\n";
    CHECK(parseSkillContent("pdf-tools", s, sk, w));
    CHECK(sk.name == "pdf-tools");
    CHECK(sk.description == "Extract text from PDFs.");
    CHECK(!sk.disableModelInvocation);
    CHECK(w.empty());
    CHECK(sk.body.find("Read references/formats.md first.") != std::string::npos);
    CHECK(sk.body.find("description:") == std::string::npos);

    // name falls back to the directory hint; unknown keys are ignored.
    w.clear();
    s = "---\ndescription: hint named\nlicense: MIT\nmetadata: whatever\n---\nBody.\n";
    CHECK(parseSkillContent("hint-dir", s, sk, w));
    CHECK(sk.name == "hint-dir");
    CHECK(w.empty());

    // quoted values
    w.clear();
    s = "---\nname: quoted\ndescription: \"a quoted description\"\n---\nBody.\n";
    CHECK(parseSkillContent("quoted", s, sk, w));
    CHECK(sk.description == "a quoted description");

    // disable-model-invocation
    w.clear();
    s = "---\nname: secret\ndescription: explicit only\ndisable-model-invocation: true\n---\nB.\n";
    CHECK(parseSkillContent("secret", s, sk, w));
    CHECK(sk.disableModelInvocation);

    // A missing description drops the skill: it is the only routing signal.
    w.clear();
    CHECK(!parseSkillContent("x", "---\nname: x\n---\nbody\n", sk, w));
    CHECK(hasDiagnostic(w, "description is required"));

    // Bad names only warn; the skill still loads.
    w.clear();
    s = "---\nname: Bad_Name\ndescription: bad name\n---\nBody.\n";
    CHECK(parseSkillContent("Bad_Name", s, sk, w));
    CHECK(sk.name == "Bad_Name");
    CHECK(!w.empty());

    w.clear();
    s = "---\nname: -lead\ndescription: leading hyphen\n---\nBody.\n";
    CHECK(parseSkillContent("-lead", s, sk, w));
    CHECK(!w.empty());

    w.clear();
    s = "---\nname: a--b\ndescription: double hyphen\n---\nBody.\n";
    CHECK(parseSkillContent("a--b", s, sk, w));
    CHECK(!w.empty());

    // Malformed files are rejected, never fatal.
    w.clear();
    CHECK(!parseSkillContent("x", "just prose, no frontmatter", sk, w));
    CHECK(!w.empty());
    w.clear();
    CHECK(!parseSkillContent("x", "---\nname: x\ndescription: y\nunclosed\n", sk, w));
    CHECK(!w.empty());
}

static void testDiscovery() {
    printf("discovery\n");
    const std::string root = "/tmp/miniagent_skills_test/disc";
    std::filesystem::remove_all(root);

    writeFile(root + "/alpha/SKILL.md",
              "---\nname: alpha\ndescription: alpha skill\n---\nAlpha body.\n");
    writeFile(root + "/alpha/references/deep.md", "not a skill, a reference\n");
    // Inside a skill root: never scanned, so this one must not be discovered.
    writeFile(root + "/alpha/nested/SKILL.md",
              "---\nname: nested\ndescription: hidden inside alpha\n---\nNope.\n");
    writeFile(root + "/beta/SKILL.md", "---\nname: beta\ndescription: beta skill\n---\nBeta body.\n");
    writeFile(root + "/gamma/SKILL.md",
              "---\nname: gamma\ndescription: disabled skill\ndisable-model-invocation: true\n---\n"
              "Gamma body.\n");
    // Standalone root .md files are skills too, named after the file stem.
    writeFile(root + "/standalone.md", "---\ndescription: standalone skill\n---\nS body.\n");
    writeFile(root + "/no-description.md", "---\nname: no-desc\n---\nignored\n");
    // Skipped locations.
    writeFile(root + "/.hidden/SKILL.md", "---\nname: hidden\ndescription: dot dir\n---\nx\n");
    writeFile(root + "/node_modules/pkg/SKILL.md",
              "---\nname: dep\ndescription: dependency\n---\nx\n");

    SkillsResult r;
    loadSkillsFromDir(root, r);

    CHECK(find(r.skills, "alpha"));
    CHECK(find(r.skills, "beta"));
    CHECK(find(r.skills, "gamma"));
    CHECK(find(r.skills, "standalone"));
    CHECK(!find(r.skills, "nested"));
    CHECK(!find(r.skills, "no-desc"));
    CHECK(!find(r.skills, "hidden"));
    CHECK(!find(r.skills, "dep"));
    CHECK(r.skills.size() == 4);

    CHECK(hasDiagnostic(r.diagnostics, "description is required"));

    // baseDir points at the skill directory (relative paths resolve there).
    const Skill* alpha = find(r.skills, "alpha");
    CHECK(alpha && alpha->baseDir == root + "/alpha");
    CHECK(alpha && alpha->filePath == root + "/alpha/SKILL.md");
    const Skill* standalone = find(r.skills, "standalone");
    CHECK(standalone && standalone->baseDir == root);

    // A missing directory yields nothing rather than failing.
    SkillsResult missing;
    loadSkillsFromDir("/tmp/miniagent_skills_test/definitely-not-here", missing);
    CHECK(missing.skills.empty());
}

static void testPrecedence() {
    printf("precedence\n");
    const std::string root = "/tmp/miniagent_skills_test/prec";
    std::filesystem::remove_all(root);

    const std::string user    = root + "/user";
    const std::string project = root + "/proj/.miniagent/skills";
    writeFile(user + "/shared/SKILL.md", "---\nname: shared\ndescription: user wins\n---\nU body.\n");
    writeFile(project + "/shared/SKILL.md", "---\nname: shared\ndescription: project loses\n---\nP body.\n");
    writeFile(project + "/only-project/SKILL.md",
              "---\nname: only-project\ndescription: project skill\n---\nP body.\n");

    SkillsResult r = loadSkills(root + "/proj", user);
    const Skill* shared = find(r.skills, "shared");
    CHECK(shared);
    CHECK(shared && shared->description == "user wins");
    CHECK(find(r.skills, "only-project"));
    CHECK(r.skills.size() == 2);
    CHECK(hasDiagnostic(r.diagnostics, "collides"));
}

static void testPromptBlock() {
    printf("prompt block\n");
    const std::string root = "/tmp/miniagent_skills_test/prompt";
    std::filesystem::remove_all(root);
    writeFile(root + "/one/SKILL.md",
              "---\nname: one\ndescription: First & <only> one\n---\nOne body.\n");
    writeFile(root + "/two/SKILL.md",
              "---\nname: two\ndescription: second\ndisable-model-invocation: true\n---\nTwo body.\n");

    SkillsResult r;
    loadSkillsFromDir(root, r);
    CHECK(r.skills.size() == 2);

    const std::string block = formatSkillsForPrompt(r.skills);
    CHECK(block.find("<available_skills>") != std::string::npos);
    CHECK(block.find("<name>one</name>") != std::string::npos);
    CHECK(block.find("First &amp; &lt;only&gt; one") != std::string::npos);
    CHECK(block.find(root + "/one/SKILL.md") != std::string::npos);
    CHECK(block.find("<name>two</name>") == std::string::npos);
    CHECK(block.find("read tool") != std::string::npos);

    SkillsResult none;
    CHECK(formatSkillsForPrompt(none.skills).empty());
    std::vector<Skill> onlyDisabled;
    for (const auto& s : r.skills)
        if (s.disableModelInvocation)
            onlyDisabled.push_back(s);
    CHECK(formatSkillsForPrompt(onlyDisabled).empty());

    // The system prompt carries the block (and drops it without file tools).
    const std::string sys = buildSystemPrompt("/tmp", true, &r.skills);
    CHECK(sys.find("<available_skills>") != std::string::npos);
    CHECK(buildSystemPrompt("/tmp", false, &r.skills).find("<available_skills>") == std::string::npos);
    CHECK(buildSystemPrompt("/tmp").find("<available_skills>") == std::string::npos);

    // The format contract is stated even when nothing was discovered, so a
    // project that has no skills yet can be turned into skills. Without file
    // tools both the contract and the block are gone.
    const std::string bare = buildSystemPrompt("/tmp");
    CHECK(bare.find("A skill is a directory holding a SKILL.md") != std::string::npos);
    CHECK(bare.find(".miniagent/skills") != std::string::npos);
    CHECK(bare.find("<available_skills>") == std::string::npos);
    CHECK(buildSystemPrompt("/tmp", false).find("A skill is a directory holding a SKILL.md") ==
          std::string::npos);
}

static void testExpansion() {
    printf("expansion\n");
    const std::string root = "/tmp/miniagent_skills_test/expand";
    std::filesystem::remove_all(root);
    writeFile(root + "/verify-ui/SKILL.md",
              "---\nname: verify-ui\ndescription: Verify the UI from screenshots\n---\n"
              "Run cmake --build build.\n");
    SkillsResult r;
    loadSkillsFromDir(root, r);
    CHECK(r.skills.size() == 1);

    const std::string out = expandSkillCommand(r.skills, "/skill:verify-ui check the panel layout");
    CHECK(out.find("<skill name=\"verify-ui\"") != std::string::npos);
    CHECK(out.find(root + "/verify-ui/SKILL.md") != std::string::npos);
    CHECK(out.find("References are relative to " + root + "/verify-ui.") != std::string::npos);
    CHECK(out.find("Run cmake --build build.") != std::string::npos);
    CHECK(out.find("</skill>\n\ncheck the panel layout") != std::string::npos);

    // No args: the block alone.
    const std::string bare = expandSkillCommand(r.skills, "/skill:verify-ui");
    CHECK(bare.find("<skill name=\"verify-ui\"") != std::string::npos);
    CHECK(bare.find("check the panel layout") == std::string::npos);

    // Unknown name and non-commands expand to nothing (caller reports it).
    CHECK(expandSkillCommand(r.skills, "/skill:nope args").empty());
    CHECK(expandSkillCommand(r.skills, "/skills").empty());
    CHECK(expandSkillCommand(r.skills, "plain prompt").empty());
    CHECK(expandSkillCommand(r.skills, "/skill:").empty());
}

static void testRoleBlock() {
    printf("role block\n");
    const std::string root = "/tmp/miniagent_skills_test/roles";
    std::filesystem::remove_all(root);
    writeFile(root + "/.miniagent/skills/worker-task/SKILL.md",
              "---\nname: worker-task\ndescription: How a role does the thing\n---\nSteps.\n");
    writeFile(root + "/.miniagent/skills/parent-only/SKILL.md",
              "---\nname: parent-only\ndescription: explicit only\ndisable-model-invocation: true\n---\n"
              "Steps.\n");

    // Fresh-context roles discover the project skills themselves and get the
    // advertisement (never the authoring contract).
    const std::string block = skillsBlockForCwd(root);
    CHECK(block.find("<name>worker-task</name>") != std::string::npos);
    CHECK(block.find("<name>parent-only</name>") == std::string::npos);
    CHECK(block.find("A skill is a directory holding a SKILL.md") == std::string::npos);
    CHECK(skillsBlockForCwd(root + "/empty-project").empty());
}

int main() {
    // An empty HOME keeps real ~/.config/miniagent/skills and SYSTEM.md out of
    // the assertions (both paths resolve HOME at call time).
    std::filesystem::create_directories("/tmp/miniagent_skills_test/home");
    setenv("HOME", "/tmp/miniagent_skills_test/home", 1);

    testFrontmatter();
    testDiscovery();
    testPrecedence();
    testPromptBlock();
    testRoleBlock();
    testExpansion();
    if (failures) {
        printf("skills_test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("skills_test: passed\n");
    return 0;
}
