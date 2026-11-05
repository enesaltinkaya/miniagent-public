#pragma once

#include <string>

// Replaces every invalid UTF-8 byte/sequence in s with U+FFFD.
// Valid sequences pass through unchanged, so already-clean input is a
// no-op (aside from the allocation). Used on untrusted bytes (command
// output, file contents) before they enter JSON or the UI: nlohmann json
// with its default strict dump() throws type_error.316 on raw bytes like
// 0xFF, which would otherwise crash the process.
std::string sanitizeUtf8(const std::string& s);
