// repairToolCallJson: the repair pass for streamed tool-call arguments
// (port of pi's repairJson). Tests the exact artifact shapes local models
// emit on heredoc-heavy bash commands - raw control characters inside string
// literals and backslashes that start no valid JSON escape.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "llm/openai.h"

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        printf("  ok: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

// Repair + parse, returning whether the result is valid JSON.
static bool parses(const std::string& args, nlohmann::json& out) {
    out = nlohmann::json::parse(repairToolCallJson(args), nullptr, false);
    return !out.is_discarded();
}

// One-shot fake SSE server: answers a single POST with `sseBody`, then
// exits. Drives OpenAIProvider::stream() end-to-end (the real assembly +
// repair + logging path) without touching the user's backend config. The
// request body lands in `outBody` when given, so the test can inspect what
// the provider actually sent.
static bool serveOneSse(int port, const std::string& sseBody, std::string* outBody) {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0)
        return false;
    int on = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(srv, 1) != 0) {
        ::close(srv);
        return false;
    }
    int cli = ::accept(srv, nullptr, nullptr);
    ::close(srv);
    if (cli < 0)
        return false;
    // Read the request (headers + full body; the body is what the provider
    // sent, which some checks below inspect).
    char buf[4096];
    std::string req;
    size_t headerEnd    = std::string::npos;
    size_t contentLength = 0;
    for (;;) {
        const ssize_t n = ::recv(cli, buf, sizeof(buf), 0);
        if (n <= 0)
            break;
        req.append(buf, (size_t)n);
        if (headerEnd == std::string::npos)
            headerEnd = req.find("\r\n\r\n");
        if (headerEnd != std::string::npos) {
            if (contentLength == 0) {
                const size_t cl = req.find("Content-Length:");
                if (cl != std::string::npos && cl < headerEnd)
                    contentLength = (size_t)std::atoi(req.c_str() + cl + 15);
            }
            if (req.size() >= headerEnd + 4 + contentLength)
                break;
        }
    }
    if (outBody && headerEnd != std::string::npos)
        *outBody = req.substr(headerEnd + 4);
    const std::string resp =
        "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " +
        std::to_string(sseBody.size()) + "\r\n\r\n" + sseBody;
    size_t sent = 0;
    while (sent < resp.size()) {
        const ssize_t n = ::send(cli, resp.data() + sent, resp.size() - sent, 0);
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
    ::close(cli);
    return true;
}

static std::string sseChunk(const std::string& json) {
    return "data: " + json + "\n\n";
}

// Stream one turn from the fake server on `port`.
static AssistantMessage streamFromFake(int port) {
    OpenAIProvider provider;
    provider.baseUrl = "http://127.0.0.1:" + std::to_string(port) + "/v1";
    provider.model   = "fake";
    std::atomic<bool> abort{false};
    std::vector<AgentMessage> history{UserMessage{"go"}};
    return provider.stream("sys", history, {}, nullptr, nullptr, abort);
}

