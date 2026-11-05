#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/types.h"
#include "agent/vcc/brief.h"
#include "agent/vcc/compile.h"
#include "agent/vcc/constants.h"
#include "agent/vcc/normalize.h"
#include "agent/vcc/rank.h"
#include "agent/vcc/sections.h"
#include "agent/vcc/text.h"
#include "agent/vcc/tokens.h"

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static void checkContains(const std::string& haystack, const std::string& needle, const char* what) {
    if (haystack.find(needle) == std::string::npos) {
        std::printf("FAIL: %s\n  expected to contain: %s\n  got: %.400s\n", what, needle.c_str(),
                    haystack.c_str());
        ++failures;
    }
}

static void checkNotContains(const std::string& haystack, const std::string& needle, const char* what) {
    if (haystack.find(needle) != std::string::npos) {
        std::printf("FAIL: %s\n  expected NOT to contain: %s\n", what, needle.c_str());
        ++failures;
    }
}

namespace {

using namespace vcc;

UserMessage um(const std::string& text) {
    UserMessage m;
    m.text = text;
    m.timestamp = 1;
    return m;
}

AssistantMessage am(const std::string& text) {
    AssistantMessage m;
    m.text = text;
    m.timestamp = 2;
    return m;
}

AssistantMessage toolCall(const std::string& name, const nlohmann::json& args) {
    AssistantMessage m;
    m.timestamp = 2;
    ToolCallBlock tc;
    tc.id = "c1";
    tc.name = name;
    tc.arguments = args;
    m.toolCalls.push_back(tc);
    return m;
}

ToolResultMessage trm(const std::string& name, const std::string& args, const std::string& content,
                      bool isError = false) {
    ToolResultMessage m;
    m.toolCallId = "c1";
    m.toolName = name;
    m.content = content;
    m.isError = isError;
    m.args = args;
    m.timestamp = 3;
    return m;
}

std::string bashResult(const std::string& command, const std::string& output, int exitCode) {
    nlohmann::json args = {{"command", command}};
    std::string content = output + "\n[exit code: " + std::to_string(exitCode) + "]";
    ToolResultMessage m = trm("bash", args.dump(), content, exitCode != 0);
    return content;
}

std::vector<std::optional<int>> idx(size_t n) {
    return std::vector<std::optional<int>>(n, std::nullopt);
}

}  // namespace

