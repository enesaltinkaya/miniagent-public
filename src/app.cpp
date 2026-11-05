#include "app.h"

#include "agent/agents.h"
#include "agent/history_seed.h"
#include "agent/scaffold.h"
#include "agent/skills.h"
#include "agent/system_prompt.h"
#include "app_commands.h"
#include "imgui.h"
#include "tools/factories.h"
#include "tools/recall.h"
#include "llm/backends.h"
#include "ui/chords.h"
#include "ui/draw.h"
#include "ui/history.h"
#include "ui/fonts.h"
#include "util/imagetex.h"
#include "util/log.h"
#include "util/notify.h"
#include "util/process.h"
#include "util/settings.h"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
// file_tools=0 in ~/.config/miniagent/miniagent.settings disables the
// read/write/edit tools so only bash touches files (experiment setting).
bool fileToolsEnabled() {
    auto settings = loadSettings();
    auto it       = settings.find("file_tools");
    return it == settings.end() || (it->second != "0" && it->second != "false");
}

// apply_patch=1 in ~/.config/miniagent/miniagent.settings replaces the edit
// tool with codex-style apply_patch editing (see tools/apply_patch.cpp):
// one patch per call, fuzzy-matched, with add/delete/move support. Takes
// effect for the next prompt (rebuildTools runs per send).
bool applyPatchEnabled() {
    auto settings = loadSettings();
    auto it       = settings.find("apply_patch");
    return it != settings.end() && (it->second == "1" || it->second == "true");
}

// The web tools talk to a local SearXNG instance (its JSON API). $SEARXNG_URL
// overrides ~/.config/miniagent/miniagent.settings' 'searxng_url', which in
// turn defaults to the localhost instance; an empty/off value unregisters both
// web tools.
std::string searxngBaseUrl() {
    if (const char* env = std::getenv("SEARXNG_URL"))
        return *env ? std::string(env) : std::string();
    auto settings = loadSettings();
    auto it       = settings.find("searxng_url");
    if (it == settings.end())
        return "http://localhost:7070";
    const std::string& v = it->second;
    if (v.empty() || v == "off" || v == "0" || v == "false")
        return {};
    return v;
}

// Fallback output-token estimate from a streamed character count, used only
// when no deltas were counted (e.g. a ToolCallStart for a call that streamed
// no argument fragments). Deliberately conservative: a chars/N heuristic with
// N < 4 over-counts tokens (whitespace-heavy code often exceeds 4 chars per
// token), which pushed the live tok/s above the backend's reported rate.
int estimateOutputTokens(std::size_t chars) {
    return static_cast<int>((chars + 3) / 4);
}
} // namespace

App::App() {
    char buf[PATH_MAX] = {0};
    if (getcwd(buf, sizeof(buf)))
        cwd = buf;
    else
        cwd = ".";
    sessions = std::make_unique<SessionStore>(cwd);
    refreshSessionList();
    rebuildTools();
    LoadHistory(history);

    // User-level MCP servers: load the config and start the enabled ones in
    // the background (never on this thread: a first uvx/npx run downloads a
    // package and may take longer than the window is even showing). Their
    // tools land in the registry when a connect finishes (mcpDirty -> Frame).
    mcpServers = loadMcpServers();
    for (size_t i = 0; i < mcpServers.size(); ++i)
        if (mcpServers[i].enabled)
            startMcpServer(i);
    rebuildTools();

    // Skills (<cwd>/.miniagent/skills, ~/.config/miniagent/skills): advertised
    // in the system prompt, forced with "/skill:name", listed with "/skills".
    reloadSkills();
    for (const auto& d : skillDiagnostics)
        logLine("skill warning: %s (%s)", d.message.c_str(), d.path.c_str());

    // Persisted settings (tool-output display, selected backend) and the LLM
    // backend list.
    if (EnsureConfigDir()) {
        auto settings = loadSettings();
        std::string backendErr;
        backends = loadBackends(&backendErr);
        if (!backendErr.empty()) {
            backendError = backendErr;
            logLine("%s", backendErr.c_str());
        }
        if (!backends.empty()) {
            // Remembered selection (Backends modal); clamped to the list.
            auto bIt = settings.find("backend_index");
            if (bIt != settings.end()) {
                const int saved = std::atoi(bIt->second.c_str());
                if (saved >= 0 && (size_t)saved < backends.size())
                    backendIndex = (size_t)saved;
            }
            // Designated image model (Backends modal); out-of-range values
            // (the backends list may have shrunk since) disable the feature.
            auto iIt = settings.find("image_backend_index");
            if (iIt != settings.end()) {
                const int saved = std::atoi(iIt->second.c_str());
                if (saved >= 0 && (size_t)saved < backends.size())
                    imageBackendIndex = (size_t)saved;
            }
            setBackend(backendIndex);
        }
        auto it = settings.find("tool_preview_lines");
        if (it != settings.end()) toolPreviewLines = std::max(0, std::atoi(it->second.c_str()));
        it = settings.find("tool_expand_key");
        if (it != settings.end() && ParseKeyChord(it->second.c_str(), toolExpandChord))
            toolExpandKey = it->second;
        else
            ParseKeyChord(toolExpandKey.c_str(), toolExpandChord);  // default, Ctrl+O
        it = settings.find("show_thinking");
        if (it != settings.end()) showThinking = it->second != "0" && it->second != "false";
        it = settings.find("send_thinking");
        if (it != settings.end()) sendThinking = it->second != "0" && it->second != "false";
        it = settings.find("context_window");
        if (it != settings.end()) {
            contextWindow = std::max(0, std::atoi(it->second.c_str()));
            contextWindowManual = contextWindow > 0;
        }
    }
}

void App::setScaffoldMode(bool on) {
    if (scaffoldMode == on) return;
    scaffoldMode = on;  // session only: the app always starts in Simple mode
}

// The Loop checkbox. Enabling does not start anything by itself: the countdown
// is armed when a run settles, so arming mid-run applies to that run's end.
// Unchecking drops a pending restart immediately.
void App::setLoopEnabled(bool on) {
    if (loopEnabled == on) return;
    loopEnabled = on;  // session only: the app always starts with the loop off
    if (!on)
        cancelLoop();
}

void App::cancelLoop(bool silent) {
    if (!loopArmed)
        return;
    loopArmed  = false;
    loopFireAt = {};
    if (silent)
        return;
    UiMessage m;
    m.kind = UiMessage::System;
    m.text = "Loop cancelled.";
    addMessage(std::move(m));
    speakLoop("loop stopped");
}

// Arm the countdown for the next restart. The log row names the prompt so the
// loop is visible in the history, not just in the checkbox.
void App::armLoop() {
    loopArmed  = true;
    loopFireAt = std::chrono::steady_clock::now() +
                 std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                     std::chrono::duration<double>(loopDelayS));
    std::string flat;
    for (char c : loopPrompt)
        flat.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
    if (flat.size() > 60)
        flat = flat.substr(0, 60) + "\u2026";
    UiMessage m;
    m.kind = UiMessage::System;
    m.text = "Loop: restarting \"" + flat + "\" in " + std::to_string((int)loopDelayS) +
             "s, fresh session each run (Esc or uncheck Loop to stop).";
    addMessage(std::move(m));
    speakLoop("loop in " + std::to_string((int)loopDelayS) + " seconds");
}

// The countdown ran out (App::tick): replay the prompt through the normal
// send path, then put back whatever the user typed in the meantime - the send
// clears inputBuf, and a draft written during the pause must not be eaten.
void App::fireLoop() {
    loopArmed  = false;
    loopFireAt = {};
    if (!loopEnabled || loopPrompt.empty())
        return;
    ++loopIteration;
    const std::string draft = inputBuf;
    speakLoop("loop " + std::to_string(loopIteration));
    // Fresh context every iteration: the loop restarts the task, it does not
    // continue the conversation. newSession() closes the session file and
    // clears the log/context counters; the dispatch below opens a new one
    // (and, in Scaffold mode, startScaffold's resume check sees a done or
    // absent ledger, so each iteration gets a fresh ledger too).
    newSession();
    loopDispatch(loopPrompt);
    if (!draft.empty()) {
        snprintf(inputBuf, sizeof(inputBuf), "%s", draft.c_str());
        promptGuardSkip = true;  // buffer rewrite must not look like a user edit
    }
}

// UI thread (the Compact button): with a run up, arm the queue and hand it to
// whichever run is live. Simple mode's Agent consumes it at the next turn
// boundary (the run continues on the compacted conversation); a scaffold run
// raises the shared flag consumed by the live role's own agent. Between roles
// (verification, state saves) there is no live context to compact, so the
// click does not queue - the button renders that state as disabled. With no run
// up there is no boundary to wait for, so the click compacts right away.
void App::queueCompaction() {
    if (compactQueued.load())
        return;
    if (!agentRunning) {
        compactIdle();
        return;
    }
    if (agent) {
        compactQueued.store(true);
        agent->requestCompaction();
        return;
    }
    if (!scaffoldRunActive || scaffoldRole.empty())
        return;
    compactQueued.store(true);
    manualCompactRequest.store(true);
}

