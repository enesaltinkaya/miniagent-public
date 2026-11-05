#include <fstream>
#include <sstream>

#include <algorithm>
#include <filesystem>
#include <system_error>

#include "tools/tools.h"
#include "util/image.h"
#include "util/path.h"
#include "util/utf8.h"

Tool makeReadTool(const std::string& cwd, const std::string& visionHint) {
    Tool t;
    t.name = "read";
    t.description = "Read the contents of a file. Supports text files and images (png, jpg, jpeg). "
                    "If given a directory, lists its entries (subdirectories with a trailing '/'). ";
    t.description += visionHint.empty()
                          ? "Images are sent to the model as attachments (resized to max 2000x2000 if needed). "
                          : "Image files are reported text-only (this backend cannot view image bytes); "
                            "use the analyze_image tool to actually view an image. ";
    t.description += "For text files, lines are numbered; output is truncated to 2000 lines or 50KB. "
                     "Use offset/limit for large files or to read a specific line range. Each line is "
                     "prefixed with 'NNNNN→'; this prefix is for display only and is not part of the "
                     "file content.";
    t.paramsSchema = {
        {"type", "object"},
        {"properties", {
            {"path", {{"type", "string"}, {"description", "File path, relative to the working directory or absolute"}}},
            {"offset", {{"type", "integer"}, {"description", "1-based line number to start at (default 1)"}}},
            {"limit", {{"type", "integer"}, {"description", "Maximum number of lines to return (default all)"}}},
        }},
        {"required", {"path"}},
    };
    t.execute = [cwd, visionHint](nlohmann::json args, const std::atomic<bool>& /*abort*/) -> ToolResult {
        if (!args.contains("path") || !args["path"].is_string())
            return {"Missing 'path' argument", true};
        std::string resolved = resolvePath(cwd, args["path"].get<std::string>());
        if (isDir(resolved)) {
            std::error_code ec;
            std::vector<std::string> names;
            for (const auto& entry : std::filesystem::directory_iterator(resolved, ec)) {
                names.push_back(entry.path().filename().string() +
                               (entry.is_directory() ? "/" : ""));
            }
            std::sort(names.begin(), names.end());
            if (names.empty())
                return {"Directory is empty: " + resolved, false};
            static const size_t kMaxListed = 200;
            const size_t shown = std::min(names.size(), kMaxListed);
            std::ostringstream out;
            out << "Directory listing of " << resolved << " (" << shown << " of "
                << names.size() << " entries):";
            for (size_t i = 0; i < shown; i++)
                out << "\n" << names[i];
            if (names.size() > shown)
                out << "\n... (" << (names.size() - shown) << " more entries not listed)";
            return {out.str(), false};
        }
        if (!isFile(resolved))
            return {"File not found: " + resolved, true};

        std::ifstream f(resolved, std::ios::binary);
        if (!f)
            return {"Cannot open file: " + resolved, true};
        std::stringstream ss;
        ss << f.rdbuf();
        std::string content = ss.str();

        ImageFormat fmt = detectImageFormat(content);
        if (fmt != ImageFormat::None) {
            auto img = processImageBytes(content);
            if (!img)
                return {"Read image file: could not decode or the image exceeds the size limit. "
                         "Consider converting or cropping it first.",
                        true};
            std::string text = "Read image file [" + img->mimeType + "] (" +
                               std::to_string(img->width) + "x" + std::to_string(img->height) + ")";
            if (!visionHint.empty()) {
                // The main backend cannot view image bytes: report the file
                // textually (no attachment) and steer to analyze_image.
                return {text + "\n" + visionHint + " Image path: " + resolved, false};
            }
            std::string hint = imageDimensionHint(*img);
            if (!hint.empty())
                text += "\n" + hint;
            ToolResult r{text, false};
            r.image = std::move(*img);
            r.imagePath = resolved;
            return r;
        }

        long long offset = args.value("offset", 1LL);
        long long limit = args.value("limit", -1LL);
        if (offset < 1)
            offset = 1;

        // Text file: sanitize so binary bytes never reach JSON/UI as-is.
        std::string text = sanitizeUtf8(content);

        std::vector<std::string> lines;
        {
            std::string cur;
            for (char c : text) {
                if (c == '\n') {
                    lines.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
            if (!cur.empty())
                lines.push_back(cur);
        }

        size_t start = (size_t)offset - 1;
        if (start >= lines.size() && !lines.empty())
            return {"Line offset " + std::to_string(offset) + " is past end of file (" +
                        std::to_string(lines.size()) + " lines)",
                    true};

        size_t end = lines.size();
        if (limit >= 0)
            end = std::min<size_t>(start + (size_t)limit, end);

        std::ostringstream out;
        for (size_t i = start; i < end; i++) {
            char num[16];
            snprintf(num, sizeof(num), "%5zu→", i + 1);
            out << num << lines[i] << "\n";
        }
        std::string full = out.str();
        size_t cut = full.size();
        std::string result = truncateToLimit(full, 2000, 50 * 1024, &cut);
        if (cut < full.size()) {
            // The display cap cut the output: point at the next chunk so the
            // model re-reads with offset instead of falling back to `sed -n`.
            // Count the rendered lines in the result (every rendered line
            // starts with the "%5zu→" prefix, so the arrow sits at index 5)
            // to know exactly how many lines the model has already seen.
            size_t shown = 0;
            size_t lineStart = 0;
            while (lineStart < result.size()) {
                size_t nl = result.find('\n', lineStart);
                size_t len = (nl == std::string::npos ? result.size() : nl) - lineStart;
                if (len >= 6 && result.compare(lineStart + 5, 3, "\xe2\x86\x92") == 0)
                    ++shown;
                if (nl == std::string::npos)
                    break;
                lineStart = nl + 1;
            }
            size_t nextLine = start + shown + 1;
            if (nextLine > lines.size()) {
                // The cap cut only the trailing newline: the whole file was
                // shown, so there is no next chunk to point at.
                result += "\n[End of file. The whole file was shown (" +
                          std::to_string(lines.size()) + " lines).]";
            } else {
                result += "\n[truncated by the read tool. Re-read with offset=" +
                          std::to_string(nextLine) +
                          " (and limit=...) for the next chunk. Total lines in file: " +
                          std::to_string(lines.size()) + ".]";
            }
        }
        return {result, false};
    };
    return t;
}
