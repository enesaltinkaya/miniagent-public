// MCP client tests (src/mcp/mcp.cpp). Spawns a tiny stdio JSON-RPC server
// written in python3 (stdlib only) into /tmp and checks: the
// initialize/initialized/tools-list handshake, tool wrapping (names,
// schemas), text / error / image tool results, failure paths (bad
// command, hung server), process cleanup on disconnect, and the
// mcp.json + miniagent.settings config layer.
#include "mcp/mcp.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <string>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

namespace {

constexpr const char* kMcpTest = "/tmp/mcp_test";
constexpr const char* kScript  = "/tmp/mcp_test/server.py";

const char kServerPy[] = R"PY(
import json, os, sys, time
def send(o):
    sys.stdout.write(json.dumps(o) + "\n"); sys.stdout.flush()
for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    req = json.loads(line)
    m, rid = req.get("method"), req.get("id")
    if m == "initialize":
        send({"jsonrpc": "2.0", "id": rid, "result": {
            "protocolVersion": req["params"]["protocolVersion"],
            "capabilities": {"tools": {}},
            "serverInfo": {"name": "test", "version": "0.1"}}})
    elif m == "notifications/initialized":
        pass
    elif m == "tools/list":
        if os.environ.get("HANG") == "1":
            time.sleep(30)
        send({"jsonrpc": "2.0", "id": rid, "result": {"tools": [
            {"name": "add", "description": "Add two numbers",
             "inputSchema": {"type": "object",
                             "properties": {"a": {"type": "number"}, "b": {"type": "number"}},
                             "required": ["a", "b"]}},
            {"name": "boom", "description": "Always fails",
             "inputSchema": {"type": "object", "properties": {}}},
            {"name": "pic", "description": "Returns a tiny image",
             "inputSchema": {"type": "object", "properties": {}}},
            {"name": "chatter", "description": "Sends progress then replies",
             "inputSchema": {"type": "object", "properties": {}}},
            {"name": "big", "description": "Returns a 100 KB single-line response",
             "inputSchema": {"type": "object", "properties": {}}},
            {"name": "drip", "description": "Trickles progress then replies",
             "inputSchema": {"type": "object", "properties": {}}}]}})
    elif m == "tools/call":
        p = req["params"]
        if p["name"] == "add":
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": str(p["arguments"]["a"] + p["arguments"]["b"])}]}})
        elif p["name"] == "boom":
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "isError": True, "content": [{"type": "text", "text": "boom failure"}]}})
        elif p["name"] == "pic":
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "image",
                            "data": "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1f+X8AAAAEVl0Q0D///+Pbz0RAAAAOUlEQVQYV2NkYGD4z8DAAAAGAwD5P8wKqQAAAABJRU5ErkJggg==",
                            "mimeType": "image/png"}]}})
        elif p["name"] == "chatter":
            # A progress notification must not be mistaken for the reply.
            sys.stdout.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/progress",
                                        "params": {"progressToken": 1}}) + "\n"); sys.stdout.flush()
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": "done"}]}})
        elif p["name"] == "big":
            # One line far larger than any single read: the client must
            # reassemble it, not parse the partial fragments.
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": "x" * 100000}]}})
        elif p["name"] == "drip":
            # Progress every 300 ms for ~3 s: an idle deadline (gap-based,
            # not total-time) must keep extending while data arrives.
            for i in range(10):
                time.sleep(0.3)
                sys.stdout.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/progress",
                                            "params": {"progressToken": i}}) + "\n"); sys.stdout.flush()
            send({"jsonrpc": "2.0", "id": rid, "result": {
                "content": [{"type": "text", "text": "dripped"}]}})
        else:
            send({"jsonrpc": "2.0", "id": rid, "error": {
                "code": -32602, "message": "unknown tool"}})
    else:
        if rid is not None:
            send({"jsonrpc": "2.0", "id": rid, "error": {"code": -32601, "message": "unknown method"}})
)PY";

std::atomic<bool> noAbort{false};

} // namespace