// The idle (no run) half of the Compact button, Simple mode only: rewrite the
// conversation the next prompt would continue from, on the click. Nothing here
// talks to the model (vccCompact is deterministic), so it runs inline on the
// UI thread; the Compaction note it emits lands through the normal event queue
// and clears compactQueued.
//
// Whichever source the next prompt seeds from is what gets compacted: the live
// agent's in-memory messages when they still belong to the open session (they
// keep image attachments, so sendPrompt seeds from them), otherwise the session
// file's projected view - compacted through a throwaway agent whose compaction
// record is persisted, so the next prompt loads the compacted view from the
// file. A Scaffold run leaves no live agent and clears liveHistoryPath, so it
// takes the file path (its own roles' conversations are per-role and gone).
void App::compactIdle() {
    if (!canCompactIdle())
        return;
    compactQueued.store(true);
    if (agent && liveHistoryPath == sessions->currentPath()) {
        agent->compactNow();
        return;
    }
    OpenAIProvider provider;
    provider.model         = model;
    provider.baseUrl       = providerUrl;
    provider.apiKey        = apiKey;
    provider.sendThinking  = sendThinking;
    provider.contextWindow = contextWindow;
    ProjectedSession proj = SessionStore::loadProjected(sessions->currentPath());
    Agent fileAgent(buildSystemPrompt(cwd, fileToolsEnabled(), &skills, webToolsEnabled,
                                      fileToolsEnabled() && applyPatchEnabled()),
                    provider,
                    &toolRegistry,
                    [this](const AgentEvent& ev) {
                        std::lock_guard lock(eventMutex);
                        eventQueue.push_back(ev);
                    },
                    [](const AgentMessage&) {},
                    [this](const CompactionRecord& r) { sessions->appendCompaction(r); });
    fileAgent.setHistory(std::move(proj.messages), proj.fileBase, /*sessionIndexed=*/true);
    fileAgent.compactNow();
}

void App::setBackend(size_t index) {
    if (backends.empty())
        return;
    if (index >= backends.size())
        index = backends.size() - 1;
    backendIndex = index;
    const LlmBackend& b = backends[index];
    backendName = b.name;
    providerUrl = b.url;
    apiKey      = b.apiKey;
    model       = b.model;
    backendGen++;
    // The new server may expose a different (or no) context window; refresh
    // it in the background. Also runs at startup via the remembered backend.
    fetchContextWindowAsync();
}

void App::fetchContextWindowAsync() {
    if (contextWindowManual)
        return;
    // Tests (headless App instances) skip the probe: it would fire a real
    // request at the user's configured backend and its detached thread races
    // with process teardown at exit.
    if (std::getenv("MINIAGENT_NO_PROBE"))
        return;
    std::string url = providerUrl, key = apiKey, mdl = model;
    const int gen = backendGen.load();
    std::thread([this, url, key, mdl, gen]() {
        if (destroying) return;
        bool found = false;
        const int w = probeContextWindow(url, key, mdl, &found);
        // Discard the result if the backend changed (or the app went away)
        // while the probe was in flight. A successful probe on a server that
        // exposes no window clears the stale value so the strip doesn't keep
        // showing the previous server's number.
        if (!destroying && !contextWindowManual && backendGen.load() == gen && found)
            contextWindow = w;
    }).detach();
}

void App::setImageBackend(size_t index) {
    if (index >= backends.size())
        return;
    // Toggling the currently designated model clears the designation.
    imageBackendIndex = (imageBackendIndex == index) ? SIZE_MAX : index;
    std::map<std::string, std::string> settings = loadSettings();
    settings["image_backend_index"] =
        imageBackendIndex == SIZE_MAX ? "-1" : std::to_string(imageBackendIndex);
    saveSettings(settings);
}

App::~App() {
    shutdown();
}

void App::toggleToolExpansion() {
    toolPreviewLines = toolPreviewLines > 0 ? 0 : 3;
    for (auto& m : uiMessages)
        if (m.kind == UiMessage::Tool) m.toolExpanded = toolPreviewLines == 0;
    invalidateMessageAdvances();
    std::map<std::string, std::string> settings = loadSettings();
    settings["tool_preview_lines"]              = std::to_string(toolPreviewLines);
    settings["tool_expand_key"]                 = toolExpandKey;
    saveSettings(settings);
}

void App::toggleThinking() {
    showThinking = !showThinking;
    invalidateMessageAdvances();
    std::map<std::string, std::string> settings = loadSettings();
    settings["show_thinking"]                   = showThinking ? "1" : "0";
    saveSettings(settings);
}

void App::rebuildTools() {
    toolRegistry = ToolRegistry{};
    if (fileToolsEnabled()) {
        // With a designated image model the main backend cannot receive image
        // bytes (most non-vision endpoints reject image parts outright): read
        // then reports images textually and steers to analyze_image.
        std::string visionHint;
        if (imageBackendIndex < backends.size())
            visionHint = "The current backend cannot view image content; do NOT describe or "
                         "interpret this image yourself. Use the analyze_image tool on this "
                         "file instead (pass its path and a question).";
        toolRegistry.add(makeReadTool(cwd, visionHint));
        toolRegistry.add(makeWriteTool(cwd));
        // apply_patch=1 swaps find-and-replace editing for codex-style
        // patches (see applyPatchEnabled).
        if (applyPatchEnabled())
            toolRegistry.add(makeApplyPatchTool(cwd));
        else
            toolRegistry.add(makeEditTool(cwd));
        toolRegistry.add(makeGrepTool(cwd));
    }
    toolRegistry.add(makeBashTool(cwd));
    // Web search/fetch: registered only when a SearXNG base URL is configured
    // (see searxngBaseUrl). The main agent gets them; runSubagent filters them
    // out again, so scaffold/subagent roles never see a web tool.
    const std::string searxng = searxngBaseUrl();
    webToolsEnabled           = !searxng.empty();
    if (webToolsEnabled) {
        toolRegistry.add(makeWebSearchTool(searxng));
        toolRegistry.add(makeWebFetchTool());
    }
    // Recall: search the session file (full history incl. compacted-away
    // turns). Registered always - even before a first compaction it is a
    // useful transcript search.
    toolRegistry.add(makeRecallTool([this] { return sessions->currentPath(); }));
    // Subagent dispatch (manager-worker scaffold roles): fresh context per
    // run, filtered tools per role, the parent's abort flag shared.
    OpenAIProvider provider;
    provider.model         = model;
    provider.baseUrl       = providerUrl;
    provider.apiKey        = apiKey;
    provider.sendThinking  = sendThinking;
    provider.contextWindow = contextWindow;
    toolRegistry.add(makeSubagentTool(
        cwd,
        provider,
        [this] { return discoverAgents(cwd); },
        [this] { return toolRegistry.all(); },
        [this] { return agent ? &agent->abortFlag() : nullptr; },
        [this](const AgentEvent& ev) {
            std::lock_guard lock(eventMutex);
            eventQueue.push_back(ev);
        }));
    // Image analysis: registered only when the Backends modal designates an
    // image model; the vision call goes straight to that backend.
    if (imageBackendIndex < backends.size())
        toolRegistry.add(makeAnalyzeImageTool(cwd, backends[imageBackendIndex]));
    // Enabled, connected MCP servers contribute their tools (named
    // mcp_<server>_<tool>); a failed server simply adds nothing.
    for (auto& s : mcpServers)
        if (s.enabled && s.running)
            for (auto& t : s.tools)
                toolRegistry.add(t);
}

// The modal's checkbox has already flipped s.enabled; this persists the
// state and starts/stops the process on a background thread. No-op while a
// run is up: the registry is then owned by the running agent.
void App::setMcpServerEnabled(size_t index, bool enabled) {
    if (index >= mcpServers.size() || agentRunning)
        return;
    McpServer& s = mcpServers[index];
    s.enabled = enabled;
    saveMcpEnabled(s.name, s.enabled);
    if (enabled)
        startMcpServer(index);
    else
        stopMcpServer(index);
    // Drop the server's tools immediately if it was on; they are re-added
    // via mcpDirty once the new connection finishes.
    rebuildTools();
}

void App::startMcpServer(size_t index) {
    if (index >= mcpServers.size())
        return;
    McpServer& s = mcpServers[index];
    if (s.connecting->load() || s.running)
        return;
    *s.connecting = true;
    s.status = "starting\u2026";
    std::thread([this, index]() {
        McpServer& server = mcpServers[index];
        server.connect(&mcpAbort);
        *server.connecting = false;
        if (server.running) server.status = "";
        mcpDirty = true;
    }).detach();
}

void App::stopMcpServer(size_t index) {
    if (index >= mcpServers.size())
        return;
    McpServer& s = mcpServers[index];
    if (!s.running && !s.connecting->load())
        return;
    std::thread([this, index]() {
        // disconnect() serializes against an in-flight connect (shared lock);
        // the stop wins: a connect that just finished gets torn down here.
        mcpServers[index].disconnect();
        *mcpServers[index].connecting = false;
        mcpServers[index].status = "";
        mcpDirty = true;
    }).detach();
}

