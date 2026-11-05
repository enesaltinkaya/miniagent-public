#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include <optional>

#include <nlohmann/json.hpp>

#include "util/image.h"

struct ToolResult {
    std::string content;
    bool isError = false;
    std::optional<ImageAttachment> image; // set when the result includes an image
    std::string imagePath;               // on-disk path of that image (resolved)
};

struct Tool {
    std::string name;
    std::string description;
    nlohmann::json paramsSchema; // JSON Schema object
    std::function<ToolResult(nlohmann::json, const std::atomic<bool>&)> execute;
};

class ToolRegistry {
public:
    void add(Tool tool) {
        index_[tool.name] = tools_.size();
        tools_.push_back(std::move(tool));
    }

    const std::vector<Tool>& all() const { return tools_; }

    ToolResult execute(const std::string& name, nlohmann::json args, const std::atomic<bool>& abort) {
        auto it = index_.find(name);
        if (it == index_.end())
            return {"Unknown tool: " + name, true};
        try {
            return tools_[it->second].execute(std::move(args), abort);
        } catch (const std::exception& e) {
            return {std::string("Tool threw: ") + e.what(), true};
        }
    }

private:
    std::vector<Tool> tools_;
    std::map<std::string, size_t> index_;
};
