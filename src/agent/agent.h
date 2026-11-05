#pragma once

#include <atomic>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "agent/types.h"
#include "llm/openai.h"
#include "tools/tools.h"

// Runs the agent loop on the calling thread. Emits AgentEvents via
// onEvent. Blocks until the run completes or abort is requested.
class Agent {
public:
    // onMessage (optional) is invoked from the agent thread for every
    // message that is appended to the conversation (user, assistant and
    // tool results), in order. onCompaction (optional) is invoked from the
    // agent thread when a compaction rewrites the conversation, so the
    // session store can persist the compaction entry before any later
    // message is appended.
    Agent(std::string systemPrompt, OpenAIProvider provider, ToolRegistry* tools,
          std::function<void(const AgentEvent&)> onEvent,
          std::function<void(const AgentMessage&)> onMessage = {},
          std::function<void(const CompactionRecord&)> onCompaction = {});

    // Seed the conversation with prior messages (e.g. a loaded session) so
    // the run continues from there instead of starting fresh. Call before
    // run(); run() appends the new user prompt on top of this history.
    // fileBase is the session-file entry index of the first message in
    // `history` (or of the first message after a leading compaction
    // summary); with sessionIndexed = true the compactor emits (#N) refs
    // into that index space (0 / false: no refs, e.g. nested agents).
    void setHistory(std::vector<AgentMessage> history, size_t fileBase = 0, bool sessionIndexed = false) {
        messages       = std::move(history);
        this->fileBase = fileBase;
        this->sessionIndexed = sessionIndexed;
    }

    // Read back the seeded/run conversation so the next run can continue
    // from the live messages instead of a session-file reload (the file
    // drops image attachments, which would change earlier turns on the wire
    // and break backend prompt caching). Only call while no run is in
    // progress.
    const std::vector<AgentMessage>& history() const { return messages; }
    size_t historyFileBase() const { return fileBase; }
    bool historySessionIndexed() const { return sessionIndexed; }

    // Blocking. Must be called from the agent thread.
    void run(UserMessage user);

    // Queue a user message while run() is in progress ("steering"): it is
    // injected into the conversation after the current LLM call finishes,
    // before the next one. A queued message also keeps the run alive: when
    // the model stops making tool calls, queued messages are delivered
    // instead of ending the run. Thread-safe; call from the UI thread.
    void pushQueuedMessage(UserMessage message);

    // Remove and return all queued messages (e.g. to restore to the prompt
    // box when the run is stopped). Thread-safe.
    std::vector<UserMessage> clearQueuedMessages();

    // Number of messages waiting in the queue. Thread-safe.
    size_t pendingMessageCount() const;

    // Queue a manual compaction (the input bar's Compact button): it runs on
    // the agent thread at the next turn boundary -- after the in-flight LLM
    // call and its tool calls settle, before the next one -- after which the
    // loop continues with the compacted conversation. Thread-safe; call
    // from the UI thread.
    void requestCompaction() { compactRequested.store(true); }

    // Share an external manual-compaction request flag (the UI's Compact
    // button during a scaffold run): the live role's agent consumes it at
    // its next turn boundary and compacts its OWN conversation. Unlike the
    // agent's own flag, a shared one is never consumed at the run's natural
    // end - a finished role's context is discarded, so compacting it then
    // would be pointless.
    void shareCompactionRequest(const std::atomic<bool>& flag) { compactFlag_ = &flag; }

    // Compact the conversation this object holds right now (the UI's Compact
    // button while no run is in progress): the same deterministic rewrite the
    // loop performs at a turn boundary, including the onCompaction persist and
    // the Compaction event. False when nothing could be compacted (the note is
    // emitted either way). Call from the UI thread while no run is in progress.
    bool compactNow() { return runCompaction("Manual compaction: ", true); }

    // Called from any thread (e.g. the UI Stop button).
    void abort() { abortFlag().store(true); }

    bool aborted() const { return abortFlag().load(); }

