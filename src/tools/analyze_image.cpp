#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "agent/types.h"
#include "llm/openai.h"
#include "tools/factories.h"
#include "tools/tools.h"
#include "util/image.h"
#include "util/path.h"

namespace {

const size_t kMaxImages = 10;

const char* kVisionSystemPrompt =
    "You are an image analysis assistant. You MUST answer the user's question "
    "about the images accurately and thoroughly. Return ONLY your analysis as "
    "plain text. Do NOT suggest further actions or offer to do additional work. "
    "Focus entirely on answering the question about the images.";

std::string loadFileBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

Tool makeAnalyzeImageTool(const std::string& cwd, LlmBackend imageBackend) {
    Tool tool;
    tool.name = "analyze_image";
    tool.description = "Analyze one or more images with a dedicated vision-capable model and "
                       "return its plain-text answer. Use this when the current backend cannot "
                       "view images directly. Pass local image file paths (png, jpg, jpeg) and a "
                       "specific question about the images.";
    tool.paramsSchema = {
        {"type", "object"},
        {"properties",
         {
             {"images",
              {
                  {"type", "array"},
                  {"items", {{"type", "string"}}},
                  {"description", "One or more local image file paths (png, jpg, jpeg)"},
              }},
             {"question",
              {
                  {"type", "string"},
                  {"description", "Question about the image(s)"},
              }},
         }},
        {"required", {"images", "question"}},
    };
    tool.execute = [cwd, imageBackend](nlohmann::json args, const std::atomic<bool>& abort) -> ToolResult {
        if (!args.contains("images") || !args["images"].is_array())
            return {"analyze_image requires an 'images' array of file paths.", true};
        if (!args.contains("question") || !args["question"].is_string() ||
            args["question"].get<std::string>().empty())
            return {"analyze_image requires a non-empty 'question' string.", true};
        const auto& images = args["images"];
        if (images.empty())
            return {"No images provided. Pass at least one image path.", true};
        if (images.size() > kMaxImages)
            return {"Too many images (" + std::to_string(images.size()) +
                        "). Maximum is " + std::to_string(kMaxImages) + ".",
                    true};
        const std::string question = args["question"].get<std::string>();

        std::vector<ImageAttachment> attachments;
        for (const auto& img : images) {
            if (!img.is_string())
                return {"Each image entry must be a file path string.", true};
            const std::string resolved = resolvePath(cwd, img.get<std::string>());
            if (!isFile(resolved))
                return {"Image file not found: " + resolved, true};
            const std::string bytes = loadFileBytes(resolved);
            if (detectImageFormat(bytes) == ImageFormat::None)
                return {"Unsupported image format: " + resolved +
                            ". Supported: png, jpg, jpeg.",
                        true};
            auto att = processImageBytes(bytes);
            if (!att)
                return {"Could not decode image (or it exceeds the size limit): " + resolved,
                        true};
            attachments.push_back(std::move(*att));
        }

        OpenAIProvider provider;
        provider.baseUrl       = imageBackend.url;
        provider.apiKey        = imageBackend.apiKey;
        provider.model         = imageBackend.model;
        provider.sendThinking  = false;
        provider.contextWindow = 0;

        UserMessage um;
        um.text      = question;
        um.timestamp = nowMs();
        um.images    = std::move(attachments);
        std::vector<AgentMessage> history{std::move(um)};

        std::string text;
        AssistantMessage out = provider.stream(kVisionSystemPrompt, history, {},
                                               [&](const std::string& d) { text += d; }, {},
                                               abort);
        if (out.stopReason == "aborted")
            return {"Image analysis aborted.", true};
        if (out.stopReason == "error")
            return {"Image analysis failed: " +
                        (out.errorMessage.empty() ? std::string("unknown error")
                                                  : out.errorMessage),
                    true};
        if (text.empty())
            return {"Image analysis failed: the vision model returned no text.", true};
        return {text, false};
    };
    return tool;
}
