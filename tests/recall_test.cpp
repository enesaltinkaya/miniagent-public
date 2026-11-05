#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "tools/recall.h"

using nlohmann::json;

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static void checkContains(const std::string& haystack, const std::string& needle, const char* what) {
    if (haystack.find(needle) == std::string::npos) {
        std::printf("FAIL: %s\n  expected to contain: %s\n  got: %.300s\n", what, needle.c_str(),
                    haystack.c_str());
        ++failures;
    }
}

namespace {

std::string fixturePath = "/tmp/miniagent_recall_test.jsonl";

json toolCall(const std::string& id, const std::string& name, const std::string& path) {
    json tc;
    tc["id"]        = id;
    tc["name"]      = name;
    tc["arguments"] = json{{"path", path}};
    return tc;
}

void writeFixture() {
    json tc1 = json::array({toolCall("c1", "read", "src/cache.cpp")});
    json tc2 = json::array({toolCall("c2", "edit", "src/cache.cpp")});

    std::ofstream f(fixturePath, std::ios::trunc);
    f << json({{"type", "header"}, {"cwd", "/tmp"}, {"title", "recall test"}}).dump() << "\n";
    f << json({{"type", "user"}, {"text", "Fix the redis cache invalidation bug"}, {"timestamp", 1}}).dump() << "\n";
    f << json({{"type", "assistant"}, {"text", "Reading the cache code."}, {"toolCalls", tc1},
               {"stopReason", "toolUse"}, {"timestamp", 2}})
                .dump()
       << "\n";
    f << json({{"type", "tool"}, {"toolCallId", "c1"}, {"toolName", "read"},
               {"content", "void invalidate() {}"},
               {"args", R"({"path":"src/cache.cpp"})"}, {"timestamp", 3}})
                .dump()
       << "\n";
    f << json({{"type", "user"}, {"text", "Now write the regression test"}, {"timestamp", 4}}).dump() << "\n";
    f << json({{"type", "assistant"}, {"text", ""}, {"toolCalls", tc2},
               {"stopReason", "toolUse"}, {"timestamp", 5}})
                .dump()
       << "\n";
    f << json({{"type", "tool"}, {"toolCallId", "c2"}, {"toolName", "edit"}, {"content", "ok"},
               {"args", R"({"path":"src/cache.cpp"})"}, {"timestamp", 6}})
                .dump()
       << "\n";
    f << json({{"type", "compaction"}, {"keepFrom", 4},
               {"summary", "[Session Goal]\n- Fix the redis cache bug"},
               {"tokensBefore", 900}, {"tokensAfter", 80}})
                .dump()
       << "\n";
    f << json({{"type", "user"}, {"text", "Does it pass now?"}, {"timestamp", 8}}).dump() << "\n";
    f << json({{"type", "assistant"}, {"text", "Yes, the cache test passes."}, {"stopReason", "stop"},
               {"timestamp", 9}})
                .dump()
       << "\n";
}

std::string run(const Tool& tool, const json& args) {
    std::atomic<bool> abort{false};
    ToolResult r = tool.execute(args, abort);
    return r.content;
}

}  // namespace

int main() {
    writeFixture();
    Tool tool = makeRecallTool([] { return fixturePath; });

    { check(run(tool, json{{"query", "redis cache"}}).find("#1") != std::string::npos, "query finds the redis turn"); }
    {
        std::string out = run(tool, json{{"query", "redis invalidation"}});
        checkContains(out, "#1", "redis query ranks entry 1 first");
        checkContains(out, "Matches", "query reports match count");
    }
    {
        std::string out = run(tool, json{{"query", "nonexistent-word-xyz"}});
        checkContains(out, "No matches", "no-match reported");
    }
    {
        std::string out = run(tool, json{{"query", "cache"}, {"page", "2"}});
        (void)out;
    }
    {
        std::string out = run(tool, json{{"query", "test"}});
        checkContains(out, "page 1/", "page indicator present");
    }
    {
        std::string out = run(tool, json{{"range", json::array({1, 3})}});
        checkContains(out, "#1 user: Fix the redis", "range includes #1");
        checkContains(out, "#3 tool_result read", "range includes #3");
        check(out.find("#4") == std::string::npos, "range stops at 3");
    }
    {
        std::string out = run(tool, json{{"range", json::array({2, 9})}});
        checkContains(out, "page 1/", "wide range paginates");
        checkContains(out, "#2", "wide range first entry");
    }
    {
        std::string out = run(tool, json{{"range", json::array({100, 200})}});
        checkContains(out, "Range is empty", "out-of-range clamps");
    }
    {
        std::string out = run(tool, json{{"expand", json::array({3})}});
        checkContains(out, "# 3 tool_result", "expand renders kind");
        checkContains(out, "void invalidate()", "expand returns full text");
        checkContains(out, "args:", "expand includes args echo");
    }
    {
        std::string out = run(tool, json{{"expand", json::array({2})}});
        checkContains(out, "tool call read path=src/cache.cpp", "expand renders tool calls");
        checkContains(out, "result #3", "expand pairs the result");
    }
    {
        std::string out = run(tool, json{{"expand", json::array({7})}});
        checkContains(out, "compaction", "expand of compaction entry");
        checkContains(out, "redis cache bug", "compaction summary text");
    }
    {
        std::string out = run(tool, json{{"expand", json::array({99})}});
        checkContains(out, "out of range", "expand reports out of range");
    }
    {
        std::string out = run(tool, json{{"mode", "touched"}});
        checkContains(out, "- src/cache.cpp", "touched lists the file");
        checkContains(out, "#2 read", "touched lists the read entry");
        checkContains(out, "#5 edit", "touched lists the edit entry");
    }
    {
        std::string out = run(tool, json{{"query", "cache"}, {"page", 1}});
        checkContains(out, "Matches", "numeric page works");
    }
    {
        std::string out = run(tool, json{{"query", "cache"}, {"page", 50}});
        checkContains(out, "Use a page between", "page bounds reported");
    }
    {
        Tool missing = makeRecallTool([] { return std::string(); });
        std::atomic<bool> abort{false};
        ToolResult r = missing.execute(json{{"query", "x"}}, abort);
        check(r.isError, "no session file is an error");
    }

    if (failures == 0)
        std::printf("recall_test: all ok\n");
    else
        std::printf("recall_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