int main() {
    // 1. Valid JSON passes through byte-identical (no repair needed).
    {
        const std::string in = "{\"command\":\"ls -la /tmp2\"}";
        check(repairToolCallJson(in) == in, "valid object untouched");
    }

    // 2. Valid escapes stay valid: \n \t \" \\ \uXXXX.
    {
        const std::string in = "{\"a\":\"x\\ny\\tq\\\"z\\\\ \\u00e9\"}";
        check(repairToolCallJson(in) == in, "valid escapes untouched");
        nlohmann::json j = nlohmann::json::parse(in);
        check(j.value("a", "") == "x\ny\tq\"z\\ \xc3\xa9", "valid escapes parse");
    }

    // 3. Raw newlines inside a string (the heredoc case from the field):
    // repaired into \n escapes, so the command parses with its newlines.
    {
        const std::string in = "{\"command\":\"cd /p && python3 - <<'PY'\np='tools/x.c'\ns=open(p).read()\nPY\"}";
        nlohmann::json j;
        check(parses(in, j), "raw newlines repaired");
        const std::string cmd = j.value("command", "");
        check(cmd == "cd /p && python3 - <<'PY'\np='tools/x.c'\ns=open(p).read()\nPY",
              "heredoc command round-trips with newlines");
    }

    // 4. Backslash that starts no valid escape ("<<'PY'\ p=..."): doubled
    // into a literal backslash, so the shell sees the original byte.
    {
        const std::string in = "{\"command\":\"<<'PY'\\ p='x'\"}";
        nlohmann::json j;
        check(parses(in, j), "stray backslash repaired");
        check(j.value("command", "") == "<<'PY'\\ p='x'", "stray backslash literal");
    }

    // 5. Windows-style paths: every \x that is not an escape becomes \\x.
    {
        const std::string in = "{\"path\":\"C:\\Users\\user\\work\"}";
        nlohmann::json j;
        check(parses(in, j), "windows path repaired");
        check(j.value("path", "") == "C:\\Users\\user\\work", "windows path literal");
    }

    // 5b. The one ambiguity of the heuristic (pi shares it): a backslash
    // before a real escape letter stays that escape, so "C:\tmp" keeps its
    // \t as a tab. Documented behavior, not a bug.
    {
        const std::string in = "{\"path\":\"C:\\tmp\"}";
        nlohmann::json j;
        check(parses(in, j), "escape-letter path repaired");
        check(j.value("path", "") == std::string("C:") + '\t' + "mp",
              "\\t kept as tab escape (pi semantics)");
    }

    // 6. Raw tabs and other control bytes inside a string get escaped.
    {
        const std::string in = "{\"a\":\"x\ty\u0001z\"}";
        nlohmann::json j;
        check(parses(in, j), "raw tab + control byte repaired");
        check(j.value("a", "") == "x\ty\x01z", "control bytes round-trip");
    }

    // 7. \u not followed by four hex digits is a stray backslash, not an
    // escape sequence: it becomes a literal \u.
    {
        const std::string in = "{\"a\":\"\\u12x\"}";
        nlohmann::json j;
        check(parses(in, j), "bad \\u repaired");
        check(j.value("a", "") == "\\u12x", "bad \\u stays literal");
    }

    // 8. Bytes outside string literals are never touched (a newline
    // between tokens is legal JSON and stays as-is).
    {
        const std::string in = "{\n  \"a\": \"b\"\n}";
        check(repairToolCallJson(in) == in, "outside-string bytes untouched");
    }

    // 9. Truncated JSON is beyond string-literal repair (pi hands those to
    // partial-json; we do not attempt them): repair must not invent a parse.
    {
        nlohmann::json j;
        check(!parses("{\"command\":\"ls", j), "truncated JSON stays unparseable");
    }

    // 10. The parse-failure record: $MINIAGENT_LOG redirects it, and the
    // entry carries everything needed to tell a truncated stream from an
    // invalid escape afterwards.
    {
        const char* tmp = "/tmp/miniagent_toolargs_test.log";
        std::remove(tmp);
        ::setenv("MINIAGENT_LOG", tmp, 1);
        const std::string path = logToolCallParseFailure(
            "qwen3-coder", "http://localhost:1234/v1", "length", "write", "call-1",
            "{\"content\":\"# notes\\n\\n## Baseline\", \"path\":\"notes.md\", \"trunc",
            "as streamed: [json.exception.parse_error.101] unexpected end of input; "
            "after repair: same");
        check(path == tmp, "MINIAGENT_LOG honored");
        std::ifstream f(tmp);
        const std::string body{std::istreambuf_iterator<char>(f),
                               std::istreambuf_iterator<char>()};
        check(body.find("write") != std::string::npos &&
                  body.find("call-1") != std::string::npos,
              "call identity logged");
        check(body.find("qwen3-coder") != std::string::npos &&
                  body.find("http://localhost:1234/v1") != std::string::npos,
              "model/endpoint logged");
        check(body.find("finish: length") != std::string::npos,
              "finish_reason logged (truncation tell)");
        check(body.find("unexpected end of input") != std::string::npos,
              "parser detail logged");
        check(body.find("\"path\":\"notes.md\", \"trunc") != std::string::npos,
              "raw arguments logged verbatim");
        ::unsetenv("MINIAGENT_LOG");
    }

    // 11. End-to-end through stream(): a streamed call whose arguments
    // carry a raw newline (what the SSE layer decodes into acc.args) is
    // repaired and executed instead of failing the turn.
    {
        const std::string body =
            sseChunk("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\","
                     "\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"command\\\":\\\"python3 - <<'PY'\\n\"}}]}}]}") +
            sseChunk("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                     "\"function\":{\"arguments\":\"p='x'\\\"}\"}}]}}]}") +
            sseChunk("{\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}") +
            "data: [DONE]\n\n";
        std::string reqBody;
        std::thread srv(serveOneSse, 18134, body, &reqBody);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const AssistantMessage msg = streamFromFake(18134);
        srv.join();
        const std::string maxTokens = "\"max_tokens\":" + std::to_string(kMaxOutputTokens);
        check(reqBody.find(maxTokens) != std::string::npos,
              "request carries the hardcoded output budget");
        check(msg.stopReason == "toolUse", "repaired call survives the stream (end-to-end)");
        check(msg.toolCalls.size() == 1 && msg.toolCalls[0].name == "bash", "call assembled");
        if (!msg.toolCalls.empty())
            check(msg.toolCalls[0].arguments.value("command", "") ==
                      "python3 - <<'PY'\np='x'",
                  "repaired command carries its newline");
    }

    // 12. End-to-end: a truncated call (finish_reason=length) is not a
    // model wobble - it is the output limit cutting the arguments off. It
    // must NOT error the turn (the retry loop would just replay the same
    // truncation); it becomes the ordinary output-limit stop, the broken
    // call is dropped, and the full record still lands in $MINIAGENT_LOG.
    {
        const char* logPath = "/tmp/miniagent_toolargs_e2e.log";
        std::remove(logPath);
        ::setenv("MINIAGENT_LOG", logPath, 1);
        const std::string body =
            sseChunk("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c2\","
                     "\"function\":{\"name\":\"write\",\"arguments\":\"{\\\"content\\\":\\\"# notes\\n\\n\"}}]}}]}") +
            sseChunk("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                     "\"function\":{\"arguments\":\"## Baseline\"}}]}}]}") +
            sseChunk("{\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}") +
            "data: [DONE]\n\n";
        std::thread srv(serveOneSse, 18135, body, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const AssistantMessage msg = streamFromFake(18135);
        srv.join();
        check(msg.stopReason == "length", "truncated call becomes the output-limit stop");
        check(msg.toolCalls.empty(), "truncated call dropped (never executed empty)");
        check(msg.errorMessage.empty(), "truncated call raises no error");
        std::ifstream f(logPath);
        const std::string logged{std::istreambuf_iterator<char>(f),
                                 std::istreambuf_iterator<char>()};
        check(logged.find("finish: length") != std::string::npos,
              "log records the truncation tell");
        check(logged.find("write") != std::string::npos &&
                  logged.find("# notes") != std::string::npos,
              "log records call identity + raw args");
        ::unsetenv("MINIAGENT_LOG");
    }

    // 13. The fallback: a server that cuts the arguments without saying
    // "length" still gets the error path (with the log record), so the
    // classification above never silently swallows a real wobble.
    {
        const char* logPath = "/tmp/miniagent_toolargs_e2e2.log";
        std::remove(logPath);
        ::setenv("MINIAGENT_LOG", logPath, 1);
        const std::string body =
            sseChunk("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c3\","
                     "\"function\":{\"name\":\"write\",\"arguments\":\"{\\\"content\\\":\\\"x\"}}]}}]}") +
            sseChunk("{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}") +
            "data: [DONE]\n\n";
        std::thread srv(serveOneSse, 18136, body, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const AssistantMessage msg = streamFromFake(18136);
        srv.join();
        check(msg.stopReason == "error", "silent truncation still errors");
        check(msg.errorMessage.find("Failed to parse tool call arguments as JSON") == 0 &&
                  msg.errorMessage.find(logPath) != std::string::npos,
              "error message keeps prefix + log pointer");
        ::unsetenv("MINIAGENT_LOG");
    }

    if (failures) {
        printf("toolargs_test: FAILED (%d)\n", failures);
        return 1;
    }
    printf("toolargs_test: passed\n");
    return 0;
}
