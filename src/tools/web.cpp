#include "tools/web.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "tools/tools.h"
#include "util/http.h"
#include "util/path.h"
#include "util/utf8.h"

namespace {

// A page is fetched for its text, so a few megabytes is already far past what
// the model can use; kResultMaxBytes is what actually lands in the context.
constexpr size_t kSearchMaxBody  = 2 * 1024 * 1024;
constexpr size_t kFetchMaxBody   = 4 * 1024 * 1024;
constexpr size_t kResultMaxBytes = 40 * 1024;
constexpr int kSearchTimeoutSec  = 30;
constexpr int kFetchTimeoutSec   = 45;
constexpr size_t kBinaryProbe    = 1024;

// Longest single output line. Tag stripping can leave one enormous paragraph;
// printing it whole would eat the whole result budget.
constexpr size_t kMaxLineChars = 2000;

const char* kUserAgent = "Mozilla/5.0 (X11; Linux x86_64) miniagent/1.0 web-fetch";

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool startsWith(const std::string& s, size_t pos, const char* prefix) {
    const size_t n = std::char_traits<char>::length(prefix);
    return s.compare(pos, n, prefix) == 0;
}

// Case-insensitive "</name" search (tag names are ASCII), without copying the
// rest of the document per dropped element.
size_t findCloseTag(const std::string& s, const std::string& name, size_t from) {
    const std::string needle = "</" + name;
    for (size_t i = from; i + needle.size() <= s.size(); ++i) {
        size_t k = 0;
        while (k < needle.size() &&
               std::tolower(static_cast<unsigned char>(s[i + k])) == needle[k])
            ++k;
        if (k == needle.size())
            return i;
    }
    return std::string::npos;
}

std::string utf8Encode(unsigned cp) {
    std::string out;
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return out;
}

const std::map<std::string, std::string>& namedEntities() {
    static const std::map<std::string, std::string> m = {
        {"amp", "&"},
        {"lt", "<"},
        {"gt", ">"},
        {"quot", "\""},
        {"apos", "'"},
        {"nbsp", " "},
        {"ensp", " "},
        {"emsp", " "},
        {"thinsp", " "},
        {"shy", ""},
        {"zwnj", ""},
        {"zwj", ""},
        {"middot", "·"},   {"bull", "•"},
        {"hellip", "…"},
        {"mdash", "—"},
        {"ndash", "–"},
        {"copy", "©"},     {"reg", "®"},         {"trade", "™"},
        {"deg", "°"},      {"plusmn", "±"},      {"times", "×"},
        {"divide", "÷"},   {"micro", "µ"},       {"para", "¶"},
        {"sect", "§"},     {"laquo", "«"},       {"raquo", "»"},
        {"lsquo", "'"},    {"rsquo", "'"},       {"ldquo", "\""},
        {"rdquo", "\""},   {"dagger", "†"},      {"euro", "€"},
        {"pound", "£"},    {"curren", "¤"},      {"yen", "¥"},
        {"cent", "¢"},     {"sup1", "¹"},        {"sup2", "²"},
        {"sup3", "³"},     {"frac12", "½"},      {"ordm", "º"},
        {"ordf", "ª"},     {"harr", "↔"},        {"larr", "←"},
        {"rarr", "→"},     {"uarr", "↑"},        {"darr", "↓"},
        {"aacute", "á"},    {"eacute", "é"},      {"iacute", "í"},
        {"oacute", "ó"},    {"uacute", "ú"},      {"yacute", "ý"},
        {"Aacute", "Á"},    {"Eacute", "É"},      {"Iacute", "Í"},
        {"Oacute", "Ó"},    {"Uacute", "Ú"},
        {"agrave", "à"},    {"egrave", "è"},      {"igrave", "ì"},
        {"ograve", "ò"},    {"ugrave", "ù"},
        {"acirc", "â"},     {"ecirc", "ê"},       {"icirc", "î"},
        {"ocirc", "ô"},     {"ucirc", "û"},
        {"atilde", "ã"},    {"etilde", "ẽ"},      {"otilde", "õ"},
        {"auml", "ä"},      {"euml", "ë"},        {"iuml", "ï"},
        {"ouml", "ö"},      {"uuml", "ü"},        {"yuml", "ÿ"},
        {"Auml", "Ä"},      {"Ouml", "Ö"},        {"Uuml", "Ü"},
        {"ccedil", "ç"},    {"Ccedil", "Ç"},      {"ntilde", "ñ"},
        {"Ntilde", "Ñ"},    {"szlig", "ß"},       {"oslash", "ø"},
        {"Oslash", "Ø"},    {"aelig", "æ"},       {"AElig", "Æ"},
        {"THORN", "Þ"},     {"thorn", "þ"},       {"ETH", "Ð"},
        {"eth", "ð"},       {"yicy", "ї"},        {"Ycy", "Ї"},
        {"ne", "≠"},       {"le", "≤"},          {"ge", "≥"},
        {"infin", "∞"},    {"alpha", "α"},       {"beta", "β"},
        {"gamma", "γ"},    {"delta", "δ"},       {"pi", "π"},
        {"mu", "µ"},       {"sigma", "σ"},       {"omega", "ω"},
        {"lambda", "λ"},   {"theta", "θ"},       {"Phi", "Φ"},
    };
    return m;
}

// Decode the entity starting at s[i] (which must be '&'). Returns the number of
// bytes consumed (1 for a stray '&', which is kept verbatim).
size_t decodeEntity(const std::string& s, size_t i, std::string& out) {
    static const std::map<std::string, std::string>& named = namedEntities();
    const size_t semi = s.find(';', i + 1);
    if (semi == std::string::npos || semi - i > 12) {
        out += '&';
        return 1;
    }
    const std::string body = s.substr(i + 1, semi - i - 1);
    if (!body.empty() && body[0] == '#') {
        bool hex  = body.size() > 1 && (body[1] == 'x' || body[1] == 'X');
        size_t ds = hex ? 2 : 1;
        std::string digits = body.substr(ds);
        if (digits.empty()) {
            out += '&';
            return 1;
        }
        unsigned long cp = 0;
        for (char c : digits) {
            unsigned v;
            if (hex) {
                if (std::isdigit(static_cast<unsigned char>(c)))
                    v = static_cast<unsigned>(c - '0');
                else if (c >= 'a' && c <= 'f')
                    v = static_cast<unsigned>(c - 'a') + 10;
                else if (c >= 'A' && c <= 'F')
                    v = static_cast<unsigned>(c - 'A') + 10;
                else {
                    cp = 0;
                    break;
                }
            } else if (std::isdigit(static_cast<unsigned char>(c))) {
                v = static_cast<unsigned>(c - '0');
            } else {
                cp = 0;
                break;
            }
            cp = cp * (hex ? 16 : 10) + v;
            if (cp > 0x10FFFF) {
                cp = 0;
                break;
            }
        }
        if (cp == 0) {
            out += '&';
            return 1;
        }
        if (cp == 0xA0) {
            // U+00A0 becomes a plain space: the whitespace collapsing below
            // works on ASCII blanks, and a no-break space inside a sentence is
            // just noise for the model.
            out += ' ';
            return semi - i + 1;
        }
        out += utf8Encode(static_cast<unsigned>(cp));
        return semi - i + 1;
    }
    auto it = named.find(body);
    if (it == named.end()) {
        out += '&';
        return 1;
    }
    out += it->second;
    return semi - i + 1;
}

// Elements whose entire subtree carries no readable prose (or is duplicated by
// the browser).
const std::set<std::string>& dropTags() {
    static const std::set<std::string> t = {
        "script", "style", "noscript", "template", "head", "svg",  "math",
        "canvas", "iframe", "object", "embed", "video", "audio", "textarea",
    };
    return t;
}

// Elements that end the current output line.
const std::set<std::string>& blockTags() {
    static const std::set<std::string> t = {
        "p",      "div",    "section",  "article", "aside",   "header",
        "footer", "nav",    "main",     "li",      "ul",      "ol",
        "dl",     "dt",     "dd",       "table",   "thead",   "tbody",
        "tfoot",  "tr",     "td",       "th",      "caption", "blockquote",
        "figure", "figcaption", "details", "summary", "fieldset", "address",
        "colgroup", "body", "html",     "form",    "select",  "option",
        "h1",     "h2",     "h3",       "h4",      "h5",      "h6",
    };
    return t;
}

} // namespace