int main() {
    std::system("rm -rf /tmp/mcp_test && mkdir -p /tmp/mcp_test");
    {
        std::ofstream f(kScript);
        f.write(kServerPy, std::strlen(kServerPy));
    }
    setenv("HOME", kMcpTest, 1);  // config files go under /tmp/mcp_test/.config

    // ---- name / key helpers ------------------------------------------
    check(mcpToolName("files", "read") == "mcp_files_read", "tool name namespacing");

    // ---- config layer -------------------------------------------------
    {
        std::filesystem::create_directories(std::string(kMcpTest) + "/.config/miniagent");
        {
            std::ofstream f(mcpConfigPath());
            f << R"({
                "mcpServers": {
                    "alpha": {"command": "python3",
                              "args": ["/tmp/mcp_test/server.py"],
                              "env": {"FOO": "bar"}},
                    "beta": {"command": "echo"},
                    "gamma": {"url": "http://localhost:9999"}
                }
            })";
        }
        auto servers = loadMcpServers();
        check(servers.size() == 2, "only stdio (command) entries are loaded");
        check(servers[0].name == "alpha" && servers[1].name == "beta",
              "servers in config order");
        check(servers[0].args.size() == 1 && servers[0].args[0] == "/tmp/mcp_test/server.py",
              "args parsed");
        check(servers[0].env.size() == 1 && servers[0].env["FOO"] == "bar", "env parsed");
        check(servers[0].enabled && servers[1].enabled, "new servers default enabled");

        saveMcpEnabled("beta", false);
        saveMcpEnabled("alpha", true);
        servers = loadMcpServers();
        check(servers[0].enabled == true, "persisted enable is read back");
        check(servers[1].enabled == false, "persisted disable is read back");
    }

    // ---- connect + handshake ------------------------------------------
    McpServer srv;
    srv.name    = "test";
    srv.command = "python3";
    srv.args    = {kScript};
    check(srv.connect(), "connect succeeds");
    check(srv.running && srv.status.empty(), "connected: running, no status");
    check(srv.tools.size() == 6, "six tools discovered");
    if (srv.tools.size() == 6) {
        check(srv.tools[0].name == "mcp_test_add", "wrapper name");
        check(srv.tools[0].description == "Add two numbers", "description passed through");
        check(srv.tools[0].paramsSchema.is_object() &&
                  srv.tools[0].paramsSchema["required"].is_array(),
              "inputSchema passed through");
    }

    // ---- tool calls -----------------------------------------------------
    if (!srv.running || srv.tools.size() != 6) {
        std::printf("  (skipping tool-call checks: %s)\n",
                    srv.status.empty() ? "not connected" : srv.status.c_str());
        check(false, "server available for tool calls");
        srv.disconnect();
        return 1;
    }
    nlohmann::json addArgs = {{"a", 2}, {"b", 3}};
    ToolResult r = srv.tools[0].execute(addArgs, noAbort);
    check(!r.isError && r.content == "5", "add(2,3) -> 5");

    r = srv.tools[1].execute(nlohmann::json::object(), noAbort);
    check(r.isError && r.content == "boom failure", "isError tool result");

    r = srv.tools[2].execute(nlohmann::json::object(), noAbort);
    check(r.image.has_value() && r.image->mimeType == "image/png" &&
              !r.image->base64.empty(),
          "image content mapped to ToolResult::image");

    r = srv.tools[3].execute(nlohmann::json::object(), noAbort);
    check(!r.isError && r.content == "done",
          "progress notification skipped, real reply matched");

    r = srv.tools[4].execute(nlohmann::json::object(), noAbort);
    check(!r.isError && r.content.size() == 100000,
          "multi-read response reassembled (line spanning chunks)");

    // The drip takes ~3.3 s total with 300 ms gaps: with a 1 s IDLE timeout
    // it must succeed only because each arriving progress notification
    // extends the deadline (a total-time timeout would fail it at 1 s).
    srv.callTimeoutMs = 1000;
    r = srv.tools[5].execute(nlohmann::json::object(), noAbort);
    check(!r.isError && r.content == "dripped",
          "slow response kept alive by idle-deadline extension");
    srv.callTimeoutMs = 300000;

    // unknown tool -> JSON-RPC error object -> error result
    r = srv.callTool("nope", nlohmann::json::object(), noAbort);
    check(r.isError && r.content.find("unknown tool") != std::string::npos,
          "unknown tool reports the server's error");

    // ---- failure paths ---------------------------------------------------
    McpServer bad;
    bad.name    = "bad";
    bad.command = "/tmp/mcp_test/definitely-not-a-binary";
    bad.args    = {};
    bad.initTimeoutMs = 2000;
    check(!bad.connect(), "bad command: connect fails");
    check(!bad.running && !bad.status.empty(), "bad command: status set");
    bad.disconnect();  // must not crash with nothing spawned

    McpServer hang;
    hang.name           = "hang";
    hang.command        = "python3";
    hang.args           = {kScript};
    hang.env             = {{"HANG", "1"}};
    hang.initTimeoutMs  = 1500;
    check(!hang.connect(), "hung server: connect times out");
    check(!hang.running && hang.status.find("timed out") != std::string::npos,
          "hung server: timeout reported");
    hang.disconnect();

    // ---- disconnect reaps the child -----------------------------------
    McpServer conn;
    conn.name    = "test";
    conn.command = "python3";
    conn.args    = {kScript};
    check(conn.connect(), "second connect");
    const pid_t child = conn.pid();
    check(child > 0, "child pid recorded");
    conn.disconnect();
    check(kill(child, 0) == -1 && errno == ESRCH, "disconnect kills the child");

    std::printf(failures == 0 ? "all mcp tests passed\n" : "%d mcp test failures\n", failures);
    return failures == 0 ? 0 : 1;
}