void App::reloadSkills() {
    SkillsResult r = loadSkills(cwd);
    skills         = std::move(r.skills);
    skillDiagnostics = std::move(r.diagnostics);
}

void App::listSkills() {
    std::string text;
    if (skills.empty())
        text = "No skills loaded.\nLooked in " + cwd + "/.miniagent/skills and " + userSkillsDir() +
               ".\nA skill is a directory holding a SKILL.md with a name and a description in its "
               "frontmatter; send \"/skill:name <extra instructions>\" to force one.";
    else {
        text = "Skills (" + std::to_string(skills.size()) + ")";
        for (const auto& s : skills) {
            text += "\n/skill:" + s.name;
            if (s.disableModelInvocation)
                text += "  (explicit only)";
            text += " - " + s.description;
        }
    }
    for (const auto& d : skillDiagnostics)
        text += "\nwarning: " + d.message + " (" + d.path + ")";
    UiMessage m;
    m.kind = UiMessage::System;
    m.text = std::move(text);
    addMessage(std::move(m));
}

void App::sendPromptText(const std::string& text) {
    snprintf(inputBuf, sizeof(inputBuf), "%s", text.c_str());
    sendPrompt();
}

void App::sendPrompt() {
    std::string prompt = inputBuf;
    size_t start       = prompt.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return;
    prompt.erase(0, start);
    while (!prompt.empty() && isspace((unsigned char)prompt.back())) prompt.pop_back();
    if (prompt.empty()) return;

    // /scaffold <task>: run the deterministic manager-worker scaffold for the
    // task (ledger + brainstorm/worker roles). The Simple/Scaffold
    // toggle does the same for every plain prompt; / commands always keep
    // their meaning in both modes.
    std::string historyText = prompt;
    std::string scaffoldTask;
    if (prompt == "/scaffold" || prompt.rfind("/scaffold ", 0) == 0) {
        const std::string t = prompt.size() > 10 ? prompt.substr(10) : "";
        if (t.empty()) {
            UiMessage m;
            m.kind = UiMessage::Error;
            m.text = "usage: /scaffold <task>";
            addMessage(std::move(m));
            return;
        }
        scaffoldTask = t;
    } else if (scaffoldMode && !isSlashCommand(prompt)) {
        scaffoldTask = prompt;
    }

    // Reload before every prompt so editing a SKILL.md takes effect on the
    // next send (pi needs a /reload for that).
    reloadSkills();

    // /skills: show what was discovered (and what failed to load) without
    // starting a run.
    if (prompt == "/skills") {
        inputBuf[0] = 0;
        listSkills();
        return;
    }

    // /skill:name args: inline a skill's instructions as the user message. The
    // model can also load a skill by itself from the <available_skills> list;
    // this path forces it, and reaches skills hidden from the prompt
    // (disable-model-invocation). The prompt box keeps showing what was typed.
    std::string modelText = prompt;
    if (prompt.rfind("/skill:", 0) == 0) {
        modelText = expandSkillCommand(skills, prompt);
        if (modelText.empty()) {
            UiMessage m;
            m.kind = UiMessage::Error;
            m.text = "unknown skill (see /skills): " + prompt;
            addMessage(std::move(m));
            return;
        }
    }

    // Enter while the agent is running: queue the message (pi's "steer" mode)
    // and let the agent worker send it as the next run.
    if (agentRunning && prompt == "/new") return;

    // Linked paste: attach the pasted images whose [image N] token is still
    // in the text; pending images whose token was deleted are dropped with
    // it, and the pending list is cleared once the prompt leaves the box.
    UserMessage um;
    um.text      = modelText;
    um.timestamp = nowMs();
    for (const auto& p : pendingImages) {
        const std::string tok = "[image " + std::to_string(p.id) + "]";
        if (prompt.find(tok) != std::string::npos) um.images.push_back(p.image);
    }
    pendingImages.clear();

    if (agentRunning) {
        if (!agent) {
            // A scaffold run is in progress: no steering queue behind it. The
            // prompt box is disabled for the whole run, so this is not
            // reachable from the UI; kept as a guard for programmatic callers
            // (the notice is added once instead of on every repeated call).
            const std::string notice =
                "The scaffold is running and cannot take a prompt (queuing is Simple-mode "
                "only); Esc stops it. Your draft is kept.";
            if (uiMessages.empty() || uiMessages.back().kind != UiMessage::System ||
                uiMessages.back().text != notice) {
                UiMessage m;
                m.kind = UiMessage::System;
                m.text = notice;
                addMessage(std::move(m));
            }
            return;
        }
        if (history.empty() || history.back() != historyText) {
            history.push_back(historyText);
            SaveHistory(history);
        }
        historyIndex = -1;
        draftInput.clear();
        inputBuf[0] = 0;

        UiMessage user;
        user.kind   = UiMessage::User;
        user.text   = prompt;
        user.queued = true;
        addMessage(std::move(user));

        agent->pushQueuedMessage(std::move(um));
        return;
    }
    if (prompt == "/new") {
        inputBuf[0] = 0;
        newSession();
        return;
    }
    if (history.empty() || history.back() != historyText) {
        history.push_back(historyText);
        SaveHistory(history);
    }
    historyIndex = -1;
    draftInput.clear();
    inputBuf[0] = 0;

    // Loop mode replays this exact prompt when the run settles. Captured here
    // rather than at the top of sendPrompt: a "/skills" or "/new" that starts
    // no run must not become the loop prompt, and a queued prompt steers the
    // live run instead of replacing what the loop would replay. A countdown
    // still running for the previous run is dropped - this run's end re-arms.
    loopPrompt = historyText;
    cancelLoop(true);

    // Until the next TurnEnd reports the real input tokens, grow the context
    // estimate with the prompt (pi re-anchors on message_end the same way).
    contextTokens += (int)(modelText.size() + 3) / 4;

    UiMessage user;
    user.kind = UiMessage::User;
    user.text = prompt;
    addMessage(std::move(user));

    if (!sessions->hasOpenSession()) {
        sessions->startSession(prompt);
        // Ensure the strip shows the current server's window (also covers
        // first runs where startup probing raced with a slow server).
        fetchContextWindowAsync();
    }

    rebuildTools();
    if (!scaffoldTask.empty()) {
        startScaffold(std::move(scaffoldTask), Scaffold::Start::Resume);
        return;
    }
    OpenAIProvider provider;
    provider.model         = model;
    provider.baseUrl       = providerUrl;
    provider.apiKey        = apiKey;
    provider.sendThinking  = sendThinking;
    provider.contextWindow = contextWindow;
    auto forwardEvents     = [this](const AgentEvent& ev) {
        std::lock_guard lock(eventMutex);
        eventQueue.push_back(ev);
    };

    runAbort = false;
    HistorySeed seed =
        selectHistorySeed(agent.get(), liveHistoryPath, sessions.get(),
                          imageBackendIndex < backends.size());
    agent = std::make_unique<Agent>(
        buildSystemPrompt(cwd, fileToolsEnabled(), &skills, webToolsEnabled,
                          fileToolsEnabled() && applyPatchEnabled()),
                                    provider,
                                    &toolRegistry,
                                    forwardEvents,
                                    [this](const AgentMessage& m) { sessions->appendMessage(m); },
                                    [this](const CompactionRecord& r) { sessions->appendCompaction(r); });
    agent->shareAbort(runAbort);

    // Continue the conversation from the live agent (same session) or the
    // loaded session instead of starting fresh. An aborted run can end with
    // an assistant message whose tool calls never got their results; the API
    // rejects that shape, so drop a trailing tool-call assistant message (and
    // the file entry with it, so summary (#N) refs stay exact). A run that
    // stopped on an LLM error persists an empty error assistant message too -
    // dropped the same way.
    if (sessions->hasOpenSession()) {
        if (seed.poppedTrailing)
            sessions->truncateToEntries(seed.entryCount - 1);
        agent->setHistory(std::move(seed.messages), seed.fileBase, seed.sessionIndexed);
        liveHistoryPath = sessions->currentPath();
    }

    agentRunning      = true;
    scaffoldRunActive = false;  // plain run: nested (subagent) events stay UI-only
    scaffoldRole.clear();
    // The agent run consumes its own steering queue, so queued prompts are
    // delivered mid-run and AgentEnd only fires once everything is done.
    agentThread = std::thread([this, u = std::move(um)]() { agent->run(std::move(u)); });
}

