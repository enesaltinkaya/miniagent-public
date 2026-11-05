#pragma once

#include <cstdlib>

// Tests that would send real requests to the LLM backend (slow, and only
// meaningful with a live server) skip by default. Run them with:
//   MINIAGENT_LIVE_LLM=1 ./build/<test>
inline bool liveLlmEnabled() {
    const char* v = std::getenv("MINIAGENT_LIVE_LLM");
    return v && v[0] && v[0] != '0';
}
