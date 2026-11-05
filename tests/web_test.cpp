// Tests for the web tools (tools/web.cpp) and the http options they rely on
// (util/http.cpp): HTML-to-text reduction, entity decoding, SearXNG result
// formatting, and both tools driven end to end against an in-process HTTP
// server (search JSON, redirect chain, byte cap, content-type handling).
#include "tools/factories.h"
#include "tools/web.h"
#include "util/http.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <sys/time.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static void checkContains(const std::string& hay, const char* needle, const char* what) {
    if (hay.find(needle) == std::string::npos) {
        std::printf("FAIL: %s (missing \"%s\")\n  got: %.400s\n", what, needle, hay.c_str());
        ++failures;
    }
}

namespace {

std::atomic<bool> noAbort{false};

// ---------------------------------------------------------------------------
// In-process HTTP server (single-threaded, sequential; the tools issue one
// request at a time). Handlers see the request target and the headers.
// ---------------------------------------------------------------------------

struct Response {
    int status               = 200;
    std::string contentType  = "text/html; charset=utf-8";
    std::string body;
    std::string location; // non-empty => 3xx redirect
};

class TestServer {
public:
    std::function<Response(const std::string& target)> handler;

    ~TestServer() {
        stop();
    }

    void start() {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int yes   = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::printf("FAIL: test server bind failed\n");
            ++failures;
            return;
        }
        socklen_t len = sizeof(addr);
        ::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        ::listen(listenFd_, 8);
        timeval tv{};
        tv.tv_usec = 100000;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        running_ = true;
        thread_  = std::thread([this] { serve(); });
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
        return "http://127.0.0.1:" + std::to_string(port_);
    }

    std::vector<std::string> targets() const {
        std::lock_guard lk(mu_);
        return targets_;
    }

    std::string lastUserAgent() const {
        std::lock_guard lk(mu_);
        return userAgent_;
    }

private:
    void serve() {
        while (running_.load()) {
            int client = ::accept(listenFd_, nullptr, nullptr);
            if (client < 0)
                continue;
            handleOne(client);
        }
    }

    void sendAll(int fd, const std::string& data) {
        size_t off = 0;
        while (off < data.size()) {
            ssize_t n = ::send(fd, data.data() + off, data.size() - off, MSG_NOSIGNAL);
            if (n <= 0)
                return; // the client stopped reading early (byte cap): expected
            off += static_cast<size_t>(n);
        }
    }

    void handleOne(int client) {
        std::string buf;
        char tmp[16384];
        for (;;) {
            ssize_t n = ::recv(client, tmp, sizeof(tmp), 0);
            if (n <= 0)
                break;
            buf.append(tmp, static_cast<size_t>(n));
            if (buf.find("\r\n\r\n") != std::string::npos)
                break;
        }
        std::string target;
        std::string userAgent;
        {
            std::istringstream ss(buf.substr(0, buf.find("\r\n\r\n")));
            std::string line;
            while (std::getline(ss, line)) {
                if (line.size() > 4 && line.compare(0, 4, "GET ") == 0) {
                    size_t sp = line.find(' ', 4);
                    target    = line.substr(4, sp - 4);
                } else if (line.size() > 13 && strncasecmp(line.c_str(), "user-agent:", 11) == 0) {
                    userAgent = line.substr(11);
                    userAgent.erase(0, userAgent.find_first_not_of(" \t"));
                }
            }
        }
        Response resp{500, "text/plain", "unhandled", ""};
        {
            std::lock_guard lk(mu_);
            targets_.push_back(target);
            userAgent_ = userAgent;
        }
        if (handler)
            resp = handler(target);

        std::string head = "HTTP/1.1 " + std::to_string(resp.status) + " X\r\n";
        if (!resp.contentType.empty())
            head += "Content-Type: " + resp.contentType + "\r\n";
        if (!resp.location.empty())
            head += "Location: " + resp.location + "\r\n";
        head += "Content-Length: " + std::to_string(resp.body.size()) + "\r\n"
                "Connection: close\r\n\r\n";
        sendAll(client, head + resp.body);
        ::close(client);
    }

