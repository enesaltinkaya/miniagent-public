#include "agent/agent.h"

#include "agent/compaction.h"
#include "agent/edit_hints.h"

#include <chrono>
#include <thread>

namespace {

std::string prettyArgs(const nlohmann::json& args) {
    try {
        // replace: never throw on invalid UTF-8 inside arguments.
        return args.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        return "{}";
    }
}

constexpr char kLengthContinuePrompt[] =
    "[system] Your previous response hit the output token limit and was cut off. "
    "Resume from the exact point where it stopped: finish the unfinished sentence or "
    "thought and carry on to the end. Do not repeat anything you already wrote and do "
    "not start the answer over.";

} // namespace

Agent::Agent(std::string systemPrompt, OpenAIProvider provider, ToolRegistry* tools,
             std::function<void(const AgentEvent&)> onEvent,
             std::function<void(const AgentMessage&)> onMessage,
             std::function<void(const CompactionRecord&)> onCompaction)
    : systemPrompt(std::move(systemPrompt)),
      provider(std::move(provider)),
      tools(tools),
      onEvent(std::move(onEvent)),
      onMessage(std::move(onMessage)),
      onCompaction(std::move(onCompaction)) {}

void Agent::pushQueuedMessage(UserMessage message) {
    std::lock_guard lock(queueMutex);
    queuedMessages.push_back(std::move(message));
}

std::vector<UserMessage> Agent::clearQueuedMessages() {
    std::lock_guard lock(queueMutex);
    std::vector<UserMessage> out;
    out.reserve(queuedMessages.size());
    for (auto& m : queuedMessages) out.push_back(std::move(m));
    queuedMessages.clear();
    return out;
}

size_t Agent::pendingMessageCount() const {
    std::lock_guard lock(queueMutex);
    return queuedMessages.size();
}

// Called from the agent thread between LLM calls: inject any waiting user
// messages into the conversation so the next model call sees them.
size_t Agent::drainQueued() {
    std::deque<UserMessage> taken;
    {
        std::lock_guard lock(queueMutex);
        taken.swap(queuedMessages);
    }
    for (const UserMessage& user : taken) {
        messages.push_back(user);
        if (onMessage)
            onMessage(user);
        AgentEvent ev;
        ev.type       = AgentEventType::UserMessage;
        ev.text       = user.text;
        ev.turnIndex  = turnIndex;
        if (onEvent)
            onEvent(ev);
    }
    return taken.size();
}

// Deterministic compaction: rewrite the conversation, persist the compaction
// entry (before any later message can be appended) and notify. False when
// nothing could be compacted.
bool Agent::runCompaction(const std::string& label, bool notifyNothingCompacted) {
    CompactionResult c =
        vccCompact(messages, fileBase, sessionIndexed, provider.contextWindow, lastInputTokens);
    if (!c.ok) {
        if (notifyNothingCompacted) {
            AgentEvent ev;
            ev.type = AgentEventType::Compaction;
            ev.text = label + "nothing to compact (" + c.note + ")";
            emit(ev);
        }
        return false;
    }
    if (c.keepFromFileIndex > 0)
        fileBase = c.keepFromFileIndex;
    if (onCompaction) {
        CompactionRecord rec;
        rec.keepFrom     = c.keepFromFileIndex;
        rec.summary      = c.summary;
        rec.tokensBefore = c.tokensBefore;
        rec.tokensAfter  = c.tokensAfter;
        onCompaction(rec);
    }
    AgentEvent ev;
    ev.type                      = AgentEventType::Compaction;
    ev.tokensBefore              = c.tokensBefore;
    ev.tokensAfter               = c.tokensAfter;
    ev.compactionKeepFromFileIndex = c.keepFromFileIndex;
    ev.compactionSummary         = c.summary;
    ev.text = label + std::to_string(c.tokensBefore) + " -> " + std::to_string(c.tokensAfter) + " tokens";
    emit(ev);
    return true;
}

bool Agent::takeCompactionRequest() {
    if (compactRequested.exchange(false))
        return true;
    if (compactFlag_)
        return const_cast<std::atomic<bool>*>(compactFlag_)->exchange(false);
    return false;
}

