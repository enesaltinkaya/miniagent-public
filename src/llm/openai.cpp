#include "llm/openai.h"

#include "llm/sse.h"
#include "util/http.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>

#include <sys/stat.h>
#include <unistd.h>

namespace {

nlohmann::json serializeMessage(const AgentMessage& msg, bool includeThinking) {
    if (const auto* u = std::get_if<UserMessage>(&msg)) {
        if (u->images.empty())
            return {{"role", "user"}, {"content", u->text}};
        nlohmann::json content = nlohmann::json::array();
        if (!u->text.empty())
            content.push_back({{"type", "text"}, {"text", u->text}});
        for (const auto& img : u->images)
            content.push_back({
                {"type", "image_url"},
                {"image_url",
                 {{"url", "data:" + img.mimeType + ";base64," + img.base64}}}});
        return {{"role", "user"}, {"content", std::move(content)}};
    }
    if (const auto* a = std::get_if<AssistantMessage>(&msg)) {
        nlohmann::json j{{"role", "assistant"}};
        if (includeThinking && !a->thinking.empty())
            j["reasoning_content"] = a->thinking;
        if (a->text.empty())
            j["content"] = nullptr;
        else
            j["content"] = a->text;
        if (!a->toolCalls.empty()) {
            nlohmann::json calls = nlohmann::json::array();
            for (const auto& tc : a->toolCalls) {
                nlohmann::json fn{{"name", tc.name}};
                if (tc.arguments.is_object())
                    fn["arguments"] =
                        tc.arguments.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
                else if (tc.arguments.is_string())
                    fn["arguments"] = tc.arguments.get<std::string>();
                else
                    fn["arguments"] = nullptr;
                calls.push_back({{"id", tc.id}, {"type", "function"}, {"function", std::move(fn)}});
            }
            j["tool_calls"] = std::move(calls);
        }
        return j;
    }
    const auto* t = std::get_if<ToolResultMessage>(&msg);
    if (t->image) {
        nlohmann::json content = nlohmann::json::array();
        content.push_back({{"type", "text"}, {"text", t->content}});
        content.push_back({
            {"type", "image_url"},
            {"image_url", {{"url", "data:" + t->image->mimeType + ";base64," + t->image->base64}}}});
        return {{"role", "tool"}, {"tool_call_id", t->toolCallId}, {"content", std::move(content)}};
    }
    return {
        {"role", "tool"},
        {"tool_call_id", t->toolCallId},
        {"content", t->content},
    };
}

struct ToolCallAcc {
    std::string id;
    std::string name;
    std::string args;
};

} // namespace

std::string repairToolCallJson(const std::string& args) {
    static const char* const kValidEscapes = "\\\"/bfnrt";
    auto isHex = [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    };
    std::string out;
    out.reserve(args.size());
    bool inString = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const unsigned char c = (unsigned char)args[i];
        if (!inString) {
            out += (char)c;
            if (c == '"')
                inString = true;
            continue;
        }
        if (c == '"') {
            out += (char)c;
            inString = false;
            continue;
        }
        if (c == '\\') {
            if (i + 1 >= args.size()) { // trailing lone backslash
                out += "\\\\";
                continue;
            }
            const char next = args[i + 1];
            if (next == 'u' && i + 5 < args.size() && isHex(args[i + 2]) &&
                isHex(args[i + 3]) && isHex(args[i + 4]) && isHex(args[i + 5])) {
                out += args.substr(i, 6); // already a valid \uXXXX
                i += 5;
                continue;
            }
            if (next != '\0' && std::strchr(kValidEscapes, next) != nullptr) {
                out += (char)c;
                out += next;
                ++i;
                continue;
            }
            // No valid escape starts here: make it a literal backslash. The
            // next byte is re-examined below (pi keeps it too, so a control
            // character after the stray backslash still gets escaped).
            out += "\\\\";
            continue;
        }
        if (c <= 0x1f) { // raw control character inside a string literal
            switch (c) {
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                }
            }
            continue;
        }
        out += (char)c;
    }
    return out;
}