    int listenFd_ = -1;
    int port_     = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mu_;
    std::vector<std::string> targets_;
    std::string userAgent_;
};

std::string searchPayload() {
    return R"({"query":"sdl3 imgui","results":[
        {"url":"https://example.com/a","title":"Alpha Page","content":"First  snippet\nwith newline"},
        {"url":"https://example.com/b","title":"Beta Page","content":"Second snippet"},
        {"url":"https://example.com/a","title":"Duplicate","content":"dup"},
        {"url":"https://example.com/c","title":"Gamma Page","content":null},
        {"title":"no url at all","content":"skipped"}
    ],"suggestions":["sdl3 gpu","imgui sdl renderer"]})";
}

// ---------------------------------------------------------------------------
// Unit: helpers
// ---------------------------------------------------------------------------

void testPercentEncode() {
    check(percentEncodeQuery("hello world") == "hello+world", "spaces become +");
    check(percentEncodeQuery("a-b_c.d~e") == "a-b_c.d~e", "unreserved kept");
    check(percentEncodeQuery("c++") == "c%2B%2B", "plus encoded");
    check(percentEncodeQuery("caf\xc3\xa9") == "caf%C3%A9", "utf-8 percent encoded");
    check(percentEncodeQuery("a/b?x=1&y") == "a%2Fb%3Fx%3D1%26y", "punctuation encoded");
}

void testSearchUrl() {
    const std::string u = searxngSearchUrl("http://localhost:7070/", "sdl imgui", 2);
    check(u == "http://localhost:7070/search?format=json&language=all&pageno=2&safesearch=0&q=sdl+imgui",
          "search url shape and trailing slash");
    check(searxngSearchUrl("http://h:1", "q", 0).find("pageno=1") != std::string::npos,
          "page clamped to 1");
}

void testHtmlToText() {
    const std::string html = R"(<!DOCTYPE html>
<html><head><title>Title Here</title><style>p{color:red}</style></head>
<body>
  <h1>Heading</h1>
  <p>Hello &amp; welcome&#160;all &lt;b&gt;bold&lt;&#47;b&gt;</p>
  <script>var x = "<p>not text</p>";</script>
  <ul><li>one</li><li>two</li></ul>
  <!-- a comment -->
  <div>Line&#8212;dash</div>
  <pre>int  main() {
    return  0;
}</pre>
  <p>Caf&eacute; &nbsp; done</p>
</body></html>)";
    const std::string text = htmlToText(html);
    checkContains(text, "Heading", "heading kept");
    checkContains(text, "Hello & welcome all <b>bold</b>", "entities decoded");
    checkContains(text, "- one", "li becomes a bullet");
    checkContains(text, "- two", "second li");
    checkContains(text, "Line\u2014dash", "numeric entity (mdash)");
    checkContains(text, "Caf\u00e9", "named latin-1 entity");
    check(text.find("color:red") == std::string::npos, "style content dropped");
    check(text.find("not text") == std::string::npos, "script content dropped");
    check(text.find("a comment") == std::string::npos, "comment dropped");
    check(text.find("Title Here") == std::string::npos, "head dropped");
    check(text.find("int  main() {\n    return  0;\n}") != std::string::npos,
          "pre keeps its whitespace");
    check(text.find("\n\n\n") == std::string::npos, "no run of blank lines");
    check(!text.empty() && text.front() != '\n' && text.back() != '\n', "outer blank lines trimmed");

    check(htmlToText("plain text, no tags") == "plain text, no tags", "tagless passthrough");
    check(htmlToText("<p>a & b</p>") == "a & b", "stray ampersand kept");
    check(htmlToText("a<br>b") == "a\nb", "br splits lines");
    check(htmlToText("") == "", "empty input");
    check(htmlToText("<div><span>x</span></div>") == "x", "inline tags dropped");
    check(htmlToText("&#x1F600;") == "\xF0\x9F\x98\x80", "hex codepoint entity");
    check(htmlToText("&notanentity;") == "&notanentity;", "unknown entity kept verbatim");
}

