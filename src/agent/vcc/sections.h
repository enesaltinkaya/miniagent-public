#pragma once

#include <set>
#include <string>
#include <vector>

#include "agent/vcc/types.h"

namespace vcc {

struct FileActivity {
    std::set<std::string> read;
    std::set<std::string> modified;
    std::set<std::string> created;
};

struct SectionData {
    std::vector<std::string> sessionGoal;
    std::vector<std::string> filesAndChanges;
    std::vector<std::string> commits;
    std::vector<std::string> outstandingContext;
    std::vector<std::string> userPreferences;
    std::string briefTranscript;
};

std::vector<std::string> extractGoals(const std::vector<Block>& blocks);
FileActivity extractFiles(const std::vector<Block>& blocks, const FileOps& fileOps);
std::vector<std::string> extractCommits(const std::vector<Block>& blocks);
std::vector<std::string> formatCommits(const std::vector<std::string>& commits, size_t limit = 8);
std::vector<std::string> extractPreferences(const std::vector<Block>& blocks);
std::vector<std::string> dedupPreferencesAgainstGoals(const std::vector<std::string>& prefs,
                                                      const std::vector<std::string>& goals);
std::vector<std::string> extractOutstandingContext(const std::vector<Block>& blocks);
std::vector<std::string> formatFileActivity(const FileActivity& act);
SectionData buildSections(const std::vector<Block>& blocks, const std::vector<Block>& briefBlocks,
                          const FileOps& fileOps);

}  // namespace vcc
