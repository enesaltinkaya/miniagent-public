#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agent/agent.h"
#include "agent/scaffold.h"
#include "agent/skills.h"
#include "agent/types.h"
#include "llm/backends.h"
#include "mcp/mcp.h"
#include "sessions/sessions.h"
#include "tools/tools.h"
#include "ui/rich_text.h"
#include "util/notify.h"
#include "util/speech.h"

struct App {
    struct UiMessage {
        enum Kind { User, Agent, Thinking, Tool, Error, System, Warning };
        Kind kind = User;
        std::string text; // User/Agent/Thinking/Error
        // Tool fields
        std::string toolName;
        std::string toolArgs;
        std::string toolResult;
        std::string toolImagePath; // read tool: on-disk path of the image result
        bool toolIsError = false;
        bool toolDone = false;
        int64_t toolStartMs = 0;
        int64_t toolElapsedMs = -1;
        // Built live from ToolArgsDelta fragments while the model is still
        // streaming the arguments (the tool cannot run until they are complete).
        bool toolStreaming = false;
        // SSE tool_calls index this row tracks; used to match ToolCallStart /
        // ToolCallEnd to the right row when a response has several calls.
        int toolIndex = -1;
        bool toolExpanded = false;
        // Collapsed read/write/edit/bash rows show a short summary (file path,
        // or the first part of the command) instead of the JSON args. Extracted from the settled JSON
        // args once and cached here (the settled args never change afterwards,
        // so no invalidation). While streaming it is re-derived live from the
        // in-flight fragment (so no raw JSON is ever shown), only when the
        // fragment grows.
        std::string toolFile;
        bool toolFileResolved = false;
        std::string toolCommand;
        // Size of the fragment last scanned while streaming.
        std::size_t toolFileScanSize = 0;
        bool queued = false; // user message typed while the agent is running
        // Set for rows belonging to a subagent run; the name tags its rows
        // so they render on the subagent background and the header row
        // ("↳ name") is printed once per run, not per turn.
        std::string nestedAgent;
        // Dispatch (index into scaffoldRoleLog) this row belongs to during a
        // scaffold run; -1 = the parent's own row / a simple-mode subagent.
        // The same role may run several times (the manager between worker
        // rounds): dispatches are told apart by this id, not the name.
        int dispatch = -1;
        // True for the one-per-run "> name" header row: text deltas must not
        // append to it, they get their own row below.
        bool subagentHeader = false;
        // Total cursor advance (px, incl. trailing gap/spacing) consumed in
        // the last drawn frame; -1 = unknown. Used to virtualize the log.
        float advanceCache = -1.0f;
        // Line/word geometry of the last drawn rich-text body (rebuilt on
        // every frame it is drawn; only usable on that frame, see
        // RichTextLayout::frame). Offsets are relative to the rendered body:
        // toolResult for Tool rows, text for everything else; for the
        // trimmed Agent/Thinking bodies, richTextOffset is where the body
        // starts inside text.
        RichTextLayout richLayout;
        int richTextOffset = 0;
    };

    App();
    ~App();
    void Frame();
    // Renderer-neutral frame advance, called by Frame() before the draw
    // pass and directly by the TUI ticker (~100 ms): pump agent events,
    // rebuild the tool registry after a background MCP op, refresh the
    // cached session list (every 2 s) and the scaffold-resume ledger state
    // (every 500 ms, Scaffold mode idle). Contains no ImGui/SDL calls, so
    // it is safe outside an ImGui frame; key chords stay per-frontend (they
    // read the frontend's own key state) and remain in Frame()/the TUI
    // event handler.
    void tick();
    void shutdown(); // abort agent, join thread; call before destroying

