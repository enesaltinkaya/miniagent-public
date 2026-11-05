#include "sessions/sessions.h"

#include "agent/compaction.h"

#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>

#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

std::string defaultRoot() {
    const char* home = std::getenv("HOME");
    if (!home || !*home)
        home = ".";
    return std::string(home) + "/.config/miniagent/sessions";
}

// Percent-encodes every byte of the path except unreserved characters.
// The result never contains '/', so it is safe to use as a directory
// name and collisions are impossible.
std::string encodeCwd(const std::string& cwd) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : cwd) {
        bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
                          c == '~';
        if (unreserved)
            out += c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0xF];
        }
    }
    return out;
}

bool mkdirp(const std::string& path) {
    if (path.empty() || path == "/")
        return true;
    struct stat st;
    if (stat(path.c_str(), &st) == 0)
        return S_ISDIR(st.st_mode);
    size_t slash = path.rfind('/');
    std::string parent = (slash == std::string::npos || slash == 0) ? std::string("/") : path.substr(0, slash);
    if (!mkdirp(parent))
        return false;
    return mkdir(path.c_str(), 0755) == 0;
}

std::string sessionFileName(int64_t createdMs) {
    time_t t = static_cast<time_t>(createdMs / 1000);
    struct tm tmv{};
    gmtime_r(&t, &tmv);
    std::string name;
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d%02d%02dT%02d%02d%02dZ",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    name += buf;
    name += '-';
    unsigned r = std::random_device{}();
    for (int i = 0; i < 3; ++i) {
        char c[3];
        snprintf(c, sizeof(c), "%02x", (r >> (i * 8)) & 0xFF);
        name += c;
    }
    name += ".jsonl";
    return name;
}

std::vector<std::string> readLines(const std::string& path) {
    std::vector<std::string> lines;
    std::ifstream f(path);
    if (!f)
        return lines;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}


int64_t fileMtimeMs(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return 0;
    return static_cast<int64_t>(st.st_mtime) * 1000;
}

json toJson(const AgentMessage& msg) {
    if (const auto* u = std::get_if<UserMessage>(&msg)) {
        return {{"type", "user"},
                {"text", u->text},
                {"timestamp", u->timestamp},
                {"synthetic", u->synthetic}};
    }
    if (const auto* a = std::get_if<AssistantMessage>(&msg)) {
        json tcs = json::array();
        for (const auto& tc : a->toolCalls)
            tcs.push_back({{"id", tc.id}, {"name", tc.name}, {"arguments", tc.arguments}});
        return {{"type", "assistant"},
                {"text", a->text},
                {"thinking", a->thinking},
                {"toolCalls", tcs},
                {"stopReason", a->stopReason},
                {"errorMessage", a->errorMessage},
                {"usage",
                 {{"inputTokens", a->usage.inputTokens},
                  {"outputTokens", a->usage.outputTokens},
                  {"totalTokens", a->usage.totalTokens}}},
                {"timestamp", a->timestamp}};
    }
    const auto* t = std::get_if<ToolResultMessage>(&msg);
    return {{"type", "tool"},
            {"toolCallId", t->toolCallId},
            {"toolName", t->toolName},
            {"content", t->content},
            {"isError", t->isError},
            {"timestamp", t->timestamp},
            {"args", t->args},
            {"imagePath", t->imagePath}};
}

bool fromJson(const json& j, AgentMessage& out) {
    const std::string type = j.value("type", std::string{});
    if (type == "user") {
        UserMessage m;
        m.text      = j.value("text", std::string{});
        m.timestamp = j.value("timestamp", int64_t(0));
        m.synthetic = j.value("synthetic", false);
        out         = m;
        return true;
    }
    if (type == "assistant") {
        AssistantMessage m;
        m.text         = j.value("text", std::string{});
        m.thinking     = j.value("thinking", std::string{});
        m.stopReason   = j.value("stopReason", std::string{});
        m.errorMessage = j.value("errorMessage", std::string{});
        m.timestamp    = j.value("timestamp", int64_t(0));
        if (j.contains("usage")) {
            const json& usage = j["usage"];
            m.usage.inputTokens = usage.value("inputTokens", 0);
            m.usage.outputTokens = usage.value("outputTokens", 0);
            m.usage.totalTokens  = usage.value("totalTokens", 0);
        }
        if (j.contains("toolCalls")) {
            for (const auto& tc : j["toolCalls"]) {
                ToolCallBlock b;
                b.id        = tc.value("id", std::string{});
                b.name      = tc.value("name", std::string{});
                b.arguments = tc.value("arguments", json::object());
                m.toolCalls.push_back(std::move(b));
            }
        }
        out = m;
        return true;
    }
    if (type == "tool") {
        ToolResultMessage m;
        m.toolCallId = j.value("toolCallId", std::string{});
        m.toolName   = j.value("toolName", std::string{});
        m.content    = j.value("content", std::string{});
        m.isError    = j.value("isError", false);
        m.timestamp  = j.value("timestamp", int64_t(0));
        m.args       = j.value("args", std::string{});
        m.imagePath  = j.value("imagePath", std::string{});
        out          = m;
        return true;
    }
    return false;
}

}  // namespace

