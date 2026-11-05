#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "agent/types.h"
#include "tools/tools.h"

// Output budget per turn, sent as `max_tokens`. Hardcoded on purpose (no
// per-backend knob yet): a local model that thinks a lot can otherwise run
// the server's own generation limit away for a whole turn. 32k output
// tokens sits comfortably above any real reply (thinking + prose + the
// largest tool-call payload) while still capping a runaway turn; it also
// reserves less of a local server's context window (servers enforce
// input + max_tokens <= ctx). A turn
// that hits it ends with finish_reason "length", which the assembly in
// openai.cpp already handles as the ordinary output-limit stop (a truncated
// tool call is dropped and logged, not retried). llama.cpp/LM Studio clamp
// the value to the slot's context; servers that reject an over-large budget
// with a 4xx would need it clamped to the probed contextWindow instead.
inline constexpr int kMaxOutputTokens = 32000;

// OpenAI-compatible /chat/completions streaming provider.
class OpenAIProvider {
public:
    std::string baseUrl = "http://localhost:1234/v1";
    std::string apiKey = "1234";
    std::string model = "local";

    // The model's context window in tokens (0 = unknown). Used only to size
    // conversation compaction when a context-overflow error is hit; it is not
    // sent in the request.
    int contextWindow = 0;

    // Include assistant thinking (streamed reasoning) from prior turns as
    // "reasoning_content" in the outgoing messages. Some endpoints reject
    // that field, in which case this should be off.
    bool sendThinking = true;

    // Blocking call on the agent thread. Streams the response, calling
    // onTextDelta for each content chunk and onThinkingDelta for each
    // reasoning chunk (delta "reasoning_content"/"reasoning", when the
    // provider streams any). Returns the assembled message; stopReason is
    // "error" (errorMessage set) on transport/API errors, "aborted" if
    // abortFlag was raised.
    AssistantMessage stream(
        const std::string& systemPrompt,
        const std::vector<AgentMessage>& history,
        const std::vector<Tool>& tools,
        const std::function<void(const std::string&)>& onTextDelta,
        const std::function<void(const std::string&)>& onThinkingDelta,
        const std::atomic<bool>& abortFlag,
        // Called on the agent thread whenever a usage chunk arrives (some
        // providers send one per chunk, others only a final one).
        const std::function<void(Usage)>& onUsage = {},
        // Called on the agent thread for each streamed tool-call argument
        // fragment (index, name-so-far, raw argument fragment), so the UI can
        // show a live "streaming" tool row before the call can actually run.
        const std::function<void(int, const std::string&, const std::string&)>&
            onToolArgsDelta = {}
    );
};

// Query GET {baseUrl}/models for the context window of `model`, reading
// whatever server-specific field the server exposes: "max_model_len"
// (vLLM, ninfer), meta "n_ctx" (llama.cpp), "context_length" (LM Studio) or
// "num_ctx" (Ollama). The OpenAI spec carries no such field, so this is
// best-effort. Returns 0 when unknown or the request fails; *outFound (when
// non-null) is true if the request succeeded and a model entry was found but
// no window field is exposed. Blocking (one short GET).
int probeContextWindow(const std::string& baseUrl, const std::string& apiKey,
                       const std::string& model, bool* outFound = nullptr);

// Repair the malformed JSON string literals local models emit in streamed
// tool-call arguments: raw control characters inside strings (a literal
// newline in a heredoc command) and backslashes that start no valid JSON
// escape ("C:\Users", "\ p"). stream() runs one of these passes before a
// call's arguments are declared unparseable. Port of pi's repairJson
// (packages/ai/src/utils/json-parse.ts); pure string function, exposed for
// tests. Never touches bytes outside string literals.
std::string repairToolCallJson(const std::string& args);

// Append one inspection record for a tool call whose arguments stayed
// unparseable even after repairToolCallJson: timestamp, model/endpoint,
// finish_reason, the call's name/id, the parser's own complaint and the
// raw accumulated argument string verbatim (the only way to tell a
// truncated stream from an invalid escape after the fact). Best effort;
// returns the log file path used: $MINIAGENT_LOG when set, otherwise
// <cwd>/.miniagent/llm-errors.log (created on demand). Exposed for tests.
std::string logToolCallParseFailure(const std::string& model,
                                    const std::string& baseUrl,
                                    const std::string& finishReason,
                                    const std::string& toolName,
                                    const std::string& callId,
                                    const std::string& rawArgs,
                                    const std::string& parseDetail);
