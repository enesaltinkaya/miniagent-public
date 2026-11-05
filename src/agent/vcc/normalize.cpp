#include "agent/vcc/normalize.h"

#include <cctype>
#include <regex>
#include <string_view>

#include <nlohmann/json.hpp>

#include "agent/vcc/text.h"

namespace vcc {
namespace {

std::string toLower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<Block> normalizeOne(const AgentMessage& msg, const std::optional<int>& src) {
    std::vector<Block> blocks;
    if (const auto* u = std::get_if<UserMessage>(&msg)) {
        std::string text = sanitize(u->text);
        if (!text.empty())
            blocks.push_back(UserBlock{text, src});
        for (const auto& img : u->images)
            blocks.push_back(UserBlock{"[image: " + img.mimeType + "]", src});
        if (blocks.empty())
            blocks.push_back(UserBlock{std::string(), src});
        return blocks;
    }
    if (const auto* a = std::get_if<AssistantMessage>(&msg)) {
        if (!a->text.empty())
            blocks.push_back(AssistantBlock{sanitize(a->text), src});
        for (const auto& tc : a->toolCalls)
            blocks.push_back(CallBlock{tc.name, tc.arguments, src});
        return blocks;
    }
    const auto* t = std::get_if<ToolResultMessage>(&msg);
    if (toLower(t->toolName) == "bash") {
        BashBlock b;
        b.src = src;
        b.output = t->content;
        nlohmann::json args = nlohmann::json::parse(t->args, nullptr, false);
        if (!args.is_discarded() && args.is_object() && args.contains("command") &&
            args["command"].is_string())
            b.command = args["command"].get<std::string>();
        static const std::regex exitRe(R"(\[exit code: (-?\d+)\])");
        std::smatch m;
        std::string tail = t->content.size() > 400 ? t->content.substr(t->content.size() - 400) : t->content;
        if (std::regex_search(tail, m, exitRe)) {
            b.hasExitCode = true;
            b.exitCode = std::stoi(m[1].str());
        }
        blocks.push_back(std::move(b));
        return blocks;
    }
    blocks.push_back(ResultBlock{t->toolName, sanitize(t->content), src});
    return blocks;
}

const std::vector<std::string_view>& noiseTools() {
    static const std::vector<std::string_view> tools = {
        "todowrite", "todoread", "toolsearch", "websearch", "askuser",
    };
    return tools;
}

const std::vector<std::string_view>& noiseStrings() {
    static const std::vector<std::string_view> strings = {
        "Continue from where you left off.",
        "No response requested.",
    };
    return strings;
}

}  // namespace

std::vector<Block> normalize(const std::vector<AgentMessage>& messages,
                             const std::vector<std::optional<int>>& sourceIndices) {
    std::vector<Block> blocks;
    blocks.reserve(messages.size() * 2);
    for (size_t i = 0; i < messages.size(); ++i) {
        const std::optional<int> src = i < sourceIndices.size() ? sourceIndices[i] : std::nullopt;
        for (Block& b : normalizeOne(messages[i], src))
            blocks.push_back(std::move(b));
    }
    return blocks;
}

std::vector<Block> filterNoise(std::vector<Block> blocks) {
    std::vector<Block> out;
    out.reserve(blocks.size());
    for (Block& b : blocks) {
        if (auto* c = std::get_if<CallBlock>(&b)) {
            if (std::find(noiseTools().begin(), noiseTools().end(), toLower(c->name)) != noiseTools().end())
                continue;
        } else if (auto* r = std::get_if<ResultBlock>(&b)) {
            if (std::find(noiseTools().begin(), noiseTools().end(), toLower(r->name)) != noiseTools().end())
                continue;
        } else if (auto* u = std::get_if<UserBlock>(&b)) {
            std::string_view trimmed(u->text);
            size_t s = trimmed.find_first_not_of(" \t\n\r");
            if (s == std::string_view::npos)
                continue;
            trimmed = trimmed.substr(s);
            bool noise = false;
            for (std::string_view n : noiseStrings())
                if (trimmed.find(n) != std::string_view::npos) {
                    noise = true;
                    break;
                }
            if (noise)
                continue;
        }
        out.push_back(std::move(b));
    }
    return out;
}

}  // namespace vcc
