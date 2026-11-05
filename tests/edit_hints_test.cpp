// Unit tests for agent/edit_hints.cpp: the stale-file note appended to failed
// edit results when the conversation modified the file after the model's last
// read of it (the "reused pre-edit oldText" failure mode seen in session logs).
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/edit_hints.h"
#include "agent/types.h"

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

namespace {

ToolResultMessage trm(const std::string& id, const std::string& tool,
                      const nlohmann::json& args, bool isError = false) {
    ToolResultMessage m;
    m.toolCallId = id;
    m.toolName = tool;
    m.content = isError ? "Edit 1: oldText not found in file" : "ok";
    m.isError = isError;
    m.args = args.dump();
    m.timestamp = 0;
    return m;
}

const std::string kPath = "src/app.cpp";
const nlohmann::json kReadArgs = {{"path", kPath}};
const nlohmann::json kEditArgs = {{"path", kPath}};
const nlohmann::json kWriteArgs = {{"path", kPath}};
const nlohmann::json kOtherArgs = {{"path", "src/other.cpp"}};
const nlohmann::json kAbsArgs = {{"path", "/proj/src/app.cpp"}};

} // namespace

int main() {
    { // Empty history: nothing to say.
        std::vector<AgentMessage> msgs;
        check(staleFileNote(msgs, kPath).empty(), "empty history: no note");
    }
    { // Read after the last modification: file state is fresh, no note.
        std::vector<AgentMessage> msgs = {
            trm("1", "edit", kEditArgs),
            trm("2", "read", kReadArgs),
        };
        check(staleFileNote(msgs, kPath).empty(), "read after edit: no note");
    }
    { // Successful edit after the last read: note, names the tool, suggests re-read.
        std::vector<AgentMessage> msgs = {
            trm("1", "read", kReadArgs),
            trm("2", "edit", kEditArgs),
        };
        std::string note = staleFileNote(msgs, kPath);
        check(!note.empty(), "edit after read: note present");
        check(note.find("edit") != std::string::npos, "note names the modifying tool");
        check(note.find("Re-read") != std::string::npos, "note suggests re-reading");
    }
    { // Successful write after the last read: note names write.
        std::vector<AgentMessage> msgs = {
            trm("1", "read", kReadArgs),
            trm("2", "write", kWriteArgs),
        };
        std::string note = staleFileNote(msgs, kPath);
        check(!note.empty() && note.find("write") != std::string::npos,
              "write after read: note names write");
    }
    { // A FAILED edit does not modify the file: no note.
        std::vector<AgentMessage> msgs = {
            trm("1", "read", kReadArgs),
            trm("2", "edit", kEditArgs, /*isError=*/true),
        };
        check(staleFileNote(msgs, kPath).empty(), "failed edit after read: no note");
    }
    { // Modification of a different path: no note.
        std::vector<AgentMessage> msgs = {
            trm("1", "read", kReadArgs),
            trm("2", "edit", kOtherArgs),
        };
        check(staleFileNote(msgs, kPath).empty(), "other path: no note");
    }
    { // Relative vs absolute path of the same file (same cwd): still stale.
        std::vector<AgentMessage> msgs = {
            trm("1", "read", kReadArgs),
            trm("2", "edit", kAbsArgs),
        };
        check(!staleFileNote(msgs, kPath).empty(), "relative/absolute same cwd: note");
    }
    { // Non-tool messages in between are ignored.
        std::vector<AgentMessage> msgs = {
            UserMessage{"please edit src/app.cpp", 0},
            AssistantMessage{},
            trm("1", "read", kReadArgs),
            UserMessage{"keep going", 0},
            trm("2", "edit", kEditArgs),
        };
        check(!staleFileNote(msgs, kPath).empty(), "mixed messages: note");
    }
    { // Only the LAST modification vs read ordering matters:
        // read -> edit -> read -> edit  =>  stale again.
        std::vector<AgentMessage> msgs = {
            trm("1", "read", kReadArgs),
            trm("2", "edit", kEditArgs),
            trm("3", "read", kReadArgs),
            trm("4", "edit", kEditArgs),
        };
        check(!staleFileNote(msgs, kPath).empty(), "read-edit-read-edit: note");
        std::vector<AgentMessage> msgs2 = {
            trm("1", "read", kReadArgs),
            trm("2", "edit", kEditArgs),
            trm("3", "read", kReadArgs),
        };
        check(staleFileNote(msgs2, kPath).empty(), "read-edit-read: no note");
    }
    { // Malformed/missing args must not crash or misfire.
        std::vector<AgentMessage> msgs = {
            trm("1", "read", kReadArgs),
            trm("2", "edit", nlohmann::json::object()),
        };
        check(staleFileNote(msgs, kPath).empty(), "edit without path arg: no note");
        ToolResultMessage bad;
        bad.toolCallId = "3";
        bad.toolName = "edit";
        bad.args = "{not json";
        std::vector<AgentMessage> msgs2 = {
            trm("1", "read", kReadArgs),
            bad,
        };
        check(staleFileNote(msgs2, kPath).empty(), "malformed args: no note, no crash");
    }

    if (failures) {
        std::printf("edit_hints_test: %d failure(s)\n", failures);
        return 1;
    }
    std::printf("edit_hints_test: passed\n");
    return 0;
}
