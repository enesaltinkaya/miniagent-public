// Retry + compaction tests.
//
// Unit checks for the error classifiers, token estimates and the
// deterministic vcc compactor (cut selection, summary marker, file refs),
// plus end-to-end agent runs against an in-process HTTP server (no real LLM
// needed):
//   1. transient server errors are retried (waits 0s, 2s, 3s) and the run
//      succeeds once the server recovers;
//   2. persistent errors stop the agent after 3 retries (4 attempts total);
//   3. deterministic errors (401) fail fast with no retry;
//   4. no preemptive compaction: a conversation past 90% of a known window
//      goes to the server in full; compaction happens only reactively;
//   5. with an unknown window, a context-overflow error compacts the
//      conversation and re-issues the turn reactively, and a second overflow
//      right after compaction stops the agent;
//   6. a connection-refused server (transport level) is retried with the same
//      visible Retry events, then the run stops with the terminal error.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/agent.h"
#include "agent/compaction.h"
#include "sessions/sessions.h"
#include <cassert>
#include "llm/openai.h"

using nlohmann::json;

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond)
        printf("  ok: %s\n", what);
    else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

static std::string filler(size_t n) {
    const std::string base = "Lorem ipsum dolor sit amet, consectetur adipiscing elit. ";
    std::string out;
    while (out.size() < n)
        out += base;
    return out.substr(0, n);
}

// ---------------------------------------------------------------------------
// In-process HTTP server (single-threaded, sequential requests - the agent
// issues its provider calls one at a time).
// ---------------------------------------------------------------------------

namespace {

size_t contentLengthOf(const std::string& headers) {
    const size_t p = headers.find("Content-Length:");
    if (p == std::string::npos)
        return 0;
    return std::strtoull(headers.c_str() + p + 15, nullptr, 10);
}

bool headerHas(const std::string& headers, const char* name) {
    std::string hay = headers;
    for (auto& c : hay)
        c = (char)tolower((unsigned char)c);
    return hay.find(name) != std::string::npos;
}

class TestServer {
public:
    using Handler = std::function<std::pair<int, std::string>(int requestIndex)>;
    Handler handler;

    TestServer() = default;
    ~TestServer() {
        stop();
    }

    void start() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral port
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
            throw std::runtime_error("bind failed");
        socklen_t len = sizeof(addr);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(listenFd_, 8);
        // accept() times out so the serve loop can observe `running_` and
        // exit; closing the fd from another thread while accept() blocks is
        // not portable, so the loop must poll.
        timeval tv{};
        tv.tv_usec = 100000;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        running_ = true;
        thread_ = std::thread([this] { serve(); });
    }

    void stop() {
        running_ = false;
        if (thread_.joinable())
            thread_.join();
        if (listenFd_ >= 0) {
            ::close(listenFd_);
            listenFd_ = -1;
        }
    }

    std::string url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }

    int requestCount() const {
        std::lock_guard lk(mu_);
        return requestIndex_;
    }

    std::string requestBody(size_t i) const {
        std::lock_guard lk(mu_);
        return i < bodies_.size() ? bodies_[i] : std::string();
    }

private:
    void serve() {
        for (;;) {
            if (!running_.load())
                break;
            int client = ::accept(listenFd_, nullptr, nullptr);
            if (client < 0)
                continue; // accept timeout / transient error; re-check running_
            handleOne(client);
        }
    }

    void sendAll(int fd, const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0)
                return;
            off += static_cast<size_t>(n);
        }
    }

    void handleOne(int client) {
        // Read the full request (headers + Content-Length body). curl sends
        // "Expect: 100-continue" for large bodies; answer it before the body
        // arrives or curl stalls for a second.
        std::string buf;
        char tmp[16384];
        size_t headerEnd = std::string::npos;
        size_t need = 0;
        for (;;) {
            ssize_t n = ::recv(client, tmp, sizeof(tmp), 0);
            if (n <= 0)
                break;
            buf.append(tmp, static_cast<size_t>(n));
            if (headerEnd == std::string::npos) {
                headerEnd = buf.find("\r\n\r\n");
                if (headerEnd != std::string::npos) {
                    need = contentLengthOf(buf.substr(0, headerEnd));
                    if (headerHas(buf.substr(0, headerEnd), "expect:"))
                        sendAll(client, "HTTP/1.1 100 Continue\r\n\r\n");
                }
            }
            if (headerEnd != std::string::npos && buf.size() >= headerEnd + 4 + need)
                break;
        }
        std::string body = (headerEnd != std::string::npos) ? buf.substr(headerEnd + 4) : std::string();

        std::pair<int, std::string> resp{500, "{}"};
        {
            std::lock_guard lk(mu_);
            bodies_.push_back(body);
            if (handler)
                resp = handler(requestIndex_);
            requestIndex_++;
        }

        std::string out = "HTTP/1.1 " + std::to_string(resp.first) + " X\r\n"
                          "Content-Type: " +
                          (resp.first == 200 ? "text/event-stream" : "application/json") + "\r\n"
                          "Content-Length: " + std::to_string(resp.second.size()) + "\r\n"
                          "Connection: close\r\n\r\n" +
                          resp.second;
        sendAll(client, out);
        ::close(client);
    }

    int listenFd_ = -1;
    int port_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    int requestIndex_ = 0;
    std::vector<std::string> bodies_;
};

