#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace vcc {

std::string sanitize(std::string text);
std::string clip(const std::string& text, size_t max = 200);
std::string clipSentence(const std::string& text, size_t max = 200);
std::vector<std::string> nonEmptyLines(const std::string& text);
std::string firstLine(const std::string& text, size_t max = 200);
bool isStopWord(const std::string& word);
std::string truncateWords(const std::string& text, size_t limit);
std::string truncateHeadTail(const std::string& text, size_t headWords, size_t tailWords);
std::string wrapLongLines(const std::string& text, size_t maxChars = 120);

}  // namespace vcc