int main() {
    {
        check(clip("hello world foo", 8) == "hello", "clip at word boundary");
        check(clip("helloworld", 5) == "hello", "clip hard cut");
        check(clipSentence("One. Two eggs. Three.", 15) == "One. Two eggs.", "clipSentence boundary");
        check(truncateWords("alpha beta gamma delta", 2) == "alpha beta...(truncated)", "truncateWords counts");
        check(truncateWords("the a an of to alpha", 1) == "the a an of to alpha",
              "stop words do not count toward budget");
        check(truncateWords("the cat sat on the mat and ran far away", 3) == "the cat sat on the mat and...(truncated)",
              "stop words free");
        std::string ht = truncateHeadTail("w1 w2 w3 w4 w5 w6 w7 w8 w9 w10", 2, 2);
        check(ht.find("...(middle truncated)...") != std::string::npos, "head/tail marker");
        check(ht.find("w1") != std::string::npos && ht.find("w10") != std::string::npos, "head/tail keeps ends");
        std::string wrapped = wrapLongLines(std::string(300, 'x'), 120);
        check(wrapped.find('\n') != std::string::npos, "wrapLongLines splits");
    }
    {
        std::string cmd = compressBash("set -euo pipefail\ncd /tmp && ls -la");
        check(cmd == "ls -la", "compressBash drops preamble, strips cd");
        check(compressBash("cat > /tmp/f <<EOF\nline1\nline2\nEOF\n") == "cat > /tmp/f <<EOF",
              "filewriter heredoc keeps opener only");
        std::string py = compressBash("python3 - <<'PY'\nimport os\nprint(os.getcwd())\nPY");
        check(py.find("python3 - <<'PY'") == 0, "non-file heredoc keeps opener");
        check(py.find("print(os.getcwd())") != std::string::npos, "heredoc body preview kept");
        check(py.find("import os") == std::string::npos, "heredoc body noise dropped");
        check(compressBash("git log --oneline | head -5") == "git log --oneline", "head pipe tail stripped");
        check(compressBash("git log --oneline | grep foo") == "git log --oneline | grep foo",
              "unknown pipe tail kept");
        check(compressBash("cat foo.txt | head -5") == "cat foo.txt", "head pipe tail stripped");
        std::string long2 = compressBash(std::string(400, 'y'));
        check(long2.size() <= 240 && long2.substr(long2.size() - 3) == "...", "bash cap");
        check(compressBash("8 << 20") == "8 << 20", "shift not heredoc");
    }
    {
        std::vector<AgentMessage> msgs;
        msgs.push_back(um("read the config"));
        msgs.push_back(toolCall("read", {{"path", "config.yaml"}}));
        msgs.push_back(trm("read", R"({"path":"config.yaml"})", "key: value"));
        AssistantMessage bash = toolCall("bash", {{"command", "make test"}});
        msgs.push_back(bash);
        std::string content = bashResult("make test", "ok", 2);
        ToolResultMessage br = trm("bash", R"({"command":"make test"})", content, true);
        msgs.push_back(br);

        std::vector<Block> blocks = filterNoise(normalize(msgs, idx(msgs.size())));
        check(blocks.size() == 5, "normalize block count");
        check(kindOf(blocks[1]) == Kind::Call, "read call block");
        check(kindOf(blocks[4]) == Kind::Bash, "bash result becomes bash block");
        const auto& sh = std::get<BashBlock>(blocks[4]);
        check(sh.command == "make test", "bash command from args");
        check(sh.hasExitCode && sh.exitCode == 2, "exit code parsed");
    }
    {
        check(isTestCommand("cargo test --all"), "cargo test");
        check(isTestCommand("npm run build"), "npm build");
        check(!isTestCommand("cat testing.md"), "not test");
        check(isTestCommand("pytest -q tests/test_app.py"), "pytest");
        check(!isTestCommand("pytest -q"), "bare runner without target word");
        check(isWorkflowCommand("git commit -m \"x\""), "git commit workflow");
        check(isWorkflowCommand("gh pr view 12"), "gh pr workflow");
        check(!isWorkflowCommand("github commit"), "github not workflow");
        check(isTrivialOnlyBash("set -e\ncd /tmp\nls"), "trivial bash");
        check(!isTrivialOnlyBash("make build"), "not trivial");
    }
    {
        std::vector<Block> blocks;
        blocks.push_back(UserBlock{"edit the file", 1});
        blocks.push_back(CallBlock{"edit", nlohmann::json{{"path", "a.cpp"}}, 2});
        blocks.push_back(ResultBlock{"edit", "ok", 3});
        blocks.push_back(BashBlock{"npm test", "", 0, true, 4});
        blocks.push_back(AssistantBlock{"Done, all good here.", 5});

        RankOptions opts;
        opts.maxBriefChars = 100000;
        opts.maxBlocks = 80;
        opts.preserveRecentBlocks = 16;
        std::vector<Block> sel = selectBriefBlocks(blocks, opts);
        check(sel.size() == 4, "selection drops only tool result");

        opts.maxBriefChars = 0;
        RankOptions tight;
        tight.maxBriefChars = 1;
        tight.preserveRecentBlocks = 1;
        std::vector<Block> few = selectBriefBlocks(blocks, tight);
        check(few.size() < blocks.size(), "tight budget selects fewer");
    }
    {
        std::vector<Block> blocks;
        blocks.push_back(UserBlock{"fix the login bug in auth.cpp (#12)", 1});
        blocks.push_back(AssistantBlock{"I will fix it now.", 2});
        blocks.push_back(CallBlock{"edit", nlohmann::json{{"path", "src/auth.cpp"}}, 3});
        blocks.push_back(BashBlock{"git commit -m \"fix auth\"", "", 0, false, 4});

        std::vector<std::string> goals = extractGoals(blocks);
        check(goals.size() == 1 && goals[0].find("fix the login bug") == 0, "goal from first user");

        std::vector<std::string> commits = extractCommits(blocks);
        check(commits.size() == 1 && commits[0] == "fix auth", "commit message parsed");

        FileActivity act = extractFiles(blocks, {});
        check(act.modified.count("src/auth.cpp") == 1, "edit marks modified");

        SectionData data = buildSections(blocks, blocks, {});
        check(!data.filesAndChanges.empty() && data.filesAndChanges[0].find("Modified: src/auth.cpp") == 0,
              "file activity line");
    }
    {
        std::vector<Block> blocks;
        blocks.push_back(UserBlock{"Fix the crash in worker pool", 1});
        blocks.push_back(AssistantBlock{"The build failed because the header is missing. I need to add it.", 2});
        std::vector<std::string> out = extractOutstandingContext(blocks);
        check(out.size() == 2, "outstanding finds blocker");
        bool found = false;
        for (const auto& o : out)
            if (o.find("build failed") != std::string::npos)
                found = true;
        check(found, "outstanding content");
    }
    {
        std::vector<Block> blocks;
        blocks.push_back(UserBlock{"always use tabs in this project", 1});
        blocks.push_back(UserBlock{"never push to main", 2});
        std::vector<std::string> prefs = extractPreferences(blocks);
        check(prefs.size() == 2, "preferences count");
    }
    {
        std::vector<AgentMessage> msgs;
        msgs.push_back(um("Implement the vcc compactor in src/agent/vcc/"));
        AssistantMessage a1 = toolCall("read", {{"path", "src/agent/compaction.cpp"}});
        msgs.push_back(a1);
        msgs.push_back(trm("read", R"({"path":"src/agent/compaction.cpp"})", "int main() {}"));
        AssistantMessage a2 = toolCall("edit", {{"path", "src/agent/compaction.cpp"}});
        msgs.push_back(a2);
        msgs.push_back(trm("edit", R"({"path":"src/agent/compaction.cpp"})", "ok"));
        AssistantMessage a3 = toolCall("bash", {{"command", "cmake --build build && ./build/tool_test"}});
        msgs.push_back(a3);
        msgs.push_back(trm("bash", R"({"command":"cmake --build build && ./build/tool_test"})", "ok\n[exit code: 0]"));
        msgs.push_back(am("The compactor is implemented and tests pass."));

        std::vector<std::optional<int>> srcs;
        for (size_t i = 0; i < msgs.size(); ++i)
            srcs.push_back(static_cast<int>(i) + 1);

        CompileInput in;
        in.messages = msgs;
        in.sourceIndices = srcs;
        std::string summary = compileRanked(in);
        check(!summary.empty(), "compileRanked produces output");
        checkContains(summary, "[Session Goal]", "summary has goal section");
        checkContains(summary, "[Files And Changes]", "summary has files section");
        checkContains(summary, "Modified: src/agent/compaction.cpp", "files modified entry");
        checkContains(summary, "(#1)", "user ref present");
        checkContains(summary, "$ cmake --build build && ./build/tool_test", "bash command in brief");
        checkContains(summary, "recall", "recall note present");
        checkNotContains(summary, "tool_result", "tool bodies dropped");

        CompileInput second;
        second.messages = {um("Also add docs"), am("Docs added.")};
        second.previousSummary = summary;
        second.sourceIndices = idx(2);
        std::string merged = compileRanked(second);
        checkContains(merged, "[Session Goal]", "merged keeps goal");
        checkContains(merged, "src/agent/compaction.cpp", "merged keeps files across compactions");
        size_t noteCount = 0;
        for (size_t p = merged.find("recall` tool to search"); p != std::string::npos;
             p = merged.find("recall` tool to search", p + 1))
            ++noteCount;
        check(noteCount == 1, "recall note does not stack");
    }
    {
        std::vector<AgentMessage> msgs;
        msgs.push_back(um(std::string(600, 'a') + " fix this"));
        msgs.push_back(am(std::string(4000, 'b')));
        msgs.push_back(um("short"));
        CompileInput in;
        in.messages = msgs;
        in.sourceIndices = idx(msgs.size());
        std::string s = compileRanked(in);
        checkContains(s, "short", "recent user kept");
    }
    {
        TokenCalibration c = calibrateCharsPerToken(4000, 1000);
        check(c.calibrated && c.charsPerToken == 4.0, "calibration exact");
        TokenCalibration clamped = calibrateCharsPerToken(100, 1000);
        check(clamped.calibrated && clamped.charsPerToken == 2.0, "calibration clamps low");
        check(!calibrateCharsPerToken(0, 100).calibrated, "calibration invalid");
        check(estimateTokensFromChars(9, 4.0) == 3, "token estimate ceil");
    }
    {
        std::vector<AgentMessage> msgs;
        UserMessage withImg = um("look");
        withImg.images.push_back(ImageAttachment{});
        msgs.push_back(withImg);
        check(messageContentChars(msgs[0]) >= kImageChars, "image chars counted");
    }

    if (failures == 0)
        std::printf("vcc_test: all ok\n");
    else
        std::printf("vcc_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
