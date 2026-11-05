#pragma once

#include <string>
#include <vector>

#include "agent/types.h"

namespace vcc {

struct TokenCalibration {
    bool calibrated = false;
    double charsPerToken = 4.0;
};

TokenCalibration calibrateCharsPerToken(long long sourceChars, long long sourceTokens);

size_t messageContentChars(const AgentMessage& message);
size_t conversationChars(const std::vector<AgentMessage>& messages);

int estimateTokensFromChars(size_t chars, double charsPerToken);
int estimateMessageTokens(const AgentMessage& message, double charsPerToken);
int estimateConversationTokens(const std::vector<AgentMessage>& messages, double charsPerToken);

}  // namespace vcc
