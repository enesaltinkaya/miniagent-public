#pragma once

#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace vcc {

struct UserBlock {
    std::string text;
    std::optional<int> src;
};

struct AssistantBlock {
    std::string text;
    std::optional<int> src;
};

struct CallBlock {
    std::string name;
    nlohmann::json args;
    std::optional<int> src;
};

struct ResultBlock {
    std::string name;
    std::string text;
    std::optional<int> src;
};

struct BashBlock {
    std::string command;
    std::string output;
    int exitCode = 0;
    bool hasExitCode = false;
    std::optional<int> src;
};

using Block = std::variant<UserBlock, AssistantBlock, CallBlock, ResultBlock, BashBlock>;

enum class Kind { User, Assistant, Call, Result, Bash };

inline Kind kindOf(const Block& b) {
    switch (b.index()) {
        case 0: return Kind::User;
        case 1: return Kind::Assistant;
        case 2: return Kind::Call;
        case 3: return Kind::Result;
        default: return Kind::Bash;
    }
}

inline const std::optional<int>& srcOf(const Block& b) {
    switch (b.index()) {
        case 0: return std::get<UserBlock>(b).src;
        case 1: return std::get<AssistantBlock>(b).src;
        case 2: return std::get<CallBlock>(b).src;
        case 3: return std::get<ResultBlock>(b).src;
        default: return std::get<BashBlock>(b).src;
    }
}

struct FileOps {
    std::vector<std::string> readFiles;
    std::vector<std::string> modifiedFiles;
    std::vector<std::string> createdFiles;
};

}  // namespace vcc
