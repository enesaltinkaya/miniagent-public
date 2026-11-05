#pragma once

#include <string>
#include <vector>

// LLM backends the app can talk to, stored as a JSON array of objects in
// ~/.config/miniagent/backends.json:
//   [ { "name": "local",
//       "url": "http://localhost:1234/v1",
//       "apiKey": "1234",
//       "model": "local" } ]
// On first run (file missing) the file is created with the default local
// backend. The GUI will make the list selectable; the current selection is
// tracked separately (App::backendIndex / App::setBackend).

struct LlmBackend {
    std::string name;    // display name for the (upcoming) selector
    std::string url;     // OpenAI-compatible base URL (…/v1)
    std::string apiKey;  // sent as the Authorization bearer
    std::string model;   // model name sent in chat/completions
};

// Path of the config file (for a future "open in editor" GUI action).
std::string backendsPath();

// Load the list. If the file does not exist it is created with the default
// local backend first. A corrupt, empty or old-format (bare URL strings)
// file falls back to the same default in memory (the file is left
// untouched); when outError is non-null it is filled with a description of
// the problem (empty on success).
std::vector<LlmBackend> loadBackends(std::string* outError = nullptr);

// Rewrite the file as a JSON array. Returns false on write error.
bool saveBackends(const std::vector<LlmBackend>& backends);