void testBinaryProbe() {
    check(!looksBinaryBytes("hello"), "text is not binary");
    check(looksBinaryBytes(std::string("hel\x00lo", 6)), "NUL means binary");
}

void testFormatSearchResults() {
    const nlohmann::json payload = nlohmann::json::parse(searchPayload());
    const std::string all        = formatSearchResults(payload, 10);
    checkContains(all, "1. Alpha Page", "numbered first result");
    checkContains(all, "https://example.com/a", "url shown");
    checkContains(all, "First snippet with newline", "snippet flattened");
    checkContains(all, "2. Beta Page", "second result");
    checkContains(all, "3. Gamma Page", "null content tolerated");
    check(all.find("Duplicate") == std::string::npos, "duplicate url dropped");
    check(all.find("no url at all") == std::string::npos, "url-less entry skipped");
    checkContains(all, "Related searches: sdl3 gpu, imgui sdl renderer", "suggestions listed");

    const std::string limited = formatSearchResults(payload, 2);
    check(limited.find("Gamma Page") == std::string::npos, "limit respected");

    check(formatSearchResults(nlohmann::json::parse(R"({"results":[]})"), 5) == "",
          "empty results give empty text");
    check(formatSearchResults(nlohmann::json::parse("{}"), 5) == "", "missing key tolerated");
    check(formatSearchResults(nlohmann::json::parse(R"({"results":"junk"})"), 5) == "",
          "wrong type tolerated");
    const std::string answered =
        formatSearchResults(nlohmann::json::parse(R"({"answers":["42 is the answer"],"results":[]})"), 5);
    checkContains(answered, "Answer: 42 is the answer", "answers surfaced");
}

// ---------------------------------------------------------------------------
// End to end: the tools against TestServer
// ---------------------------------------------------------------------------

void testWebSearchTool(TestServer& server) {
    server.handler = [](const std::string& target) -> Response {
        if (target.find("/forbidden") != std::string::npos)
            return {403, "text/html", "Access is forbidden. Enable JSON format.", ""};
        if (target.find("/notjson") != std::string::npos)
            return {200, "text/html", "<html><body>nope</body></html>", ""};
        if (target.find("/noresults") != std::string::npos)
            return {200, "application/json", R"({"query":"q","results":[]})", ""};
        if (target.find("format=json") != std::string::npos)
            return {200, "application/json", searchPayload(), ""};
        return {404, "text/plain", "nope", ""};
    };

    Tool search = makeWebSearchTool(server.url());
    check(search.name == "web_search", "tool name");

    nlohmann::json args = nlohmann::json::object();
    args["query"]       = "sdl imgui";
    ToolResult r        = search.execute(args, noAbort);
    check(!r.isError, "search succeeds");
    checkContains(r.content, "1. Alpha Page", "search result rendered");
    checkContains(r.content, "Results for \"sdl imgui\"", "query echoed");
    check(server.lastUserAgent().find("miniagent") != std::string::npos, "user agent sent");
    const auto targets = server.targets();
    check(!targets.empty() && targets.back().find("q=sdl+imgui") != std::string::npos,
          "query percent encoded in the request");

    nlohmann::json paged = nlohmann::json::object();
    paged["query"]       = "x";
    paged["page"]        = 3;
    paged["limit"]       = 1;
    ToolResult rp        = search.execute(paged, noAbort);
    check(!rp.isError, "paged search succeeds");
    check(server.targets().back().find("pageno=3") != std::string::npos, "page passed through");
    check(rp.content.find("Beta Page") == std::string::npos, "limit applied");

    ToolResult missing = search.execute(nlohmann::json::object(), noAbort);
    check(missing.isError, "missing query is an error");
    checkContains(missing.content, "query", "missing query names the argument");

    Tool noResults  = makeWebSearchTool(server.url() + "/noresults");
    ToolResult empty = noResults.execute([] {
        nlohmann::json a = nlohmann::json::object();
        a["query"]       = "empty";
        return a;
    }(), noAbort);
    check(!empty.isError, "no results is not an error");
    checkContains(empty.content, "No results for \"empty\"", "empty result reported");

    // Non-JSON and 403 replies both point at SearXNG's settings.yml.
    Tool badBase = makeWebSearchTool(server.url() + "/notjson");
    ToolResult nj = badBase.execute([] {
        nlohmann::json a = nlohmann::json::object();
        a["query"]       = "q";
        return a;
    }(), noAbort);
    check(nj.isError, "non-JSON reply is an error");
    checkContains(nj.content, "not JSON", "non-JSON explained");
    checkContains(nj.content, "search.formats", "settings.yml hint");

    Tool forbidden = makeWebSearchTool(server.url() + "/forbidden");
    ToolResult fr   = forbidden.execute([] {
        nlohmann::json a = nlohmann::json::object();
        a["query"]       = "q";
        return a;
    }(), noAbort);
    check(fr.isError, "403 is an error");
    checkContains(fr.content, "403", "status reported");

    Tool dead = makeWebSearchTool("http://127.0.0.1:1");
    ToolResult dr = dead.execute([] {
        nlohmann::json a = nlohmann::json::object();
        a["query"]       = "q";
        return a;
    }(), noAbort);
    check(dr.isError, "unreachable instance is an error");
    checkContains(dr.content, "searxng_url", "settings hint on connection failure");

    std::atomic<bool> abort{true};
    ToolResult ar = search.execute(args, abort);
    check(ar.isError, "aborted search is an error");
}

