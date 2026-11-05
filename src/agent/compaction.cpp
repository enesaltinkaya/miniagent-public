#include "agent/compaction.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <regex>

#include "agent/vcc/compile.h"
#include "agent/vcc/constants.h"
#include "agent/vcc/tokens.h"

namespace {

// ---------------------------------------------------------------------------
// Error classification (ported from pi's ai/src/utils/retry.ts + overflow.ts)
// ---------------------------------------------------------------------------

std::regex icase(const char* pattern) {
    return std::regex(pattern, std::regex::ECMAScript | std::regex::icase);
}

const std::vector<std::regex>& nonOverflowPatterns() {
    static const std::vector<std::regex> patterns = {
        icase("(?:Throttling error|Service unavailable):"),
        icase("rate limit"),
        icase("too many requests"),
    };
    return patterns;
}

const std::vector<std::regex>& nonRetryablePatterns() {
    static const std::vector<std::regex> patterns = {
        icase("GoUsageLimitError"),
        icase("FreeUsageLimitError"),
        icase("Monthly usage limit reached"),
        icase("available balance"),
        icase("insufficient_quota"),
        icase("out of budget"),
        icase("quota exceeded"),
        icase("billing"),
    };
    return patterns;
}

const std::vector<std::regex>& retryablePatterns() {
    static const std::vector<std::regex> patterns = {
        icase("overloaded"),
        icase("rate.?limit"),
        icase("too many requests"),
        icase("429"),
        icase("500"),
        icase("502"),
        icase("503"),
        icase("504"),
        icase("524"),
        icase("service.?unavailable"),
        icase("server.?error"),
        icase("internal.?error"),
        icase("provider.?returned.?error"),
        icase("exceeded request buffer limit while retrying upstream"),
        icase("network.?error"),
        icase("connection.?error"),
        icase("connection.?refused"),
        icase("connection.?lost"),
        icase("other side closed"),
        icase("fetch failed"),
        icase("couldn'?t connect"),
        icase("could not connect"),
        icase("couldn'?t resolve"),
        icase("getaddrinfo"),
        icase("ENOTFOUND"),
        icase("EAI_AGAIN"),
        icase("upstream.?connect"),
        icase("reset before headers"),
        icase("socket hang up"),
        icase("socket connection was closed"),
        icase("timed? out"),
        icase("timeout"),
        icase("terminated"),
        icase("websocket.?closed"),
        icase("websocket.?error"),
        icase("ended without"),
        icase("stream ended before message_stop"),
        icase("stream ended before a terminal response event"),
        icase("http2 request did not get a response"),
        icase("retry delay"),
        icase("you can retry your request"),
        icase("try your request again"),
        icase("please retry your request"),
        icase("ResourceExhausted"),
        icase("failed to parse"),
    };
    return patterns;
}

const std::vector<std::regex>& overflowPatterns() {
    static const std::vector<std::regex> patterns = {
        icase("prompt is too long"),
        icase("request_too_large"),
        icase("input is too long for requested model"),
        icase("exceeds the context window"),
        icase("exceeds (?:the )?(?:model'?s )?maximum context length(?: of [\\d,]+ tokens?|\\s*\\([\\d,]+\\))"),
        icase("input token count.*exceeds the maximum"),
        icase("maximum prompt length is \\d+"),
        icase("reduce the length of the messages"),
        icase("maximum context length is \\d+ tokens"),
        icase("exceeds (?:the )?maximum allowed input length of [\\d,]+ tokens?"),
        icase("input \\(\\d+ tokens\\) is longer than the model'?s context length \\(\\d+ tokens\\)"),
        icase("exceeds the limit of \\d+"),
        icase("exceeds the available context size"),
        icase("greater than the context length"),
        icase("context window exceeds limit"),
        icase("exceeded model token limit"),
        icase("too large for model with \\d+ maximum context length"),
        icase("prompt has [\\d,]+ tokens?, but the configured context size is [\\d,]+ tokens?"),
        icase("model_context_window_exceeded"),
        icase("prompt too long; exceeded (?:max )?context length"),
        icase("range of input length should be"),
        icase("context[_ ]length[_ ]exceeded"),
        icase("too many tokens"),
        icase("token limit exceeded"),
        icase("4(?:00|13)\\s*(?:status code)?\\s*\\(no body\\)"),
    };
    return patterns;
}

bool matchesAny(const std::vector<std::regex>& patterns, const std::string& text) {
    for (const auto& p : patterns)
        if (std::regex_search(text, p))
            return true;
    return false;
}

// ---------------------------------------------------------------------------
// vcc cut selection
// ---------------------------------------------------------------------------

constexpr double kOversizedTailFactor = 2.5;

bool isCutPoint(const AgentMessage& m) {
    return std::holds_alternative<UserMessage>(m) || std::holds_alternative<AssistantMessage>(m);
}

int findBudgetCutIndex(const std::vector<AgentMessage>& live, int maxTokens, double cpt) {
    int acc = 0;
    int crossed = -1;
    for (int i = static_cast<int>(live.size()) - 1; i >= 0; --i) {
        acc += vcc::estimateMessageTokens(live[i], cpt);
        if (acc >= maxTokens) {
            crossed = i;
            break;
        }
    }
    if (crossed < 0)
        return -1;
    for (size_t j = std::max(crossed, 1); j < live.size(); ++j)
        if (isCutPoint(live[j]))
            return static_cast<int>(j);
    return -1;
}

bool windowHasRenderableContent(const std::vector<AgentMessage>& window) {
    for (const auto& m : window) {
        if (const auto* u = std::get_if<UserMessage>(&m)) {
            if (!u->text.empty() || !u->images.empty())
                return true;
        } else if (const auto* a = std::get_if<AssistantMessage>(&m)) {
            if (!a->text.empty() || !a->toolCalls.empty())
                return true;
        }
    }
    return false;
}

}  // namespace

