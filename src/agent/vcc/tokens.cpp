#include "agent/vcc/tokens.h"

#include <algorithm>
#include <nlohmann/json.hpp>

#include "agent/vcc/constants.h"

namespace vcc {
namespace {

std::string argsText(const nlohmann::json& j) {
    try {
        return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        return "{}";
    }
}

}  // namespace

TokenCalibration calibrateCharsPerToken(long long sourceChars, long long sourceTokens) {
    if (sourceTokens <= 0 || sourceChars <= 0)
        return {};
    double raw = static_cast<double>(sourceChars) / static_cast<double>(sourceTokens);
    if (!(raw > 0.0))
        return {};
    TokenCalibration c;
    c.calibrated = true;
    c.charsPerToken = std::min(kMaxCharsPerToken, std::max(kMinCharsPerToken, raw));
    return c;
}

size_t messageContentChars(const AgentMessage& message) {
    size_t chars = 0;
    if (const auto* u = std::get_if<UserMessage>(&message)) {
        chars += u->text.size() + kImageChars * u->images.size();
    } else if (const auto* a = std::get_if<AssistantMessage>(&message)) {
        chars += a->text.size() + a->thinking.size();
        for (const auto& tc : a->toolCalls)
            chars += tc.name.size() + argsText(tc.arguments).size();
    } else if (const auto* t = std::get_if<ToolResultMessage>(&message)) {
        chars += t->content.size() + (t->image ? kImageChars : 0);
    }
    return chars;
}

size_t conversationChars(const std::vector<AgentMessage>& messages) {
    size_t total = 0;
    for (const auto& m : messages)
        total += messageContentChars(m);
    return total;
}

int estimateTokensFromChars(size_t chars, double charsPerToken) {
    if (charsPerToken <= 0.0)
        charsPerToken = kDefaultCharsPerToken;
    return static_cast<int>(static_cast<double>(chars) / charsPerToken + 0.999999);
}

int estimateMessageTokens(const AgentMessage& message, double charsPerToken) {
    return estimateTokensFromChars(messageContentChars(message), charsPerToken);
}

int estimateConversationTokens(const std::vector<AgentMessage>& messages, double charsPerToken) {
    return estimateTokensFromChars(conversationChars(messages), charsPerToken);
}

}  // namespace vcc
