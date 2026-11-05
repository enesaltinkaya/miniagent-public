#pragma once

#include <string>
#include <variant>
#include <vector>

#include "agent/agent.h"
#include "agent/types.h"
#include "sessions/sessions.h"

// The conversation a new run continues from.
struct HistorySeed {
    std::vector<AgentMessage> messages;
    size_t fileBase = 0;
    bool sessionIndexed = true;
    size_t entryCount = 0;
    bool poppedTrailing = false;
    bool fromLiveAgent = false;
};

// Chooses the seed for the next run of the open session: the previous run's
// in-memory messages when they belong to that session, otherwise the session
// file's projected view. The session file never persisted image attachments,
// so reloading from disk resends earlier turns without their image parts -
// the serialized history changes shape mid-conversation and the backend's
// prompt cache misses from the first image message on. The in-memory path
// keeps the wire format byte-identical to what the previous run sent.
//
// `liveHistoryPath` is the session file the previous agent's messages belong
// to (empty / mismatched = the file is the source of truth, e.g. after an
// app restart, a session switch, a rewind or a scaffold run). When
// `stripToolImages` is set (an image model is designated, so the main backend
// rejects image parts), image attachments are removed from tool results in
// either path.
//
// A trailing assistant message whose tool calls never got results (an aborted
// run) is dropped and reported via poppedTrailing; the caller must also drop
// it from the session file (truncateToEntries(entryCount - 1)) so the
// file-entry indices the compactor emits stay exact.
inline HistorySeed selectHistorySeed(const Agent* prevAgent,
                                     const std::string& liveHistoryPath,
                                     const SessionStore* sessions,
                                     bool stripToolImages) {
    HistorySeed seed;
    if (prevAgent && sessions->hasOpenSession() && liveHistoryPath == sessions->currentPath()) {
        seed.messages       = prevAgent->history();
        seed.fileBase       = prevAgent->historyFileBase();
        seed.sessionIndexed = prevAgent->historySessionIndexed();
        seed.entryCount     = sessions->entryCount();
        seed.fromLiveAgent  = true;
    } else if (sessions->hasOpenSession()) {
        ProjectedSession proj = SessionStore::loadProjected(sessions->currentPath());
        seed.messages   = std::move(proj.messages);
        seed.fileBase   = proj.fileBase;
        seed.entryCount = proj.entryCount;
    } else {
        return seed;
    }
    if (!seed.messages.empty() && std::holds_alternative<AssistantMessage>(seed.messages.back())) {
        const auto& a = std::get<AssistantMessage>(seed.messages.back());
        if (!a.toolCalls.empty() ||
            (a.stopReason == "error" && a.text.empty() && a.toolCalls.empty())) {
            seed.messages.pop_back();
            seed.poppedTrailing = true;
        }
    }
    if (stripToolImages)
        for (auto& m : seed.messages)
            if (auto* t = std::get_if<ToolResultMessage>(&m))
                t->image.reset();
    return seed;
}