    void sendPrompt();
    // Frontend-neutral prompt entry: fills inputBuf then sendPrompt(). The
    // TUI keeps its own unbounded editor buffer and funnels sends through
    // here; the fixed 4096-byte inputBuf truncates longer prompts (accepted
    // loss, PLAN_TUI.md §8).
    void sendPromptText(const std::string& text);
    void stopAgent();
    // Scaffold resume affordance (input bar, idle Scaffold mode): continue an
    // interrupted run from the ledger's stored task, or discard the ledger so
    // the row disappears and the next scaffold prompt starts fresh. Both are
    // no-ops while a run is active; a typed draft in the box is kept.
    void continueScaffold();
    // A finished run that ran out of rounds with open tasks left: start
    // another round budget on the SAME ledger (plan.md / tasks.json / notes.md
    // are kept, the loop restarts at curate).
    void continueScaffoldLedger();
    void newScaffoldSession();
    // Throttled re-read of the ledger status the buttons render from
    // (Scaffold::ledgerStatus). Skipped while a run owns the ledger.
    void refreshScaffoldResume();
    void onEscape(); // closes an open popup (sessions modal / session tree); otherwise a single Esc stops the agent, double Esc opens the session tree
    void newSession();           // start a fresh session and clear the chat
    void openSession(const std::string& path); // load an existing session
    // Copy the first user message to the clipboard; if the session has
    // none (e.g. it aborted before a turn), fall back to the title.
    void copyFirstUserMessage(const std::string& path, const std::string& fallbackTitle);
    // Arm the transient toast ("Text copied" after a drag-select, "Message
    // copied" after the session-list right-click): it stays up for
    // TOAST_DURATION_S, and DrawToast() renders the fading pill.
    void showToast(const std::string& text);
    void deleteSession(const std::string& path); // remove a session file
    // Session tree: truncate the open session at the message at `index`
    // (message becomes the last one) and rebuild the UI from the file.
    void rewindToMessage(size_t index);
    // Rewind *before* the user message at `index` (it is removed too) and
    // paste its text into the prompt box so the user can edit and resend it.
    void rewindToUserMessage(size_t index);
    // Put the agent's queued (not-yet-sent) messages back into the prompt
    // box, including any pasted images with renumbered [image N] tokens.
    void restoreQueuedMessages();
    void pumpEvents(); // public for tests; safe to call every frame
    void processEventForTest(const AgentEvent& ev) { // public for tests
        processEvent(ev);
    }
    void refreshSessionList(); // re-read the cached session list (cheap enough on demand)
    void toggleToolExpansion(); // Ctrl+O: collapse/expand tool output, persisted
    void toggleThinking();      // Ctrl+T: show/hide thinking blocks, persisted
    void setScaffoldMode(bool on); // Simple/Scaffold send mode, session only
    // Loop mode (the Loop checkbox next to the mode toggle), session only.
    // When a run settles, the SAME prompt is re-sent after loopDelayS
    // seconds; setLoopEnabled(false) also cancels a pending countdown.
    void setLoopEnabled(bool on);
    // Drop a pending loop restart (unchecking, Esc, session switch...). The
    // silent form is used by paths that clear the log right after.
    void cancelLoop(bool silent = false);
    // Manual compaction (the Compact button, right end of the input bar's
    // Simple/Scaffold row). queueCompaction() is the button handler: while a
    // run is up it arms the queue - Simple mode forwards to the live Agent,
    // which compacts between turns and continues the run on the smaller
    // context; Scaffold mode raises the shared request flag consumed by the
    // live role's agent at its next turn boundary (the role's OWN conversation
    // is compacted; the parent session and ledger are untouched). With no run
    // up (Simple mode only) it compacts immediately instead - see
    // compactIdle(). The flag clears when the compaction note lands or the run
    // ends.
    void queueCompaction();
    // Simple mode with no run up: the finished run's conversation is still the
    // one the next prompt continues from, so the button has something to do -
    // the deterministic (no model call) compaction just runs on the click
    // rather than at a turn boundary that will never come.
    bool canCompactIdle() const {
        return !agentRunning && !scaffoldMode && sessions && sessions->hasOpenSession();
    }
    // Perform that immediate compaction (queueCompaction's idle branch).
    void compactIdle();
    // Enable/disable an MCP server (MCP modal): persists the state and starts
    // or stops the child process on a background thread (first uvx/npx runs
    // download a package and can take a while; the UI must not block on it).
    void setMcpServerEnabled(size_t index, bool enabled);
    // Start / stop server `index` on a detached thread; the finished
    // operation sets mcpDirty so Frame() can rebuild the tool registry.
    void startMcpServer(size_t index);
    void stopMcpServer(size_t index);
    void invalidateMessageAdvances(); // forget cached per-message layout advances
    // Re-scan <cwd>/.miniagent/skills and ~/.config/miniagent/skills into
    // `skills` (called at startup and before every run, so editing a SKILL.md
    // takes effect on the next prompt without a restart).
    void reloadSkills();
    // Render the loaded skills (and any load diagnostics) as a System message
    // - the "/skills" command.
    void listSkills();