std::string logToolCallParseFailure(const std::string& model,
                                    const std::string& baseUrl,
                                    const std::string& finishReason,
                                    const std::string& toolName,
                                    const std::string& callId,
                                    const std::string& rawArgs,
                                    const std::string& parseDetail) {
    std::string path;
    if (const char* env = std::getenv("MINIAGENT_LOG"))
        path = env;
    else {
        char dir[4096];
        const std::string base =
            ::getcwd(dir, sizeof(dir)) ? std::string(dir) : std::string("/tmp");
        ::mkdir((base + "/.miniagent").c_str(), 0755); // best effort
        path = base + "/.miniagent/llm-errors.log";
    }
    FILE* f = std::fopen(path.c_str(), "ab");
    if (!f)
        return path; // best effort; the caller still reports the failure
    char stamp[32];
    const std::time_t now = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(f,
                 "[%s] tool call arguments unparseable after repair\n"
                 "  model: %s @ %s  finish: %s\n"
                 "  call: %s (id=%s)  args: %zu bytes\n"
                 "  parse: %s\n"
                 "  raw args:\n",
                 stamp, model.c_str(), baseUrl.c_str(),
                 finishReason.empty() ? "-" : finishReason.c_str(),
                 toolName.c_str(), callId.c_str(), rawArgs.size(),
                 parseDetail.c_str());
    // The arguments verbatim - the whole point of the record (it is the only
    // way to tell a truncated stream from an invalid escape afterwards).
    // Bounded so one runaway write cannot grow the log without limit.
    const size_t kMaxLogged = 64 * 1024;
    std::fwrite(rawArgs.data(), 1, rawArgs.size() < kMaxLogged ? rawArgs.size() : kMaxLogged, f);
    if (rawArgs.size() > kMaxLogged)
        std::fprintf(f, "\n[truncated: %zu of %zu bytes]", kMaxLogged, rawArgs.size());
    std::fputs("\n====\n", f);
    std::fclose(f);
    return path;
}

int probeContextWindow(const std::string& baseUrl, const std::string& apiKey,
                       const std::string& model, bool* outFound) {
    if (outFound) *outFound = false;
    std::atomic<bool> noAbort{false};
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    if (!apiKey.empty())
        headers.push_back({"Authorization", "Bearer " + apiKey});
    headers.push_back({"Accept", "application/json"});
    HttpResult res;
    httpGet(baseUrl + "/models", headers, [&](const char* d, size_t n) { body.append(d, n); }, noAbort, res);
    if (res.error.empty() && res.statusCode == 200) {
        nlohmann::json j = nlohmann::json::parse(body, nullptr, false);
        if (!j.is_discarded() && j.is_object()) {
            nlohmann::json arr = j.value("data", nlohmann::json::array());
            if (!arr.is_array() || arr.empty())
                arr = j.value("models", nlohmann::json::array());
            if (arr.is_array() && !arr.empty()) {
                const nlohmann::json* pick = nullptr;
                if (!model.empty()) {
                    for (const auto& e : arr) {
                        if (!e.is_object())
                            continue;
                        bool match = e.value("id", std::string()) == model
                                     || e.value("name", std::string()) == model;
                        auto al = e.find("aliases");
                        if (!match && al != e.end() && al->is_array())
                            for (const auto& a : *al)
                                if (a.is_string() && a.get<std::string>() == model)
                                    match = true;
                        if (match) {
                            pick = &e;
                            break;
                        }
                    }
                }
                if (!pick)
                    pick = &arr.front();
                auto field = [](const nlohmann::json& o, const char* k) -> int {
                    if (o.contains(k) && o[k].is_number_integer() && o[k].get<int>() > 0)
                        return o[k].get<int>();
                    return 0;
                };
                int w = field(*pick, "max_model_len");
                if (w <= 0)
                    w = field(*pick, "context_length");
                if (w <= 0)
                    w = field(*pick, "num_ctx");
                if (w <= 0 && pick->contains("meta") && (*pick)["meta"].is_object())
                    w = field((*pick)["meta"], "n_ctx");
                if (w > 0 && w < 1024)
                    w = 0;  // implausible
                if (outFound)
                    *outFound = true;
                return w;
            }
        }
    }
    return 0;
}

