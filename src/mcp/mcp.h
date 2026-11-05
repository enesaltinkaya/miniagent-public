#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <sys/types.h>

#include "tools/tools.h"

// MCP (Model Context Protocol) server support, stdio transport.
//
// Servers are defined in ~/.config/miniagent/mcp.json (user-level only):
//   { "mcpServers": {
//       "filesystem": { "command": "npx",
//                       "args": ["-y", "@modelcontextprotocol/server-filesystem", "/tmp"],
//                       "env": { "KEY": "value" },
//                       "initTimeoutMs": 30000,
//                       "callTimeoutMs": 300000 } } }
// Each entry with a "command" string is a stdio server: it is spawned
// (fork + execvp), initialized per the MCP handshake (initialize ->
// notifications/initialized -> tools/list), and every server tool is
// exposed to the agent as a Tool named mcp_<server>_<tool>. Entries without
// a command (e.g. future HTTP/SSE transports) are skipped. Set
// MINIAGENT_NO_MCP in the environment to disable loading entirely (tests).
//
// Connects happen on background threads (a first `uvx`/`npx` run may
// download a package and take a while), so the UI thread never blocks on a
// slow server. Enable/disable state is persisted in miniagent.settings as
// mcp_disabled_<name> (default: enabled), toggled from the MCP modal.

struct McpServer {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env; // overrides for the child's environment
    int initTimeoutMs = 30000;  // idle timeout for the handshake (no-data window,
                                // extended while a response is still streaming);
                                // first uvx/npx runs also download a package: slow
    int callTimeoutMs = 300000; // tools/call deadline (5 min)

    bool enabled = true;   // persisted state
    bool running = false;  // child alive and initialized
    // A background connect is in flight (unique_ptr: keeps McpServer movable).
    std::unique_ptr<std::atomic<bool>> connecting =
        std::make_unique<std::atomic<bool>>(false);
    std::string status;   // "" = connected; otherwise the current/failure text
    std::vector<Tool> tools; // wrappers built at connect time

    // Spawn + full MCP handshake + tools/list. On success running = true and
    // tools is filled; on failure running = false and status holds the error.
    // May take up to initTimeoutMs: never call this on the UI thread.
    // Safe to call repeatedly (a live connection is replaced). `abort`, when
    // set, interrupts the handshake (used at app shutdown).
    bool connect(const std::atomic<bool>* abort = nullptr);
    // Terminate the child (SIGTERM, then SIGKILL), close the pipes and drop
    // the tools. No-op when not connected. May block for a short while if a
    // connect is in flight (they serialize on the same lock).
    void disconnect();
    // Send one tools/call for `tool` (by its raw server-side name). Public
    // so tests can reach the JSON-RPC error path directly.
    ToolResult callTool(const std::string& tool, const nlohmann::json& args,
                        const std::atomic<bool>& abort);
    // Child pid while connected, -1 otherwise (tests verify cleanup).
    pid_t pid() const { return pid_; }

private:
    int inFd = -1;  // our write end of the server's stdin
    int outFd = -1; // our read end of the server's stdout
    pid_t pid_ = -1;

    int nextId = 0;
    // One lock for the whole operation (connect/disconnect/tool call), not
    // just the individual RPC: a tool can only be called once connected, so
    // this only ever serializes connect-vs-disconnect (a toggle of a
    // still-starting server waits out the handshake).
    std::unique_ptr<std::mutex> opMtx = std::make_unique<std::mutex>();

    bool spawn();
    bool teardownLocked();
    bool writeLine(const nlohmann::json& value);
    // Assumes *opMtx is held. Writes one JSON-RPC request line, then reads
    // newline-delimited responses until the one with the matching id (or a
    // JSON-RPC error) arrives. Returns false (with outError) on timeout,
    // pipe death or abort.
    bool rpcLocked(const std::string& method, const nlohmann::json& params,
                   int timeoutMs, const std::atomic<bool>* abort,
                   nlohmann::json& outResult, std::string& outError);
};

// Parse ~/.config/miniagent/mcp.json, filling in each server's persisted
// enabled state from miniagent.settings. Missing/unparseable file: empty
// vector (no servers), never an error. Returns empty when the environment
// variable MINIAGENT_NO_MCP is set.
std::vector<McpServer> loadMcpServers();

// Path of the user-level config file (shown in the MCP modal).
std::string mcpConfigPath();

// Persist one server's enabled flag (mcp_disabled_<name> = 0/1).
void saveMcpEnabled(const std::string& name, bool enabled);

// mcp_<server>_<tool>, namespaced so server tools never collide with the
// built-ins or with tools of another server.
std::string mcpToolName(const std::string& server, const std::string& tool);
