// Pure unit tests for the agent definition parser and discovery.
// No network: parseAgentContent, frontmatter forms, .pi/ledger substitution,
// and project-dir discovery/override.

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "agent/agents.h"
#include "agent/notes_limit.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL: %s (line %d)\n", #cond, __LINE__);               \
            ++failures;                                                      \
        }                                                                    \
    } while (0)

static void testFrontmatterForms() {
    printf("frontmatter forms\n");
    AgentDef def;

    // tools as a comma string
    std::string s = "---\nname: alpha\ndescription: does alpha things\n"
                    "tools: read, bash\n---\nBody text.\n";
    CHECK(parseAgentContent("alpha", s, def));
    CHECK(def.name == "alpha");
    CHECK(def.description == "does alpha things");
    CHECK(def.tools.size() == 2);
    CHECK(def.tools[0] == "read" && def.tools[1] == "bash");
    CHECK(def.systemPrompt.find("Body text.") != std::string::npos);

    // tools as a YAML list
    s = "---\nname: beta\ndescription: list tools\ntools:\n  - read\n"
        "  - write\n---\nBeta body.\n";
    CHECK(parseAgentContent("beta", s, def));
    CHECK(def.tools.size() == 2);
    CHECK(def.tools[0] == "read" && def.tools[1] == "write");

    // no tools key => empty (all tools allowed)
    s = "---\nname: gamma\ndescription: no tools\n---\nGamma body.\n";
    CHECK(parseAgentContent("gamma", s, def));
    CHECK(def.tools.empty());
    CHECK(def.systemPrompt.find("Gamma body.") != std::string::npos);

    // unknown keys are ignored
    s = "---\nname: delta\ndescription: x\nmodel: local\ntemperature: 0.2\n"
        "---\nDelta body.\n";
    CHECK(parseAgentContent("delta", s, def));
    CHECK(def.tools.empty());
}

static void testRejects() {
    printf("rejects\n");
    AgentDef def;
    CHECK(!parseAgentContent("x", "no frontmatter at all", def));
    CHECK(!parseAgentContent("x", "---\nname: x\ndescription: y\nno closing delimiter", def));
    CHECK(!parseAgentContent("x", "---\ndescription: missing name\n---\nbody", def));
    CHECK(!parseAgentContent("x", "---\nname: x\nno description\n---\nbody", def));
}

static void testLedgerSubstitution() {
    printf("ledger substitution\n");
    AgentDef def;
    std::string s = "---\nname: pi\ndescription: pi paths\ntools: read\n---\n"
                    "Read .pi/ledger/task.md then .pi/ledger/notes.md.\n";
    CHECK(parseAgentContent("pi", s, def));
    CHECK(def.systemPrompt.find(".pi/ledger") == std::string::npos);
    CHECK(def.systemPrompt.find(".miniagent/ledger/task.md") != std::string::npos);
    CHECK(def.systemPrompt.find(".miniagent/ledger/notes.md") != std::string::npos);
}

static void testDiscovery() {
    printf("discovery\n");
    const std::string dir = "/tmp/miniagent_agents_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir + "/proj/.miniagent/agents");

    // Project-local agent + an override of a built-in.
    {
        std::ofstream(dir + "/proj/.miniagent/agents/local.md")
            << "---\nname: local\ndescription: project only\ntools: read\n---\nLocal body.\n";
        std::ofstream(dir + "/proj/.miniagent/agents/worker.md")
            << "---\nname: worker\ndescription: project override\n---\nOverride body.\n";
        // A malformed file in the same dir must be skipped, not fatal.
        std::ofstream(dir + "/proj/.miniagent/agents/bad.md")
            << "---\nno name here\n---\nwhatever\n";
    }

    auto agents = discoverAgents(dir + "/proj");
    auto find = [&agents](const std::string& name) -> const AgentDef* {
        for (const auto& a : agents)
            if (a.name == name)
                return &a;
        return nullptr;
    };

    const AgentDef* worker   = find("worker");
    const AgentDef* local    = find("local");
    const AgentDef* brain    = find("brainstorm");
    CHECK(worker);
    CHECK(local);
    CHECK(brain);
    // Project worker overrides the built-in by name, built-in body replaced.
    CHECK(worker && worker->description == "project override");
    CHECK(worker && worker->path.find("proj/.miniagent/agents") != std::string::npos);
    // Built-ins carry the right tool restrictions.
    CHECK(brain && brain->tools.size() == 6 && brain->tools[0] == "read" &&
          brain->tools[1] == "grep" &&
          brain->tools[3] == "edit" &&
          brain->tools[5] == "mcp_codegraph_codegraph_explore");
    CHECK(local && local->tools.size() == 1 && local->tools[0] == "read");
    // Only the three roles are agents; the scaffold prompt is not one.
    CHECK(!find("scaffold"));

    // No project dir: built-ins still present.
    auto plain = discoverAgents("/tmp/definitely-missing-dir");
    auto findPlain = [&plain](const std::string& name) {
        for (const auto& a : plain)
            if (a.name == name)
                return true;
        return false;
    };
    CHECK(findPlain("brainstorm") && findPlain("worker"));
}

static void testNotesLimitSubstitution() {
    printf("notes limit substitution\n");
    AgentDef def;
    std::string s = "---\nname: r\ndescription: limit\n---\n"
                    "Keep notes.md under {{NOTES_LIMIT}} characters.\n";
    CHECK(parseAgentContent("r", s, def));
    CHECK(def.systemPrompt.find("{{NOTES_LIMIT}}") == std::string::npos);
    CHECK(def.systemPrompt.find(notesCharLimitText() + " characters") != std::string::npos);
    CHECK(notesCharLimitText() == "15,000");
}

static void testBuiltinNotesLimit() {
    printf("built-in agents use the notes limit\n");
    const std::string limit = notesCharLimitText();
    for (const auto& a : discoverAgents("")) {
        CHECK(a.systemPrompt.find("{{NOTES_LIMIT}}") == std::string::npos);
        CHECK(a.systemPrompt.find(limit + " characters") != std::string::npos);
        CHECK(a.systemPrompt.find("8,000 characters") == std::string::npos);
    }
}

int main() {
    testFrontmatterForms();
    testRejects();
    testLedgerSubstitution();
    testNotesLimitSubstitution();
    testBuiltinNotesLimit();
    testDiscovery();
    if (failures) {
        printf("agents_test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("agents_test: passed\n");
    return 0;
}