void App::startScaffold(std::string task, Scaffold::Start start) {
    rebuildTools();
    OpenAIProvider provider;
    provider.model         = model;
    provider.baseUrl       = providerUrl;
    provider.apiKey        = apiKey;
    provider.sendThinking  = sendThinking;
    provider.contextWindow = contextWindow;
    auto forwardEvents     = [this](const AgentEvent& ev) {
        std::lock_guard lock(eventMutex);
        eventQueue.push_back(ev);
    };

    runAbort = false;
    manualCompactRequest = false;  // a stale request must not hit the new run's roles
    liveHistoryPath.clear();
    // Deterministic manager-worker scaffold loop instead of the plain agent.
    agent.reset();
    scaffold = std::make_unique<Scaffold>(
        cwd,
        provider,
        &toolRegistry,
        forwardEvents,
        [this](const AgentMessage& m) { sessions->appendMessage(m); },
        &manualCompactRequest);
    // Hand the whole chat history over to the scaffold: it is written to
    // ledger/context.md so the fresh-context roles see the conversation that
    // led to the task (a resumed or continued run keeps its ledger's existing
    // copy).
    // Same trailing-message cleanup as the plain run above; images are
    // stripped (roles may run on another backend).
    if (sessions->hasOpenSession()) {
        std::vector<AgentMessage> hist = SessionStore::loadMessages(sessions->currentPath());
        if (!hist.empty() && std::holds_alternative<AssistantMessage>(hist.back())) {
            const auto& a = std::get<AssistantMessage>(hist.back());
            if (!a.toolCalls.empty() ||
                (a.stopReason == "error" && a.text.empty() && a.toolCalls.empty()))
                hist.pop_back();
        }
        std::string transcript;
        for (const AgentMessage& m : hist) {
            if (auto* u = std::get_if<UserMessage>(&m)) {
                if (!u->text.empty() && !u->synthetic)
                    transcript += "## user\n\n" + u->text + "\n\n";
            } else if (auto* a2 = std::get_if<AssistantMessage>(&m)) {
                if (!a2->text.empty())
                    transcript += "## assistant\n\n" + a2->text + "\n\n";
            }
        }
        scaffold->setContext(std::move(transcript));
    }
    agentRunning      = true;
    scaffoldRunActive = true;  // role events feed the strip's speed/context
    scaffoldRole.clear();
    scaffoldRoleLog.clear();   // fresh log for this run's role dispatches
    verifyRunning = false;
    verifyCmd.clear();
    scaffoldViewDispatch = -1;
    scaffoldPanelCollapsed = false;  // a new run shows the panel unfolded
    // The thread uses the member (like the plain run above): moving the
    // unique_ptr out would leave app.scaffold null during the run, and the
    // UI (task-list modal, current-task highlight) needs it alive.
    agentThread = std::thread([this, t = std::move(task), start]() {
        scaffold->run(std::move(t), runAbort, start);
    });
}

void App::dispatchScaffold(std::string task, Scaffold::Start start, std::string display) {
    if (display.empty())
        display = task;
    // Until the next TurnEnd reports the real input tokens, grow the context
    // estimate with the task (same as a typed prompt).
    contextTokens += (int)(display.size() + 3) / 4;
    UiMessage user;
    user.kind = UiMessage::User;
    user.text = display;
    addMessage(std::move(user));
    if (!sessions->hasOpenSession()) {
        sessions->startSession(display);
        fetchContextWindowAsync();
    }
    startScaffold(std::move(task), start);
}

void App::continueScaffold() {
    if (agentRunning || !scaffoldResume.resumable) return;
    // The stored task.md text goes back verbatim, so Scaffold::run's
    // exact-match resume check is satisfied by construction - the run picks
    // up at the saved phase instead of wiping the ledger.
    dispatchScaffold(scaffoldResume.task, Scaffold::Start::Resume);
}

void App::continueScaffoldLedger() {
    if (agentRunning || !scaffoldResume.continuable) return;
    // The run ended without a clean sign-off because its round budget ran out,
    // not because the work is done: another budget runs on the untouched
    // ledger. The task text stays the ledger's own (the objective is
    // unchanged, and the session file keeps its slug); only the chat line says
    // this is a continuation.
    dispatchScaffold(scaffoldResume.task,
                     Scaffold::Start::ContinueLedger,
                     "Continue with the current ledger (" +
                         std::to_string(scaffoldResume.openTasks) +
                         " task(s) still open):\n" + scaffoldResume.task);
}

void App::newScaffoldSession() {
    if (agentRunning || !scaffoldResume.exists) return;
    // Abandon the leftover ledger (interrupted via "New Session", finished
    // or unreadable via "Clear Ledger"): wipe it so the next scaffold
    // prompt starts fresh, and re-check immediately (not on the throttled
    // path) so the row disappears this frame. A typed draft in the box is
    // kept; the run itself is not restarted from here.
    Scaffold::discardLedger(cwd);
    scaffoldResume = Scaffold::ledgerStatus(cwd);
    scaffoldResumeRefresh = std::chrono::steady_clock::now();
}

void App::refreshScaffoldResume() {
    if (agentRunning) return; // a live run owns the ledger
    const auto now = std::chrono::steady_clock::now();
    if (now - scaffoldResumeRefresh < std::chrono::milliseconds(500)) return;
    scaffoldResumeRefresh = now;
    scaffoldResume = Scaffold::ledgerStatus(cwd);
}

void App::restoreQueuedMessages() {
    // A scaffold run has no agent (and no steering queue) behind it; the
    // Esc-abort and AgentEnd paths both land here, so bail out early.
    if (!agent) return;
    // Replace the k-th "[image N]" token in s with "[image id]" (0-based) so a
    // restored queued message's images get renumbered consistently.
    auto renumberToken = [](std::string& s, size_t k, int id) {
        size_t occ = 0;
        for (size_t pos = 0; (pos = s.find("[image ", pos)) != std::string::npos;) {
            const size_t end = s.find(']', pos);
            if (end == std::string::npos) return;
            bool digits = true;
            for (size_t i = pos + 7; i < end; ++i)
                if (!isdigit((unsigned char)s[i])) {
                    digits = false;
                    break;
                }
            if (digits && occ == k) {
                s.replace(pos + 7, end - pos - 7, std::to_string(id));
                return;
            }
            if (digits) ++occ;
            pos = end + 1;
        }
    };

    std::string joined;
    size_t tokIdx = 0;
    for (const UserMessage& m : agent->clearQueuedMessages()) {
        if (!joined.empty()) joined += "\n";
        joined += m.text;
        for (const ImageAttachment& img : m.images) {
            const int id = ++nextImageId;
            pendingImages.push_back({id, img});
            renumberToken(joined, tokIdx++, id);
        }
    }
    if (!joined.empty()) {
        snprintf(inputBuf, sizeof(inputBuf), "%s", joined.c_str());
        promptGuardSkip = true;  // buffer rewrite must not look like a user edit
    }
}

void App::stopAgent() {
    cancelLoop();  // a pending loop restart dies with the run it follows
    if (agentRunning) {
        runAbort = true;  // stops the scaffold loop too (Agent shares it)
        if (agent) agent->abort();
        // Force-stop any bash command still running, including descendants
        // that outlived their runShellCommand call (a background job the
        // command spawned keeps running even after the call returned).
        killLiveShellProcesses();
        // Like pi restoring queued messages to the editor, put queued
        // prompts back into the prompt box.
        restoreQueuedMessages();
    }
}

void App::onEscape() {
    escPressed = true;
    // While a popup is up, Escape closes it. Dear ImGui does not close
    // modals on Escape by itself; the next frame's BeginPopupModal() with
    // open==false tears the popup state down. The session list is checked
    // before the tree: both are closed from the same key, and the one the
    // user just opened is the one that should close.
    if (backendModalOpen) {
        backendModalOpen = false;
        lastEscape       = std::chrono::steady_clock::time_point{};
        return;
    }
    if (sessionsModalOpen) {
        sessionsModalOpen = false;
        lastEscape    = std::chrono::steady_clock::time_point{};
        return;
    }
    if (mcpModalOpen) {
        mcpModalOpen = false;
        lastEscape   = std::chrono::steady_clock::time_point{};
        return;
    }
    if (treeOpen) {
        treeOpen      = false;
        treeSelection = -1;
        lastEscape    = std::chrono::steady_clock::time_point{};
        return;
    }
    // A single Esc while the agent is outputting stops it; we never open the
    // session tree mid-run.
    if (agentRunning) {
        stopAgent();
        lastEscape = std::chrono::steady_clock::time_point{};
        return;
    }
    // A single Esc while a loop countdown is up cancels the pending restart -
    // the same key that stops a live run stops the one that has not started.
    if (loopArmed) {
        cancelLoop();
        lastEscape = std::chrono::steady_clock::time_point{};
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto gap =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEscape).count();
    const bool dbl = lastEscape.time_since_epoch().count() != 0 && gap <= 400;
    lastEscape     = now;
    if (dbl) {
        treeOpen      = true;
        treeSelection = -1;
    }
}

void App::refreshSessionList() {
    sessionList        = sessions->listSessions();
    sessionListRefresh = std::chrono::steady_clock::now();
}

