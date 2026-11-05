#pragma once

#include <optional>
#include <vector>

#include "agent/types.h"
#include "agent/vcc/types.h"

namespace vcc {

std::vector<Block> normalize(const std::vector<AgentMessage>& messages,
                             const std::vector<std::optional<int>>& sourceIndices);
std::vector<Block> filterNoise(std::vector<Block> blocks);

}  // namespace vcc
