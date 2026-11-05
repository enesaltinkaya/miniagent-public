#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

#include "util/image.h"

struct Usage {
    int inputTokens = 0;
    int outputTokens = 0;
    int totalTokens = 0;
};

// One recorded conversation compaction: the summary replaces the session
// entries before `keepFrom`, the tail from `keepFrom` on stays live. Entry
// indices are 1-based over the non-header lines of the session file.
struct CompactionRecord {
    size_t keepFrom = 0;
    std::string summary;
    int tokensBefore = 0;
    int tokensAfter = 0;
};

struct UserMessage {
    std::string text;
    int64_t timestamp = 0;
    std::vector<ImageAttachment> images; // pasted images; text references them as [image N]
    // Injected by the harness instead of typed by the user (the output-limit
    // continuation prompt). It reaches the model as an ordinary user turn but
    // renders as a system line in the log.
    bool synthetic = false;
};

struct ToolCallBlock {
    std::string id;
    std::string name;
    nlohmann::json arguments; // JSON object (parsed from the streamed argument fragments)
};

struct AssistantMessage {
    std::string text;
    std::string thinking; // model reasoning, if the provider streams any
    std::vector<ToolCallBlock> toolCalls;
    std::string stopReason; // "stop" | "length" | "toolUse" | "error" | "aborted"
    std::string errorMessage;
    Usage usage;
    int64_t timestamp = 0;
};

struct ToolResultMessage {
    std::string toolCallId;
    std::string toolName;
    std::string content;
    bool isError = false;
    int64_t timestamp = 0;
    std::string args; // JSON args, echoed back for display
    std::optional<ImageAttachment> image; // image part sent to the LLM
    std::string imagePath;                // on-disk path of that image (for UI rendering)
};

using AgentMessage = std::variant<UserMessage, AssistantMessage, ToolResultMessage>;

inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

enum class AgentEventType {
    AgentStart,
    TurnStart,
    TextDelta,
    ThinkingDelta,
    ToolCallStart,
    ToolCallEnd,
    // A fragment of the streamed tool-call arguments arrived (live view of the
    // in-flight call; text carries the raw argument fragment, toolName the name
    // so far, toolIndex the SSE tool_calls index).
    ToolArgsDelta,
    TurnEnd,
    AgentEnd,
    Error,
    // A streaming usage chunk arrived (cumulative usage for the in-flight LLM
    // call; usage.outputTokens is the live output token count when the
    // provider streams one). usage carries the cumulative values.
    UsageDelta,
    // A message queued while running (pushQueuedMessage) was injected into
    // the conversation mid-run, between LLM calls; text carries its content.
    UserMessage,
    // A transient server error was caught and the LLM call is being retried;
    // text carries a human-readable note (attempt, wait, error excerpt).
    Retry,
    // The context overflowed the model's window: the conversation was compacted
    // (old messages summarized) and the turn re-issued; text carries a note.
    Compaction,
    Warning,
    // The scaffold harness gate (the plan's pinned `Verification:` command)
    // started or landed: verifyRunning says which, text carries the command on
    // the start and the outcome line on the end. A gate is a build/test run
    // with no model output at all, so the UI needs this to show what the wait
    // actually is instead of a silent "Working...".
    Verification,
};

struct AgentEvent {
    AgentEventType type;
    std::string text;         // TextDelta/ThinkingDelta/Error
    std::string toolName;     // ToolCallStart/ToolCallEnd
    std::string toolArgs;     // JSON args (ToolCallStart)
    std::string toolResult;   // ToolCallEnd
    std::string toolImagePath; // ToolCallEnd: on-disk path of an image result
    bool toolIsError = false;
    int turnIndex = 0;
    int64_t timestamp = 0;
    Usage usage;              // TurnEnd: tokens for that LLM call
    int toolIndex = 0;         // tool_calls index (ToolArgsDelta/ToolCallStart/ToolCallEnd)
    bool nested = false;       // forwarded from a subagent run
    std::string nestedAgent;   // name of the subagent the event belongs to
    // Retry: 1-based attempt number, max attempts this run, wait before the
    // retry (ms; 0 = immediate). Compaction: estimated context tokens before
    // and after the compaction (usage.inputTokens/outputTokens).
    int retryAttempt = 0;
    int retryMaxAttempts = 0;
    int retryDelayMs = 0;
    // Verification: true on the gate's start event, false on its end event
    // (which carries verifyExit / verifyTimedOut).
    bool verifyRunning  = false;
    int verifyExit      = 0;
    bool verifyTimedOut = false;
    int tokensBefore = 0;
    int tokensAfter = 0;
    // Compaction: session-file entry index of the first kept message (0 =
    // unknown / not persisted) and the generated summary text. Consumed by
    // the session store callback, not the UI.
    size_t compactionKeepFromFileIndex = 0;
    std::string compactionSummary;
};