void App::shutdown() {
    destroying = true;
    if (agentRunning) {
        runAbort = true;
        if (agent) agent->abort();
    }
    // Force-stop any bash command (and descendants) still alive so the app
    // can actually exit without leaving spawned processes running.
    killLiveShellProcesses();
    if (agentThread.joinable()) agentThread.join();
    // MCP: interrupt in-flight handshakes (a connect poll loop checks
    // mcpAbort every ~50 ms), wait for the background ops to wind down, then
    // stop the children so the app exits clean.
    mcpAbort = true;
    for (int i = 0; i < 200; ++i) {
        bool busy = false;
        for (const auto& s : mcpServers)
            if (s.connecting->load()) busy = true;
        if (!busy)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    for (auto& s : mcpServers) s.disconnect();
}

void App::newSession() {
    if (agentRunning) return;
    cancelLoop(true);  // the prompt to replay belongs to the session being left
    sessions->closeSession();
    uiMessages.clear();
    runRows.clear();
    pendingImages.clear();
    inputBuf[0] = 0;
    clearImageTexCache();
    sessionUsage  = Usage{};
    contextTokens = 0;
    resetSpeedStats();
    busyMsTotal = 0;
    busyActive  = false;
    resetStreamingTool();
    refreshSessionList();
}

void App::reloadUiFromSession() {
    const std::string& path = sessions->currentPath();
    uiMessages.clear();
    runRows.clear();
    if (path.empty()) {
        sessionUsage  = Usage{};
        contextTokens = 0;
        resetSpeedStats();
        resetStreamingTool();
        return;
    }
    for (const AgentMessage& m : SessionStore::loadMessages(path)) {
        App::UiMessage um;
        if (const auto* u = std::get_if<UserMessage>(&m)) {
            um.kind = u->synthetic ? App::UiMessage::System : App::UiMessage::User;
            um.text = u->text;
        } else if (const auto* a = std::get_if<AssistantMessage>(&m)) {
            if (!a->thinking.empty()) {
                App::UiMessage th;
                th.kind = App::UiMessage::Thinking;
                th.text = a->thinking;
                uiMessages.push_back(std::move(th));
            }
            um.kind = App::UiMessage::Agent;
            um.text = a->text;
        } else {
            const auto* t    = std::get_if<ToolResultMessage>(&m);
            um.kind          = App::UiMessage::Tool;
            um.toolName      = t->toolName;
            um.toolArgs      = t->args;
            um.toolResult    = t->content;
            um.toolImagePath = t->imagePath;
            um.toolIsError   = t->isError;
            um.toolDone      = true;
            um.toolExpanded  = toolPreviewLines == 0;
        }
        uiMessages.push_back(std::move(um));
    }
    resetStreamingTool();
    sessionUsage  = Usage{};
    contextTokens = 0;
    resetSpeedStats();
    for (const AgentMessage& m : SessionStore::loadMessages(path)) {
        if (const auto* a = std::get_if<AssistantMessage>(&m)) {
            sessionUsage.inputTokens += a->usage.inputTokens;
            sessionUsage.outputTokens += a->usage.outputTokens;
            sessionUsage.totalTokens += a->usage.totalTokens;
            contextTokens = a->usage.inputTokens;
        }
    }
}

void App::openSession(const std::string& path) {
    if (agentRunning) return;
    if (path == sessions->currentPath()) return;
    cancelLoop(true);
    sessions->openSession(path);
    reloadUiFromSession();
    pendingImages.clear();
    inputBuf[0] = 0;
    busyMsTotal = 0;
    busyActive  = false;
}

void App::copyFirstUserMessage(const std::string& path, const std::string& fallbackTitle) {
    auto msgs = SessionStore::loadMessages(path);
    for (const auto& m : msgs) {
        if (const auto* u = std::get_if<UserMessage>(&m)) {
            setClipboardText(u->text);
            showToast("Message copied");
            return;
        }
    }
    if (!fallbackTitle.empty()) {
        setClipboardText(fallbackTitle);
        showToast("Title copied");
    }
}

void App::showToast(const std::string& text) {
    const auto now = std::chrono::steady_clock::now();
    toast.text  = text;
    toast.until = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                            std::chrono::duration<double>(TOAST_DURATION_S));
}

void App::rewindToMessage(size_t index) {
    if (agentRunning) return;
    const std::string& path = sessions->currentPath();
    if (path.empty()) return;
    auto msgs = SessionStore::loadMessages(path);
    if (msgs.empty()) return;
    if (index >= msgs.size()) index = msgs.size() - 1;
    cancelLoop(true);
    if (index == msgs.size() - 1) return;  // nothing to trim
    if (!sessions->truncateMessages(index + 1)) {
        return;
    }
    liveHistoryPath.clear();
    reloadUiFromSession();
    refreshSessionList();
}

void App::rewindToUserMessage(size_t index) {
    if (agentRunning) return;
    const std::string& path = sessions->currentPath();
    if (path.empty()) return;
    auto msgs = SessionStore::loadMessages(path);
    if (msgs.empty() || index >= msgs.size()) return;
    cancelLoop(true);
    const auto* u = std::get_if<UserMessage>(&msgs[index]);
    if (!u) return;
    if (!sessions->truncateMessages(index)) {
        return;
    }
    liveHistoryPath.clear();
    reloadUiFromSession();
    refreshSessionList();
    pendingImages.clear();
    inputBuf[0]     = 0;
    promptGuardSkip = true;  // buffer rewrite must not look like a user edit
    snprintf(inputBuf, sizeof(inputBuf), "%s", u->text.c_str());
    historyIndex = -1;
    draftInput.clear();
    promptFocusPending = true;
}

void App::deleteSession(const std::string& path) {
    if (agentRunning) return;
    if (path == sessions->currentPath()) {
        cancelLoop(true);
        sessions->closeSession();
        uiMessages.clear();
        runRows.clear();
        pendingImages.clear();
        inputBuf[0]   = 0;
        sessionUsage  = Usage{};
        contextTokens = 0;
        resetSpeedStats();
        busyMsTotal = 0;
        busyActive  = false;
        resetStreamingTool();
    }
    sessions->deleteSession(path);
    refreshSessionList();
}

void App::addMessage(UiMessage msg) {
    uiMessages.push_back(std::move(msg));
    stickToBottom = true;
}

void App::invalidateMessageAdvances() {
    for (auto& m : uiMessages) m.advanceCache = -1.0f;
}

void App::resetStreamingTool() {
    // Land every in-flight streaming row (if any) as a plain pending tool row;
    // on an abort their arguments may be incomplete and they never run.
    for (size_t i : liveToolRows)
        if (i < uiMessages.size())
            uiMessages[i].toolStreaming = false;
    liveToolRows.clear();
}

App::UiMessage* App::findLiveToolRow(int toolIndex, const std::string& nestedAgent) {
    for (auto it = liveToolRows.rbegin(); it != liveToolRows.rend(); ++it) {
        auto& m = uiMessages[*it];
        if (m.toolStreaming && m.toolIndex == toolIndex && m.nestedAgent == nestedAgent)
            return &m;
    }
    return nullptr;
}

void App::recordOutput() {
    const auto now = std::chrono::steady_clock::now();
    // The turn's first output token, latched on the first chunk. The TurnEnd
    // fallback uses it to measure the generation span, which excludes the
    // time-to-first-token (the backend's own tok/s counts decoding only).
    if (firstTokenAt == turnStart) firstTokenAt = now;
    // A gap longer than this means the previous burst is over (a tool is
    // running, the network stalled): restart the window so the rate is
    // measured over one contiguous burst, never across a quiet period.
    if (!speedSamples.empty() && now - speedSamples.back().t > std::chrono::milliseconds(500))
        speedSamples.clear();
    // Prune samples more than two windows old so the deque stays small.
    const auto cutoff = now - std::chrono::duration<double>(SPEED_WINDOW_S * 2);
    while (!speedSamples.empty() && speedSamples.front().t < cutoff) speedSamples.pop_front();
    // Token estimate while the true total is unknown. OpenAI-compatible
    // backends mostly emit ~1 token per SSE delta, but some (notably vLLM)
    // batch several tokens into one delta, which makes a pure chunk count
    // under-read by the batch factor. Taking the larger of chunk count and
    // chars/4 covers both: for 1-token deltas chars/4 sits at or below the
    // chunk count, for batched deltas it exceeds it by roughly the number of
    // tokens the backend packed into each chunk.
    int est = streamedDeltas;
    if (int byChars = estimateOutputTokens(streamedChars); byChars > est) est = byChars;
    int out       = std::max(streamedUsageOutput, est);
    // The server reported a true cumulative output-token count (its usage
    // chunk) that the character estimate under-counts. Rebase every retained
    // sample onto the true count so the window rate is token-accurate: this
    // lifts the live reading the moment the final usage chunk lands, and
    // because the scale is applied across the window (not just the latest
    // sample) it avoids the false spike that attributing the whole accumulated
    // difference to the short window would otherwise produce.
    if (streamedUsageOutput > est && est > 0) {
        const int usage = streamedUsageOutput;
        for (auto& s : speedSamples) {
            const long long scaled =
                (static_cast<long long>(s.out) * usage + est / 2) / est;
            s.out = (int)std::min<long long>(usage, scaled);
        }
        out = usage;
    }
    speedSamples.push_back({now, out});
    if (speedSamples.size() >= 2) {
        const auto& first = speedSamples.front();
        const auto& last  = speedSamples.back();
        const double span =
            std::chrono::duration_cast<std::chrono::duration<double>>(last.t - first.t).count();
        const int dtok = last.out - first.out;
        // Below 0.5s of span the rate is just one chunk's worth of noise;
        // keep the previous value (also what freezes the reading while the
        // model is quiet, since no new samples then arrive).
        if (span >= 0.5 && dtok > 0) liveTokPerS = dtok / span;
    }
}

