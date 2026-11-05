#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "agent/types.h"

struct SessionInfo {
    std::string path;      // absolute path of the session file
    std::string cwd;
    std::string title;     // first user prompt
    int64_t createdMs = 0;
    int64_t updatedMs = 0; // file mtime
    int messageCount = 0;  // entries after the header line
};

// The conversation state a compaction entry describes (see CompactionRecord in
// agent/types.h): the summary replaces the message entries before `keepFrom`,
// the tail from `keepFrom` on stays live.

// A session replayed through its compaction entries: the messages the LLM
// should continue from (summary + live tail) plus the bookkeeping the agent
// needs to keep emitting stable file-entry refs.
struct ProjectedSession {
    std::vector<AgentMessage> messages;  // [summary?, live tail...]
    size_t fileBase = 0;                 // file entry index of the first live message
    size_t entryCount = 0;               // total non-header lines (messages + compactions)
    bool compacted = false;              // the projection applied a compaction entry
};

// One NDJSON file per conversation, stored under <root>/<encoded-cwd>/.
// The first line is a header ({"type":"header","cwd":...,"title":...}),
// then one message per line; malformed/truncated trailing lines are
// skipped on load.
class SessionStore {
public:
    // root defaults to ~/.config/miniagent/sessions when empty (used by
    // tests to redirect storage).
    explicit SessionStore(std::string cwd, std::string root = {});

    // Creates a new session file for this cwd, makes it the open session,
    // and returns its path (empty string on failure).
    std::string startSession(const std::string& title);

    // Appends one message to the open session. False if nothing is open or
    // the write fails.
    bool appendMessage(const AgentMessage& msg);

    // Appends a compaction entry (see CompactionRecord). The full message
    // history stays in the file; only the projected view changes.
    bool appendCompaction(const CompactionRecord& record);

    // Replays a session file through its compaction entries: the result is
    // what the agent should continue from. loadMessages() instead returns
    // every message line, ignoring compaction entries (UI export, scaffolds).
    static ProjectedSession loadProjected(const std::string& path);

    // Rewind: rewrite the open session file keeping the header and only the
    // first keepCount messages (compaction entries are dropped, so the
    // rewound history is the raw transcript). No-op (true) when keepCount
    // covers the whole conversation. False if nothing is open or the write
    // fails.
    bool truncateMessages(size_t keepCount);

    // Line-based rewind: keep the header plus the first keepEntries non-header
    // lines regardless of type. Used to drop a trailing entry exactly.
    bool truncateToEntries(size_t keepEntries);

    // Total non-header entries (messages + compaction records) in the open
    // session file, counted the same way loadProjected() counts them.
    size_t entryCount() const;

    void closeSession();
    const std::string& currentPath() const { return currentPath_; }

    // True if a session file is currently open (created or loaded).
    bool hasOpenSession() const { return !currentPath_.empty(); }

    // Makes an existing session file the open one (for resuming from the list).
    void openSession(const std::string& path) { currentPath_ = path; }

    // Removes a session file. Refuses paths outside this store's directory;
    // also clears the open session if it was the deleted one.
    bool deleteSession(const std::string& path);

    static std::vector<AgentMessage> loadMessages(const std::string& path);
    static SessionInfo loadInfo(const std::string& path);

    // All sessions for this cwd, newest first.
    std::vector<SessionInfo> listSessions() const;


private:
    std::string cwd_;
    std::string dirPath_;
    std::string currentPath_;
};