SessionStore::SessionStore(std::string cwd, std::string root)
    : cwd_(std::move(cwd)), dirPath_((root.empty() ? defaultRoot() : std::move(root)) + "/" + encodeCwd(cwd_)) {}

std::string SessionStore::startSession(const std::string& title) {
    if (!mkdirp(dirPath_))
        return {};
    // Strictly increasing within the process so rapid consecutive sessions
    // (e.g. two "New Session" clicks in the same millisecond) still sort in
    // creation order.
    static std::atomic<int64_t> lastCreated{0};
    int64_t created = std::max(nowMs(), lastCreated.load() + 1);
    lastCreated = created;
    std::string path = dirPath_ + "/" + sessionFileName(created);

    json header = {{"type", "header"}, {"cwd", cwd_}, {"created", created}, {"title", title}};
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return {};
    std::fputs(header.dump(-1, ' ', false, json::error_handler_t::replace).c_str(), f);
    std::fputc('\n', f);
    std::fclose(f);

    currentPath_ = path;
    return path;
}

bool SessionStore::appendMessage(const AgentMessage& msg) {
    if (currentPath_.empty())
        return false;
    std::FILE* f = std::fopen(currentPath_.c_str(), "a");
    if (!f)
        return false;
    std::fputs(toJson(msg).dump(-1, ' ', false, json::error_handler_t::replace).c_str(), f);
    std::fputc('\n', f);
    return std::fclose(f) == 0;
}

bool SessionStore::appendCompaction(const CompactionRecord& record) {
    if (currentPath_.empty())
        return false;
    json j = {{"type", "compaction"},
              {"keepFrom", record.keepFrom},
              {"summary", record.summary},
              {"tokensBefore", record.tokensBefore},
              {"tokensAfter", record.tokensAfter},
              {"compactor", "vcc"}};
    std::FILE* f = std::fopen(currentPath_.c_str(), "a");
    if (!f)
        return false;
    std::fputs(j.dump(-1, ' ', false, json::error_handler_t::replace).c_str(), f);
    std::fputc('\n', f);
    return std::fclose(f) == 0;
}

bool SessionStore::truncateMessages(size_t keepCount) {
    if (currentPath_.empty())
        return false;
    auto lines = readLines(currentPath_);
    if (lines.empty())
        return false;
    std::vector<std::string> out;
    out.push_back(lines[0]);
    size_t kept = 0;
    for (size_t i = 1; i < lines.size(); ++i) {
        if (kept >= keepCount)
            break;
        json j = json::parse(lines[i], nullptr, false);
        if (!j.is_discarded() && j.value("type", std::string{}) == "compaction")
            continue;
        out.push_back(std::move(lines[i]));
        ++kept;
    }
    if (out.size() == lines.size())
        return true;
    std::FILE* f = std::fopen(currentPath_.c_str(), "w");
    if (!f)
        return false;
    for (const auto& l : out) {
        std::fputs(l.c_str(), f);
        std::fputc('\n', f);
    }
    return std::fclose(f) == 0;
}

bool SessionStore::truncateToEntries(size_t keepEntries) {
    if (currentPath_.empty())
        return false;
    auto lines = readLines(currentPath_);
    if (lines.empty())
        return false;
    const size_t msgLines = lines.size() - 1;
    if (keepEntries >= msgLines)
        return true;
    std::FILE* f = std::fopen(currentPath_.c_str(), "w");
    if (!f)
        return false;
    for (size_t i = 0; i <= keepEntries && i < lines.size(); ++i) {
        std::fputs(lines[i].c_str(), f);
        std::fputc('\n', f);
    }
    return std::fclose(f) == 0;
}

size_t SessionStore::entryCount() const {
    if (currentPath_.empty())
        return 0;
    size_t entries = 0;
    for (const std::string& line : readLines(currentPath_)) {
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || j.value("type", std::string{}) == "header")
            continue;
        ++entries;
    }
    return entries;
}

void SessionStore::closeSession() { currentPath_.clear(); }

