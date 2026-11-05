#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

#include "agent/agent.h"
#include "agent/history_seed.h"

namespace fs = std::filesystem;

int main() {
    const std::string root = "/tmp/miniagent_history_seed_test";
    system("rm -rf /tmp/miniagent_history_seed_test && mkdir -p /tmp/miniagent_history_seed_test");

    ImageAttachment img;
    img.mimeType = "image/png";
    img.base64   = "aGVsbG8=";

    UserMessage u;
    u.text = "look at this [image 1]";
    u.images.push_back(img);

    AssistantMessage a;
    a.thinking   = "need the screenshot";
    ToolCallBlock tc;
    tc.id        = "c1";
    tc.name      = "read";
    tc.arguments = {{"path", "/tmp/shot.png"}};
    a.toolCalls.push_back(tc);
    a.stopReason = "toolUse";

    ToolResultMessage tr;
    tr.toolCallId = "c1";
    tr.toolName   = "read";
    tr.content    = "the screenshot";
    tr.image      = img;

    AssistantMessage fin;
    fin.text       = "the button is misaligned";
    fin.thinking   = "compared to the design";
    fin.stopReason = "stop";

    SessionStore store("/tmp/miniagent_history_seed_test/proj", root);
    const std::string path = store.startSession("probe");
    assert(!path.empty());
    assert(store.appendMessage(u));
    assert(store.appendMessage(a));
    assert(store.appendMessage(tr));
    assert(store.appendMessage(fin));

    {
        ProjectedSession proj = SessionStore::loadProjected(path);
        HistorySeed seed =
            selectHistorySeed(nullptr, "", &store, false);
        assert(!seed.fromLiveAgent);
        assert(seed.messages.size() == 4);
        assert(seed.fileBase == proj.fileBase && seed.entryCount == proj.entryCount);
        const auto* t = std::get_if<ToolResultMessage>(&seed.messages[2]);
        assert(t && !t->image.has_value());
        assert(!seed.poppedTrailing);
        assert(seed.sessionIndexed);
    }

    {
        std::vector<AgentMessage> hist = {u, a, tr, fin};
        Agent live("", OpenAIProvider{}, nullptr, {});
        live.setHistory(hist, 5, true);

        HistorySeed seed = selectHistorySeed(&live, path, &store, false);
        assert(seed.fromLiveAgent);
        assert(seed.messages.size() == 4);
        assert(seed.fileBase == 5 && seed.sessionIndexed);
        assert(!seed.poppedTrailing);
        const auto* t = std::get_if<ToolResultMessage>(&seed.messages[2]);
        assert(t && t->image.has_value());
        const auto* fu = std::get_if<UserMessage>(&seed.messages[0]);
        assert(fu && fu->images.size() == 1);

        HistorySeed stripped = selectHistorySeed(&live, path, &store, true);
        const auto* st = std::get_if<ToolResultMessage>(&stripped.messages[2]);
        assert(st && !st->image.has_value());

        HistorySeed other = selectHistorySeed(&live, "/tmp/other.jsonl", &store, false);
        assert(!other.fromLiveAgent);
        const auto* ot = std::get_if<ToolResultMessage>(&other.messages[2]);
        assert(ot && !ot->image.has_value());
    }

    {
        AssistantMessage dangling;
        ToolCallBlock tc2;
        tc2.id   = "c2";
        tc2.name = "bash";
        tc2.arguments = {{"command", "ls"}};
        dangling.toolCalls.push_back(tc2);
        dangling.stopReason = "toolUse";
        assert(store.appendMessage(dangling));

        std::vector<AgentMessage> hist = {u, a, tr, fin, dangling};
        Agent live("", OpenAIProvider{}, nullptr, {});
        live.setHistory(hist, 5, true);

        HistorySeed seed = selectHistorySeed(&live, path, &store, false);
        assert(seed.poppedTrailing);
        assert(seed.messages.size() == 4);
        assert(seed.entryCount == 5);
        const auto* t = std::get_if<ToolResultMessage>(&seed.messages[2]);
        assert(t && t->image.has_value());

        assert(store.truncateToEntries(seed.entryCount - 1));
        assert(store.entryCount() == 4);
        assert(SessionStore::loadMessages(path).size() == 4);

        HistorySeed fromFile = selectHistorySeed(nullptr, "", &store, false);
        assert(!fromFile.poppedTrailing);
        assert(fromFile.messages.size() == 4);
    }

    {
        SessionStore empty("/tmp/miniagent_history_seed_test/other", root);
        HistorySeed seed = selectHistorySeed(nullptr, "", &empty, false);
        assert(seed.messages.empty() && seed.fileBase == 0 && seed.entryCount == 0);
    }

    fs::remove_all(root);
    printf("history_seed_test: all passed\n");
    return 0;
}