void Agent::emit(AgentEvent event) {
    event.timestamp = nowMs();
    if (event.type != AgentEventType::AgentStart)
        event.turnIndex = turnIndex;
    if (onEvent)
        onEvent(event);
}

void Agent::run(UserMessage user) {
    ownAbort.store(false);
    turnIndex     = 0;
    errorStreak   = 0;
    lengthStreak  = 0;
    lastInputTokens = 0;
    overflowRecoveryUsed = false;
    emit(AgentEvent{AgentEventType::AgentStart, {}, {}, {}, {}, {}, false, 0, 0});

    user.timestamp = nowMs();
    messages.push_back(user);
    if (onMessage)
        onMessage(user);

    while (true) {
        if (abortFlag().load()) {
            emit(AgentEvent{AgentEventType::Error, "Aborted", {}, {}, {}, {}, {}, false, 0, 0});
            emit(AgentEvent{AgentEventType::AgentEnd, {}, {}, {}, {}, {}, false, 0, 0});
            return;
        }
        // Manual compaction (the Compact button): consumed between turns,
        // where steering messages land -- the in-flight call has settled and
        // the next one sees the rewritten (compacted) conversation. Both the
        // agent's own flag (a plain run) and a shared one (a scaffold role
        // armed from the UI) land here.
        if (takeCompactionRequest())
            runCompaction("Manual compaction: ", true);
        // Steering: inject user messages queued while the previous LLM call
        // was in flight, so they reach the model at the start of this turn.
        drainQueued();
        ++turnIndex;
        emit(AgentEvent{AgentEventType::TurnStart, {}, {}, {}, {}, {}, false, turnIndex, 0});

        AssistantMessage response = provider.stream(
            systemPrompt,
            messages,
            tools ? tools->all() : std::vector<Tool>{},
            [this](const std::string& delta) {
                if (abortFlag().load())
                    return;
                AgentEvent ev;
                ev.type = AgentEventType::TextDelta;
                ev.text = delta;
                ev.turnIndex = turnIndex;
                if (onEvent)
                    onEvent(ev);
            },
            [this](const std::string& delta) {
                if (abortFlag().load())
                    return;
                AgentEvent ev;
                ev.type = AgentEventType::ThinkingDelta;
                ev.text = delta;
                ev.turnIndex = turnIndex;
                if (onEvent)
                    onEvent(ev);
            },
            abortFlag(),
            [this](Usage u) {
                AgentEvent ev;
                ev.type  = AgentEventType::UsageDelta;
                ev.usage = u;
                emit(ev);
            },
            [this](int toolIndex, const std::string& name, const std::string& fragment) {
                if (abortFlag().load())
                    return;
                AgentEvent ev;
                ev.type      = AgentEventType::ToolArgsDelta;
                ev.text      = fragment;
                ev.toolName  = name;
                ev.toolIndex = toolIndex;
                ev.turnIndex = turnIndex;
                if (onEvent)
                    onEvent(ev);
            }
        );
        if (response.stopReason == "error") {
            if (handleServerError(response) != ErrorAction::GiveUp)
                continue;  // the failed message is not committed; the loop re-issues the turn
            if (abortFlag().load()) {
                emit(AgentEvent{AgentEventType::Error, "Aborted", {}, {}, {}, {}, {}, false, turnIndex, 0});
                emit(AgentEvent{AgentEventType::AgentEnd, {}, {}, {}, {}, {}, false, 0, 0});
                return;
            }
            // GiveUp: commit the terminal error so it survives session reloads.
            messages.push_back(response);
            if (onMessage)
                onMessage(response);
            emit(AgentEvent{AgentEventType::Error, response.errorMessage, {}, {}, {}, {}, {}, false, turnIndex, 0});
            emit(AgentEvent{AgentEventType::AgentEnd, {}, {}, {}, {}, {}, false, 0, 0});
            return;
        }

        // A non-error response resets the recovery state (pi resets its retry
        // counter on every successful assistant message too).
        errorStreak = 0;
        if (response.usage.inputTokens > 0)
            lastInputTokens = response.usage.inputTokens;
        if (response.stopReason != "length") {
            overflowRecoveryUsed = false;
            lengthStreak         = 0;
        }

        messages.push_back(response);
        if (onMessage)
            onMessage(response);

        if (abortFlag().load() || response.stopReason == "aborted") {
            emit(AgentEvent{AgentEventType::Error, "Aborted", {}, {}, {}, {}, {}, false, turnIndex, 0});
            emit(AgentEvent{AgentEventType::AgentEnd, {}, {}, {}, {}, {}, false, 0, 0});
            return;
        }

        if (response.toolCalls.empty()) {
            // The model stopped; but queued messages keep the run going: they
            // are injected at the top of the next iteration instead.
            if (pendingMessageCount() == 0) {
                const bool producedNothing =
                    response.text.empty() && response.thinking.empty() &&
                    response.usage.outputTokens == 0;
                if (response.stopReason == "length" && !producedNothing &&
                    lengthStreak < kMaxLengthContinuations) {
                    ++lengthStreak;
                    emitLengthNotice(response, LengthStop::Continuing);
                    UserMessage cont;
                    cont.text      = kLengthContinuePrompt;
                    cont.timestamp = nowMs();
                    cont.synthetic = true;
                    messages.push_back(cont);
                    if (onMessage)
                        onMessage(cont);
                    emit(AgentEvent{AgentEventType::TurnEnd, {}, {}, {}, {}, {}, false, turnIndex, 0, response.usage});
                    continue;
                }
                if (response.stopReason == "length")
                    emitLengthNotice(response,
                                     producedNothing ? LengthStop::Empty : LengthStop::Exhausted);
                // A compaction queued while the last call was in flight still
                // answers "after the current run": the agent's own request
                // compacts now so the next prompt starts from the compacted
                // session history; a shared (scaffold role) request has
                // nothing left to compact - that context dies with the run -
                // so it is spent with a note instead of leaking to the next
                // role.
                if (compactRequested.exchange(false))
                    runCompaction("Manual compaction: ", true);
                else if (compactFlag_ &&
                         const_cast<std::atomic<bool>*>(compactFlag_)->exchange(false)) {
                    AgentEvent ev;
                    ev.type = AgentEventType::Compaction;
                    ev.text = "Manual compaction: nothing to compact (the run already finished)";
                    emit(ev);
                }
                emit(AgentEvent{AgentEventType::TurnEnd, {}, {}, {}, {}, {}, false, turnIndex, 0, response.usage});
                emit(AgentEvent{AgentEventType::AgentEnd, {}, {}, {}, {}, {}, false, 0, 0});
                return;
            }
            emit(AgentEvent{AgentEventType::TurnEnd, {}, {}, {}, {}, {}, false, turnIndex, 0, response.usage});
            continue;
        }

        for (size_t i = 0; i < response.toolCalls.size(); ++i) {
            const auto& tc = response.toolCalls[i];
            if (abortFlag().load()) {
                emit(AgentEvent{AgentEventType::Error, "Aborted", {}, {}, {}, {}, {}, false, turnIndex, 0});
                emit(AgentEvent{AgentEventType::AgentEnd, {}, {}, {}, {}, {}, false, 0, 0});
                return;
            }

            AgentEvent start;
            start.type      = AgentEventType::ToolCallStart;
            start.toolName  = tc.name;
            start.toolArgs  = prettyArgs(tc.arguments);
            start.toolIndex = (int)i;
            emit(start);

            ToolResult result;
            if (tools)
                result = tools->execute(tc.name, tc.arguments, abortFlag());
            else
                result = ToolResult{"No tools registered", true};

            // A failing edit on a file this conversation modified after its
            // last read is usually stale oldText; say so explicitly so the
            // model re-reads instead of guessing at indentation.
            if (tc.name == "edit" && result.isError &&
                result.content.find("oldText") != std::string::npos &&
                tc.arguments.is_object() && tc.arguments.contains("path") &&
                tc.arguments["path"].is_string()) {
                std::string note =
                    staleFileNote(messages, tc.arguments["path"].get<std::string>());
                if (!note.empty())
                    result.content += "\n" + note;
            }

            AgentEvent end;
            end.type          = AgentEventType::ToolCallEnd;
            end.toolName      = tc.name;
            end.toolResult    = result.content;
            end.toolImagePath = result.imagePath;
            end.toolIsError   = result.isError;
            end.toolIndex     = (int)i;
            emit(end);

            ToolResultMessage trm;
            trm.toolCallId = tc.id;
            trm.toolName = tc.name;
            trm.content = result.content;
            trm.isError = result.isError;
            trm.image = result.image;
            trm.timestamp = nowMs();
            trm.args = prettyArgs(tc.arguments);
            trm.imagePath = result.imagePath;
            messages.push_back(trm);
            if (onMessage)
                onMessage(trm);
        }

        emit(AgentEvent{AgentEventType::TurnEnd, {}, {}, {}, {}, {}, false, turnIndex, 0, response.usage});
    }
}

