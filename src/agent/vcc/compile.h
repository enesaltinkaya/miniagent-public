#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agent/types.h"
#include "agent/vcc/types.h"

namespace vcc {

struct CompileInput {
    std::vector<AgentMessage> messages;
    std::vector<std::optional<int>> sourceIndices;
    std::string previousSummary;
    FileOps fileOps;
    double charsPerToken = 4.0;
    int briefFloorTokens = 1100;
    int briefCeilingTokens = 2000;
    int briefTokensPerBlock = 15;
};

std::string compile(const CompileInput& input);
std::string compileRanked(const CompileInput& input);

}  // namespace vcc