bool SessionStore::deleteSession(const std::string& path) {
    // Only ever remove files that live inside this store's directory.
    const std::string prefix = dirPath_ + "/";
    if (path.compare(0, prefix.size(), prefix) != 0)
        return false;
    if (path == currentPath_)
        currentPath_.clear();
    return std::remove(path.c_str()) == 0;
}

std::vector<AgentMessage> SessionStore::loadMessages(const std::string& path) {
    std::vector<AgentMessage> out;
    for (const std::string& line : readLines(path)) {
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || j.value("type", std::string{}) == "header")
            continue;
        AgentMessage m;
        if (fromJson(j, m))
            out.push_back(std::move(m));
    }
    return out;
}

SessionInfo SessionStore::loadInfo(const std::string& path) {
    SessionInfo info;
    info.path = path;
    info.updatedMs = fileMtimeMs(path);

    // The header is line 1 and the message count is the line count minus one:
    // read only what is needed (no per-line storage).
    std::ifstream f(path);
    if (!f)
        return info;
    std::string line, headerLine;
    int lineCount = 0;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (lineCount == 0)
            headerLine = std::move(line);
        ++lineCount;
    }
    if (lineCount == 0)
        return info;
    json header = json::parse(headerLine, nullptr, false);
    if (header.is_discarded() || header.value("type", std::string{}) != "header")
        return info;
    info.cwd       = header.value("cwd", std::string{});
    info.title     = header.value("title", std::string{});
    info.createdMs = header.value("created", int64_t(0));
    info.messageCount = std::max(0, lineCount - 1);
    return info;
}


ProjectedSession SessionStore::loadProjected(const std::string& path) {
    ProjectedSession out;
    std::vector<AgentMessage> all;
    std::vector<CompactionRecord> compactions;   // parallel to nothing: index within its own vector
    std::vector<size_t> compactionEntryIndex;    // entry index (1-based) per compaction
    std::vector<size_t> messageEntryIndex;       // entry index per message in `all`

    size_t entry = 0;
    for (const std::string& line : readLines(path)) {
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded())
            continue;
        const std::string type = j.value("type", std::string{});
        if (type == "header")
            continue;
        ++entry;
        if (type == "compaction") {
            CompactionRecord rec;
            rec.keepFrom     = j.value("keepFrom", size_t{0});
            rec.summary      = j.value("summary", std::string{});
            rec.tokensBefore = j.value("tokensBefore", 0);
            rec.tokensAfter  = j.value("tokensAfter", 0);
            compactions.push_back(std::move(rec));
            compactionEntryIndex.push_back(entry);
            continue;
        }
        AgentMessage m;
        if (type != "header" && fromJson(j, m)) {
            all.push_back(std::move(m));
            messageEntryIndex.push_back(entry);
        }
    }
    out.entryCount = entry;

    if (compactions.empty()) {
        out.messages    = std::move(all);
        out.fileBase    = messageEntryIndex.empty() ? 1 : messageEntryIndex.front();
        out.compacted   = false;
        return out;
    }

    const size_t last          = compactions.size() - 1;
    const size_t keepFrom      = compactions[last].keepFrom;
    const size_t compEntry     = compactionEntryIndex[last];

    std::vector<AgentMessage> live;
    size_t fileBase = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        const size_t idx = messageEntryIndex[i];
        if (idx < compEntry && idx >= keepFrom && keepFrom > 0) {
            if (live.empty())
                fileBase = idx;
            live.push_back(all[i]);
        } else if (idx > compEntry) {
            if (live.empty())
                fileBase = idx;
            live.push_back(all[i]);
        }
    }

    UserMessage summary;
    summary.text      = std::string(kCompactionSummaryPrefix()) + "\n" + compactions[last].summary;
    summary.timestamp = nowMs();
    out.messages.push_back(std::move(summary));
    out.messages.insert(out.messages.end(), std::make_move_iterator(live.begin()),
                        std::make_move_iterator(live.end()));
    out.fileBase  = fileBase == 0 ? out.entryCount + 1 : fileBase;
    out.compacted = true;
    return out;
}

std::vector<SessionInfo> SessionStore::listSessions() const {
    std::vector<SessionInfo> out;
    DIR* d = opendir(dirPath_.c_str());
    if (!d)
        return out;
    struct dirent* e;
    while ((e = readdir(d))) {
        std::string name = e->d_name;
        if (name.size() < 6 || name.substr(name.size() - 6) != ".jsonl")
            continue;
        out.push_back(loadInfo(dirPath_ + "/" + name));
    }
    closedir(d);
    std::sort(out.begin(),
              out.end(),
              [](const SessionInfo& a, const SessionInfo& b) {
                  if (a.createdMs != b.createdMs)
                      return a.createdMs > b.createdMs;
                  return a.path > b.path;
              });
    return out;
}