void App::resetSpeedStats() {
    speedSamples.clear();
    liveTokPerS = -1.0;
    lastTokPerS = -1.0;
}

bool App::hasPendingEvents() {
    std::lock_guard lock(eventMutex);
    return !eventQueue.empty();
}

int App::scaffoldDisplayDispatch() const {
    // scaffoldRole is empty while the parent itself talks (before the first
    // dispatch, during the final report): those phases show the full log.
    if (!scaffoldRunActive || scaffoldRole.empty() || scaffoldRoleLog.empty())
        return -1;
    if (scaffoldViewDispatch >= 0)
        return scaffoldViewDispatch;
    return (int)scaffoldRoleLog.size() - 1;
}

void App::pumpEvents() {
    std::deque<AgentEvent> events;
    {
        std::lock_guard lock(eventMutex);
        events.swap(eventQueue);
    }
    for (const auto& ev : events) processEvent(ev);
}

// Row a run's interleaved deltas append to. Reasoning providers stream
// reasoning_content and content deltas alternately, so the newest message
// is not a reliable match: each turn (runRows[start], set in TurnStart)
// owns one text row and one thinking row, and a delta rejoins its own.
// A session operation may have rebuilt uiMessages, so the cached index is
// re-validated and otherwise the turn's rows are re-scanned.
static App::UiMessage* runRow(std::vector<App::UiMessage>& msgs, App::RunRows& rr, bool thinking) {
    const std::size_t cached = thinking ? rr.think : rr.text;
    if (cached < msgs.size()) {
        const App::UiMessage& m = msgs[cached];
        if (thinking ? m.kind == App::UiMessage::Thinking
                     : m.kind == App::UiMessage::Agent && !m.subagentHeader && !m.text.empty())
            return &msgs[cached];
    }
    const std::size_t begin = rr.start == SIZE_MAX ? 0 : rr.start;
    for (std::size_t i = msgs.size(); i > begin; --i) {
        const App::UiMessage& m = msgs[i - 1];
        if (thinking ? m.kind == App::UiMessage::Thinking
                     : m.kind == App::UiMessage::Agent && !m.subagentHeader && !m.text.empty()) {
            if (thinking)
                rr.think = i - 1;
            else
                rr.text = i - 1;
            return &msgs[i - 1];
        }
    }
    return nullptr;
}