std::string sseCompletion(const std::string& text, int promptTokens = 100, int completionTokens = 5) {
    json first = {{"choices", {{{"delta", {{"role", "assistant"}, {"content", text}}}}, {"index", 0}}}};
    json last = {{"choices", {{{"delta", json::object()}, {"finish_reason", "stop"}, {"index", 0}}}},
                 {"usage",
                  {{"prompt_tokens", promptTokens},
                   {"completion_tokens", completionTokens},
                   {"total_tokens", promptTokens + completionTokens}}}};
    return "data: " + first.dump() + "\n\n"
           "data: " + last.dump() + "\n\n"
           "data: [DONE]\n\n";
}

std::string sseFinish(const std::string& text, const std::string& finish, int promptTokens = 100,
                      int completionTokens = 5) {
    json first = {{"choices", {{{"delta", {{"role", "assistant"}, {"content", text}}}}, {"index", 0}}}};
    json last = {{"choices", {{{"delta", json::object()}, {"finish_reason", finish}, {"index", 0}}}},
                 {"usage",
                  {{"prompt_tokens", promptTokens},
                   {"completion_tokens", completionTokens},
                   {"total_tokens", promptTokens + completionTokens}}}};
    return "data: " + first.dump() + "\n\n"
           "data: " + last.dump() + "\n\n"
           "data: [DONE]\n\n";
}

std::string sseErrorText(const std::string& message) {
    json e = {{"error", {{"message", message}}}};
    return e.dump();
}

} // namespace

// ---------------------------------------------------------------------------
// Agent runner
// ---------------------------------------------------------------------------

struct Captured {
    std::vector<AgentEvent> events;
    std::vector<AssistantMessage> assistant; // every committed assistant message
    std::vector<UserMessage> user;           // every committed user message
    int retryEvents = 0;
    int compactionEvents = 0;
    int warningEvents = 0;
    std::vector<std::string> warningTexts;
    int errorEvents = 0;
    int agentEnds = 0;
    std::string lastError;
    std::vector<int> retryAttempts;
    std::vector<int> retryDelays;
    int tokensBefore = 0, tokensAfter = 0;
    std::string compactionText;
};