    // ---- state ----
    std::string cwd;
    // True while a SearXNG base URL is configured: the web_search/web_fetch
    // tools are registered and the system prompt carries their guidance.
    bool webToolsEnabled = false;
    // LLM backends (~/.config/miniagent/backends.json, created with the
    // default local backend on first run). Selectable from the status
    // panel's URL link (Backends modal); the selection persists in
    // miniagent.settings and takes effect for the next prompt.
    std::vector<LlmBackend> backends;
    size_t backendIndex = 0;  // selected backend (index into backends)
    // Designated "image model" backend (index into backends); SIZE_MAX means
    // none selected and the analyze_image tool is not registered.
    size_t imageBackendIndex = SIZE_MAX;
    // Non-empty when backends.json was corrupt / old-format at startup:
    // shown in red in the status panel (right side) instead of the URL.
    std::string backendError;
    // Fields of the selected backend (mirrored so the rest of the code and
    // the status strip keep using plain strings):
    std::string providerUrl = "http://localhost:1234/v1"; // backends[backendIndex].url
    std::string apiKey = "1234";                          // backends[backendIndex].apiKey
    std::string model = "local";                           // backends[backendIndex].model
    std::string backendName = "local";                      // backends[backendIndex].name
    void setBackend(size_t index); // pick a backend (called from the Backends modal)
    // Async GET {url}/models probe that refreshes contextWindow from the
    // selected backend (skipped when settings pinned context_window). Runs on
    // a detached thread so the UI never blocks; safe at shutdown (destroying).
    void fetchContextWindowAsync();
    // Pick or clear (passing the current index clears) the image model;
    // persists the choice in miniagent.settings.
    void setImageBackend(size_t index);

    char inputBuf[4096] = {0};
    bool promptFocusPending = true; // focus the prompt box once at startup

    // Images pasted via Ctrl+V while the clipboard held an image. The prompt
    // box references them as [image N] tokens; sendPrompt only attaches the
    // ones whose token is still in the text (backspacing a token detaches it).
    struct PendingImage {
        int id;
        ImageAttachment image;
    };
    std::vector<PendingImage> pendingImages;
    int nextImageId = 0;
    bool promptActive = false; // prompt box was the active item last frame
    // Set by code that rewrites inputBuf outside the input widget (history
    // recall, queue restore, rewind): the prompt guard skips one frame so it
    // does not mistake the rewrite for a user edit.
    bool promptGuardSkip = false;
    bool escPressed = false; // Escape hit this frame (DrawInputBar undoes its cancel-revert)

    // Prompt history (persisted next to the executable)
    std::vector<std::string> history;
    std::string draftInput;   // in-progress prompt, remembered while browsing history
    int historyIndex = -1;    // -1 = not browsing; else index into history