void Agent::sleepInterruptible(int ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (abortFlag().load())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void Agent::emitLengthNotice(const AssistantMessage& response, LengthStop kind) {
    const char* where = kind == LengthStop::Empty
                            ? ""
                            : (response.text.empty() && !response.thinking.empty()
                                   ? " while thinking"
                                   : " mid-reply");
    AgentEvent ev;
    ev.type = AgentEventType::Warning;
    ev.text = "Output limit reached" +
              (response.usage.outputTokens > 0
                   ? " (" + std::to_string(response.usage.outputTokens) + " tokens)"
                   : std::string()) +
              where;
    switch (kind) {
        case LengthStop::Continuing:
            ev.text += ", continuing where it stopped (" + std::to_string(lengthStreak) + "/" +
                       std::to_string(kMaxLengthContinuations) + ")";
            break;
        case LengthStop::Exhausted:
            ev.text += " " + std::to_string(kMaxLengthContinuations) +
                       " times in a row, the reply stays cut off";
            break;
        case LengthStop::Empty:
            ev.text += " with no output at all: the server had no room to answer";
            break;
    }
    emit(ev);
}

Agent::ErrorAction Agent::handleServerError(AssistantMessage& response) {
    const std::string& err = response.errorMessage;

    // 1) Context overflow: compact the conversation and re-issue the turn.
    //    Overflow errors are never retried (retrying the same request would
    //    just overflow again) - the conversation must shrink first.
    if (isContextOverflowError(err)) {
        if (overflowRecoveryUsed) {
            response.errorMessage = "Context overflow persists after compaction - try a smaller "
                                     "task or a larger-context model. Original error: " + err;
            return ErrorAction::GiveUp;
        }
        overflowRecoveryUsed = true;
        if (!runCompaction("Context compacted: ")) {
            response.errorMessage = "Context compaction failed - try a smaller task or a "
                                    "larger-context model. Original error: " + err;
            return ErrorAction::GiveUp;
        }
        return ErrorAction::Compacted;
    }

    // 2) Transient server error: bounded retry (up to kMaxRetries attempts,
    //    waits 0s / 2s / 3s), then the agent stops.
    if (isRetryableServerError(err)) {
        if (errorStreak >= kMaxRetries)
            return ErrorAction::GiveUp;
        const int delayMs = kRetryDelaysMs[errorStreak++];
        AgentEvent ev;
        ev.type             = AgentEventType::Retry;
        ev.retryAttempt     = errorStreak;
        ev.retryMaxAttempts = kMaxRetries;
        ev.retryDelayMs     = delayMs;
        ev.text = "Retrying (" + std::to_string(errorStreak) + "/" + std::to_string(kMaxRetries) + ")" +
                  (delayMs > 0 ? " in " + std::to_string(delayMs / 1000) + "s" : "") + " after: " + err;
        emit(ev);
        if (delayMs > 0)
            sleepInterruptible(delayMs);
        return ErrorAction::Retry;
    }

    // 3) Anything else is deterministic (auth, bad request, ...): fail fast.
    return ErrorAction::GiveUp;
}