    // Share an external abort flag (a parent agent's): the loop checks it and
    // abort() sets it, so one Stop stops parent and subagent together.
    void shareAbort(const std::atomic<bool>& flag) { abortFlag_ = &flag; }

    // The abort flag this agent checks (its own by default). A shared flag
    // is the parent agent's own (mutable) atomic; the const_cast only drops
    // the pointer's top-level const.
    std::atomic<bool>& abortFlag() {
        return abortFlag_ ? *const_cast<std::atomic<bool>*>(abortFlag_) : ownAbort;
    }
    const std::atomic<bool>& abortFlag() const {
        return abortFlag_ ? *abortFlag_ : ownAbort;
    }

private:
    // Outcome of an LLM call error: Retry/Compacted -> the run continues
    // (the failed message was dropped, not committed); GiveUp -> the run
    // ends with the error.
    enum class ErrorAction { Retry, Compacted, GiveUp };

    enum class LengthStop { Continuing, Exhausted, Empty };

    std::string systemPrompt;
    OpenAIProvider provider;
    ToolRegistry* tools;
    std::function<void(const AgentEvent&)> onEvent;
    std::function<void(const AgentMessage&)> onMessage;
    std::function<void(const CompactionRecord&)> onCompaction;

    // Transient server errors are retried up to kMaxRetries times before the
    // agent stops; the waits before retry attempts 1..kMaxRetries grow (0 =
    // immediate). Consecutive errors within a run accumulate; any successful
    // LLM call resets the streak.
    static constexpr int kMaxRetries = 3;
    static constexpr int kRetryDelaysMs[kMaxRetries] = {0, 2000, 3000};
    static constexpr int kMaxLengthContinuations = 8;

    std::vector<AgentMessage> messages;
    std::atomic<bool> ownAbort{false};
    const std::atomic<bool>* abortFlag_ = nullptr;
    int turnIndex = 0;
    int errorStreak = 0;             // consecutive failed LLM calls
    int lengthStreak = 0;
    bool overflowRecoveryUsed = false;  // compacted + re-issued this run
    // Real input tokens of the last successful LLM call (server count);
    // compaction uses it to calibrate the chars/token estimate.
    int lastInputTokens = 0;
    // Session-file entry index of the first non-summary message (see
    // setHistory); maintained across compactions so summary (#N) refs stay
    // stable. 0 = not session-indexed.
    size_t fileBase = 0;
    bool sessionIndexed = false;

    // Steering queue: user messages typed while the agent is running.
    mutable std::mutex queueMutex;
    std::deque<UserMessage> queuedMessages;
    // Manual compaction armed from the UI thread (requestCompaction), or a
    // shared flag (shareCompactionRequest) consumed by scaffold role agents.
    std::atomic<bool> compactRequested{false};
    const std::atomic<bool>* compactFlag_ = nullptr;

    void emit(AgentEvent event);
    // Deterministic compaction (vccCompact) + persistence callback + event.
    // False when nothing could be compacted (notifyNothingCompacted emits a
    // Compaction note anyway, so the manual path's click always answers).
    bool runCompaction(const std::string& label, bool notifyNothingCompacted = false);
    // Consume a pending manual-compaction request: the agent's own flag, or
    // the shared one (a scaffold role armed from the UI).
    bool takeCompactionRequest();
    // Move all queued messages into the conversation, emitting events.
    // Returns how many were injected. Called from the agent thread.
    size_t drainQueued();
    // Decide what to do with a failed LLM call (context-overflow -> compact
    // and re-issue; transient server error -> bounded retry; otherwise fail
    // fast). May rewrite `messages` (compaction) and `response.errorMessage`.
    ErrorAction handleServerError(AssistantMessage& response);
    void emitLengthNotice(const AssistantMessage& response, LengthStop kind);
    // Sleep up to `ms`, returning early when the abort flag is raised.
    void sleepInterruptible(int ms);
};
