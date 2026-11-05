#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agent/vcc/types.h"

namespace vcc {

struct RankOptions {
    size_t maxBlocks = 80;
    size_t preserveRecentBlocks = 16;
    std::optional<size_t> maxBriefChars;
    FileOps fileOps;
};

bool isTestCommand(const std::string& command);
bool isWorkflowCommand(const std::string& command);
bool isTrivialOnlyBash(const std::string& raw);
std::optional<std::string> pathFromBlock(const Block& b);
std::optional<std::string> bashCommandFromBlock(const Block& b);

std::vector<Block> selectBriefBlocks(const std::vector<Block>& blocks, const RankOptions& options);

}  // namespace vcc
