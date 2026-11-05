#include <cassert>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include <filesystem>

#include "agent/compaction.h"
#include "sessions/sessions.h"

namespace fs = std::filesystem;

int main() {
    const std::string base = "/tmp/miniagent_sessions_test";
    const std::string root = base + "/sessions";
    system("rm -rf /tmp/miniagent_sessions_test && mkdir -p /tmp/miniagent_sessions_test");

    const std::string cwd1 = "/tmp/miniagent_sessions_test/proj with space";
    const std::string cwd2 = "/tmp/miniagent_sessions_test/other";

    // startSession + append + load round-trip for every message kind.
    {
        SessionStore store(cwd1, root);
        std::string path = store.startSession("first prompt");
        assert(!path.empty());
        assert(fs::exists(path));
        assert(store.currentPath() == path);

        assert(store.appendMessage(UserMessage{"say hi", 1}));
        AssistantMessage a;
        a.text        = "hello";
        a.stopReason  = "stop";
        a.usage.totalTokens = 42;
        a.timestamp   = 2;
        ToolCallBlock tc;
        tc.id        = "tc1";
        tc.name      = "bash";
        tc.arguments = {{"command", "ls"}};
        a.toolCalls.push_back(tc);
        assert(store.appendMessage(a));
        assert(store.appendMessage(ToolResultMessage{"tc1", "bash", "ok", false, 3}));

        auto msgs = SessionStore::loadMessages(path);
        assert(msgs.size() == 3);
        auto& u = std::get<UserMessage>(msgs[0]);
        assert(u.text == "say hi" && u.timestamp == 1);
        auto& a2 = std::get<AssistantMessage>(msgs[1]);
        assert(a2.text == "hello" && a2.stopReason == "stop" && a2.usage.totalTokens == 42);
        assert(a2.toolCalls.size() == 1 && a2.toolCalls[0].name == "bash" &&
               a2.toolCalls[0].arguments.at("command") == "ls");
        auto& t = std::get<ToolResultMessage>(msgs[2]);
        assert(t.toolCallId == "tc1" && t.content == "ok" && !t.isError);
    }

    // listSessions: one entry, correct metadata, newest first.
    {
        SessionStore store(cwd1, root);
        auto list = store.listSessions();
        assert(list.size() == 1);
        assert(list[0].cwd == cwd1);
        assert(list[0].title == "first prompt");
        assert(list[0].messageCount == 3);
        assert(list[0].createdMs > 0);

        std::string second = store.startSession("second prompt");
        assert(!second.empty());
        list = store.listSessions();
        assert(list.size() == 2);
        assert(list[0].title == "second prompt"); // newest first
        assert(list[0].path == second);
        assert(list[1].title == "first prompt");
    }

    // Different cwds land in different directories.
    {
        SessionStore store2(cwd2, root);
        assert(store2.listSessions().empty());
        std::string p = store2.startSession("other project");
        assert(!p.empty());
        assert(store2.listSessions().size() == 1);
        // cwd1's store does not see cwd2's session.
        SessionStore store1(cwd1, root);
        assert(store1.listSessions().size() == 2);
    }

    // A truncated trailing line (crash mid-write) is skipped on load.
    {
        SessionStore store(cwd1, root);
        std::string p = store.startSession("crash test");
        store.appendMessage(UserMessage{"full", 1});
        FILE* f = fopen(p.c_str(), "a");
        assert(f);
        fputs("{\"type\":\"assistant\",\"text\":\"cut off\"", f); // no closing brace, no newline
        fclose(f);
        auto msgs = SessionStore::loadMessages(p);
        assert(msgs.size() == 1);
        assert(std::get<UserMessage>(msgs[0]).text == "full");
    }

    // closeSession stops appends.
    {
        SessionStore store(cwd1, root);
        std::string p = store.startSession("closed");
        assert(store.appendMessage(UserMessage{"x", 1}));
        store.closeSession();
        assert(!store.appendMessage(UserMessage{"y", 2}));
        assert(SessionStore::loadMessages(p).size() == 1);
    }

    // deleteSession removes the file, clears the open session, and refuses
    // paths outside the store's directory.
    {
        SessionStore store(cwd1, root);
        std::string p = store.startSession("doomed");
        assert(!store.currentPath().empty());
        assert(store.deleteSession(p));
        assert(!std::filesystem::exists(p));
        assert(!store.hasOpenSession());
        assert(store.deleteSession("/etc/hosts") == false);
    }

    // truncateMessages (rewind): keeps the header and the first N messages.
    {
        SessionStore store(cwd1, root);
        std::string p = store.startSession("rewind");
        store.appendMessage(UserMessage{"one", 1});
        store.appendMessage(UserMessage{"two", 2});
        store.appendMessage(UserMessage{"three", 3});
        assert(store.truncateMessages(9)); // covering keepCount is a no-op
        assert(SessionStore::loadMessages(p).size() == 3);
        assert(store.truncateMessages(2));
        auto msgs = SessionStore::loadMessages(p);
        assert(msgs.size() == 2);
        assert(std::get<UserMessage>(msgs[0]).text == "one");
        assert(std::get<UserMessage>(msgs[1]).text == "two");
        assert(store.truncateMessages(0));
        assert(SessionStore::loadMessages(p).size() == 0);
        // Header survived; the session can still be appended to.
        assert(SessionStore::loadInfo(p).title == "rewind");
        assert(store.appendMessage(UserMessage{"four", 4}));
        assert(SessionStore::loadMessages(p).size() == 1);
    }

    // Compaction entries: append + projection round-trip.
    {
        SessionStore store(cwd1, root);
        std::string p = store.startSession("compact me");
        for (int i = 1; i <= 6; ++i)
            store.appendMessage(UserMessage{"m" + std::to_string(i), (int64_t)i});

        // No compaction yet: projection is the raw history.
        ProjectedSession proj = SessionStore::loadProjected(p);
        assert(proj.messages.size() == 6);
        assert(!proj.compacted);
        assert(proj.fileBase == 1);
        assert(proj.entryCount == 6);

        // Summarize entries 1..3 (keep entries 4..6).
        CompactionRecord rec;
        rec.keepFrom     = 4;
        rec.summary      = "[Session Goal]\n- did things 1-3";
        rec.tokensBefore = 100;
        rec.tokensAfter  = 20;
        assert(store.appendCompaction(rec));

        proj = SessionStore::loadProjected(p);
        assert(proj.compacted);
        assert(proj.entryCount == 7);  // 6 messages + 1 compaction line
        assert(proj.messages.size() == 4);  // summary + 3 live
        const auto& head = std::get<UserMessage>(proj.messages[0]);
        assert(head.text.rfind(std::string(kCompactionSummaryPrefix()) + "\n", 0) == 0);
        assert(head.text.find("did things 1-3") != std::string::npos);
        assert(std::get<UserMessage>(proj.messages[1]).text == "m4");
        assert(std::get<UserMessage>(proj.messages[3]).text == "m6");
        assert(proj.fileBase == 4);

        // Messages appended after the compaction join the live tail.
        assert(store.appendMessage(UserMessage{"m7", 7}));
        proj = SessionStore::loadProjected(p);
        assert(proj.messages.size() == 5);
        assert(std::get<UserMessage>(proj.messages.back()).text == "m7");
        assert(proj.fileBase == 4);

        // A second compaction replaces the view: its keepFrom counts from
        // the file start (entries up to 8 summarized, m8 at entry 9 kept).
        assert(store.appendMessage(UserMessage{"m8", 8}));
        CompactionRecord rec2;
        rec2.keepFrom = 9;
        rec2.summary  = "[Session Goal]\n- second summary";
        assert(store.appendCompaction(rec2));
        proj = SessionStore::loadProjected(p);
        assert(proj.messages.size() == 2);  // new summary + m8
        assert(std::get<UserMessage>(proj.messages[1]).text == "m8");
        assert(std::get<UserMessage>(proj.messages[0]).text.find("second summary") != std::string::npos);
        assert(proj.fileBase == 9);

        // loadMessages still sees the full raw history (compaction lines
        // skipped).
        assert(SessionStore::loadMessages(p).size() == 8);

        // Rewind past the compactions: compaction entries are dropped and
        // the raw messages survive.
        assert(store.truncateMessages(2));
        auto msgs = SessionStore::loadMessages(p);
        assert(msgs.size() == 2);
        assert(std::get<UserMessage>(msgs[0]).text == "m1");
        assert(std::get<UserMessage>(msgs[1]).text == "m2");
        proj = SessionStore::loadProjected(p);
        assert(!proj.compacted && proj.messages.size() == 2);

        // truncateToEntries: exact line-based trim (compaction lines count).
        assert(store.appendCompaction(rec));       // now line 4
        assert(store.appendMessage(UserMessage{"m9", 9}));  // line 5
        assert(store.truncateToEntries(3));
        msgs = SessionStore::loadMessages(p);
        assert(msgs.size() == 2);  // header + m1 + m2 + compaction kept... m9 dropped
        // (compaction at line 4 kept, message m9 dropped)
        FILE* f = fopen(p.c_str(), "r");
        int lineCount = 0;
        char buf[4096];
        while (fgets(buf, sizeof(buf), f)) ++lineCount;
        fclose(f);
        assert(lineCount == 4);  // header + m1 + m2 + compaction
    }

    printf("sessions_test: OK\n");
    return 0;
}
