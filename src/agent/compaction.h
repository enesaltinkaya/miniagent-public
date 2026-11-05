#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "agent/types.h"

// Error classification, ported from pi's ai/src/utils/retry.ts and
// overflow.ts. Both classifiers take the errorMessage of a failed LLM call
// (e.g. "HTTP 503: {...}", "HTTP request failed: ...").

// Transient provider/transport failures worth retrying (overloaded, rate
// limit, 5xx, network drops). Quota/billing exhaustion is NOT retryable.
bool isRetryableServerError(const std::string& errorMessage);

// The request exceeded the model's context window (provider-specific error
// texts). Rate-limit / throttling messages are excluded, so an overloaded
// endpoint is retried instead of compacted.
bool isContextOverflowError(const std::string& errorMessage);

// Conservative chars/4 token estimate for one message (an attached image
// counts as ~1200 tokens).
int estimateMessageTokens(const AgentMessage& message);

// Estimated tokens of the whole conversation.
int estimateConversationTokens(const std::vector<AgentMessage>& messages);

// ===========================================================================
// Conversation compaction (pi-vcc port: deterministic, no LLM call)
// ===========================================================================

// Marker prefix of the summary message that replaces a summarized prefix.
const char* kCompactionSummaryPrefix();

// True for the summary message produced by compaction (a user message whose
// text starts with kCompactionSummaryPrefix()).
bool isCompactionSummaryMessage(const AgentMessage& message);

struct CompactionResult {
    bool ok = false;         // false: nothing was compacted
    int keepStart = -1;      // first kept index into the pre-compaction vector
                             // (-1: everything was summarized)
    size_t keepFromFileIndex = 0;  // session-file entry index of the first kept
                                   // message; 0 when unknown (not session-indexed)
    std::string summary;     // generated summary text (no marker), when used
    int tokensBefore = 0;    // estimated tokens before compaction
    int tokensAfter = 0;     // estimated tokens after compaction
    std::string note;        // human-readable outcome
};

// Compact `messages` in place with the deterministic vcc algorithm: choose a
// cut anchored at the last user turn(s) (smart keep grows the kept tail while
// it stays under a window-scaled budget; a token-budget cut is the fallback
// when no user anchor exists or the tail is oversized), summarize the prefix
// with vcc::compileRanked folding in a previous summary (messages[0] when it
// is one), then rebuild `messages` as [summary message, kept tail].
//
// `fileBase` is the session-file entry index of the first non-summary message
// (0 when messages[0] is live); `sessionIndexed` false disables (#N) refs
// (nested agents have no session file). `lastInputTokens` is the server's own
// input-token count of the last successful call, used to calibrate the
// chars/token estimate (0 = fall back to 4).
CompactionResult vccCompact(std::vector<AgentMessage>& messages, size_t fileBase, bool sessionIndexed,
                            int contextWindow, int lastInputTokens);
