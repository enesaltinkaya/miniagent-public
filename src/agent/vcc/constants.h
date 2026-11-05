#pragma once

#include <cstddef>

namespace vcc {

constexpr size_t kTruncateUserWords = 256;
constexpr size_t kAssistantHeadWords = 80;
constexpr size_t kAssistantTailWords = 120;
constexpr size_t kSegmentClosingHeadWords = 120;
constexpr size_t kSegmentClosingTailWords = 120;
constexpr size_t kMinSegmentClosingAssistantChars = 120;
constexpr size_t kBashCapChars = 240;
constexpr size_t kToolCallsPerTurn = 8;
constexpr size_t kHeredocBodyCap = 80;
constexpr size_t kBriefMaxLines = 120;
constexpr size_t kTuiSafeLineChars = 120;
constexpr size_t kToolResultRenderChars = 400;
constexpr size_t kToolResultExpandChars = 4000;
constexpr size_t kMaxBriefBlocks = 80;
constexpr size_t kPreserveRecentBlocks = 16;

constexpr int kBriefFloorTokens = 1100;
constexpr int kBriefCeilingTokens = 2000;
constexpr int kBriefTokensPerBlock = 15;

constexpr size_t kGoalCap = 8;
constexpr size_t kGoalFirstLines = 6;
constexpr size_t kScopeChangeLines = 3;
constexpr size_t kScopeChangeTaskLines = 2;
constexpr size_t kMaxGoalChars = 200;
constexpr size_t kFilesPerCategory = 10;
constexpr size_t kCommitCap = 8;
constexpr size_t kPreferenceCap = 10;
constexpr size_t kMergedPreferenceCap = 15;
constexpr size_t kOutstandingCap = 5;
constexpr size_t kOutstandingTailBlocks = 20;

constexpr double kDefaultCharsPerToken = 4.0;
constexpr double kMinCharsPerToken = 2.0;
constexpr double kMaxCharsPerToken = 6.0;
constexpr size_t kImageChars = 4800;

constexpr int kScoreRecencyMax = 12;
constexpr int kScoreUserTurn = 18;
constexpr int kScoreAssistant = 10;
constexpr int kScoreToolResult = 1;
constexpr int kScoreEditTool = 34;
constexpr int kScoreTestCommand = 26;
constexpr int kScoreReadTool = 6;
constexpr int kScoreToolCall = 12;
constexpr int kScoreWorkflowCommand = 14;
constexpr int kScoreTrivialBash = -16;
constexpr int kScoreNonzeroExit = 24;
constexpr int kScoreBash = 8;
constexpr int kScoreHookModifiedFile = 18;
constexpr int kScoreHookReadFile = 6;
constexpr int kScoreLongToolResult = -8;
constexpr size_t kLongToolResultChars = 1000;
constexpr int kScoreNearImportantUser = 10;
constexpr int kScoreNearImportantAssistant = 7;
constexpr int kScoreAfterImportant = 5;
constexpr int kScoreSegmentClosingAssistant = 14;
constexpr int kImportantScoreThreshold = 34;
constexpr size_t kAdjacencyUserBack = 8;
constexpr size_t kAdjacencyAssistantBack = 4;
constexpr size_t kAdjacencyForward = 4;

inline const char* kRecallNote =
    "Use the `recall` tool to search earlier conversation turns, including anything dropped by "
    "compaction. Do not redo work already completed.";

}  // namespace vcc
