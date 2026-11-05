#include "agent/edit_hints.h"

#include <nlohmann/json.hpp>

namespace {

std::string jsonPath(const std::string& argsJson) {
    try {
        auto j = nlohmann::json::parse(argsJson);
        if (j.is_object() && j.contains("path") && j["path"].is_string())
            return j["path"].get<std::string>();
    } catch (...) {
    }
    return "";
}

// Same file? Exact string match, or one is the other with a "/..." (or
// "dir/...") cwd prefix — read was called with a relative path, the edit with
// an absolute one (or vice versa).
bool samePath(const std::string& a, const std::string& b) {
    if (a == b)
        return true;
    auto isSuffixPath = [](const std::string& long_, const std::string& short_) {
        if (long_.size() <= short_.size())
            return false;
        size_t off = long_.size() - short_.size();
        return long_[off - 1] == '/' && long_.compare(off, short_.size(), short_) == 0;
    };
    return isSuffixPath(a, b) || isSuffixPath(b, a);
}

} // namespace

std::string staleFileNote(const std::vector<AgentMessage>& messages, const std::string& path) {
    if (path.empty())
        return "";
    long long lastRead = -1;
    long long lastMod = -1;
    std::string modTool;
    for (size_t i = 0; i < messages.size(); i++) {
        if (!std::holds_alternative<ToolResultMessage>(messages[i]))
            continue;
        const ToolResultMessage& trm = std::get<ToolResultMessage>(messages[i]);
        if (trm.isError)
            continue;
        std::string p = jsonPath(trm.args);
        if (p.empty() || !samePath(p, path))
            continue;
        if (trm.toolName == "read")
            lastRead = (long long)i;
        else if (trm.toolName == "edit" || trm.toolName == "write") {
            lastMod = (long long)i;
            modTool = trm.toolName;
        }
    }
    if (lastMod < 0 || lastMod <= lastRead)
        return "";
    return "Note: this file was modified by your own " + modTool +
           " after your most recent successful read of it, so oldText is likely stale. "
           "Re-read the file (or the relevant region) before editing.";
}
