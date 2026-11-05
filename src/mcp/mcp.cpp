#include "mcp/mcp.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <utility>

#include "util/settings.h"

extern char** environ;

namespace {

// Settings keys may not carry arbitrary characters, so server names are
// flattened for the mcp_disabled_<name> keys.
std::string keySafe(const std::string& name) {
    std::string out = name;
    for (auto& c : out)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-')
            c = '_';
    return out;
}

// execve (used to keep a custom environment) does not search PATH, so a
// bare command name is resolved here. Falls back to the name itself (the
// exec then fails with the usual ENOENT message).
std::string resolveCommand(const std::string& cmd,
                           const std::map<std::string, std::string>& env) {
    if (cmd.find('/') != std::string::npos)
        return cmd;
    const char* path = "";
    const auto it = env.find("PATH");
    if (it != env.end())
        path = it->second.c_str();
    else
        path = std::getenv("PATH");
    if (!path || !*path)
        return cmd;
    std::string dirs = path;
    size_t start = 0;
    for (;;) {
        const size_t sep = dirs.find(':', start);
        const std::string dir = dirs.substr(start, sep == std::string::npos ? std::string::npos : sep - start);
        if (!dir.empty()) {
            const std::string candidate = dir + "/" + cmd;
            if (access(candidate.c_str(), X_OK) == 0)
                return candidate;
        }
        if (sep == std::string::npos)
            break;
        start = sep + 1;
    }
    return cmd;
}

} // namespace

std::string mcpConfigPath() { return miniagentConfigDir() + "/mcp.json"; }

std::string mcpToolName(const std::string& server, const std::string& tool) {
    return "mcp_" + server + "_" + tool;
}

bool McpServer::spawn() {
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0) {
        if (inPipe[0] >= 0) close(inPipe[0]);
        if (inPipe[1] >= 0) close(inPipe[1]);
        if (outPipe[0] >= 0) close(outPipe[0]);
        if (outPipe[1] >= 0) close(outPipe[1]);
        status = "pipe() failed";
        return false;
    }
    pid_t p = fork();
    if (p < 0) {
        close(inPipe[0]); close(inPipe[1]);
        close(outPipe[0]); close(outPipe[1]);
        status = "fork() failed";
        return false;
    }
    if (p == 0) {
        // Child: wire the pipes and exec the server.
        close(inPipe[1]);
        close(outPipe[0]);
        if (dup2(inPipe[0], 0) < 0 || dup2(outPipe[1], 1) < 0) _exit(126);
        // stderr goes to /dev/null: server logging must not mix into the
        // app's console, and it is unreadable here anyway.
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 2);
            close(devnull);
        }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(command.c_str()));
        for (const auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        std::vector<std::string> envStrs;
        for (char** e = environ; e && *e; ++e)
            envStrs.push_back(*e);
        for (const auto& [k, v] : env)
            envStrs.push_back(k + "=" + v);
        std::vector<char*> envp;
        for (auto& s : envStrs)
            envp.push_back(s.data());
        envp.push_back(nullptr);
        execve(resolveCommand(command, env).c_str(), argv.data(), envp.data());
        _exit(127);
    }
    close(inPipe[0]);
    close(outPipe[1]);
    inFd = inPipe[1];
    outFd = outPipe[0];
    pid_ = p;
    return true;
}