void testWebFetchTool(TestServer& server) {
    const std::string page =
        "<html><head><title>T</title></head><body><h1>Doc</h1><p>Alpha &amp; Beta</p>"
        "<script>hidden()</script><pre>keep  me</pre></body></html>";
    server.handler = [&page](const std::string& target) -> Response {
        if (target == "/page")
            return {200, "text/html; charset=utf-8", page, ""};
        if (target == "/plain")
            return {200, "text/plain", "raw\ttext\nsecond line", ""};
        if (target == "/json")
            return {200, "application/json", "{\"a\":1}", ""};
        if (target == "/redir")
            return {302, "text/html", "redirecting", "/page"};
        if (target == "/gone")
            return {404, "text/html", "<html><body>not found</body></html>", ""};
        if (target == "/pdf")
            return {200, "application/pdf", "%PDF-1.4 binary", ""};
        if (target == "/typeless")
            return {200, "", std::string("bin\x00ary", 8), ""};
        if (target == "/big")
            return {200, "text/plain", std::string(5 * 1024 * 1024, 'x'), ""};
        if (target == "/slowheaders")
            return {200, "text/html", "<html><body>ok</body></html>", ""};
        return {404, "text/plain", "nope", ""};
    };

    Tool fetch = makeWebFetchTool();
    check(fetch.name == "web_fetch", "tool name");

    auto fetchUrl = [&](const std::string& path) {
        nlohmann::json a = nlohmann::json::object();
        a["url"]         = server.url() + path;
        return fetch.execute(a, noAbort);
    };

    ToolResult html = fetchUrl("/page");
    check(!html.isError, "html fetch succeeds");
    checkContains(html.content, "Doc", "heading extracted");
    checkContains(html.content, "Alpha & Beta", "entities decoded");
    checkContains(html.content, "keep  me", "pre whitespace kept");
    check(html.content.find("hidden()") == std::string::npos, "script dropped");
    checkContains(html.content, "text/html", "content type reported");

    ToolResult plain = fetchUrl("/plain");
    check(!plain.isError, "plain text fetch succeeds");
    checkContains(plain.content, "raw\ttext", "plain body passed through");

    ToolResult json = fetchUrl("/json");
    check(!json.isError, "json fetch succeeds");
    checkContains(json.content, "{\"a\":1}", "json body passed through");

    ToolResult redir = fetchUrl("/redir");
    check(!redir.isError, "redirect followed");
    checkContains(redir.content, "redirected", "redirect reported");
    checkContains(redir.content, "/page", "effective url reported");
    checkContains(redir.content, "Doc", "final body used");

    ToolResult gone = fetchUrl("/gone");
    check(gone.isError, "404 is an error");
    checkContains(gone.content, "404", "status reported");

    ToolResult pdf = fetchUrl("/pdf");
    check(pdf.isError, "pdf refused");
    checkContains(pdf.content, "application/pdf", "content type named");

    ToolResult typeless = fetchUrl("/typeless");
    check(typeless.isError, "typeless binary refused");
    checkContains(typeless.content, "binary", "binary explained");

    ToolResult big = fetchUrl("/big");
    check(!big.isError, "oversized body still succeeds");
    checkContains(big.content, "capped", "byte cap reported");
    check(big.content.size() < 60 * 1024, "result stays inside the context budget");

    ToolResult scheme = fetch.execute([] {
        nlohmann::json a = nlohmann::json::object();
        a["url"]         = "file:///etc/passwd";
        return a;
    }(), noAbort);
    check(scheme.isError, "non-http scheme refused");
    checkContains(scheme.content, "http://", "scheme hint in the error");

    check(fetch.execute(nlohmann::json::object(), noAbort).isError, "missing url is an error");

    std::atomic<bool> abort{true};
    nlohmann::json a = nlohmann::json::object();
    a["url"]         = server.url() + "/page";
    check(fetch.execute(a, abort).isError, "aborted fetch is an error");
}

