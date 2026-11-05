#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Helpers of tools/web.cpp, exposed for tests (the tool factories live in
// tools/factories.h).

// Form-style percent encoding (spaces become '+', everything outside the
// RFC 3986 unreserved set becomes %XX).
std::string percentEncodeQuery(const std::string& s);

// SearXNG JSON search URL: <base>/search?format=json&language=all&pageno=N&q=...
// A trailing '/' on base is dropped.
std::string searxngSearchUrl(const std::string& base, const std::string& query, int page);

// Render a SearXNG /search?format=json payload as a numbered
// title / url / snippet list (duplicate URLs dropped). Returns "" when the
// payload holds no usable results.
std::string formatSearchResults(const nlohmann::json& payload, int limit);

// HTML to readable text: script/style/head and friends dropped, block-level
// tags turned into line breaks (li prefixed with "- "), entities decoded,
// whitespace collapsed per line - except inside <pre>, where it is preserved.
std::string htmlToText(const std::string& html);

// True when the payload looks binary (a NUL byte in its first bytes): such a
// body has no readable text for the model.
bool looksBinaryBytes(const std::string& s);