    std::vector<UiMessage> uiMessages;
    // Drag-selection of a message body: left-drag over a message's rich
    // text selects a byte range of it; the selection is auto-copied to the
    // clipboard when the button is released.
    struct MessageSelection {
        bool active = false;
        std::size_t msg = std::size_t(-1);  // index into uiMessages: the vector
        // is push_back'ed (and reallocates) while the agent streams, so a raw
        // pointer into it would die mid-drag; the index survives and is only
        // re-validated against the current size.
        int start = 0;  // byte offsets within the message body,
        int end = 0;    // either order
    };
    MessageSelection msgSel;
    // Transient toast: armed by showToast(), cleared once it has faded.
    static constexpr double TOAST_DURATION_S = 1.8;
    struct Toast {
        std::string text;
        std::chrono::steady_clock::time_point until;
    };
    Toast toast;
    // Rows currently being built from ToolArgsDelta fragments (indices into
    // uiMessages). A response may stream several calls in parallel, so there
    // is one live row per in-flight call, each tracking its own SSE
    // tool_calls index. A call's ToolCallStart settles its row (the row keeps
    // the running state, args replaced by the parsed ones); resetStreamingTool
    // lands the leftovers at a turn/run boundary.
    std::vector<std::size_t> liveToolRows;
    // Row indices each run's ("" = parent, otherwise the subagent/role name)
    // streaming deltas append to. Reasoning providers interleave
    // reasoning_content and content deltas, so the newest message is not a
    // reliable match for an incoming delta: a run keeps one text row and one
    // thinking row per turn, and TurnStart resets the pair so a new turn
    // starts fresh rows. SIZE_MAX = no row yet this turn.
    struct RunRows {
        std::size_t start = SIZE_MAX; // uiMessages.size() at the turn's start
        std::size_t text = SIZE_MAX;
        std::size_t think = SIZE_MAX;
    };
    std::map<std::string, RunRows> runRows;
    // Width (px) the message log was last laid out at; a change invalidates
    // the per-message advance caches (text wrapping depends on it).
    float msgLayoutWidth = -1.0f;
    bool stickToBottom = true;
    Usage sessionUsage;
    int contextTokens = 0; // input tokens of the latest LLM call
    // Live context size (pi's "tokens-live" style): contextTokens is the
    // settled base, and while an assistant message is streaming we add the
    // partial it is accumulating — live output tokens from streaming usage
    // chunks when the provider sends them, else a chars/4 estimate.
    size_t streamedChars = 0;
    int streamedDeltas = 0;  // streamed chunks (1 token/chunk on most backends, batched on vLLM)
    int streamedUsageOutput = 0;

