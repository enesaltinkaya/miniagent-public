// Abort-latency tests for httpPostStream/httpGet (util/http.cpp).
// The transfer loop runs on the curl multi interface so the abort flag is
// polled on a timer: Escape must end a request even when the server is
// silent (a reasoning model sends no bytes before its first token, which the
// old write-callback-only check could not interrupt).
#include "util/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static double secsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Minimal one-shot HTTP server on an ephemeral port.
class FakeServer {
public:
    explicit FakeServer(std::function<void(int)> handler) : handler_(std::move(handler)) {
        listenFd = ::socket(AF_INET, SOCK_STREAM, 0);
        int on = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        bind(listenFd, (sockaddr*)&addr, sizeof(addr));
        socklen_t len = sizeof(addr);
        getsockname(listenFd, (sockaddr*)&addr, &len);
        port = ntohs(addr.sin_port);
        listen(listenFd, 4);
        thread_ = std::thread([this] {
            while (!stop_.load()) {
                int fd = ::accept(listenFd, nullptr, nullptr);
                if (fd < 0) break;
                char buf[4096];
                recv(fd, buf, sizeof(buf), 0); // drain the request, content unused
                handler_(fd);
                ::close(fd);
            }
        });
    }

    ~FakeServer() {
        stop_ = true;
        ::shutdown(listenFd, SHUT_RDWR);
        ::close(listenFd);
        if (thread_.joinable()) thread_.join();
    }

    int getPort() const { return port; }

private:
    std::function<void(int)> handler_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    int listenFd = -1;
    int port     = 0;
};

static std::string urlFor(int port) {
    return "http://127.0.0.1:" + std::to_string(port) + "/v1/chat/completions";
}

int main() {
    signal(SIGPIPE, SIG_IGN);

    { // a normal streamed response still works
        FakeServer server([](int fd) {
            const char body[] = "data: hi\n\ndata: [DONE]\n\n";
            std::string head = std::string("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: ") +
                               std::to_string(sizeof(body) - 1) + "\r\n\r\n";
            send(fd, head.data(), head.size(), MSG_NOSIGNAL);
            send(fd, body, sizeof(body) - 1, MSG_NOSIGNAL);
        });
        std::string got;
        std::atomic<bool> abort{false};
        HttpResult res;
        httpPostStream(urlFor(server.getPort()), {{"Accept", "text/event-stream"}}, "{}",
                       [&](const char* d, size_t n) { got.append(d, n); }, abort, res);
        check(res.error.empty(), "stream: no transport error");
        check(res.statusCode == 200, "stream: status 200");
        check(!res.aborted, "stream: not aborted");
        check(got.find("[DONE]") != std::string::npos, "stream: chunks delivered");
    }

    { // a plain GET (listModels path) still completes
        FakeServer server([](int fd) {
            const char body[] = "{\"data\":[]}";
            std::string head = std::string("HTTP/1.1 200 OK\r\nContent-Length: ") +
                               std::to_string(sizeof(body) - 1) + "\r\n\r\n";
            send(fd, head.data(), head.size(), MSG_NOSIGNAL);
            send(fd, body, sizeof(body) - 1, MSG_NOSIGNAL);
        });
        std::string got;
        std::atomic<bool> abort{false};
        HttpResult res;
        httpGet(urlFor(server.getPort()), {}, [&](const char* d, size_t n) { got.append(d, n); }, abort, res);
        check(res.error.empty(), "get: no transport error");
        check(res.statusCode == 200, "get: status 200");
        check(got == "{\"data\":[]}", "get: full body received");
    }

    { // silent server (no bytes after the request): abort lands fast
        FakeServer server([](int fd) {
            (void)fd;
            std::this_thread::sleep_for(std::chrono::seconds(5)); // never responds
        });
        std::atomic<bool> abort{false};
        HttpResult res;
        std::thread caller([&] {
            httpPostStream(urlFor(server.getPort()), {}, "{}",
                           [&](const char*, size_t) {}, abort, res);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto marked = std::chrono::steady_clock::now();
        abort = true;
        caller.join();
        double sinceAbort = secsSince(marked);
        std::printf("abort while silent: returned %.2fs after the flag\n", sinceAbort);
        check(res.aborted, "silent: result marked aborted");
        check(res.statusCode == 0, "silent: no status code");
        check(res.error.empty(), "silent: no transport error reported");
        check(sinceAbort < 1.0, "silent: abort lands within a second of the flag");
    }

    { // abort mid-stream, while bytes are still flowing
        FakeServer server([](int fd) {
            const char head[] = "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\n\r\n";
            send(fd, head, sizeof(head) - 1, MSG_NOSIGNAL);
            for (int i = 0; i < 50; ++i) {
                const char chunk[] = "data: chunk\n\n";
                if (send(fd, chunk, sizeof(chunk) - 1, MSG_NOSIGNAL) < 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        std::atomic<bool> abort{false};
        std::atomic<int> chunks{0};
        HttpResult res;
        std::thread caller([&] {
            httpPostStream(urlFor(server.getPort()), {}, "{}",
                           [&](const char*, size_t) { ++chunks; }, abort, res);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        auto marked = std::chrono::steady_clock::now();
        abort = true;
        caller.join();
        double sinceAbort = secsSince(marked);
        std::printf("abort mid-stream: returned %.2fs after the flag\n", sinceAbort);
        check(res.aborted, "mid-stream: result marked aborted");
        check(chunks.load() > 0, "mid-stream: some chunks arrived first");
        check(sinceAbort < 1.0, "mid-stream: abort lands within a second of the flag");
    }

    { // connection refused is still a plain transport error
        int deadPort = 1; // nothing listens here
        FakeServer probe([](int) {});
        deadPort = probe.getPort() + 1;
        while (true) {
            int fd = ::socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a{};
            a.sin_family      = AF_INET;
            a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            a.sin_port        = htons((uint16_t)deadPort);
            if (connect(fd, (sockaddr*)&a, sizeof(a)) != 0) { ::close(fd); break; }
            ::close(fd);
            ++deadPort;
        }
        std::atomic<bool> abort{false};
        HttpResult res;
        httpGet(urlFor(deadPort), {}, [&](const char*, size_t) {}, abort, res);
        check(!res.aborted, "refused: not marked aborted");
        check(!res.error.empty(), "refused: transport error reported");
        check(res.statusCode == 0, "refused: status 0");
    }

    std::printf(failures ? "FAILURES: %d\n" : "OK\n", failures);
    return failures ? 1 : 0;
}