bool isRetryableServerError(const std::string& errorMessage) {
    if (errorMessage.empty())
        return false;
    if (matchesAny(nonRetryablePatterns(), errorMessage))
        return false;
    return matchesAny(retryablePatterns(), errorMessage);
}

bool isContextOverflowError(const std::string& errorMessage) {
    if (errorMessage.empty())
        return false;
    if (matchesAny(nonOverflowPatterns(), errorMessage))
        return false;
    return matchesAny(overflowPatterns(), errorMessage);
}

int estimateMessageTokens(const AgentMessage& message) {
    return vcc::estimateMessageTokens(message, vcc::kDefaultCharsPerToken);
}

int estimateConversationTokens(const std::vector<AgentMessage>& messages) {
    return vcc::estimateConversationTokens(messages, vcc::kDefaultCharsPerToken);
}

const char* kCompactionSummaryPrefix() {
    return "[miniagent:compacted-conversation]";
}

bool isCompactionSummaryMessage(const AgentMessage& message) {
    const auto* u = std::get_if<UserMessage>(&message);
    if (!u)
        return false;
    const std::string prefix = kCompactionSummaryPrefix();
    return u->text.size() > prefix.size() && u->text.compare(0, prefix.size(), prefix) == 0;
}

CompactionResult vccCompact(std::vector<AgentMessage>& messages, size_t fileBase, bool sessionIndexed,
                            int contextWindow, int lastInputTokens) {
    CompactionResult result;

    size_t liveStart = 0;
    std::string previousSummary;
    if (!messages.empty() && isCompactionSummaryMessage(messages[0])) {
        const auto* u = std::get_if<UserMessage>(&messages[0]);
        previousSummary = u->text.substr(std::string(kCompactionSummaryPrefix()).size());
        liveStart = 1;
    }

    std::vector<AgentMessage> window(messages.begin() + liveStart, messages.end());
    result.tokensBefore = vcc::estimateConversationTokens(messages, vcc::kDefaultCharsPerToken);
    if (window.size() <= 2) {
        result.note = "too few live messages";
        return result;
    }
    if (!windowHasRenderableContent(window)) {
        result.note = "nothing renderable to summarize";
        return result;
    }

    const vcc::TokenCalibration cal =
        vcc::calibrateCharsPerToken(static_cast<long long>(vcc::conversationChars(window) + previousSummary.size()),
                                    lastInputTokens);
    const double cpt = cal.charsPerToken;

    int minTailTokens = 5000;
    int maxTailTokens = 25000;
    if (contextWindow > 0) {
        minTailTokens = std::min(5000, std::max(1024, contextWindow / 8));
        maxTailTokens = std::min(25000, std::max(2048, contextWindow / 4));
    }

    std::vector<size_t> userIdx;
    for (size_t i = 0; i < window.size(); ++i)
        if (std::holds_alternative<UserMessage>(window[i]))
            userIdx.push_back(i);

    auto tailTokensForKeep = [&](size_t keep) -> std::optional<int> {
        if (keep == 0 || keep > userIdx.size())
            return std::nullopt;
        const size_t cut = userIdx[userIdx.size() - keep];
        if (cut == 0)
            return std::nullopt;
        int tokens = 0;
        for (size_t i = cut; i < window.size(); ++i)
            tokens += vcc::estimateMessageTokens(window[i], cpt);
        return tokens;
    };

    size_t keep = 1;
    if (!userIdx.empty()) {
        auto base = tailTokensForKeep(1);
        if (base && *base <= minTailTokens) {
            for (size_t k = 2; k <= userIdx.size(); ++k) {
                auto t = tailTokensForKeep(k);
                if (!t || *t > maxTailTokens)
                    break;
                keep = k;
            }
        }
    }

    long cutIdx = -1;
    if (!userIdx.empty()) {
        const size_t target = userIdx.size() - keep;
        const long candidate = static_cast<long>(userIdx[target]);
        if (candidate > 0)
            cutIdx = candidate;
    }

    if (cutIdx < 0) {
        const int idx = findBudgetCutIndex(window, maxTailTokens, cpt);
        if (idx < 0) {
            result.note = "no safe cut point";
            return result;
        }
        cutIdx = idx;
    } else {
        int tailTokens = 0;
        for (size_t i = static_cast<size_t>(cutIdx); i < window.size(); ++i)
            tailTokens += vcc::estimateMessageTokens(window[i], cpt);
        if (tailTokens > static_cast<int>(maxTailTokens * kOversizedTailFactor)) {
            const int idx = findBudgetCutIndex(window, maxTailTokens, cpt);
            if (idx > cutIdx)
                cutIdx = idx;
        }
    }

    if (cutIdx < 1) {
        result.note = "cut leaves nothing to summarize";
        return result;
    }

    const size_t cut = static_cast<size_t>(cutIdx);
    if (!windowHasRenderableContent(std::vector<AgentMessage>(window.begin(), window.begin() + cut))) {
        result.note = "prefix has no renderable content";
        return result;
    }

    vcc::CompileInput input;
    input.messages.assign(window.begin(), window.begin() + cut);
    if (sessionIndexed && fileBase > 0) {
        for (size_t i = 0; i < cut; ++i)
            input.sourceIndices.push_back(static_cast<int>(fileBase + i));
    }
    input.previousSummary = previousSummary;
    input.charsPerToken = cpt;
    input.briefFloorTokens = vcc::kBriefFloorTokens;
    input.briefCeilingTokens = vcc::kBriefCeilingTokens;
    input.briefTokensPerBlock = vcc::kBriefTokensPerBlock;
    if (contextWindow > 0 && contextWindow < 16000) {
        input.briefFloorTokens = vcc::kBriefFloorTokens / 2;
        input.briefCeilingTokens = vcc::kBriefCeilingTokens / 2;
        input.briefTokensPerBlock = vcc::kBriefTokensPerBlock / 2;
    }
    std::string summary = vcc::compileRanked(input);
    if (summary.empty()) {
        result.note = "summary compiled empty";
        return result;
    }

    UserMessage head;
    head.text = std::string(kCompactionSummaryPrefix()) + "\n" + summary;
    head.timestamp = nowMs();

    std::vector<AgentMessage> kept(window.begin() + cut, window.end());
    result.ok = true;
    result.keepStart = static_cast<int>(liveStart + cut);
    result.keepFromFileIndex = (sessionIndexed && fileBase > 0) ? fileBase + cut : 0;
    result.summary = summary;

    messages.clear();
    messages.push_back(std::move(head));
    messages.insert(messages.end(), std::make_move_iterator(kept.begin()),
                    std::make_move_iterator(kept.end()));
    result.tokensAfter = vcc::estimateConversationTokens(messages, cpt);
    result.note = "summary generated";
    return result;
}