void testHttpOptions(TestServer& server) {
    server.handler = [](const std::string& target) -> Response {
        if (target == "/redir")
            return {302, "text/html", "", "/page"};
        return {200, "text/html; charset=utf-8", "<html><body>final</body></html>", ""};
    };

    // httpGet (no options) keeps the historical behaviour: a redirect is a
    // 302 with the redirect body, not the target page.
    {
        std::string body;
        HttpResult res;
        httpGet(server.url() + "/redir", {}, [&](const char* d, size_t n) { body.append(d, n); }, noAbort,
                res);
        check(res.statusCode == 302, "httpGet does not follow redirects");
        check(res.contentType == "text/html", "content type captured");
    }
    {
        std::string body;
        HttpResult res;
        httpGetEx(server.url() + "/redir", {}, [&](const char* d, size_t n) { body.append(d, n); },
                  noAbort, res, HttpGetOpts{true, 20, 0});
        check(res.statusCode == 200, "httpGetEx follows redirects");
        check(res.effectiveUrl.find("/page") != std::string::npos, "effective url reported");
        checkContains(body, "final", "followed body delivered");
    }
    {
        std::string body;
        HttpResult res;
        httpGetEx(server.url() + "/page", {}, [&](const char* d, size_t n) { body.append(d, n); },
                  noAbort, res, HttpGetOpts{false, 20, 10});
        check(res.truncated, "byte cap marks truncation");
        check(res.statusCode == 200, "byte cap keeps the status");
        check(res.error.empty(), "byte cap is not an error");
        check(body.size() <= 10, "byte cap enforced");
    }
    {
        std::string body;
        HttpResult res;
        httpGetEx("http://127.0.0.1:1/page", {}, [&](const char* d, size_t n) { body.append(d, n); },
                  noAbort, res, HttpGetOpts{true, 5, 1024});
        check(!res.error.empty(), "connection failure reported");
    }
}

} // namespace

int main() {
    testPercentEncode();
    testSearchUrl();
    testHtmlToText();
    testBinaryProbe();
    testFormatSearchResults();

    {
        TestServer server;
        server.start();
        testWebSearchTool(server);
        testWebFetchTool(server);
        testHttpOptions(server);
        server.stop();
    }

    if (failures == 0) {
        std::printf("web_test: all checks passed\n");
        return 0;
    }
    std::printf("web_test: %d failure(s)\n", failures);
    return 1;
}