Captured runAgent(OpenAIProvider provider, std::vector<AgentMessage> history, const std::string& prompt) {
    Captured c;
    Agent agent("test system", provider, nullptr,
                [&](const AgentEvent& ev) {
                    c.events.push_back(ev);
                    switch (ev.type) {
                        case AgentEventType::Retry:
                            c.retryEvents++;
                            c.retryAttempts.push_back(ev.retryAttempt);
                            c.retryDelays.push_back(ev.retryDelayMs);
                            break;
                        case AgentEventType::Compaction:
                            c.compactionEvents++;
                            c.tokensBefore = ev.tokensBefore;
                            c.tokensAfter  = ev.tokensAfter;
                            c.compactionText = ev.text;
                            break;
                        case AgentEventType::Warning:
                            c.warningEvents++;
                            c.warningTexts.push_back(ev.text);
                            break;
                        case AgentEventType::Error:
                            c.errorEvents++;
                            c.lastError = ev.text;
                            break;
                        case AgentEventType::AgentEnd:
                            c.agentEnds++;
                            break;
                        default:
                            break;
                    }
                },
                [&](const AgentMessage& m) {
                    if (auto* a = std::get_if<AssistantMessage>(&m))
                        c.assistant.push_back(*a);
                    else if (auto* u = std::get_if<UserMessage>(&m))
                        c.user.push_back(*u);
                });
    agent.setHistory(std::move(history));
    UserMessage um;
    um.text = prompt;
    agent.run(std::move(um));
    return c;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

int main() {
    printf("== classifier unit tests ==\n");
    {
        check(isRetryableServerError("overloaded_error"), "overloaded_error retryable");
        check(isRetryableServerError("HTTP 503: {\"error\":{\"message\":\"server_error\"}}"),
              "503 server_error retryable");
        check(isRetryableServerError("HTTP 429: rate limit exceeded"), "429 retryable");
        check(isRetryableServerError("HTTP 500: internal server error"), "500 retryable");
        check(isRetryableServerError("HTTP 529: overloaded"), "529 retryable");
        check(isRetryableServerError("HTTP request failed: Connection refused (curl code 7)"),
              "connection refused retryable");
        check(isRetryableServerError("HTTP request failed: Operation timed out after 300000 ms"),
              "timeout retryable");
        check(isRetryableServerError("The socket connection was closed unexpectedly"),
              "socket close retryable");
        check(isRetryableServerError("Failed to parse tool call arguments as JSON"),
              "tool-call args parse failure retryable");
        check(!isRetryableServerError("HTTP 401: {\"error\":{\"message\":\"Invalid API key\"}}"),
              "401 not retryable");
        check(!isRetryableServerError("HTTP 400: invalid request body"), "400 not retryable");
        check(!isRetryableServerError("insufficient_quota: billing limit reached"), "quota not retryable");
        check(!isRetryableServerError("You've hit your monthly usage limit"), "usage limit not retryable");
        check(!isRetryableServerError(""), "empty not retryable");

        check(isContextOverflowError("prompt is too long: 213462 tokens > 200000 maximum"),
              "anthropic overflow detected");
        check(isContextOverflowError("HTTP 400: {\"error\":{\"message\":\"Your input exceeds the context window of this model\"}}"),
              "openai overflow detected");
        check(isContextOverflowError("This model's maximum prompt length is 131072 but the request contains 537812 tokens"),
              "xai overflow detected");
        check(isContextOverflowError("HTTP 413: {\"error\":{\"type\":\"request_too_large\"}}"),
              "413 request_too_large detected");
        check(isContextOverflowError("Input length (265330) exceeds model's maximum context length (262144)."),
              "openai-compatible overflow detected");
        check(isContextOverflowError("The input token count (1196265) exceeds the maximum number of tokens allowed"),
              "gemini overflow detected");
        check(!isContextOverflowError("overloaded_error"), "overload is not overflow");
        check(!isContextOverflowError("HTTP 429: rate limit - too many tokens, please wait"),
              "throttling excluded from overflow");
        check(!isContextOverflowError(""), "empty not overflow");
    }

    printf("== token estimate unit tests ==\n");
    {
        UserMessage u;
        u.text = "abcd";
        check(estimateMessageTokens(u) == 1, "user message chars/4");
        ToolResultMessage t;
        t.content = filler(8000);
        check(estimateMessageTokens(t) == 2000, "tool result chars/4");
        AssistantMessage a;
        a.text = filler(4000);
        ToolCallBlock tc{"id1", "read", {{"path", "/tmp/a"}}};
        a.toolCalls.push_back(tc);
        check(estimateMessageTokens(a) > 1000, "assistant text + tool call args");
    }

    printf("== vcc compactor unit tests ==\n");
    {
        // Too few messages: no compaction.
        {
            std::vector<AgentMessage> v;
            UserMessage u;
            u.text = "hi";
            v.push_back(u);
            AssistantMessage a;
            a.text = "hello";
            v.push_back(a);
            CompactionResult r = vccCompact(v, 1, true, 16000, 0);
            check(!r.ok, "tiny conversation not compacted");
        }
        // User-anchored cut: summarize everything before the last user turn.
        {
            std::vector<AgentMessage> v;
            for (int i = 1; i <= 10; ++i) {
                UserMessage u;
                u.text = (i == 1 ? std::string("Port the compactor.\n") : std::string()) +
                         "Question " + std::to_string(i) + ": " + filler(2000);
                v.push_back(u);
                AssistantMessage a;
                a.text = "Answer " + std::to_string(i) + ": " + filler(2000);
                a.stopReason = "stop";
                v.push_back(a);
            }
            CompactionResult r = vccCompact(v, 1, true, 0, 0);
            check(r.ok, "conversation compacted");
            check(v.size() < 20, "conversation shrank");
            check(isCompactionSummaryMessage(v[0]), "summary message at index 0");
            check(std::holds_alternative<UserMessage>(v[1]), "kept tail starts at a user message");
            check(r.keepStart > 0 && r.keepStart < 20, "keepStart in range");
            check(r.keepFromFileIndex == (size_t)r.keepStart + 1, "file index maps the cut (+1 base)");
            check(r.tokensBefore > r.tokensAfter && r.tokensAfter > 0, "tokens shrank");
            const std::string& summary = std::get<UserMessage>(v[0]).text;
            check(summary.find("[Session Goal]") != std::string::npos, "summary has goal section");
            check(summary.find("(#1)") != std::string::npos, "summary carries file-entry refs");
            check(summary.find("Question 1") != std::string::npos, "summary covers the oldest turn");
            check(std::holds_alternative<AssistantMessage>(v.back()), "kept tail retains the newest answer");
        }
        // Without session indexing no refs are emitted, keepFromFileIndex 0.
        {
            std::vector<AgentMessage> v;
            for (int i = 1; i <= 6; ++i) {
                UserMessage u;
                u.text = "Q" + std::to_string(i) + " " + filler(2000);
                v.push_back(u);
                AssistantMessage a;
                a.text = "A" + std::to_string(i) + " " + filler(2000);
                v.push_back(a);
            }
            CompactionResult r = vccCompact(v, 0, false, 0, 0);
            check(r.ok, "nested-style compaction works");
            check(r.keepFromFileIndex == 0, "no file index without a session");
            check(std::get<UserMessage>(v[0]).text.find("(#") == std::string::npos, "no refs without a session");
        }
        // A previous summary folds into the new one instead of being
        // re-summarized as a block.
        {
            std::vector<AgentMessage> v;
            UserMessage head;
            head.text = std::string(kCompactionSummaryPrefix()) + "\n[Session Goal]\n- Original goal";
            v.push_back(head);
            for (int i = 1; i <= 6; ++i) {
                UserMessage u;
                u.text = "Q" + std::to_string(i) + " " + filler(2000);
                v.push_back(u);
                AssistantMessage a;
                a.text = "A" + std::to_string(i) + " " + filler(2000);
                v.push_back(a);
            }
            CompactionResult r = vccCompact(v, 1, true, 0, 0);
            check(r.ok, "second compaction works");
            const std::string& summary = std::get<UserMessage>(v[0]).text;
            check(summary.find("Original goal") != std::string::npos, "previous summary folded in");
            check(r.keepFromFileIndex == (size_t)r.keepStart, "file index matches the cut");
        }
        // No user anchor: the budget cut still yields a valid boundary.
        {
            std::vector<AgentMessage> v;
            for (int i = 0; i < 8; ++i) {
                AssistantMessage a;
                a.text = "note " + std::to_string(i) + " " + filler(3000);
                a.stopReason = "stop";
                v.push_back(a);
                ToolResultMessage t;
                t.toolCallId = "c" + std::to_string(i);
                t.toolName = "read";
                t.content = filler(3000);
                v.push_back(t);
            }
            CompactionResult r = vccCompact(v, 1, true, 8192, 0);
            check(r.ok, "no-anchor conversation compacted via budget cut");
            check(std::holds_alternative<AssistantMessage>(v[1]), "budget-cut tail starts at an assistant message");
        }
        // Small window: brief budgets halve but compaction still succeeds.
        {
            std::vector<AgentMessage> v;
            for (int i = 1; i <= 10; ++i) {
                UserMessage u;
                u.text = "Question " + std::to_string(i) + ": " + filler(500);
                v.push_back(u);
                AssistantMessage a;
                a.text = "Answer " + std::to_string(i) + ": " + filler(500);
                v.push_back(a);
            }
            CompactionResult r = vccCompact(v, 1, true, 8192, 0);
            check(r.ok, "small-window compaction works");
            check(isCompactionSummaryMessage(v[0]), "small-window summary at index 0");
        }
        // Summary message marker round-trips.
        UserMessage sm;
        sm.text = std::string(kCompactionSummaryPrefix()) + "\n## Goal\nx";
        UserMessage plain;
        plain.text = "hi there";
        check(isCompactionSummaryMessage(sm), "summary message detected");
        check(!isCompactionSummaryMessage(plain), "plain user message not a summary");
    }

    printf("== e2e: transient errors are retried ==\n");
    {
        TestServer srv;
        srv.handler = [](int req) {
            if (req == 0)
                return std::make_pair(503, sseErrorText("overloaded_error"));
            if (req == 1)
                return std::make_pair(503, sseErrorText("server_error, please try again"));
            return std::make_pair(200, sseCompletion("Done.", 50, 3));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model = "test";
        Captured c = runAgent(p, {}, "hello");
        check(srv.requestCount() == 3, "three provider calls (initial + 2 retries)");
        check(c.retryEvents == 2, "two Retry events");
        check(c.retryDelays.size() == 2 && c.retryDelays[0] == 0 && c.retryDelays[1] == 2000,
              "waits are 0s then 2s");
        check(c.retryAttempts.size() == 2 && c.retryAttempts[0] == 1 && c.retryAttempts[1] == 2,
              "attempt numbers 1, 2");
        check(c.assistant.size() == 1, "only the successful message committed");
        check(!c.assistant.empty() && c.assistant.back().stopReason == "stop" &&
                  c.assistant.back().text == "Done.",
              "final response delivered");
        check(c.errorEvents == 0, "no terminal Error event");
        check(c.agentEnds == 1, "one AgentEnd");
    }

    printf("== e2e: persistent errors stop the agent after 3 retries ==\n");
    {
        TestServer srv;
        srv.handler = [](int) { return std::make_pair(503, sseErrorText("overloaded_error")); };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model = "test";
        Captured c = runAgent(p, {}, "hello");
        check(srv.requestCount() == 4, "initial call + 3 retries, then stop");
        check(c.retryEvents == 3, "three retries");
        check(c.retryDelays.size() == 3 && c.retryDelays[0] == 0 && c.retryDelays[1] == 2000 &&
                  c.retryDelays[2] == 3000,
              "waits are 0s, 2s, 3s");
        check(c.errorEvents == 1, "one terminal Error event");
        check(c.lastError.find("503") != std::string::npos, "terminal error carries the 503 message");
        check(c.assistant.size() == 1 && c.assistant.back().stopReason == "error",
              "terminal error committed to the conversation");
        check(c.agentEnds == 1, "one AgentEnd");
    }

    printf("== e2e: deterministic error fails fast ==\n");
    {
        TestServer srv;
        srv.handler = [](int) { return std::make_pair(401, sseErrorText("Invalid API key")); };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model = "test";
        Captured c = runAgent(p, {}, "hello");
        check(srv.requestCount() == 1, "no retry for a 401");
        check(c.retryEvents == 0, "no Retry events");
        check(c.errorEvents == 1 && c.lastError.find("401") != std::string::npos,
              "terminal 401 reported");
    }

    printf("== e2e: connection refused is retried with visible events ==\n");
    {
        // Point the provider at a freshly-closed loopback port so the request
        // fails at the transport level (connection refused). The agent-level
        // retry must kick in: Retry events flow to the UI log each time, then
        // the run stops with the terminal error.
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // ephemeral; we only need its number, then close
        ::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        socklen_t alen = sizeof(addr);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &alen);
        const int port = ntohs(addr.sin_port);
        ::close(fd);

        OpenAIProvider p;
        p.baseUrl = "http://127.0.0.1:" + std::to_string(port) + "/v1";
        p.model = "test";
        Captured c = runAgent(p, {}, "hello");
        check(c.retryEvents == 3, "connection refused retried 3 times");
        check(c.retryDelays.size() == 3 && c.retryDelays[0] == 0 && c.retryDelays[1] == 2000 &&
                  c.retryDelays[2] == 3000,
              "waits are 0s, 2s, 3s");
        check(c.errorEvents == 1 && c.lastError.find("HTTP request failed") != std::string::npos,
              "terminal error reports the failed request");
        check(c.assistant.size() == 1 && c.assistant.back().stopReason == "error",
              "terminal error committed to the conversation");
        check(c.agentEnds == 1, "one AgentEnd");
    }

    printf("== e2e: no preemptive compaction - the full history goes to the server ==\n");
    {
        // Compaction is reactive only: even past 90% of a known window the
        // turn request carries the whole conversation, and no compaction
        // event fires. (The vcc compactor runs only after an overflow error.)
        std::vector<AgentMessage> hist;
        for (int i = 1; i <= 30; ++i) {
            UserMessage u;
            u.text = (i == 1 ? std::string("Build the thing.\n") : std::string()) +
                     "Question " + std::to_string(i) + ": " + filler(1000);
            hist.push_back(u);
            AssistantMessage a;
            a.text = "Answer " + std::to_string(i) + ": " + filler(1000);
            a.stopReason = "stop";
            hist.push_back(a);
        }

        TestServer srv;
        srv.handler = [](int) {
            return std::make_pair(200, sseCompletion("Answer delivered.", 15000, 10));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model = "test";
        p.contextWindow = 16384;
        Captured c = runAgent(p, hist, "Continue the task.");

        check(srv.requestCount() == 1, "one turn call");
        check(c.compactionEvents == 0, "no compaction without an overflow error");
        check(c.errorEvents == 0, "no terminal Error event");
        check(!c.assistant.empty() && c.assistant.back().text == "Answer delivered.",
              "the turn ran on the full history");

        json j1 = json::parse(srv.requestBody(0));
        const auto& msgs = j1["messages"];
        const std::string all = json({{"m", msgs}}).dump();
        check(all.find("Question 1:") != std::string::npos, "oldest messages still present");
        check(all.find("Answer 30") != std::string::npos, "newest messages still present");
        check(msgs.back()["content"].get<std::string>() == "Continue the task.", "new prompt last");
    }

    printf("== e2e: overflow with unknown window -> reactive compaction -> second overflow stops ==\n");
    {
        // contextWindow = 0: the preemptive pass needs a known window, so
        // recovery happens only reactively after the server reports it. The
        // server overflows every request, so after one deterministic
        // compaction the re-issued turn still fails and the agent stops.
        std::vector<AgentMessage> hist;
        for (int i = 1; i <= 40; ++i) {
            UserMessage u;
            u.text = "Question " + std::to_string(i) + ": " + filler(1000);
            hist.push_back(u);
            AssistantMessage a;
            a.text = "Answer " + std::to_string(i) + ": " + filler(1000);
            a.stopReason = "stop";
            hist.push_back(a);
        }
        TestServer srv;
        srv.handler = [](int) {
            return std::make_pair(400, sseErrorText("prompt is too long: 45231 tokens > 16384 maximum"));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model = "test";
        Captured c = runAgent(p, hist, "Continue the task.");
        check(srv.requestCount() == 2, "overflowed turn + re-issued turn (overflowed again)");
        check(c.compactionEvents == 1, "exactly one compaction");
        check(c.compactionText.rfind("Context compacted: ", 0) == 0, "compaction event is the reactive kind");
        check(c.errorEvents == 1 && c.lastError.find("persists after compaction") != std::string::npos,
              "second overflow stops the agent with a clear error");
        check(c.assistant.size() == 1 && c.assistant.back().stopReason == "error",
              "terminal error committed");
    }

    printf("== e2e: compaction persists to the session and reloads projected ==\n");
    {
        system("rm -rf /tmp/miniagent_compact_e2e && mkdir -p /tmp/miniagent_compact_e2e");
        const std::string root = "/tmp/miniagent_compact_e2e/sessions";
        const std::string cwd  = "/tmp/miniagent_compact_e2e/proj";
        SessionStore store(cwd, root);
        std::string path = store.startSession("persist");
        assert(!path.empty());

        std::vector<AgentMessage> hist;
        for (int i = 1; i <= 30; ++i) {
            UserMessage u;
            u.text = (i == 1 ? std::string("Build the thing.\n") : std::string()) +
                     "Question " + std::to_string(i) + ": " + filler(1000);
            hist.push_back(u);
            AssistantMessage a;
            a.text = "Answer " + std::to_string(i) + ": " + filler(1000);
            a.stopReason = "stop";
            hist.push_back(a);
        }
        for (const auto& m : hist)
            store.appendMessage(m);

        TestServer srv;
        srv.handler = [](int req) {
            if (req == 0)
                return std::make_pair(400, sseErrorText("prompt is too long: 17231 tokens > 16384 maximum"));
            return std::make_pair(200, sseCompletion("Final answer.", 9000, 10));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model = "test";
        p.contextWindow = 16384;

        Captured c;
        Agent agent("test system", p, nullptr,
                    [&](const AgentEvent& ev) {
                        if (ev.type == AgentEventType::Compaction) {
                            c.compactionEvents++;
                            c.compactionText = ev.text;
                        }
                    },
                    [&](const AgentMessage& m) { store.appendMessage(m); },
                    [&](const CompactionRecord& r) { store.appendCompaction(r); });

        ProjectedSession proj0 = SessionStore::loadProjected(path);
        agent.setHistory(proj0.messages, proj0.fileBase, true);
        UserMessage prompt;
        prompt.text = "Continue the task.";
        agent.run(prompt);
        check(c.compactionEvents == 1, "one compaction during the run");
        check(!c.assistant.empty() || srv.requestCount() >= 1, "run reached the server");

        // The file keeps the FULL history plus one compaction entry plus the
        // messages committed after it.
        std::vector<AgentMessage> raw = SessionStore::loadMessages(path);
        check(raw.size() >= 61, "full history preserved on disk");
        ProjectedSession proj = SessionStore::loadProjected(path);
        check(proj.compacted, "projection applies the compaction");
        check(isCompactionSummaryMessage(proj.messages[0]), "reloaded conversation starts with the summary");
        check(proj.messages.size() < raw.size(), "projected conversation is much smaller than the file");
        check(proj.fileBase >= 1 && proj.fileBase <= raw.size(), "fileBase inside the file");
        const std::string& summaryText = std::get<UserMessage>(proj.messages[0]).text;
        check(summaryText.find("Build the thing") != std::string::npos, "goal survived into the reloaded summary");
        check(summaryText.find("recall") != std::string::npos, "recall note survives reload");
        check(summaryText.find("(#") != std::string::npos, "file refs survive reload");

        // The kept tail + post-compaction messages are the live window.
        bool sawPrompt = false;
        for (const auto& m : proj.messages)
            if (const auto* u = std::get_if<UserMessage>(&m); u && u->text == "Continue the task.")
                sawPrompt = true;
        check(sawPrompt, "the post-compaction prompt is live");
    }

    printf("== e2e: an output-limit stop continues the turn ==\n");
    {
        TestServer srv;
        srv.handler = [](int req) {
            if (req == 0)
                return std::make_pair(200, sseFinish("Alpha Beta Gamm", "length", 100, 16000));
            return std::make_pair(200, sseCompletion("ma rest of the answer.", 120, 40));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model   = "test";
        Captured c  = runAgent(p, {}, "hello");
        check(srv.requestCount() == 2, "the turn was re-issued after the length stop");
        check(c.warningEvents == 1 && !c.warningTexts.empty() &&
                  c.warningTexts[0].find("Output limit reached (16000 tokens) mid-reply") != std::string::npos &&
                  c.warningTexts[0].find("continuing where it stopped (1/8)") != std::string::npos,
              "one Warning event names the limit and the continuation");
        check(c.errorEvents == 0 && c.agentEnds == 1, "no terminal error, one clean AgentEnd");
        check(c.assistant.size() == 2 && c.assistant[0].stopReason == "length" &&
                  c.assistant[1].stopReason == "stop",
              "both halves committed, the second one completes the turn");
        check(c.user.size() == 2 && c.user[1].synthetic &&
                  c.user[1].text.find("output token limit") != std::string::npos,
              "a synthetic resume message was committed after the truncated reply");
        json j = json::parse(srv.requestBody(1));
        const auto& msgs = j["messages"];
        check(msgs.back()["role"].get<std::string>() == "user" &&
                  msgs.back()["content"].get<std::string>().find("Resume from the exact point") !=
                      std::string::npos,
              "the re-issued request ends with the resume instruction");
        bool sawTruncated = false;
        for (const auto& m : msgs)
            if (m["role"].get<std::string>() == "assistant" && m["content"].is_string() &&
                m["content"].get<std::string>() == "Alpha Beta Gamm")
                sawTruncated = true;
        check(sawTruncated, "the truncated reply stays in the history");
    }

    printf("== e2e: the continuation budget is bounded ==\n");
    {
        TestServer srv;
        srv.handler = [](int) {
            return std::make_pair(200, sseFinish("cut off ", "length", 100, 16000));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model   = "test";
        Captured c  = runAgent(p, {}, "hello");
        check(srv.requestCount() == 9, "initial turn + 8 continuations, then stop");
        check(c.warningEvents == 9, "8 continuation notes + 1 final note");
        check(c.warningTexts.size() == 9 &&
                  c.warningTexts[7].find("(8/8)") != std::string::npos &&
                  c.warningTexts[8].find("8 times in a row, the reply stays cut off") !=
                      std::string::npos,
              "the last note says the reply stays cut off");
        check(c.errorEvents == 0 && c.agentEnds == 1, "ends as a warning, not an error");
    }

    printf("== e2e: a length stop with no output does not continue ==\n");
    {
        TestServer srv;
        srv.handler = [](int) {
            return std::make_pair(200, sseFinish("", "length", 100, 0));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model   = "test";
        Captured c  = runAgent(p, {}, "hello");
        check(srv.requestCount() == 1, "no continuation for an empty length stop");
        check(c.warningEvents == 1 && !c.warningTexts.empty() &&
                  c.warningTexts[0].find("no output at all") != std::string::npos,
              "one Warning event explains why nothing was resumable");
        check(c.user.size() == 1, "no resume instruction injected");
        check(c.agentEnds == 1, "one AgentEnd");
    }

    printf("== e2e: a finish_reason \"error\" stop is an error, not a clean stop ==\n");
    {
        TestServer srv;
        srv.handler = [](int) {
            return std::make_pair(200, sseFinish("", "error", 100, 0));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model   = "test";
        Captured c  = runAgent(p, {}, "hello");
        check(srv.requestCount() == 4,
              "the finish_reason error is retried like a transient server error");
        check(c.retryEvents == 3, "one Retry event per retry attempt");
        check(c.errorEvents == 1 && c.agentEnds == 1, "the run ends in one terminal error");
        check(!c.assistant.empty() && c.assistant.back().stopReason == "error",
              "the committed message carries the error stop");
        check(c.assistant.back().errorMessage.find("finish_reason") != std::string::npos,
              "the error names the finish reason");
    }

    printf("== e2e: manual compaction queued mid-run runs at the turn boundary ==\n");
    {
        // The Compact button's path: requestCompaction() lands while the
        // first turn's tool call is still settling; the compaction runs at
        // the next turn boundary and the run continues on the rewritten
        // (smaller) conversation.
        std::vector<AgentMessage> hist;
        for (int i = 1; i <= 40; ++i) {
            UserMessage u;
            u.text = "Question " + std::to_string(i) + ": " + filler(1000);
            hist.push_back(u);
            AssistantMessage a;
            a.text = "Answer " + std::to_string(i) + ": " + filler(1000);
            a.stopReason = "stop";
            hist.push_back(a);
        }
        TestServer srv;
        srv.handler = [](int req) {
            if (req == 0)
                return std::make_pair(
                    200,
                    std::string("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                                "\"id\":\"c1\",\"type\":\"function\",\"function\":"
                                "{\"name\":\"noop\",\"arguments\":\"{}\"}}]}}]}\n\n") +
                        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n" +
                        "data: [DONE]\n\n");
            return std::make_pair(200, sseCompletion("Done.", 100, 5));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model = "test";
        p.contextWindow = 16384;

        Captured c;
        std::vector<AgentEventType> seq;
        Agent agent("test system", p, nullptr,
                    [&](const AgentEvent& ev) {
                        seq.push_back(ev.type);
                        switch (ev.type) {
                            case AgentEventType::Compaction:
                                c.compactionEvents++;
                                c.compactionText = ev.text;
                                c.tokensBefore   = ev.tokensBefore;
                                c.tokensAfter    = ev.tokensAfter;
                                break;
                            case AgentEventType::AgentEnd:
                                c.agentEnds++;
                                break;
                            default:
                                break;
                        }
                        // The UI's Compact button, pressed while the first
                        // turn is still running.
                        if (ev.type == AgentEventType::ToolCallEnd)
                            agent.requestCompaction();
                    });
        agent.setHistory(std::move(hist));
        UserMessage prompt;
        prompt.text = "Continue the task.";
        agent.run(prompt);

        check(srv.requestCount() == 2, "two turns: the tool-call turn, then the final one");
        check(c.compactionEvents == 1, "one manual compaction");
        check(c.compactionText.rfind("Manual compaction: ", 0) == 0,
              "compaction note is the manual kind");
        check(c.tokensBefore > c.tokensAfter, "context shrank");
        auto compactionAt = std::find(seq.begin(), seq.end(), AgentEventType::Compaction);
        check(compactionAt != seq.end() &&
                  std::count(seq.begin(), compactionAt, AgentEventType::TurnStart) == 1,
              "it ran at the turn boundary, between the two turns");
        json j = json::parse(srv.requestBody(1));
        const auto& msgs = j["messages"];
        bool summaryFirst = false;
        for (const auto& m : msgs)
            if (m["role"].get<std::string>() == "user") {
                summaryFirst = m["content"].get<std::string>().rfind(kCompactionSummaryPrefix(), 0) == 0;
                break;
            }
        check(summaryFirst, "the second request opens with the compaction summary");
        const std::string all = json({{"m", msgs}}).dump();
        check(all.find("Question 1:") == std::string::npos, "summarized prefix dropped");
        check(all.find("Question 40") != std::string::npos, "kept tail survives");
        check(c.agentEnds == 1 && c.errorEvents == 0, "the run continued and ended cleanly");
    }

    printf("== e2e: manual compaction queued during the last turn runs before the run ends ==\n");
    {
        // Queued while the final (tool-free) call streams: the run is about
        // to end, so the compaction runs "after the current run" - just
        // before AgentEnd - and answers even when there is nothing to cut.
        TestServer srv;
        srv.handler = [](int) { return std::make_pair(200, sseCompletion("All done.", 100, 5)); };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model = "test";

        std::vector<AgentEventType> seq;
        int compactions = 0;
        std::string note;
        Agent agent("test system", p, nullptr, [&](const AgentEvent& ev) {
            seq.push_back(ev.type);
            if (ev.type == AgentEventType::Compaction) {
                compactions++;
                note = ev.text;
            }
            if (ev.type == AgentEventType::TextDelta)
                agent.requestCompaction();
        });
        UserMessage um;
        um.text = "hello";
        agent.run(um);
        check(compactions == 1, "queued compaction ran");
        check(note.rfind("Manual compaction: nothing to compact", 0) == 0,
              "short conversation answers with the nothing-to-compact note");
        auto compactionAt = std::find(seq.begin(), seq.end(), AgentEventType::Compaction);
        auto endAt        = std::find(seq.begin(), seq.end(), AgentEventType::AgentEnd);
        check(compactionAt != seq.end() && endAt != seq.end() && compactionAt < endAt,
              "it ran after the last turn, before AgentEnd");
    }

    printf("== e2e: a shared (scaffold-role) compaction request compacts the live agent, "
           "not the next run ==\n");
    {
        // shareCompactionRequest is what a scaffold role agent gets: the
        // request is consumed at the live agent's turn boundary (its own
        // conversation shrinks) and - unlike the agent's own flag - is spent
        // with a note at the natural run end instead of compacting, because
        // a finished role's context is discarded anyway.
        std::vector<AgentMessage> hist;
        for (int i = 1; i <= 40; ++i) {
            UserMessage u;
            u.text = "Question " + std::to_string(i) + ": " + filler(1000);
            hist.push_back(u);
            AssistantMessage a;
            a.text = "Answer " + std::to_string(i) + ": " + filler(1000);
            a.stopReason = "stop";
            hist.push_back(a);
        }
        TestServer srv;
        srv.handler = [](int req) {
            if (req == 0)
                return std::make_pair(
                    200,
                    std::string("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                                "\"id\":\"c1\",\"type\":\"function\",\"function\":"
                                "{\"name\":\"noop\",\"arguments\":\"{}\"}}]}}]}\n\n") +
                        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n" +
                        "data: [DONE]\n\n");
            return std::make_pair(200, sseCompletion("Done.", 100, 5));
        };
        srv.start();
        OpenAIProvider p;
        p.baseUrl = srv.url();
        p.model = "test";
        p.contextWindow = 16384;

        std::atomic<bool> shared{false};
        Captured c;
        std::vector<AgentEventType> seq;
        Agent agent("role system", p, nullptr, [&](const AgentEvent& ev) {
            seq.push_back(ev.type);
            if (ev.type == AgentEventType::Compaction) {
                c.compactionEvents++;
                c.compactionText = ev.text;
                c.tokensBefore = ev.tokensBefore;
                c.tokensAfter = ev.tokensAfter;
            }
            if (ev.type == AgentEventType::AgentEnd)
                c.agentEnds++;
            // The Compact button pressed while the role's first turn runs.
            if (ev.type == AgentEventType::ToolCallEnd)
                shared.store(true);
        });
        agent.shareCompactionRequest(shared);
        agent.setHistory(std::move(hist));
        UserMessage prompt;
        prompt.text = "Continue the task.";
        agent.run(prompt);

        check(srv.requestCount() == 2, "role agent: two turns, run continued after compacting");
        check(c.compactionEvents == 1, "role agent: one shared-request compaction");
        check(c.compactionText.rfind("Manual compaction: ", 0) == 0,
              "role agent: note is the manual kind");
        check(c.tokensBefore > c.tokensAfter, "role agent: its own conversation shrank");
        json j = json::parse(srv.requestBody(1));
        const auto& msgs = j["messages"];
        bool summaryFirst = false;
        for (const auto& m : msgs)
            if (m["role"].get<std::string>() == "user") {
                summaryFirst = m["content"].get<std::string>().rfind(kCompactionSummaryPrefix(), 0) == 0;
                break;
            }
        check(summaryFirst, "role agent: second request opens with the summary");
        check(!shared.load(), "role agent: the shared flag was consumed");

        // Natural-end semantics: a request raised during the FINAL turn is
        // spent with a note (no pointless compaction of a dying context).
        std::atomic<bool> late{false};
        int lateNotes = 0;
        std::string lateText;
        TestServer srv2;
        srv2.handler = [](int) { return std::make_pair(200, sseCompletion("All done.", 100, 5)); };
        srv2.start();
        OpenAIProvider p2;
        p2.baseUrl = srv2.url();
        p2.model = "test";
        Agent agent2("role system", p2, nullptr, [&](const AgentEvent& ev) {
            if (ev.type == AgentEventType::TextDelta)
                late.store(true);
            if (ev.type == AgentEventType::Compaction) {
                lateNotes++;
                lateText = ev.text;
            }
        });
        agent2.shareCompactionRequest(late);
        UserMessage last;
        last.text = "hello";
        agent2.run(last);
        check(lateNotes == 1 && lateText.find("nothing to compact") != std::string::npos &&
                  lateText.find("already finished") != std::string::npos,
              "role agent: a late request is spent with a note, not a compaction");
        check(!late.load(), "role agent: late shared flag consumed");
    }

    printf("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures,
           failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