bool McpServer::writeLine(const nlohmann::json& value) {
    const std::string line = value.dump() + "\n";
    size_t off = 0;
    while (off < line.size()) {
        ssize_t n = write(inFd, line.data() + off, line.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

bool McpServer::rpcLocked(const std::string& method, const nlohmann::json& params,
                          int timeoutMs, const std::atomic<bool>* abort,
                          nlohmann::json& outResult, std::string& outError) {
    const int myId = ++nextId;
    nlohmann::json req = {
        {"jsonrpc", "2.0"},
        {"id", myId},
        {"method", method},
    };
    if (params.is_object() && !params.empty())
        req["params"] = params;
    if (!writeLine(req)) {
        outError = "write to server failed";
        return false;
    }

    // Idle deadline: it measures how long we have seen NO data, so a slow
    // but progressing response (blender-mcp streams its ~30 KB tool list
    // for tens of seconds) is not mistaken for a hang; a hard cap (4x)
    // still bounds a server that trickles one byte at a time.
    const auto start     = std::chrono::steady_clock::now();
    const auto hardLimit = start + std::chrono::milliseconds((int64_t)timeoutMs * 4);
    auto deadline = start + std::chrono::milliseconds(timeoutMs);
    std::string buf;
    for (;;) {
        if (abort && abort->load()) {
            outError = "aborted";
            return false;
        }
        pollfd pfd{outFd, POLLIN, 0};
        const int pr = poll(&pfd, 1, 50);
        if (pr < 0) {
            if (errno == EINTR) continue;
            outError = "poll failed";
            return false;
        }
        if (pr == 0) {
            // Nothing pending: the deadline and a dead child are the only
            // things that can unblock us.
            int st = 0;
            if (waitpid(pid_, &st, WNOHANG) > 0) {
                outError = "server exited";
                return false;
            }
            if (std::chrono::steady_clock::now() > deadline) {
                outError = "timed out";
                return false;
            }
            continue;
        }
        char tmp[8192];
        const ssize_t n = read(outFd, tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            outError = "read from server failed";
            return false;
        }
        if (n == 0) {
            outError = "server closed the connection";
            return false;
        }
        // Data arrived: the server is alive, give it a fresh idle window.
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        if (deadline > hardLimit)
            deadline = hardLimit;
        buf.append(tmp, (size_t)n);
        // Some servers stream one line in many chunks (blender-mcp's
        // tools/list is ~30 KB): a line is complete only once its newline
        // arrived; partial data stays in buf and is never parsed.
        for (;;) {
            const size_t nl = buf.find('\n');
            if (nl == std::string::npos)
                break; // no complete line yet: wait for more data
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            nlohmann::json msg = nlohmann::json::parse(line, nullptr, false);
            if (msg.is_discarded() || !msg.contains("id") || !msg["id"].is_number_integer() ||
                msg["id"].get<int>() != myId)
                continue; // notification, progress, or an older request's reply
            if (msg.contains("error")) {
                outError = msg["error"].is_object()
                               ? msg["error"].value("message", "MCP JSON-RPC error")
                               : "MCP JSON-RPC error";
                return false;
            }
            outResult = msg.value("result", nlohmann::json());
            return true;
        }
    }
}

bool McpServer::connect(const std::atomic<bool>* abort) {
    // One lock for the whole handshake: a disconnect (toggle-off of a
    // still-starting server) waits out the handshake instead of tearing
    // the pipes out from under it.
    std::lock_guard lock(*opMtx);
    status = "";
    if (command.empty()) {
        status = "no command";
        return false;
    }
    if (!spawn()) {
        return false;
    }
    nlohmann::json resp;
    std::string err;
    nlohmann::json initParams = {
        {"protocolVersion", "2025-06-18"},
        {"capabilities", nlohmann::json::object()},
        {"clientInfo", {{"name", "miniagent"}, {"version", "0.1"}}},
    };
    // rootUri tells the server which project this session is about (MCP's
    // term for the workspace root). The app never chdirs, so the process
    // cwd is the project miniagent was launched in.
    char cwdBuf[4096];
    if (::getcwd(cwdBuf, sizeof(cwdBuf))) {
        static const char* hex = "0123456789ABCDEF";
        std::string uri = "file://";
        for (unsigned char c : std::string(cwdBuf)) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                c == '/' || c == '-' || c == '.' || c == '_' || c == '~') {
                uri += (char)c;
            } else {
                uri += '%';
                uri += hex[c >> 4];
                uri += hex[c & 15];
            }
        }
        initParams["rootUri"] = uri;
    }
    if (!rpcLocked("initialize", initParams, initTimeoutMs, abort, resp, err)) {
        status = "initialize failed: " + err;
        teardownLocked();
        return false;
    }
    // Handshake step 2: the initialized notification (no id, no reply).
    if (!writeLine({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})) {
        status = "notifications/initialized write failed";
        teardownLocked();
        return false;
    }
    if (!rpcLocked("tools/list", nlohmann::json::object(), initTimeoutMs, abort, resp, err)) {
        status = "tools/list failed: " + err;
        teardownLocked();
        return false;
    }
    if (!resp.contains("tools") || !resp["tools"].is_array()) {
        status = "tools/list returned no tool list";
        teardownLocked();
        return false;
    }

    tools.clear();
    for (const auto& t : resp["tools"]) {
        const std::string tname = t.value("name", "");
        if (tname.empty())
            continue;
        Tool tool;
        tool.name        = mcpToolName(name, tname);
        tool.description = t.value("description", "");
        tool.paramsSchema = t.value("inputSchema", nlohmann::json::object());
        McpServer* self   = this;
        tool.execute      = [self, tname](nlohmann::json a, const std::atomic<bool>& ab) {
            return self->callTool(tname, a, ab);
        };
        tools.push_back(std::move(tool));
    }
    running = true;
    return true;
}

void McpServer::disconnect() {
    std::lock_guard lock(*opMtx);
    teardownLocked();
}

// Assumes *opMtx is held.
bool McpServer::teardownLocked() {
    tools.clear();
    running = false;
    if (inFd >= 0) {
        close(inFd);
        inFd = -1;
    }
    if (outFd >= 0) {
        close(outFd);
        outFd = -1;
    }
    if (pid_ > 0) {
        kill(pid_, SIGTERM);
        int st = 0;
        // Give it a moment to exit cleanly before escalating.
        for (int i = 0; i < 25 && waitpid(pid_, &st, WNOHANG) == 0; ++i)
            usleep(20000);
        if (waitpid(pid_, &st, WNOHANG) == 0)
            kill(pid_, SIGKILL);
        waitpid(pid_, &st, 0);
        pid_ = -1;
    }
    return true;
}

ToolResult McpServer::callTool(const std::string& tool, const nlohmann::json& args,
                               const std::atomic<bool>& abort) {
    std::lock_guard lock(*opMtx);
    if (!running)
        return {name + ": not connected", true};

    nlohmann::json params = {{"name", tool}};
    params["arguments"] = args.is_object() ? args : nlohmann::json::object();
    nlohmann::json resp;
    std::string err;
    if (!rpcLocked("tools/call", params, callTimeoutMs, &abort, resp, err))
        return {name + ": " + err, true};

    ToolResult out;
    // rpc() already unwrapped the "result" field.
    const nlohmann::json& res = resp;
    if (!res.is_object())
        return {name + ": malformed result from " + tool, true};
    const nlohmann::json& content = res.value("content", nlohmann::json::array());
    if (content.is_array()) {
        for (const auto& c : content) {
            const std::string type = c.value("type", "");
            if (type == "text") {
                if (!out.content.empty())
                    out.content += "\n";
                out.content += c.value("text", std::string());
            } else if (type == "image" && !out.image) {
                // First image content item becomes the result attachment.
                ImageAttachment img;
                img.base64   = c.value("data", std::string());
                img.mimeType = c.value("mimeType", std::string("image/png"));
                out.image   = std::move(img);
            } else if (type == "resource" && c.contains("resource") &&
                       c["resource"].contains("text")) {
                if (!out.content.empty())
                    out.content += "\n";
                out.content += c["resource"].value("text", std::string());
            }
        }
    }
    out.isError = res.value("isError", false);
    if (out.content.empty() && !out.image)
        out.content = "(no content)";
    return out;
}

std::vector<McpServer> loadMcpServers() {
    if (std::getenv("MINIAGENT_NO_MCP"))
        return {};
    std::vector<McpServer> out;
    std::ifstream f(mcpConfigPath());
    if (!f)
        return out;
    nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.contains("mcpServers") || !j["mcpServers"].is_object())
        return out;

    const auto settings = loadSettings();
    for (const auto& [name, def] : j["mcpServers"].items()) {
        if (!def.is_object())
            continue;
        const std::string command = def.value("command", std::string());
        if (command.empty())
            continue; // non-stdio transport (http/sse): unsupported for now
        McpServer s;
        s.name    = name;
        s.command = command;
        const nlohmann::json args = def.value("args", nlohmann::json::array());
        if (args.is_array())
            for (const auto& a : args)
                if (a.is_string())
                    s.args.push_back(a.get<std::string>());
        const nlohmann::json env = def.value("env", nlohmann::json::object());
        if (env.is_object())
            for (const auto& [k, v] : env.items())
                if (v.is_string())
                    s.env[k] = v.get<std::string>();
        if (def.contains("initTimeoutMs") && def["initTimeoutMs"].is_number_integer())
            s.initTimeoutMs = std::max(1000, def["initTimeoutMs"].get<int>());
        if (def.contains("callTimeoutMs") && def["callTimeoutMs"].is_number_integer())
            s.callTimeoutMs = std::max(1000, def["callTimeoutMs"].get<int>());
        s.enabled = true;
        const auto it = settings.find("mcp_disabled_" + keySafe(name));
        if (it != settings.end() && (it->second == "1" || it->second == "true"))
            s.enabled = false;
        out.push_back(std::move(s));
    }
    return out;
}

void saveMcpEnabled(const std::string& name, bool enabled) {
    auto settings = loadSettings();
    settings["mcp_disabled_" + keySafe(name)] = enabled ? "0" : "1";
    saveSettings(settings);
}
