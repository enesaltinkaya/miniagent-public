#pragma once

#include <string>
#include <vector>

#include "agent/vcc/types.h"

namespace vcc {

struct BriefSection {
    std::string header;
    std::vector<std::string> lines;
};

int heredocCloseIndex(const std::vector<std::string>& lines, size_t i);
std::string compressBash(const std::string& raw);
std::string toolOneLiner(const std::string& name, const nlohmann::json& args);
std::vector<BriefSection> buildBriefSections(const std::vector<Block>& blocks);
std::string stringifyBrief(const std::vector<BriefSection>& sections);
std::string compileBrief(const std::vector<Block>& blocks);

}  // namespace vcc