AssistantMessage OpenAIProvider::stream(
    const std::string& systemPrompt,
    const std::vector<AgentMessage>& history,
    const std::vector<Tool>& tools,
    const std::function<void(const std::string&)>& onTextDelta,
    const std::function<void(const std::string&)>& onThinkingDelta,
    const std::atomic<bool>& abortFlag,
    const std::function<void(Usage)>& onUsage,
    const std::function<void(int, const std::string&, const std::string&)>& onToolArgsDelta
) {
    AssistantMessage out;
    out.timestamp = nowMs();

    nlohmann::json req;
    req["model"] = model;
    req["stream"] = true;
    req["stream_options"] = {{"include_usage", true}};
    req["max_tokens"] = kMaxOutputTokens;

    nlohmann::json messages = nlohmann::json::array();
    if (!systemPrompt.empty())
        messages.push_back({{"role", "system"}, {"content", systemPrompt}});
    for (const auto& m : history)
        messages.push_back(serializeMessage(m, sendThinking));
    req["messages"] = std::move(messages);

    if (!tools.empty()) {
        nlohmann::json toolArr = nlohmann::json::array();
        for (const auto& t : tools) {
            toolArr.push_back({
                {"type", "function"},
                {"function", {
                    {"name", t.name},
                    {"description", t.description},
                    {"parameters", t.paramsSchema},
                }},
            });
        }
        req["tools"] = std::move(toolArr);
    }

    std::string body = req.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    if (const char* dump = std::getenv("MINIAGENT_DUMP_REQ")) {
        if (FILE* df = std::fopen(dump, "ab")) {
            std::fputs(body.c_str(), df);
            std::fputs("\n=====\n", df);
            std::fclose(df);
        }
    }
    std::string rawResponse;
    std::map<int, ToolCallAcc> toolCalls;
    std::string finishReason;
    bool parseFailed = false;
    int streamedUnits = 0; // text/thinking/tool fragments received so far

    SSEParser sse([&](SSEEvent e) {
        if (e.data == "[DONE]")
            return;
        nlohmann::json chunk;
        try {
            chunk = nlohmann::json::parse(e.data);
        } catch (...) {
            return;
        }
        if (chunk.contains("error")) {
            out.stopReason = "error";
            out.errorMessage = chunk["error"].is_string() ? chunk["error"].get<std::string>()
                                                         : chunk["error"].dump(-1, ' ', false,
                                                                               nlohmann::json::error_handler_t::replace);
            return;
        }
        if (chunk.contains("usage") && chunk["usage"].is_object()) {
            const auto& u = chunk["usage"];
            out.usage.inputTokens = u.value("prompt_tokens", out.usage.inputTokens);
            out.usage.outputTokens = u.value("completion_tokens", out.usage.outputTokens);
            out.usage.totalTokens = u.value("total_tokens", out.usage.inputTokens + out.usage.outputTokens);
            if (onUsage)
                onUsage(out.usage);
        }
        const auto choices = chunk.value("choices", std::vector<nlohmann::json>{});
        if (choices.empty())
            return;
        const auto& choice = choices[0];

        if (choice.contains("delta") && choice["delta"].is_object()) {
            const auto& delta = choice["delta"];
            if (delta.contains("content") && delta["content"].is_string() && !delta["content"].get<std::string>().empty()) {
                out.text += delta["content"].get<std::string>();
                streamedUnits++;
                if (onTextDelta)
                    onTextDelta(delta["content"].get<std::string>());
            }
            // Reasoning streams use "reasoning_content" (DeepSeek/Qwen-style
            // proxies) or "reasoning" depending on the provider.
            for (const char* key : {"reasoning_content", "reasoning"}) {
                if (delta.contains(key) && delta[key].is_string() && !delta[key].get<std::string>().empty()) {
                    const std::string t = delta[key].get<std::string>();
                    out.thinking += t;
                    streamedUnits++;
                    if (onThinkingDelta)
                        onThinkingDelta(t);
                    break;
                }
            }
            if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
                for (const auto& tc : delta["tool_calls"]) {
                    int idx = tc.value("index", 0);
                    ToolCallAcc& acc = toolCalls[idx];
                    bool nameGrew = false;
                    std::string fragment;
                    if (tc.contains("id") && tc["id"].is_string() && !tc["id"].get<std::string>().empty())
                        acc.id = tc["id"].get<std::string>();
                    if (tc.contains("function") && tc["function"].is_object()) {
                        const auto& fn = tc["function"];
                        if (fn.contains("name") && fn["name"].is_string() && !fn["name"].get<std::string>().empty()) {
                            acc.name += fn["name"].get<std::string>();
                            nameGrew = true;
                        }
                        if (fn.contains("arguments") && fn["arguments"].is_string()) {
                            fragment = fn["arguments"].get<std::string>();
                            acc.args += fragment;
                        }
                    }
                    // Live view of the in-flight call: fire on any argument
                    // fragment, and on the chunk(s) that grow the name (so a
                    // name-only leading chunk still starts the UI row).
                    if (onToolArgsDelta && (!fragment.empty() || nameGrew)) {
                        streamedUnits++;
                        onToolArgsDelta(idx, acc.name, fragment);
                    }
                }
            }
        }
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()
            && !choice["finish_reason"].get<std::string>().empty()) {
            finishReason = choice["finish_reason"].get<std::string>();
        }
    });

    std::vector<std::pair<std::string, std::string>> headers{
        {"Authorization", "Bearer " + apiKey},
        {"Content-Type", "application/json"},
    };

    // Transient failures (connection refused, DNS, 429, 5xx, mid-stream
    // drops) are NOT retried inside the provider: the agent loop owns retry
    // policy (pi's retryAssistantCall pattern) and emits a visible Retry
    // event to the message log for every attempt, so the user sees the
    // backoff instead of a silent reconnect loop. Single-shot callers that
    // want their own policy (e.g. the summarizer) read the error below.
    HttpResult res;
    httpPostStream(
        baseUrl + "/chat/completions",
        headers,
        body,
        [&](const char* data, size_t len) {
            rawResponse.append(data, len);
            sse.feed(data, len);
        },
        abortFlag,
        res
    );

    if (res.aborted) {
        out.stopReason = "aborted";
    } else if (!res.error.empty()) {
        out.stopReason = "error";
        out.errorMessage = "HTTP request failed: " + res.error;
    } else if (res.statusCode != 200) {
        out.stopReason = "error";
        out.errorMessage = "HTTP " + std::to_string(res.statusCode) + ": " + rawResponse;
    } else {
        // Assemble tool calls. The accumulated arguments must be valid
        // JSON per the streaming spec, but local models wobble exactly on
        // heredoc-heavy commands (raw newlines, stray backslashes inside
        // string literals) - so run one repair pass (pi's
        // parseJsonWithRepair) before declaring the call unparseable, and
        // only the still-broken ones cost the turn anything. A call that
        // stays broken is logged verbatim (logToolCallParseFailure): the raw
        // arguments are the only way to tell a truncated stream from an
        // invalid escape afterwards, and the two are handled differently
        // below - truncation replays identically on every retry, a wobble
        // usually does not.
        std::string parseFailDetail;
        std::string parseLogPath;
        for (auto& [idx, acc] : toolCalls) {
            ToolCallBlock block;
            block.id = acc.id;
            block.name = acc.name;
            nlohmann::json j;
            bool parsed = false;
            std::string detail;
            try {
                j = nlohmann::json::parse(acc.args.empty() ? "{}" : acc.args);
                parsed = true;
            } catch (const nlohmann::json::exception& e) {
                detail = std::string("as streamed: ") + e.what();
                try {
                    j = nlohmann::json::parse(repairToolCallJson(acc.args));
                    parsed = true; // repaired into valid JSON (silent, as pi)
                } catch (const nlohmann::json::exception& e2) {
                    detail += "; after repair: " + std::string(e2.what());
                }
            }
            if (!parsed) {
                parseFailed  = true;
                parseLogPath = logToolCallParseFailure(model, baseUrl, finishReason,
                                                       acc.name, acc.id, acc.args, detail);
                if (!parseFailDetail.empty())
                    parseFailDetail += "; ";
                parseFailDetail += (acc.name.empty() ? "call " + std::to_string(idx) : acc.name) +
                                   " [" + std::to_string(acc.args.size()) + " bytes] " + detail;
                // finish_reason "length" means the stream was cut off inside
                // the arguments: no repair can complete them, and retrying
                // the identical request replays the identical truncation
                // (that used to burn the retry budget and then kill a whole
                // scaffold round). Drop the call - executing it with empty
                // arguments would be worse - and let the ordinary
                // output-limit stop below take over.
                if (finishReason == "length")
                    continue;
                block.arguments = nlohmann::json::object();
            } else {
                block.arguments = std::move(j);
            }
            out.toolCalls.push_back(std::move(block));
        }
        // toolCalls is a map keyed by the SSE tool_calls index, so this
        // assembly already yields the calls in stream order (the model's own
        // order for parallel calls). Never re-sort by id: the ids are
        // provider-assigned strings and lexicographic order is unrelated to
        // call order (pi keeps its stream-order block list too).
        if (parseFailed && finishReason == "length") {
            // Output limit truncated the call: report the same stop the loop
            // already handles for a truncated text reply (nested roles
            // surface it as "stopped: output limit reached"; the scaffold's
            // cut-off summarizer then costs the round, not the run).
            out.stopReason = "length";
        } else if (parseFailed) {
            out.stopReason = "error";
            out.errorMessage = "Failed to parse tool call arguments as JSON (" +
                               parseFailDetail + ") - raw arguments logged to " + parseLogPath;
        } else if (finishReason == "tool_calls" || finishReason == "function_call" || !out.toolCalls.empty()) {
            out.stopReason = "toolUse";
        } else if (finishReason == "length") {
            out.stopReason = "length";
        } else if (!finishReason.empty() && finishReason != "stop") {
            out.stopReason = "error";
            out.errorMessage = "server error: the turn ended with finish_reason \"" + finishReason + "\"";
        } else {
            out.stopReason = "stop";
        }
    }
    return out;
}
