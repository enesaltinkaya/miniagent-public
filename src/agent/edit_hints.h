#pragma once

#include <string>
#include <vector>

#include "agent/types.h"

// Returns a short note when the conversation shows `path` being modified by a
// successful edit or write AFTER the last successful read of that path — the
// situation where a failing edit's oldText is most likely stale (the model
// retyped or reused pre-modification text). Returns "" otherwise.
//
// Paths are matched on the "path" field of each ToolResultMessage's args JSON;
// relative and absolute forms of the same file (same working directory) match.
std::string staleFileNote(const std::vector<AgentMessage>& messages, const std::string& path);
