#include "util/http.h"

#include <curl/curl.h>

#include <cctype>
#include <cstdio>

namespace {

// How long the transfer loop waits for socket activity per iteration. The
// abort flag is checked after every wake-up, so Escape lands within this
// window even when the server is silent (a reasoning model thinking before
// its first token sends no bytes, and a write-callback check would then only
// fire when the first chunk arrives).
constexpr int kPollMs = 50;

struct WriteCtx {
    const std::function<void(const char*, size_t)>* onChunk;
    const std::atomic<bool>* abortFlag;
    size_t remaining = 0;  // 0 = unlimited
    HttpResult* out  = nullptr;
};

size_t writeCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* ctx = static_cast<WriteCtx*>(userdata);
    size_t total = size * nmemb;
    if (ctx->abortFlag->load()) {
        // Pause delivery; the loop below ends the transfer on the flag.
        return 0;
    }
    if (ctx->remaining && total > ctx->remaining) {
        // Byte cap reached: deliver the part that still fits and end the
        // transfer. curl reports CURLE_WRITE_ERROR for the zero return, which
        // perform() maps back to a success when out->truncated is set.
        total = ctx->remaining;
        (*ctx->onChunk)(ptr, total);
        ctx->remaining = 0;
        if (ctx->out)
            ctx->out->truncated = true;
        return 0;
    }
    if (ctx->remaining)
        ctx->remaining -= total;
    (*ctx->onChunk)(ptr, total);
    return total;
}

struct HeaderCtx {
    HttpResult* out;
};

size_t headerCb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    const size_t total = size * nmemb;
    auto* ctx = static_cast<HeaderCtx*>(userdata);
    if (ctx && ctx->out) {
        std::string line(ptr, total);
        auto lower = [](unsigned char c) -> char {
            return static_cast<char>(std::tolower(c));
        };
        constexpr const char* kKey = "content-type:";
        if (line.size() > 13) {
            bool match = true;
            for (size_t i = 0; i < 13; ++i) {
                if (lower((unsigned char)line[i]) != kKey[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string v = line.substr(13);
                size_t begin = v.find_first_not_of(" \t");
                v            = begin == std::string::npos ? "" : v.substr(begin);
                size_t end = v.find_last_not_of(" \r\n\t");
                v = end == std::string::npos ? "" : v.substr(0, end + 1);
                size_t semi = v.find(';');
                if (semi != std::string::npos)
                    v = v.substr(0, semi);
                for (auto& c : v)
                    c = lower((unsigned char)c);
                ctx->out->contentType = v;
            }
        }
    }
    return total;
}

void perform(
    const std::string& url,
    bool post,
    const std::string& body,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::function<void(const char* data, size_t len)>& onChunk,
    const std::atomic<bool>& abortFlag,
    HttpResult& out,
    const HttpGetOpts& opts
) {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::vector<std::string> headerLines;
    headerLines.reserve(headers.size());
    for (const auto& [k, v] : headers)
        headerLines.push_back(k + ": " + v);

    CURL* curl = curl_easy_init();
    if (!curl) {
        out.error = "curl_easy_init failed";
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    if (post) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    if (opts.followRedirects) {
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    }
    if (opts.totalTimeoutSec > 0)
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)opts.totalTimeoutSec);

    curl_slist* headerList = nullptr;
    for (const auto& line : headerLines)
        headerList = curl_slist_append(headerList, line.c_str());
    if (headerList)
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    WriteCtx ctx{&onChunk, &abortFlag, opts.maxBodyBytes, &out};
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    HeaderCtx hctx{&out};
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &hctx);

    // The multi interface (not curl_easy_perform) so the abort flag is polled
    // on a timer instead of only when bytes arrive.
    CURLM* multi = curl_multi_init();
    if (!multi) {
        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
        out.error = "curl_multi_init failed";
        return;
    }

    CURLMcode mres = curl_multi_add_handle(multi, curl);
    CURLcode transferRes = CURLE_OK;
    bool aborted    = false;
    bool handleOut  = false;
    if (mres == CURLM_OK) {
        int running = 0;
        while (true) {
            mres = curl_multi_perform(multi, &running);
            if (mres != CURLM_OK || running == 0) break;
            if (abortFlag.load()) {
                aborted   = true;
                handleOut = true;
                curl_multi_remove_handle(multi, curl);
                break;
            }
            int nfds = 0;
            CURLMcode pres = curl_multi_poll(multi, nullptr, 0, kPollMs, &nfds);
            if (pres != CURLM_OK) {
                mres = pres;
                break;
            }
        }
        int msgsLeft = 0;
        if (CURLMsg* msg = curl_multi_info_read(multi, &msgsLeft)) {
            do {
                if (msg->msg == CURLMSG_DONE) transferRes = msg->data.result;
            } while ((msg = curl_multi_info_read(multi, &msgsLeft)));
        }
    }

    long statusCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCode);
    char* effective = nullptr;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
    if (effective)
        out.effectiveUrl = effective;

    if (aborted) {
        out.aborted = true;
        out.statusCode = 0;
    } else if (mres != CURLM_OK) {
        out.error = std::string(curl_multi_strerror(mres)) + " (multi code " + std::to_string((int)mres) + ")";
        out.statusCode = 0;
    } else if (transferRes != CURLE_OK && !(transferRes == CURLE_WRITE_ERROR && out.truncated)) {
        if (abortFlag.load()) {
            out.aborted = true;
        } else if (transferRes == CURLE_ABORTED_BY_CALLBACK) {
            out.aborted = true;
        } else {
            out.error = std::string(curl_easy_strerror(transferRes)) + " (curl code " + std::to_string((int)transferRes) + ")";
        }
        out.statusCode = 0;
    } else {
        out.statusCode = statusCode;
    }

    if (!handleOut)
        curl_multi_remove_handle(multi, curl);
    curl_multi_cleanup(multi);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);
}

} // namespace

void httpPostStream(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::string& body,
    const std::function<void(const char* data, size_t len)>& onChunk,
    const std::atomic<bool>& abortFlag,
    HttpResult& out
) {
    perform(url, true, body, headers, onChunk, abortFlag, out, HttpGetOpts{});
}

void httpGet(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::function<void(const char* data, size_t len)>& onChunk,
    const std::atomic<bool>& abortFlag,
    HttpResult& out
) {
    perform(url, false, {}, headers, onChunk, abortFlag, out, HttpGetOpts{});
}

void httpGetEx(
    const std::string& url,
    const std::vector<std::pair<std::string, std::string>>& headers,
    const std::function<void(const char* data, size_t len)>& onChunk,
    const std::atomic<bool>& abortFlag,
    HttpResult& out,
    const HttpGetOpts& opts
) {
    perform(url, false, {}, headers, onChunk, abortFlag, out, opts);
}