void App::processEvent(const AgentEvent& ev) {
    switch (ev.type) {
        case AgentEventType::AgentStart:
            if (ev.nested) {
                // One header row per subagent run (not per LLM turn inside it).
                // ASCII prefix: the mono font has no arrow-glyph ranges.
                if (scaffoldRunActive && !ev.nestedAgent.empty())
                    scaffoldRoleLog.push_back(
                        {ev.nestedAgent, std::chrono::steady_clock::now(), -1}); // role-log overlay column
                UiMessage h;
                h.kind           = UiMessage::Agent;
                h.nestedAgent    = ev.nestedAgent;
                h.subagentHeader = true;
                h.text           = "> " + ev.nestedAgent;
                h.dispatch       = liveDispatchIndex();
                uiMessages.push_back(std::move(h));
                stickToBottom = true;
                break;  // the parent owns the busy/lifecycle state
            }
            // Start a busy segment (already running = steering, keep the one).
            if (!busyActive) {
                busyActive = true;
                busyStart  = std::chrono::steady_clock::now();
            }
            runLastError.clear();
            break;
        case AgentEventType::TurnStart: {
            resetStreamingTool();
            // A new turn gets fresh rows: deltas of this turn must not grow
            // the previous turn's text/thinking rows.
            runRows[ev.nestedAgent] = RunRows{uiMessages.size(), SIZE_MAX, SIZE_MAX};
            // Outside a scaffold the nested turns are subagent tool calls and
            // must not touch the parent's streaming state; during a scaffold
            // every role is nested, so its events carry the stats.
            if (ev.nested) {
                if (scaffoldRunActive) {
                    // Scaffold roles are nested turns, so their events carry
                    // the stats; the header row already came from the nested
                    // AgentStart, so no labeled row is pushed here.
                    scaffoldRole        = ev.nestedAgent;  // role shown in the status strip
                    streamedChars       = 0;
                    streamedDeltas      = 0;
                    streamedUsageOutput = 0;
                    turnStart           = std::chrono::steady_clock::now();
                    firstTokenAt        = turnStart;
                    lastTokenAt         = turnStart;
                    speedSamples.clear();
                    liveTokPerS = -1.0;
                }
                break;
            }
            scaffoldRole.clear();  // non-scaffold turn: nothing to show
            scaffoldViewDispatch = -1;  // the run's answer is the payoff: full log
            UiMessage m;
            m.kind = UiMessage::Agent;
            uiMessages.push_back(std::move(m));
            streamedChars       = 0;
            streamedDeltas      = 0;
            streamedUsageOutput = 0;
            turnStart           = std::chrono::steady_clock::now();
            firstTokenAt        = turnStart;
            lastTokenAt         = turnStart;  // no tokens yet: generation span is zero
            speedSamples.clear();
            liveTokPerS = -1.0;
            break;
        }
        case AgentEventType::TextDelta: {
            if (ev.nested && !scaffoldRunActive) {
                // Append the subagent's text to its own row without touching
                // the parent's speed stats or streaming state. The subagent's
                // reasoning and content deltas may arrive interleaved, so the
                // row is tracked per run, not matched against the newest one.
                auto& rr = runRows[ev.nestedAgent];
                if (UiMessage* row = runRow(uiMessages, rr, false)) {
                    row->text += ev.text;
                    row->advanceCache = -1.0f;
                } else {
                    UiMessage m;
                    m.kind        = UiMessage::Agent;
                    m.nestedAgent = ev.nestedAgent;
                    m.text        = ev.text;
                    rr.text       = uiMessages.size();
                    uiMessages.push_back(std::move(m));
                }
                break;
            }
            streamedChars += ev.text.size();
            ++streamedDeltas;
            lastTokenAt = std::chrono::steady_clock::now();
            recordOutput();
            // Same-run deltas rejoin this turn's text row even when a
            // thinking delta (or the provider's interleaving) is newer.
            auto& rr = runRows[ev.nested ? ev.nestedAgent : std::string{}];
            if (UiMessage* row = runRow(uiMessages, rr, false)) {
                row->text += ev.text;
                row->advanceCache = -1.0f;
            } else {
                UiMessage m;
                m.kind        = UiMessage::Agent;
                m.nestedAgent = ev.nestedAgent;  // tags scaffold-role rows like their thinking rows
                m.dispatch    = liveDispatchIndex();
                m.text        = ev.text;
                rr.text       = uiMessages.size();
                uiMessages.push_back(std::move(m));
            }
            break;
        }
        case AgentEventType::ThinkingDelta: {
            if (ev.nested) {
                // Subagent / scaffold-role thinking: shown as its own thinking
                // row tagged with the role. Scaffold roles feed the stats strip
                // (like their text deltas); outside a scaffold the parent's
                // streaming state stays untouched. The row is tracked per run:
                // a role's content and thinking deltas may interleave, so the
                // newest message is not a reliable match.
                if (scaffoldRunActive) {
                    streamedChars += ev.text.size();
                    ++streamedDeltas;
                    lastTokenAt = std::chrono::steady_clock::now();
                    recordOutput();
                }
                auto& rr = runRows[ev.nestedAgent];
                if (UiMessage* row = runRow(uiMessages, rr, true)) {
                    row->text += ev.text;
                    row->advanceCache = -1.0f;
                } else {
                    UiMessage m;
                    m.kind        = UiMessage::Thinking;
                    m.text        = ev.text;
                    m.nestedAgent = ev.nestedAgent;
                    m.dispatch    = liveDispatchIndex();
                    rr.think      = uiMessages.size();
                    uiMessages.push_back(std::move(m));
                }
                break;
            }
            streamedChars += ev.text.size();
            ++streamedDeltas;
            lastTokenAt = std::chrono::steady_clock::now();
            recordOutput();
            auto& rr = runRows[std::string{}];
            if (UiMessage* row = runRow(uiMessages, rr, true)) {
                row->text += ev.text;
                row->advanceCache = -1.0f;
            } else {
                UiMessage m;
                m.kind   = UiMessage::Thinking;
                m.text   = ev.text;
                rr.think = uiMessages.size();
                uiMessages.push_back(std::move(m));
            }
            break;
        }
        case AgentEventType::ToolArgsDelta: {
            if (ev.nested && !scaffoldRunActive)
                break;  // simple-mode subagent calls have no live streaming rows
            // Live view of the in-flight call: grow the streaming tool row so
            // a long write/edit is visible while the model is still typing it
            // (nested = a scaffold role's call; the row is tagged purple via
            // nestedAgent, like the role's settled tool rows).
            streamedChars += ev.text.size();
            ++streamedDeltas;
            lastTokenAt = std::chrono::steady_clock::now();
            recordOutput();
            // Several calls of one response stream their arguments in turn
            // (each with its own SSE tool_calls index), so append to the live
            // row this index/run owns; create a new one only when the call has
            // none yet. (Matching on the index keeps a later call's fragments
            // from growing an earlier call's row when they interleave.)
            if (UiMessage* m = findLiveToolRow(ev.toolIndex, ev.nestedAgent)) {
                m->toolArgs += ev.text;
                // The name usually arrives in the call's first chunk; adopt it
                // as it grows (some providers stream it in several pieces).
                if (!ev.toolName.empty() && ev.toolName.size() > m->toolName.size())
                    m->toolName = ev.toolName;
            } else {
                UiMessage row;
                row.kind           = UiMessage::Tool;
                row.toolName       = ev.toolName;
                row.toolArgs       = ev.text;
                row.toolDone       = false;
                row.toolStreaming  = true;
                row.toolIndex      = ev.toolIndex;
                row.toolExpanded   = toolPreviewLines == 0;
                row.nestedAgent    = ev.nestedAgent;
                row.dispatch       = liveDispatchIndex();
                liveToolRows.push_back(uiMessages.size());
                uiMessages.push_back(std::move(row));
                stickToBottom = true;
            }
            break;
        }
        case AgentEventType::ToolCallStart: {
            // If this call was already shown live from argument deltas, settle
            // that row (parsed args, running state) instead of adding a second
            // one. A response with several parallel calls has one live row per
            // call, so match on the (tool_calls index, run) pair -- never on
            // "the" streaming row, which belongs to the newest call.
            UiMessage* live = findLiveToolRow(ev.toolIndex, ev.nestedAgent);
            if (live) {
                live->toolName      = ev.toolName;
                live->toolArgs      = ev.toolArgs;
                live->toolStreaming = false;
                live->toolStartMs   = ev.timestamp;
                live->advanceCache  = -1.0f;
            } else {
                // Not seen in the deltas (the call streamed no fragments): push
                // the row directly. Count the args toward the output estimate
                // now that they exist as one string (a live row counted its
                // fragments per chunk; a simple-mode subagent row stays outside
                // the parent's stats).
                if (!ev.nested || scaffoldRunActive) {
                    streamedChars += ev.toolName.size() + ev.toolArgs.size();
                    lastTokenAt   = std::chrono::steady_clock::now();
                    recordOutput();
                }
                UiMessage m;
                m.kind         = UiMessage::Tool;
                m.toolName     = ev.toolName;
                m.toolArgs     = ev.toolArgs;
                m.toolDone     = false;
                m.toolIndex    = ev.toolIndex;
                m.toolExpanded = toolPreviewLines == 0;
                m.nestedAgent  = ev.nestedAgent;
                if (!m.nestedAgent.empty())
                    m.dispatch = liveDispatchIndex();
                m.toolStartMs  = ev.timestamp;
                uiMessages.push_back(std::move(m));
            }
            break;
        }
        case AgentEventType::ToolCallEnd: {
            // Attach the result to the row this call's index tracks; if no row
            // matches (e.g. rows rebuilt from a session have no index), fall
            // back to the oldest pending row. The run must agree too: a
            // subagent's call reuses index 0..n of the parent's own calls, so
            // an index-only match would steal results across runs.
            auto it = uiMessages.end();
            for (size_t i = uiMessages.size(); i-- > 0;) {
                auto& m = uiMessages[i];
                if (m.kind == UiMessage::Tool && !m.toolDone && m.toolIndex == ev.toolIndex &&
                    m.nestedAgent == ev.nestedAgent) {
                    it = std::next(uiMessages.begin(), i);
                    break;
                }
            }
            if (it == uiMessages.end())
                for (size_t i = 0; i < uiMessages.size(); ++i) {
                    auto& m = uiMessages[i];
                    if (m.kind == UiMessage::Tool && !m.toolDone && m.nestedAgent == ev.nestedAgent) {
                        it = std::next(uiMessages.begin(), i);
                        break;
                    }
                }
            if (it != uiMessages.end()) {
                it->toolResult    = ev.toolResult;
                it->toolImagePath = ev.toolImagePath;
                it->toolIsError   = ev.toolIsError;
                it->toolDone      = true;
                it->toolElapsedMs =
                    it->toolStartMs > 0 && ev.timestamp >= it->toolStartMs
                        ? ev.timestamp - it->toolStartMs
                        : -1;
                it->advanceCache  = -1.0f;  // result may change the row height
            }
            break;
        }
        case AgentEventType::TurnEnd:
            if (ev.nested && !scaffoldRunActive) {
                // Only the usage matters: nested turns must not rewrite the
                // parent's context estimate, speed stats or tool rows.
                sessionUsage.inputTokens += ev.usage.inputTokens;
                sessionUsage.outputTokens += ev.usage.outputTokens;
                sessionUsage.totalTokens += ev.usage.totalTokens;
                break;
            }
            sessionUsage.inputTokens += ev.usage.inputTokens;
            sessionUsage.outputTokens += ev.usage.outputTokens;
            sessionUsage.totalTokens += ev.usage.totalTokens;
            contextTokens = ev.usage.inputTokens;
            {
                // Finished-turn speed: the rate of the newest generation
                // window at turn end. The whole-turn average (span below)
                // would be dragged down by the time-to-first-token and by
                // tool gaps, so it is only a fallback for turns whose
                // generation was too short to fill the window.
                if (liveTokPerS > 0.0) {
                    lastTokPerS = liveTokPerS;
                } else {
                    // Generation span from the first output token: the
                    // time-to-first-token (and the turn's own tail) are not
                    // decoding time, so including them would drag the whole-turn
                    // average below the backend's tok/s. Turn with no output
                    // degrades to turnStart -- but then secs stays ~0 and the
                    // rate is not committed anyway.
                    const auto spanStart = firstTokenAt > turnStart ? firstTokenAt : turnStart;
                    const double secs = std::chrono::duration_cast<std::chrono::duration<double>>(
                                            lastTokenAt - spanStart)
                                            .count();
                    int out           = streamedUsageOutput > 0
                                            ? streamedUsageOutput
                                            : streamedDeltas;
                    if (streamedUsageOutput <= 0) {
                        const int byChars = estimateOutputTokens(streamedChars);
                        if (byChars > out) out = byChars;
                    }
                    if (secs > 0.1 && out > 0) lastTokPerS = out / secs;
                }
            }
            speedSamples.clear();
            liveTokPerS         = -1.0;
            streamedChars       = 0;
            streamedUsageOutput = 0;
            // Every tool call of this turn has already settled by now; any row
            // still pending never ran (streamed args that never became a call
            // in the response) -- land it so it does not read [streaming...]/
            // [running...] forever.
            for (auto& m : uiMessages)
                if (m.kind == UiMessage::Tool && !m.toolDone) {
                    m.toolDone      = true;
                    m.toolStreaming = false;
                    m.advanceCache  = -1.0f;
                }
            break;
        case AgentEventType::UsageDelta:
            if (ev.nested) break;
            // Cumulative output tokens for the in-flight call; the last one
            // (usually a final chunk) wins.
            if (ev.usage.outputTokens > streamedUsageOutput)
                streamedUsageOutput = ev.usage.outputTokens;
            lastTokenAt = std::chrono::steady_clock::now();
            recordOutput();
            break;
        case AgentEventType::Retry:
        case AgentEventType::Compaction:
        case AgentEventType::Warning: {
            // Transient system lines (retry notes, compaction notes). A
            // compaction note answers the queued Compact button - the
            // parent's own in a plain run, or the live role's during a
            // scaffold run (nested notes; a Simple-mode subagent's automatic
            // overflow compaction must not re-arm it).
            if (ev.type == AgentEventType::Compaction &&
                (!ev.nested || scaffoldRunActive))
                compactQueued = false;
            // The strip's context figure is normally re-anchored by the next
            // call's real input-token count; an idle compaction (the button
            // with no run up) has no next call, so the compactor's estimate
            // lands here instead.
            if (ev.type == AgentEventType::Compaction && !ev.nested && !agentRunning &&
                ev.tokensAfter > 0)
                contextTokens = ev.tokensAfter;
            UiMessage m;
            m.kind        = ev.type == AgentEventType::Warning ? UiMessage::Warning
                                                               : UiMessage::System;
            m.nestedAgent = ev.nestedAgent;  // empty for a parent note
            if (ev.nested)
                m.dispatch = liveDispatchIndex();
            m.text        = ev.nested ? ev.nestedAgent + ": " + ev.text : ev.text;
            addMessage(std::move(m));
            break;
        }
        case AgentEventType::Error: {
            UiMessage m;
            m.kind        = UiMessage::Error;
            m.nestedAgent = ev.nestedAgent;  // empty for a parent error
            if (ev.nested)
                m.dispatch = liveDispatchIndex();
            m.text        = ev.nested ? ev.nestedAgent + ": " + ev.text : ev.text;
            addMessage(std::move(m));
            if (!ev.nested)
                runLastError = ev.text;  // feeds the AgentEnd notification
            break;
        }
        case AgentEventType::Verification: {
            // The harness gate (the plan's pinned build/test command) starting
            // or landing. It emits no model output, so without these rows a
            // multi-minute gate is indistinguishable from a freeze; the status
            // strip also counts its seconds while it runs. The row joins the
            // dispatch the user is looking at (the worker whose round just
            // ended), otherwise the scaffold page filter would hide it.
            if (ev.verifyRunning) {
                verifyRunning = true;
                verifyCmd     = ev.text;
                verifyStart   = std::chrono::steady_clock::now();
            } else {
                verifyRunning = false;
                verifyCmd.clear();
            }
            UiMessage m;
            m.kind = ev.verifyRunning ? UiMessage::System
                                      : ((ev.verifyTimedOut || ev.verifyExit != 0)
                                             ? UiMessage::Error
                                             : UiMessage::Warning);
            m.text =
                ev.verifyRunning
                    ? "harness verification: `" + ev.text + "` running (no model call)"
                    : ev.text;
            m.dispatch = liveDispatchIndex();
            addMessage(std::move(m));
            break;
        }
        case AgentEventType::UserMessage: {
            // A queued message was injected mid-run: unmark it in the log and
            // grow the context estimate with it (the next TurnEnd reconciles).
            for (auto& m : uiMessages) {
                if (m.kind == UiMessage::User && m.queued) {
                    m.queued       = false;  // label becomes "User", wrapping can change
                    m.advanceCache = -1.0f;
                    break;
                }
            }
            contextTokens += (int)(ev.text.size() + 3) / 4;
            break;
        }
        case AgentEventType::AgentEnd: {
            if (ev.nested) {
                // Close the newest open log entry of this role so the task
                // panel's subagent-log column can show its elapsed time.
                for (auto it = scaffoldRoleLog.rbegin(); it != scaffoldRoleLog.rend(); ++it)
                    if (it->elapsedMs < 0 && it->role == ev.nestedAgent) {
                        it->elapsedMs =
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - it->start)
                                .count();
                        break;
                    }
                break;  // the parent's own AgentEnd drives the lifecycle
            }
            scaffoldRunActive = false;
            scaffoldRole.clear();
            scaffoldRoleLog.clear();
            scaffoldViewDispatch = -1;
            verifyRunning = false;  // the gate cannot outlive the run
            verifyCmd.clear();
            if (busyActive) {
                busyMsTotal += std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - busyStart)
                                   .count();
                busyActive = false;
            }
            if (agentThread.joinable()) agentThread.join();
            agentRunning = false;
            compactQueued = false;       // an aborted run never consumed the queue
            manualCompactRequest = false;  // nor armed a role compaction
            resetStreamingTool();
            // After an abort or a hard error the queue may still hold
            // messages; restore them to the prompt box.
            restoreQueuedMessages();
            // Drop the in-flight partial; the settled base is what we keep.
            streamedChars       = 0;
            streamedDeltas      = 0;
            streamedUsageOutput = 0;
            // An abort (or a hard error) emits no ToolCallEnd for calls that
            // never ran; land them so they do not stay [running...].
            for (auto& m : uiMessages)
                if (m.kind == UiMessage::Tool && !m.toolDone) {
                    m.toolDone      = true;
                    m.toolStreaming = false;
                    m.advanceCache  = -1.0f;
                }
            // Desktop notification: the run really settled (queued steering
            // drained, thread joined), the moment the user is left waiting.
            // Nested (subagent) ends never notify - only the parent's end.
            const bool aborted = runAbort.load();
            const bool isError = !aborted && !runLastError.empty() &&
                                 runLastError != "Aborted";
            const std::string title =
                isError ? "miniagent \u2014 error" : "miniagent";
            const std::string body =
                aborted ? "Stopped (aborted by user)"
                : isError ? runLastError
                          : "Done \u2014 waiting for input";
            notifyRun(title, body, isError);
            if (!aborted)
                speakRun(isError);
            runLastError.clear();

            // Loop mode (the Loop checkbox): restart the same prompt once the
            // pause runs out, in a FRESH session (fireLoop calls newSession):
            // the loop re-runs the task, it does not grow the conversation.
            // An abort never re-arms - the user said stop. Error runs count
            // towards a streak that stops the loop, so a dead backend is not
            // hammered every loopDelayS seconds forever.
            if (isError)
                ++loopErrorsInARow;
            else
                loopErrorsInARow = 0;
            if (loopEnabled && !aborted && !loopPrompt.empty()) {
                if (loopErrorsInARow >= kLoopErrorLimit) {
                    // Stop for real: the checkbox unchecks itself, so the next
                    // prompt cannot silently restart the cycle (the row and the
                    // spoken cue say why).
                    loopErrorsInARow = 0;
                    loopEnabled      = false;
                    UiMessage m;
                    m.kind = UiMessage::Warning;
                    m.text = "Loop stopped: " + std::to_string(kLoopErrorLimit) +
                             " runs in a row ended in an error.";
                    addMessage(std::move(m));
                    speakLoop("loop stopped after errors");
                } else {
                    armLoop();
                }
            }
            break;
        }
        default:
            break;
    }
}

