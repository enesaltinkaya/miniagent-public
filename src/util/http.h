#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

struct HttpResult {
    long statusCode = 0;
    std::string error; // non-empty on transport error
    bool aborted = false;
    // Response Content-Type, lowercased and parameters stripped ("text/html");
    // empty when the response carried none. Captured for the last response of
    // a redirect chain.
    std::string contentType;
    // URL the body actually came from (equals the request URL unless
    // followRedirects was on). Lets a caller report a cross-host redirect.
    std::string effectiveUrl;
    // Set when the body was cut off at HttpGetOpts::maxBodyBytes: the transfer
    // ended early on purpose, so statusCode still carries the HTTP status.
    bool truncated = false;
};

// Extra knobs for plain GETs (the web tools). Every default preserves the
// historical behaviour of httpGet/httpPostStream: no redirect following, no
// total timeout (only the connect timeout), unlimited body.
struct HttpGetOpts {
    bool followRedirects = false;
    int totalTimeoutSec  = 0;  // 0 = no limit
    size_t maxBodyBytes  = 0;  // 0 = unlimited
};

// Blocking HTTP POST with streaming response delivery.
// onChunk receives raw body bytes as they arrive. If abortFlag is set, the
// request is aborted mid-stream and the result is marked aborted.
// On success (transport level), statusCode is the HTTP status code.
// On transport failure (connection refused, DNS, etc.), error is set and
// statusCode is 0.
void httpPostStream(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    const std::function<void(const char* data, size_t len)>& onChunk,
    const std::atomic<bool>& abortFlag,
    HttpResult& out
);

// Blocking HTTP GET (no body). Same semantics as httpPostStream.
void httpGet(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::function<void(const char* data, size_t len)>& onChunk,
    const std::atomic<bool>& abortFlag,
    HttpResult& out
);

// Blocking GET with the options arbitrary web content needs: redirect
// following, a total transfer timeout, and a hard byte cap (delivery stops at
// the cap and HttpResult::truncated is set - not an error).
void httpGetEx(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::function<void(const char* data, size_t len)>& onChunk,
    const std::atomic<bool>& abortFlag,
    HttpResult& out,
    const HttpGetOpts& opts
);
