#include <filesystem>
#include <fstream>

#include "tools/tools.h"
#include "util/path.h"

Tool makeWriteTool(const std::string& cwd) {
    Tool t;
    t.name = "write";
    t.description = "Write content to a file (creates it, or overwrites it). Parent directories are created as needed.";
    t.paramsSchema = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "File path, relative to the working directory or absolute"}}},
            {"content", {{"type", "string"}, {"description", "Full content to write (UTF-8)"}}},
        }},
        {"required", {"path", "content"}},
    };
    t.execute = [cwd](nlohmann::json args, const std::atomic<bool>& /*abort*/) -> ToolResult {
        if (!args.contains("path") || !args["path"].is_string())
            return {"Missing 'path' argument", true};
        if (!args.contains("content") || !args["content"].is_string())
            return {"Missing 'content' argument", true};

        std::string resolved = resolvePath(cwd, args["path"].get<std::string>());
        std::string content = args["content"].get<std::string>();

        std::filesystem::path parent(resolved);
        if (parent.has_parent_path()) {
            std::error_code ec;
            std::filesystem::create_directories(parent.parent_path(), ec);
            if (ec)
                return {"Failed to create directories for " + resolved + ": " + ec.message(), true};
        }

        std::ofstream f(resolved, std::ios::binary | std::ios::trunc);
        if (!f)
            return {"Cannot open file for writing: " + resolved, true};
        f.write(content.data(), content.size());
        f.close();
        if (!f)
            return {"Write failed: " + resolved, true};

        return {"Wrote " + std::to_string(content.size()) + " bytes to " + resolved, false};
    };
    return t;
}