void App::tick() {
    pumpEvents();

    // A background MCP start/stop finished: rebuild the registry on the UI
    // thread. Deferred while a run is up (the registry belongs to it then);
    // the next prompt rebuilds anyway.
    if (mcpDirty.load() && !agentRunning) {
        rebuildTools();
        mcpDirty = false;
    }

    // Keep the cached session list fresh (listSessions() reads every file, so
    // it must not run per frame; 2s keeps counts/mtimes close enough).
    const auto now = std::chrono::steady_clock::now();
    if (now - sessionListRefresh >= std::chrono::seconds(2)) refreshSessionList();

    // Scaffold resume affordance (idle Scaffold mode, the Continue / New
    // Session buttons in the input bar): throttled ledger-status refresh,
    // formerly driven from DrawInputBar. refreshScaffoldResume() itself
    // bails out while a run owns the ledger.
    if (scaffoldMode) refreshScaffoldResume();

    // Loop mode: the countdown armed at the parent AgentEnd lands here. tick()
    // is driven by the desktop Frame() and the TUI ticker alike, so the pause
    // needs no timer thread and nothing to join at shutdown.
    if (loopArmed && !loopEnabled)
        cancelLoop(true);  // the checkbox went off by any path: drop the pause
    else if (loopArmed && !agentRunning && now >= loopFireAt)
        fireLoop();
}

void App::Frame() {
    tick();

    // Global chord, not a text-editing combination: safe to check before any
    // ImGui widget consumes the frame (Ctrl+letter never inserts a char).
    if (ChordPressed(toolExpandChord)) toggleToolExpansion();
    const KeyChord thinkChord{(int)ImGuiKey_T, true, false, false};
    if (ChordPressed(thinkChord)) toggleThinking();
    const KeyChord newSessionChord{(int)ImGuiKey_N, true, false, false};
    if (ChordPressed(newSessionChord)) newSession();

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetMainViewport()->Size);
    // No padding: the top bar must hug the window top so the panes below
    // (positioned at y = topBarH) do not overlap it.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    // No border around the main window (no window flag exists for this; the
    // border is a style property read at End).
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##main",
                 nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus |
                     ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PopStyleVar();

    DrawTopBar(*this);

    const ImVec2 winSize  = ImGui::GetWindowSize();
    const float topBarH   = kTopBarH;
    const float paneY     = topBarH + kPaneGap;
    const float contentH  = std::max(winSize.y - paneY, 0.0f);

    // Left: message log + input bar + status panel. The session list is not
    // a pane anymore: it opens as a modal from the status strip (Sessions
    // button), so the log keeps the full window width.
    ImGui::SetCursorScreenPos(ImVec2(0, paneY));
    // All four children have fixed heights summing to exactly the pane
    // height, so scrolling here would just hide the status panel.
    ImGui::BeginChild("##left", ImVec2(winSize.x, contentH),
                      ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    DrawMessageLog(*this);
    DrawStatusStrip(*this);
    DrawInputBar(*this);
    DrawStatusPanel(*this);
    ImGui::EndChild();

    ImGui::End();

    DrawSessionsModal(*this);
    DrawMcpModal(*this);
    DrawBackendModal(*this);
    DrawSessionTree(*this);
    DrawScaffoldTasksModal(*this);
    // Toast last: it floats on the foreground draw list, above every window.
    DrawToast(*this);
}