    // Cumulative time the agent has spent working in this session (idle time
    // is not counted). Segments accumulate across prompts; /new or switching
    // sessions resets it. Displayed in the status strip.
    int64_t busyMsTotal = 0;
    std::chrono::steady_clock::time_point busyStart;
    bool busyActive = false;
    // The scaffold harness gate (the plan's pinned `Verification:` command)
    // while it runs. A build/test gate emits no model output at all, which is
    // exactly when a run looks frozen - so the status strip names it and counts
    // its seconds, and the log gets a row at each end of it (fed by
    // AgentEventType::Verification).
    bool verifyRunning = false;
    std::string verifyCmd;
    std::chrono::steady_clock::time_point verifyStart;
    int64_t verifyElapsedMs() const {
        if (!verifyRunning)
            return 0;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - verifyStart)
            .count();
    }
    // Start of the current turn (for the live tok/s speed in the status strip).
    std::chrono::steady_clock::time_point turnStart;
    // Timestamp of the turn's first streamed output token. The finished-turn
    // fallback average divides by (firstTokenAt -> lastTokenAt) instead of
    // (turnStart -> lastTokenAt), so the time-to-first-token does not drag the
    // rate below the backend's own figure (servers count decoding time only).
    // Equal to turnStart until the first output lands; the TurnEnd fallback
    // falls back to turnStart if the turn streamed nothing.
    std::chrono::steady_clock::time_point firstTokenAt;
    // Last streamed token of the current turn; the speed freezes while tools
    // run after the model stopped emitting (a turn spans the tool calls too).
    std::chrono::steady_clock::time_point lastTokenAt;
    // tok/s of the most recent finished turn; shown while idle. -1 = none yet.
    double lastTokPerS = -1.0;
    // Sliding window behind the live tok/s: every output chunk of the
    // in-flight turn records (time, cumulative output tokens). The rate is
    // computed over the newest SPEED_WINDOW_S of samples, not over the
    // whole turn, so a constant stream reads its true rate immediately
    // instead of ramping up (a turn-start average is dragged down by the
    // time-to-first-token and by tool-call gaps inside the turn).
    static constexpr double SPEED_WINDOW_S = 1.0;
    struct SpeedSample {
        std::chrono::steady_clock::time_point t;
        int out;
    };
    std::deque<SpeedSample> speedSamples;
    // Latest window rate; held (not recomputed) while the model is quiet,
    // so the display freezes during tool runs instead of decaying. -1 = none.
    double liveTokPerS = -1.0;

    // Session-list modal (opened by the "Sessions" button in the status strip).
    bool sessionsModalOpen = false;
    // MCP server list modal (opened by the "MCP" button next to New Session).
    bool mcpModalOpen = false;
    // Backend list modal (opened by clicking the backend URL link in the
    // status panel; pick a row to switch backend).
    bool backendModalOpen = false;
    // Session tree popup
    bool treeOpen = false;
    int treeSelection = -1;  // index into the session file's message list
    std::chrono::steady_clock::time_point lastEscape; // double-Esc detection

    // Agent running state
    bool agentRunning = false;
    // Manual-compaction queue (the Compact button), armed on the UI thread.
    // compactQueued mirrors the button's disabled state (cleared when the
    // compaction note lands or the run ends); manualCompactRequest is the
    // shared flag handed to a scaffold run's role agents - whichever role is
    // live when the button is pressed consumes it and compacts its own
    // conversation (cleared at run end so it never leaks to a later run).
    std::atomic<bool> compactQueued{false};
    std::atomic<bool> manualCompactRequest{false};
    // Text of the last non-nested (parent run's) Error event of the current
    // run. Cleared on AgentStart; consumed by the AgentEnd desktop
    // notification to pick between the done / aborted / error toast.
    std::string runLastError;
    // Clipboard write, frontend-supplied: the desktop installs SDL's
    // SDL_SetClipboardText, the TUI an OSC 52 escape writer. Called from the
    // copy helpers (copyFirstUserMessage) and the drag-select release in
    // draw_message_log. Default: drop (headless tests never paste).
    std::function<void(const std::string& text)> setClipboardText = [](const std::string&) {};
    // Desktop notification fired when a run settles (done / aborted / error).
    // A function member so tests can swap in a recorder.
    std::function<void(const std::string& title, const std::string& body, bool isError)> notifyRun =
        [](const std::string& title, const std::string& body, bool isError) {
            notifyDesktop(title, body, isError);
        };
    // Spoken outcome when a run settles: "miniagent done" / "miniagent
    // error". Aborted runs stay silent. A function member so tests can swap
    // in a recorder.
    std::function<void(bool isError)> speakRun = [](bool isError) {
        speak(isError ? "miniagent error" : "miniagent done");
    };
    // The model's context window in tokens (settings: context_window, 0 =
    // unknown). Used to size context compaction and shown in the status
    // strip next to the live context usage.
    int contextWindow = 0;
    // True when context_window was pinned via settings: suppresses the
    // /models probes (session start, setBackend) from overwriting it.
    bool contextWindowManual = false;
    // Set between the scaffold's own AgentStart and AgentEnd: while it is on,
    // the nested role events feed the strip's speed/context stats and the
    // current role name is tracked for the status strip.
    bool scaffoldRunActive = false;
    std::string scaffoldRole; // current scaffold role ("" outside a scaffold run)
    // Chronological log of the scaffold roles that have run (one entry per
    // role dispatch); feeds the subagent-log column of the task overlay.
    // elapsedMs is -1 while the role is still running (the column shows the
    // live time computed from start; the nested AgentEnd fills it in).
    struct ScaffoldRoleEntry {
        std::string role;
        std::chrono::steady_clock::time_point start;
        int64_t elapsedMs = -1;
    };
    std::vector<ScaffoldRoleEntry> scaffoldRoleLog;
    // Message-log view during a scaffold run: -1 follows the live dispatch
    // (each dispatch that starts gets a clean page in the chat area),
    // otherwise the index (into scaffoldRoleLog) of the dispatch pinned for
    // review. Set by clicking an entry in the subagent-log column (click the
    // live dispatch, or the pinned one again, to return to following).
    // Cleared when the parent itself starts talking (the final report) and
    // when the run ends.
    int scaffoldViewDispatch = -1;
    // One-shot scroll request consumed by DrawMessageLog: scroll to the top
    // of the log instead of sticking to the bottom (a just-pinned role
    // review opens at the start of that role's transcript).
    bool msgScrollToTop = false;
    // Task-list modal (opened by the button next to the role name in the
    // status strip); closed again once the scaffold run ends.
    bool scaffoldTasksOpen = false;
    // Fold state of the inline scaffold tasks panel: while true the panel
    // collapses to a small "Scaffold tasks" pill that re-expands on click.
    bool scaffoldPanelCollapsed = false;
    bool hasPendingEvents(); // agent events waiting for the UI thread
    // True while a run is up that cannot take a queued prompt (a scaffold run:
    // no Agent and therefore no steering queue behind it). The prompt box is
    // fully disabled for the duration (like the mode toggle and the session
    // list); Esc still stops the run.
    bool promptSendBlocked() const { return agentRunning && !agent; }
    // The dispatch (index into scaffoldRoleLog) whose messages the chat log
    // shows during a scaffold run: the pinned scaffoldViewDispatch, else the
    // newest dispatch. -1 outside a run, before the first dispatch starts,
    // and while the parent itself talks (the final report): then the full
    // log is shown.
    int scaffoldDisplayDispatch() const;
    // Dispatch (index into scaffoldRoleLog) rows created right now belong
    // to: the newest entry while a scaffold run is up (dispatches run
    // sequentially, and the scaffold's own retry notes for a failed dispatch
    // arrive between its AgentEnd and the next AgentStart, so "newest entry"
    // stays correct there too). -1 outside a run.
    int liveDispatchIndex() const { return scaffoldRunActive && !scaffoldRoleLog.empty()
                                        ? (int)scaffoldRoleLog.size() - 1
                                        : -1; }
    // User-level MCP servers (~/.config/miniagent/mcp.json): loaded at
    // startup, enabled ones connected in the background, their tools added
    // in rebuildTools() once a connect finishes (mcpDirty).
    std::vector<McpServer> mcpServers;
    std::atomic<bool> mcpDirty{false};  // a background MCP op finished
    std::atomic<bool> mcpAbort{false};  // set at shutdown: abort in-flight handshakes
    // Set in shutdown(): lets the detached context-window probe thread notice
    // the app is going away before it writes into members.
    std::atomic<bool> destroying{false};
    // Bumped by setBackend(): a probe in flight for an older backend must not
    // overwrite the window after a switch.
    std::atomic<int> backendGen{0};
    ToolRegistry toolRegistry;
    // Discovered skills (see agent/skills.h) plus the diagnostics their load
    // produced; refreshed by reloadSkills(), advertised in the system prompt
    // and invocable as "/skill:name".
    std::vector<Skill> skills;
    std::vector<SkillDiagnostic> skillDiagnostics;
    std::unique_ptr<Agent> agent;
    // Session file the live agent's conversation belongs to: non-empty while
    // `agent` holds a finished run's in-memory messages for that session, so
    // the next prompt seeds from them instead of the file (which drops image
    // attachments and would break backend prompt caching). Cleared whenever
    // the file becomes the source of truth again (scaffold runs, rewinds);
    // session switches/deletes are caught by the path comparison.
    std::string liveHistoryPath;
    std::unique_ptr<Scaffold> scaffold; // non-null while a scaffold run is active
    // Cached result of Scaffold::ledgerStatus(cwd) for the Continue /
    // New Session buttons; refreshed at most every 500 ms while idle
    // (refreshScaffoldResume).
    Scaffold::LedgerStatus scaffoldResume;
    std::chrono::steady_clock::time_point scaffoldResumeRefresh{};
    // Shared abort flag: the Stop button sets it; the current Agent shares it
    // (shareAbort) and the scaffold loop checks it between roles.
    std::atomic<bool> runAbort{false};
    std::thread agentThread;
    std::unique_ptr<SessionStore> sessions; // per-cwd session files under ~/.config/miniagent/sessions

    // Cached session list (listSessions() reads every file, so it is not done
    // per frame): refreshed when a session is created/deleted/rewound and
    // unconditionally every 2 seconds.
    std::vector<SessionInfo> sessionList;
    std::chrono::steady_clock::time_point sessionListRefresh;
    // Tool output collapse/expand (pi-style): toolPreviewLines > 0 means
    // collapsed (only that many lines shown); 0 means fully expanded.
    int toolPreviewLines = 3;
    std::string toolExpandKey = "Ctrl+O"; // display name
    bool showThinking = true; // Ctrl+T: off renders thinking blocks as a "Thinking..." placeholder
    bool sendThinking = true; // send_thinking setting: echo thinking back as reasoning_content
    // Send mode: Simple runs the plain agent loop; Scaffold expands every
    // prompt to the manager-worker scaffold (ledger + subagents). Not persisted;
    // the app always starts in Simple mode.
    bool scaffoldMode = false;
    // ---- loop mode (the Loop checkbox next to the mode toggle) ----
    // Not persisted: the app always starts with the loop off. Arming happens
    // when a run settles (the parent AgentEnd); the countdown is driven by
    // tick(), so there is no timer thread to join at shutdown.
    bool loopEnabled = false;
    // Raw text of the prompt the last run was dispatched with (captured at the
    // dispatch point of sendPrompt). The loop replays it verbatim, so a
    // re-run is exactly re-typing the prompt and pressing Enter: same mode
    // dispatch, same history/skill handling, same session.
    std::string loopPrompt;
    bool loopArmed = false;
    // Pause between a run settling and the re-send. A member (not a constant)
    // so tests can shrink it.
    double loopDelayS = 10.0;
    // Whole iterations fired since the loop was armed (speech + log label).
    int loopIteration = 0;
    // Runs that ended in an error in a row; the loop stops at kLoopErrorLimit
    // so a dead backend is not hammered forever. Reset by any clean run.
    int loopErrorsInARow = 0;
    static constexpr int kLoopErrorLimit = 3;
    std::chrono::steady_clock::time_point loopFireAt;
    // Whole seconds left on the countdown (0 when nothing is armed); what the
    // checkbox renders next to its label.
    int loopSecondsLeft() const {
        if (!loopArmed)
            return 0;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              loopFireAt - std::chrono::steady_clock::now())
                              .count();
        return left > 0 ? (int)((left + 999) / 1000) : 0;
    }
    // The re-send itself, a function member so tests can record the replay
    // instead of starting a real run. Default: the normal prompt path.
    std::function<void(const std::string& prompt)> loopDispatch =
        [this](const std::string& prompt) { sendPromptText(prompt); };
    // Spoken loop cues ("loop in 10 seconds", "loop 2", "loop stopped"),
    // queued through the same fire-and-forget speaker as the run-end cue. A
    // function member so tests can swap in a recorder.
    std::function<void(const std::string& phrase)> speakLoop =
        [](const std::string& phrase) { speak(phrase); };
    struct KeyChord { int key = -1; bool ctrl = true; bool shift = false; bool alt = false; };
    KeyChord toolExpandChord = {}; // parsed in the ctor, default Ctrl+O

    // Thread-safe event queue (agent thread -> UI thread)
    std::mutex eventMutex;
    std::deque<AgentEvent> eventQueue;

