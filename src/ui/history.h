#pragma once

#include <string>
#include <vector>

// Prompt history, stored one entry per line with backslashes and newlines
// escaped, in ~/.config/miniagent/history.txt.
void SaveHistory(const std::vector<std::string>& hist);
void LoadHistory(std::vector<std::string>& hist);