std::string percentEncodeQuery(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    auto unreserved        = [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~';
    };
    std::string out;
    for (unsigned char c : s) {
        if (unreserved(c))
            out += static_cast<char>(c);
        else if (c == ' ')
            out += '+';
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string searxngSearchUrl(const std::string& base, const std::string& query, int page) {
    std::string b = base;
    while (!b.empty() && b.back() == '/')
        b.pop_back();
    std::ostringstream ss;
    ss << b << "/search?format=json&language=all&pageno=" << (page < 1 ? 1 : page)
       << "&safesearch=0&q=" << percentEncodeQuery(query);
    return ss.str();
}

bool looksBinaryBytes(const std::string& s) {
    const size_t n = std::min(s.size(), kBinaryProbe);
    for (size_t i = 0; i < n; ++i)
        if (s[i] == '\0')
            return true;
    return false;
}

std::string htmlToText(const std::string& html) {
    std::vector<std::pair<std::string, bool>> lines; // text, wasInsidePre
    std::string cur;
    int preDepth = 0;
    auto flush = [&]() {
        lines.emplace_back(cur, preDepth > 0);
        cur.clear();
    };

    size_t i = 0;
    while (i < html.size()) {
        const char c = html[i];
        if (c == '<') {
            if (startsWith(html, i, "<!--")) {
                size_t end = html.find("-->", i + 4);
                i          = end == std::string::npos ? html.size() : end + 3;
                continue;
            }
            if (i + 1 < html.size() && (std::isalpha(static_cast<unsigned char>(html[i + 1])) ||
                                        html[i + 1] == '/')) {
                const size_t end = html.find('>', i + 2);
                if (end == std::string::npos) {
                    i = html.size();
                    continue;
                }
                std::string tag = lower(html.substr(i + 1, end - i - 1));
                const bool closing = !tag.empty() && tag[0] == '/';
                if (closing)
                    tag.erase(0, 1);
                size_t nameLen = 0;
                while (nameLen < tag.size() && std::isalnum(static_cast<unsigned char>(tag[nameLen])))
                    ++nameLen;
                const std::string name = tag.substr(0, nameLen);
                if (dropTags().count(name)) {
                    if (closing) {
                        i = end + 1;
                        continue;
                    }
                    const size_t close = findCloseTag(html, name, end + 1);
                    i = close == std::string::npos ? html.size() : html.find('>', close) == std::string::npos
                                                                    ? html.size()
                                                                    : html.find('>', close) + 1;
                    continue;
                }
                if (name == "pre") {
                    flush();
                    if (closing) {
                        if (preDepth > 0)
                            --preDepth;
                    } else {
                        ++preDepth;
                    }
                    i = end + 1;
                    continue;
                }
                if (name == "br" || blockTags().count(name)) {
                    flush();
                    if (name == "li" && !closing)
                        cur = "- ";
                    i = end + 1;
                    continue;
                }
                i = end + 1; // inline tag: dropped, its text kept
                continue;
            }
            if (i + 1 < html.size() && (html[i + 1] == '!' || html[i + 1] == '?')) {
                const size_t end = html.find('>', i + 2);
                i = end == std::string::npos ? html.size() : end + 1;
                continue;
            }
            cur += '&';
            ++i;
            continue;
        }
        if (c == '>') { // stray closer (its opener was already dropped)
            ++i;
            continue;
        }
        if (c == '\n') {
            flush();
            ++i;
            continue;
        }
        if (c == '&') {
            i += decodeEntity(html, i, cur);
            continue;
        }
        cur += c;
        ++i;
    }
    flush();

    std::string out;
    for (const auto& [raw, pre] : lines) {
        std::string t;
        if (pre) {
            t = raw;
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r'))
                t.pop_back();
        } else {
            bool pendingSpace = false;
            for (char ch : raw) {
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\f' || ch == '\v') {
                    pendingSpace = true;
                    continue;
                }
                if (pendingSpace && !t.empty())
                    t += ' ';
                pendingSpace = false;
                t += ch;
            }
        }
        if (t.size() > kMaxLineChars)
            t = t.substr(0, kMaxLineChars) + "…";
        out += t;
        out += '\n';
    }

    std::string collapsed;
    size_t p = 0;
    while (p < out.size()) {
        if (out[p] == '\n') {
            size_t q = p;
            while (q < out.size() && out[q] == '\n')
                ++q;
            collapsed.append(q - p >= 3 ? "\n\n" : std::string(q - p, '\n'));
            p = q;
        } else {
            collapsed += out[p++];
        }
    }
    const size_t first = collapsed.find_first_not_of("\n ");
    if (first == std::string::npos)
        return "";
    const size_t last = collapsed.find_last_not_of("\n \t");
    return collapsed.substr(first, last - first + 1);
}

std::string formatSearchResults(const nlohmann::json& payload, int limit) {
    auto strField = [](const nlohmann::json& o, const char* key) -> std::string {
        if (!o.is_object() || !o.contains(key) || !o[key].is_string())
            return {};
        return o[key].get<std::string>();
    };
    auto oneLine = [](std::string s) {
        for (char& c : s)
            if (c == '\n' || c == '\r' || c == '\t')
                c = ' ';
        while (s.find("  ") != std::string::npos) {
            std::string t;
            bool prevSpace = false;
            for (char c : s) {
                if (c == ' ') {
                    if (prevSpace)
                        continue;
                    prevSpace = true;
                } else {
                    prevSpace = false;
                }
                t += c;
            }
            s = t;
        }
        size_t a = s.find_first_not_of(' ');
        size_t b = s.find_last_not_of(' ');
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };

    std::ostringstream out;
    if (payload.contains("answers") && payload["answers"].is_array()) {
        for (const auto& a : payload["answers"]) {
            std::string text = a.is_string() ? oneLine(a.get<std::string>()) : oneLine(strField(a, "content"));
            if (text.empty())
                continue;
            out << "Answer: " << text << "\n\n";
        }
    }

    nlohmann::json results = nlohmann::json::array();
    if (payload.contains("results") && payload["results"].is_array())
        results = payload["results"];

    std::set<std::string> seen;
    int shown = 0;
    for (const auto& r : results) {
        const std::string url = oneLine(strField(r, "url"));
        if (url.empty() || !seen.insert(url).second)
            continue;
        if (shown >= limit)
            break;
        ++shown;
        std::string title   = oneLine(sanitizeUtf8(strField(r, "title")));
        std::string snippet = oneLine(sanitizeUtf8(strField(r, "content")));
        if (snippet.size() > 300)
            snippet = snippet.substr(0, 300) + "…";
        out << shown << ". " << (title.empty() ? url : title) << "\n   " << url << "\n";
        if (!snippet.empty())
            out << "   " << snippet << "\n";
    }

    if (shown == 0)
        return out.str();

    if (payload.contains("suggestions") && payload["suggestions"].is_array()) {
        std::string sug;
        for (const auto& s : payload["suggestions"]) {
            if (!s.is_string())
                continue;
            if (!sug.empty())
                sug += ", ";
            sug += s.get<std::string>();
            if (sug.size() > 300)
                break;
        }
        if (!sug.empty())
            out << "\nRelated searches: " << sug << "\n";
    }
    return out.str();
}

Tool makeWebSearchTool(const std::string& baseUrl) {
    Tool t;
    t.name = "web_search";
    t.description =
        "Search the web and get a ranked list of title / URL / snippet results. This is the "
        "dedicated web search tool - do not run curl or wget through bash to search. Start a "
        "research step here, then web_fetch the promising URLs to read their content. Use "
        "specific query wording (a few keywords, not a full sentence).";
    t.paramsSchema = {
        {"type", "object"},
        {"properties",
         {
             {"query", {{"type", "string"}, {"description", "Search keywords"}}},
             {"limit", {{"type", "integer"},
                        {"description", "Maximum number of results to return (default 5, max 20)"}}},
             {"page", {{"type", "integer"},
                       {"description", "Result page number, for going deeper than the first page (default 1)"}}},
         }},
        {"required", {"query"}},
    };
    t.execute = [baseUrl](nlohmann::json args, const std::atomic<bool>& abort) -> ToolResult {
        std::string query;
        if (args.is_object() && args.contains("query") && args["query"].is_string())
            query = args["query"].get<std::string>();
        if (query.empty())
            return {"Missing 'query' argument", true};

        int limit = 5;
        if (args.is_object() && args.contains("limit") && args["limit"].is_number())
            limit = args["limit"].get<int>();
        limit = std::clamp(limit, 1, 20);

        int page = 1;
        if (args.is_object() && args.contains("page") && args["page"].is_number())
            page = args["page"].get<int>();
        if (page < 1)
            page = 1;

        const std::string url = searxngSearchUrl(baseUrl, query, page);
        std::string body;
        HttpResult res;
        httpGetEx(
            url,
            {{"User-Agent", kUserAgent}, {"Accept", "application/json"}},
            [&](const char* d, size_t n) { body.append(d, n); },
            abort,
            res,
            HttpGetOpts{true, kSearchTimeoutSec, kSearchMaxBody});

        const std::string where = " (SearXNG is expected at " + baseUrl +
                                  "; set 'searxng_url' in ~/.config/miniagent/miniagent.settings to "
                                  "point elsewhere)";
        if (res.aborted)
            return {"web_search aborted", true};
        if (!res.error.empty())
            return {"web_search failed: " + res.error + where, true};
        if (res.statusCode == 403)
            return {"web_search failed: SearXNG refused the JSON API (HTTP 403). The SearXNG "
                    "settings.yml must list 'json' under search.formats." +
                        where,
                    true};
        if (res.statusCode != 200)
            return {"web_search failed: HTTP " + std::to_string(res.statusCode) + " from " + url + where,
                    true};

        nlohmann::json payload = nlohmann::json::parse(body, nullptr, false);
        if (payload.is_discarded())
            return {"web_search failed: the reply from " + url +
                    " was not JSON (HTTP 200). The SearXNG settings.yml must list 'json' under "
                    "search.formats." +
                    where,
                    true};

        std::string text = formatSearchResults(payload, limit);
        if (text.empty())
            return {"No results for \"" + query + "\" (page " + std::to_string(page) +
                    "). Try fewer or different keywords.",
                    false};
        std::string out = "Results for \"" + query + "\" (page " + std::to_string(page) + "):\n\n" + text;
        if (res.truncated)
            out += "\n[response body capped at " + std::to_string(kSearchMaxBody) + " bytes]";
        return {truncateToLimit(out, 2000, kResultMaxBytes), false};
    };
    return t;
}

Tool makeWebFetchTool() {
    Tool t;
    t.name = "web_fetch";
    t.description =
        "Fetch one URL and return its content as readable text: HTML is converted to text "
        "(scripts, styles and navigation dropped, entities decoded), plain text and JSON are "
        "returned as-is. Follow up on URLs from web_search, read documentation, or pull a source "
        "file from a hoster like raw.githubusercontent.com. Do not run curl or wget through bash to "
        "read a page. Binary content (PDF, images, archives) is not supported - download it with "
        "bash instead. Output is truncated to 40 KB.";
    t.paramsSchema = {
        {"type", "object"},
        {"properties",
         {
             {"url", {{"type", "string"}, {"description", "Absolute http(s) URL to fetch"}}},
         }},
        {"required", {"url"}},
    };
    t.execute = [](nlohmann::json args, const std::atomic<bool>& abort) -> ToolResult {
        std::string url;
        if (args.is_object() && args.contains("url") && args["url"].is_string())
            url = args["url"].get<std::string>();
        if (url.empty())
            return {"Missing 'url' argument", true};
        if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0)
            return {"Only http:// and https:// URLs can be fetched (got: " + url + ")", true};

        std::string body;
        HttpResult res;
        httpGetEx(
            url,
            {{"User-Agent", kUserAgent},
             {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,text/plain;q=0.8,*/*;q=0.5"},
             {"Accept-Language", "en"}},
            [&](const char* d, size_t n) { body.append(d, n); },
            abort,
            res,
            HttpGetOpts{true, kFetchTimeoutSec, kFetchMaxBody});

        if (res.aborted)
            return {"web_fetch aborted", true};
        if (!res.error.empty())
            return {"web_fetch failed: " + res.error + " (" + url + ")", true};

        const std::string from = res.effectiveUrl.empty() ? url : res.effectiveUrl;
        if (res.statusCode >= 400)
            return {"web_fetch failed: HTTP " + std::to_string(res.statusCode) + " for " + from, true};

        const std::string& ctype = res.contentType;
        const bool isHtml        = ctype.find("html") != std::string::npos;
        const bool textish       = isHtml || ctype.find("text/") != std::string::npos ||
                            ctype.find("json") != std::string::npos ||
                            ctype.find("xml") != std::string::npos ||
                            ctype.find("javascript") != std::string::npos ||
                            ctype.find("csv") != std::string::npos || ctype.empty();
        if (!textish)
            return {"web_fetch cannot read this content type: " + ctype + " (" + from +
                    "). Download the file with bash instead.",
                    true};
        if (ctype.empty() && looksBinaryBytes(body))
            return {"web_fetch got binary content with no content type from " + from +
                    ". Download the file with bash instead.",
                    true};

        std::string text = sanitizeUtf8(isHtml ? htmlToText(body) : body);

        std::ostringstream out;
        out << "Fetched " << from << " (HTTP " << res.statusCode << ", "
            << (ctype.empty() ? "unknown type" : ctype) << ", " << body.size() << " bytes";
        if (res.truncated)
            out << ", body capped at " << kFetchMaxBody;
        if (from != url)
            out << ", redirected";
        out << ")\n\n";
        if (text.empty())
            out << "(no readable text in the response)";
        else
            out << text;
        return {truncateToLimit(out.str(), 2000, kResultMaxBytes), false};
    };
    return t;
}