private:
    void processEvent(const AgentEvent& ev);
    // Loop mode internals: armLoop() starts the countdown (parent AgentEnd),
    // fireLoop() replays the prompt when it runs out (App::tick).
    void armLoop();
    void fireLoop();
    // Start the manager-worker scaffold loop on `task` (shared code path of a
    // typed Scaffold-mode prompt and the Continue / New Session buttons).
    // `start` decides what happens to a leftover ledger (resume / wipe /
    // continue on it - see Scaffold::Start).
    void startScaffold(std::string task, Scaffold::Start start);
    // Shared tail of the Continue button's scaffold dispatch: record the turn
    // the way sendPrompt does for a typed prompt, then start the run.
    // `display` overrides the chat-log line when it differs from the task text
    // the run gets (the continuation button shows "Continue ...").
    void dispatchScaffold(std::string task, Scaffold::Start start,
                          std::string display = {});
    // Newest live (toolStreaming) row tracking this SSE tool_calls index for
    // this run (nestedAgent; "" for the parent's own calls); nullptr when the
    // call has no live row yet.
    UiMessage* findLiveToolRow(int toolIndex, const std::string& nestedAgent);
    void resetStreamingTool(); // drop/land the in-flight ToolArgsDelta row
    void recordOutput();       // note an output chunk for the speed window
    void resetSpeedStats();    // clear the window + both tok/s caches
    void addMessage(UiMessage msg);
    void rebuildTools();
    void reloadUiFromSession(); // rebuild uiMessages/usage from the open session file
};
