#include "ui/draw.h"

#include "SDL3/SDL.h"
#include "imgui_internal.h"
#include "ui/fonts.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <unordered_set>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// 0-based line index of the input text cursor within the text buffer.
int InputCursorLine(const ImGuiInputTextState* st) {
    if (!st)
        return 0;
    int pos = st->GetCursorPos();
    const char* s = st->TextA.Data;
    if (pos > st->TextLen)
        pos = st->TextLen;
    int line = 0;
    for (int i = 0; i < pos; ++i)
        if (s[i] == '\n')
            ++line;
    return line;
}

// pi-style busy duration: "42s", "1m 05s", "1h 02m 03s".
std::string FormatBusyMs(int64_t ms) {
    const int s = (int)(ms / 1000);
    char buf[32];
    if (s < 60)
        snprintf(buf, sizeof(buf), "%ds", s);
    else if (s < 3600)
        snprintf(buf, sizeof(buf), "%dm %02ds", s / 60, s % 60);
    else
        snprintf(buf, sizeof(buf), "%dh %02dm %02ds", s / 3600, (s % 3600) / 60, s % 60);
    return buf;
}

// One entry of the scaffold's ledger task list (.miniagent/ledger/tasks.json).
struct LedgerTask {
    int id = -1;
    std::string desc;
    std::string status;
    std::string result; // one-line outcome, set once the task is done
};

// Tasks from tasks.json, in file order (empty when the file is missing or
// malformed; the manager rewrites it between rounds, so read it fresh).
std::vector<LedgerTask> readLedgerTasks(const std::string& path) {
    std::vector<LedgerTask> out;
    std::ifstream f(path);
    if (!f)
        return out;
    std::ostringstream ss;
    ss << f.rdbuf();
    try {
        auto j = nlohmann::json::parse(ss.str());
        if (!j.is_array())
            return out;
        for (const auto& t : j) {
            if (!t.is_object())
                continue;
            LedgerTask lt;
            lt.id = t.value("id", -1);
            lt.desc = t.value("desc", "");
            lt.status = t.value("status", "");
            if (t.contains("result") && t["result"].is_string())
                lt.result = t["result"].get<std::string>();
            out.push_back(std::move(lt));
        }
    } catch (...) {
    }
    return out;
}

// Per-role accent: the scaffold role name in the status strip and the
// matching task row in the task-list modal share it.
ImVec4 scaffoldRoleColor(const std::string& role) {
    if (role == "manager")
        return ImVec4(0.80f, 0.58f, 0.95f, 1.0f); // purple
    if (role == "brainstorm")
        return ImVec4(0.95f, 0.85f, 0.40f, 1.0f); // yellow
    if (role == "finalizer")
        return ImVec4(0.90f, 0.52f, 0.52f, 1.0f);
    if (role == "worker")
        return ImVec4(0.42f, 0.86f, 0.86f, 1.0f); // teal
    return ImVec4(0.62f, 0.63f, 0.72f, 1.0f);     // default grey
}

// Live view of the ledger's task list, shared by the task modal and the
// inline panel drawn while a scaffold run is in progress: the ledger is
// rewritten by the agent thread between rounds, so tasks.json is re-read
// on a light throttle, and expanded rows are tracked by task id (the tasks
// vector is replaced on every re-read).
struct ScaffoldTaskView {
    std::chrono::steady_clock::time_point lastRead;
    std::vector<LedgerTask> tasks;
    std::unordered_set<int> expandedIds;
    int autoExpandedId = -1; // last current task opened expanded
};
ScaffoldTaskView& scaffoldTaskView() {
    static ScaffoldTaskView v;
    return v;
}

// Re-read tasks.json at most once every 500 ms (no-op while there is no
// source: no run in progress and no debug file).
void refreshScaffoldTasks(App& app) {
    const char* dbgTasks = SDL_getenv("MINIAGENT_DEBUG_TASKS");
    if (!dbgTasks && !app.scaffold)
        return;
    ScaffoldTaskView& v = scaffoldTaskView();
    const auto now = std::chrono::steady_clock::now();
    if (now - v.lastRead < std::chrono::milliseconds(500))
        return;
    v.lastRead = now;
    v.tasks = readLedgerTasks(dbgTasks ? std::string(dbgTasks)
                                       : app.scaffold->ledgerDir() + "/tasks.json");
}

// The task list body shared by the modal and the inline panel: the
// progress line, then a scrollable task list filling the remaining height
// (minus `footerH`, reserved by the caller for anything below the list).
void DrawScaffoldTaskBody(App& app, float footerH, bool autoExpandCurrent) {
    ScaffoldTaskView& v = scaffoldTaskView();
    const char* dbgTasks = SDL_getenv("MINIAGENT_DEBUG_TASKS");
    if (dbgTasks && SDL_getenv("MINIAGENT_DEBUG_EXPAND")) { // TEMP DEBUG
        for (const char* c = SDL_getenv("MINIAGENT_DEBUG_EXPAND"); *c; ++c)
            if (isdigit((unsigned char)*c))
                v.expandedIds.insert(atoi(c));
    }
    const char* dbgCurrent = SDL_getenv("MINIAGENT_DEBUG_CURRENT");
    const int currentId = app.scaffold ? app.scaffold->currentTaskId()
                                        : (dbgTasks ? atoi(dbgCurrent ? dbgCurrent : "-1") : -1);
    int done = 0;
    for (const LedgerTask& t : v.tasks)
        if (t.status == "done")
            ++done;
    if (!v.tasks.empty()) {
        char prog[32];
        snprintf(prog, sizeof(prog), "%d/%d done", done, (int)v.tasks.size());
        std::string progLine = prog;
        // The gate belongs to the round the user is watching: name it here too
        // (the panel is where a scaffold run is read from).
        if (app.verifyRunning)
            progLine += "  \u00b7  verifying " + FormatBusyMs(app.verifyElapsedMs());
        ImGui::TextDisabled("%s", progLine.c_str());
        ImGui::Spacing();
    }
    ImGui::Separator();
    ImGui::Spacing();

    const float listH = std::max(ImGui::GetContentRegionAvail().y - footerH, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 16.0f * gFontScale);
    ImGui::BeginChild("##tasklist", ImVec2(0.0f, listH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    if (v.tasks.empty()) {
        ImGui::TextDisabled("No tasks yet (the manager seeds tasks.json first).");
    } else {
        if (gFontMono)
            ImGui::PushFont(gFontMono);
        for (const LedgerTask& t : v.tasks) {
            const bool isDone = (t.status == "done");
            // A task dispatched by the scaffold but not yet curated back to
            // pending/done. Stale ones (interrupted run, not yet re-curated)
            // stay flagged until the manager resets them.
            const bool isStarted = (t.status == "in_progress");
            // The scaffold keeps currentTaskId() pointing at a task from its
            // dispatch until the next one, so it still names the task whose
            // worker just returned while the manager curates it. "In play"
            // (the highlight, the > mark, the live role's colour) therefore
            // spans both halves of a round; once tasks.json says done, done
            // wins and the row settles to green.
            const bool current = (t.id == currentId) && !isDone;
            const bool expanded = v.expandedIds.count(t.id) != 0;
            // Color coding: current = the live role's accent, done = green,
            // stale in_progress = amber, everything else dim grey.
            ImVec4 rowCol;
            if (current)
                rowCol = scaffoldRoleColor(app.scaffoldRole);
            else if (isDone)
                rowCol = ImVec4(0.50f, 0.85f, 0.60f, 1.0f);
            else if (isStarted)
                rowCol = ImVec4(0.92f, 0.66f, 0.33f, 1.0f);
            else
                rowCol = ImVec4(0.60f, 0.61f, 0.66f, 1.0f);
            const char* mark = current ? "> " : isDone ? "x " : isStarted ? "~ " : "  ";
            char head[16] = "";
            snprintf(head, sizeof(head), "#%d", t.id);
            // The row stays one line: the leading +/- marks it as expandable
            // and the desc is ellipsized to fit; clicking the row opens the
            // full text (and the result, once set) underneath.
            const std::string prefix =
                std::string(mark) + (expanded ? "- " : "+ ") + head + "  ";
            const float descMaxW =
                std::max(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(prefix.c_str()).x -
                            8.0f * gFontScale,
                        0.0f);
            const std::string label =
                prefix + (t.desc.empty() ? std::string("\u2026") : SingleLineEllipsized(t.desc, descMaxW));
            ImGui::PushStyleColor(ImGuiCol_Text, rowCol);
            ImGui::PushID(t.id);
            if (ImGui::Selectable(label.c_str(), current))
                if (expanded)
                    v.expandedIds.erase(t.id);
                else
                    v.expandedIds.insert(t.id);
            ImGui::PopID();
            ImGui::PopStyleColor();
            if (expanded) {
                // The full text starts at the list's left edge (no indent:
                // aligning it under the description column looked like the
                // following rows were pushed over).
                if (!t.desc.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, rowCol);
                    ImGui::TextWrapped("%s", t.desc.c_str());
                    ImGui::PopStyleColor();
                }
                if (isDone && !t.result.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.70f, 0.50f, 1.0f));
                    ImGui::TextWrapped("result: %s", t.result.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::Spacing();
            }
            // The live task opens expanded (and re-opens when the run moves
            // to the next one, so the panel follows the run as it goes). The
            // inline overlay panel passes autoExpandCurrent = false: it
            // appears without the user's consent, so rows stay one line
            // there (manual click-to-expand still works).
            if (autoExpandCurrent && current && t.id != v.autoExpandedId) {
                v.expandedIds.insert(t.id);
                v.autoExpandedId = t.id;
                ImGui::SetScrollHereY(0.5f);
            }
        }
        if (gFontMono)
            ImGui::PopFont();
    }
    ImGui::PopStyleVar();  // WindowPadding
    ImGui::EndChild();
    ImGui::PopStyleVar();  // ScrollbarSize
}

}  // namespace

void DrawTopBar(App& app) {
    const float barH = kTopBarH;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.18f, 0.10f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::BeginChild("##topbar", ImVec2(0, barH), ImGuiChildFlags_None);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    // Left accent bar.
    dl->AddRectFilled(ImVec2(origin.x, origin.y),
                      ImVec2(origin.x + 10, origin.y + barH - 2),
                      IM_COL32(64, 156, 255, 255));

    const float x0 = origin.x + 18.0f;

    // Title.
    ImFont* titleFont = gFontBold ? gFontBold : gFontDefault;
    const float titleSize = FONT_SIZE_TITLE * gFontScale;
    const ImFontBaked* bakedTitle = titleFont->GetFontBaked(titleSize);
    const float textH = bakedTitle->Ascent - bakedTitle->Descent;
    const float ty = (barH - textH) * 0.5f;
    dl->AddText(titleFont,
                titleSize,
                ImVec2(x0, origin.y + ty),
                IM_COL32(240, 242, 248, 255),
                "miniagent");

    ImGui::EndChild();
}

void DrawInputBar(App& app) {
    // User prompts render in the mono font (like the user-message bubbles);
    // the whole input bar — text, hint, height — follows it.
    ImGui::PushFont(gFontMono ? gFontMono : gFontDefault);
    float boxH = PromptBoxHeight(app.inputBuf);
    float padX = ImGui::GetStyle().FramePadding.x;
    // A scaffold run has no steering queue, so the prompt box is fully
    // disabled for its duration (like the mode toggle and the session list):
    // nothing can be sent while it runs, so the box is inert, not just muted.
    // The disable scope below wraps only the input widget itself: the toggle
    // row's buttons manage their own state, so the Compact button stays
    // clickable during scaffold runs.
    const bool sendBlocked = app.promptSendBlocked();
    // If the box was focused when the run started (the user just pressed
    // Enter inside it), drop the active id: ImGui only releases the active
    // id of a newly disabled item when it is hovered, and a disabled-but-
    // active input would keep eating keystrokes while looking inert.
    if (sendBlocked && app.promptActive)
        ImGui::ClearActiveID();
    // Thicker border than the default 1px child border.
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 4.0f);
    ImGui::BeginChild("##inputbar", ImVec2(0, InputBarHeight(app.inputBuf)), ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();
    // Sample the cursor line and line count BEFORE the widget processes this
    // frame's keys: sampling afterwards, an Up press has already moved the
    // cursor one line up, so "cursor is on the first line" would be true
    // whenever it was on the second line.
    int curLineBefore = 0;
    int totalLinesBefore = 1;
    {
        ImGuiInputTextState* preSt = ImGui::GetInputTextState(ImGui::GetID("##prompt"));
        if (preSt && preSt->Stb) {
            curLineBefore = InputCursorLine(preSt);
            for (int i = 0; i < preSt->TextLen; ++i)
                if (preSt->TextA.Data[i] == '\n')
                    ++totalLinesBefore;
        }
    }
    // Ctrl+V while the prompt is focused: claim the key (so InputText's own
    // paste shortcut never fires) and paste manually. An image clipboard
    // becomes an [image N] attachment plus placeholder; a text clipboard is
    // inserted at the cursor the same way native input is. The clipboard is
    // only queried on the press frame -- querying it every frame forces a
    // round trip with the clipboard owner (X11 selection request / Wayland
    // data device) which stalls the UI. Deleting a token's characters
    // detaches that image (App::sendPrompt only attaches images whose token
    // is still present).
    //
    // The claim goes through the shortcut routing system: InputText
    // registers its Ctrl+V paste with the default RouteFocused policy
    // (score 300 while it is the active item); registering ours with
    // RouteGlobal|RouteOverActive (score 400) wins the route, so on the
    // press frame the key's owner is our id and InputText's internal
    // Shortcut() fails its owner test. Plain IsKeyChordPressed() would not
    // claim anything (no owner check) and both pastes would fire. The owner
    // id must differ from the prompt's item id or InputText's own test
    // would match it.
    if (app.promptActive &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V,
                        ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive,
                        ImGui::GetID("##prompt-paste-claim"))) {
        ImGuiInputTextState* preSt = ImGui::GetInputTextState(ImGui::GetID("##prompt"));
        size_t clipSize = 0;
        void* clip = SDL_GetClipboardData("image/png", &clipSize);
        if (!clip) {
            clipSize = 0;
            clip = SDL_GetClipboardData("image/jpeg", &clipSize);
        }
        if (clip && clipSize > 0) {
            std::string bytes(static_cast<const char*>(clip), clipSize);
            SDL_free(clip);
            if (auto img = processImageBytes(bytes)) {
                const int id = ++app.nextImageId;
                app.pendingImages.push_back({id, std::move(*img)});
                const std::string tok = "[image " + std::to_string(id) + "] ";
                if (preSt)
                    for (const char c : tok)
                        preSt->OnCharPressed(static_cast<ImWchar>(static_cast<unsigned char>(c)));
            }
        } else {
            if (clip)
                SDL_free(clip);
            if (char* text = SDL_GetClipboardText()) {
                if (text[0] && preSt) {
                    const char* p = text;
                    const char* e = text + std::strlen(text);
                    while (p < e) {
                        unsigned int c = 0;
                        const int n = ImTextCharFromUtf8(&c, p, e);
                        if (n <= 0) {
                            ++p;
                            continue;
                        }
                        preSt->OnCharPressed(c);
                        p += n;
                    }
                }
                SDL_free(text);
            }
        }
    }
    // Send-mode toggle (segmented): Simple runs the plain agent loop,
    // Scaffold expands the next prompt to the manager-worker scaffold
    // (ledger + brainstorm/worker subagents). The dark-green look
    // is the app-wide button default (see main.cpp); the active mode is the
    // brighter green.
    {
        const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
        auto segButton = [&](const char* label, bool selected) -> bool {
            const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
            if (selected) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.45f, 0.29f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(225, 230, 255, 255));
            }
            const bool hit = HandButton(label, ImVec2(w, 0));
            if (selected)
                ImGui::PopStyleColor(2);
            return hit;
        };
        // Mode can't change mid-run (it would apply to a queued prompt that
        // was already interpreted under the old mode). InputBarHeight()
        // reserves the row's height from the same (global) frame padding.
        ImGui::BeginDisabled(app.agentRunning);
        if (segButton("Simple", !app.scaffoldMode))
            app.setScaffoldMode(false);
        if (app.scaffoldMode && ImGui::IsItemHovered())
            ImGui::SetTooltip("Normal agent loop.");
        ImGui::SameLine(0, spacing);
        if (segButton("Scaffold", app.scaffoldMode))
            app.setScaffoldMode(true);
        if (!app.scaffoldMode && ImGui::IsItemHovered())
            ImGui::SetTooltip("Runs the manager-worker scaffold on every prompt: a ledger on "
                               "disk plus brainstorm / worker subagents. Roughly "
                               "doubles the token cost.");
        ImGui::EndDisabled();
        // Loop: when the run settles, the same prompt is re-sent after a
        // 10 s pause. Deliberately outside the BeginDisabled() above - it can
        // be armed or dropped mid-run (the checkbox applies to that run's
        // end). The label stays constant so the item id never changes while
        // the countdown ticks; the remaining seconds render next to it.
        ImGui::SameLine(0, spacing);
        if (segButton("Loop", app.loopEnabled)) {
            if (app.loopArmed)
                app.cancelLoop();
            app.setLoopEnabled(!app.loopEnabled);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "%s",
                app.loopArmed
                    ? "Restarting in %d s \u2014 click here or press Esc to stop it."
                    : app.loopEnabled
                          ? "Restarts the same prompt in a FRESH session %d seconds after "
                            "the run ends. An abort never restarts; %d error runs in a row "
                            "stop the loop. Esc or unchecking cancels a pending restart."
                          : "Restart the same prompt in a fresh session %d seconds after a "
                            "run ends, every time. Costs a full run per iteration.",
                app.loopArmed ? app.loopSecondsLeft() : (int)app.loopDelayS,
                App::kLoopErrorLimit);
        if (app.loopArmed) {
            ImGui::SameLine(0, spacing * 0.5f);
            ImGui::TextDisabled("%ds", app.loopSecondsLeft());
        }
    }
    // A leftover scaffold ledger, next to the mode toggle. Two different leftovers: an INTERRUPTED run (resume from the saved
    // phase) and a FINISHED run that used up its round budget with open tasks
    // left (run another budget on the same ledger). Both get Continue / New
    // Session; only a fully signed-off or unreadable ledger gets Clear Ledger.
    if (app.scaffoldMode && !app.agentRunning) {
        // (The cached ledger status is refreshed by App::tick since the M0b
        // extraction, before the draw pass; this used to call
        // app.refreshScaffoldResume() here.)
        const bool resumable   = app.scaffoldResume.resumable;
        const bool continuable = !resumable && app.scaffoldResume.continuable;
        if (resumable || continuable) {
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            ImGui::SameLine(0, spacing);
            ImGui::AlignTextToFramePadding();
            std::string flat;
            for (char c : app.scaffoldResume.task)
                flat.push_back(c == '\n' || c == '\r' || c == '\t' ? ' ' : c);
            // Fit the task preview into the width the row's own buttons leave
            // behind: an over-long preview wraps the row onto a second line,
            // which would draw over the prompt box below (the bar reserves
            // exactly one toggle row).
            const float padX = ImGui::GetStyle().FramePadding.x;
            float reserved   = ImGui::CalcTextSize("unfinished: \"\"").x + spacing;
            for (const char* l : {"Simple", "Scaffold", "Loop", "Continue", "New Session"})
                reserved += ImGui::CalcTextSize(l).x + padX * 2.0f + spacing;
            if (app.loopArmed)
                reserved += ImGui::CalcTextSize("99s").x + spacing;
            flat = SingleLineEllipsized(flat,
                                        std::max(ImGui::GetContentRegionAvail().x - reserved,
                                                 24.0f));
            if (resumable) {
                ImGui::TextDisabled("unfinished: \"%s\"", flat.c_str());
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "The ledger in .miniagent/ledger/ holds an interrupted scaffold run "
                        "(saved phase: %s). Continue resumes it; New Session wipes it.",
                        app.scaffoldResume.phase.c_str());
            } else {
                ImGui::TextDisabled("rounds used up, %d task%s left",
                                    app.scaffoldResume.openTasks,
                                    app.scaffoldResume.openTasks == 1 ? "" : "s");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "The last scaffold run ended WITHOUT a clean sign-off: %d task(s) "
                        "in .miniagent/ledger/tasks.json are still open, so the round "
                        "budget ran out before the work did.\n\n\"%s\"\n\nContinue runs "
                        "another round budget on this ledger; New Session wipes it.",
                        app.scaffoldResume.openTasks, flat.c_str());
            }
            ImGui::SameLine(0, spacing);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.45f, 0.29f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(225, 230, 255, 255));
            const bool cont = HandButton("Continue");
            ImGui::PopStyleColor(2);
            if (cont) {
                if (resumable)
                    app.continueScaffold();
                else
                    app.continueScaffoldLedger();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "%s",
                    resumable
                        ? "Resume the interrupted run from the ledger: it picks up at the "
                          "saved phase (worker/curate/...) instead of starting over."
                        : "Run another round budget on the SAME ledger: plan.md, "
                          "tasks.json and notes.md are kept as they are, the manager "
                          "re-curates and dispatches the next open task.");
            ImGui::SameLine(0, spacing);
            if (HandButton("New Session"))
                app.newScaffoldSession();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Discard the leftover ledger: the row disappears and the next "
                    "scaffold prompt starts fresh. A typed draft is kept.");
        } else if (app.scaffoldResume.exists) {
            // Nothing left to continue (a clean sign-off, or an unreadable
            // state): no Continue row, but the ledger still sits on disk -
            // offer to wipe it with the same action New Session uses.
            const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
            ImGui::SameLine(0, spacing);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s",
                                app.scaffoldResume.finished ? "finished" : "stale ledger");
            ImGui::SameLine(0, spacing);
            if (HandButton("Clear Ledger"))
                app.newScaffoldSession();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Remove the leftover ledger in .miniagent/ledger/ (saved phase: "
                    "%s). The next scaffold prompt starts fresh.",
                    app.scaffoldResume.phase.c_str());
        }
    }
    // Manual compaction, pinned to the right end of the toggle row. While a
    // run is up the click is queued: Simple mode's agent loop consumes it
    // between turns and continues on the compacted conversation; a scaffold
    // run's live role agent compacts its OWN conversation at its next turn
    // boundary (between roles there is no live context, so the button waits).
    // In Simple mode with no run up the button stays up too: the finished
    // run's conversation is what the next prompt continues from, so the click
    // compacts it right away (no model call) instead of waiting for a turn
    // boundary that never comes. Once armed, it stays disabled until the
    // compaction note lands (or the run ends without consuming it).
    const bool idleCompact = !app.agentRunning && !app.scaffoldMode;
    ImVec2 compactBtnMin(0, 0), compactBtnMax(0, 0);
    if (app.agentRunning || idleCompact) {
        const bool queued = app.compactQueued.load();
        const bool roleGap = !app.agent && app.scaffoldRunActive && app.scaffoldRole.empty();
        const bool idleEmpty = idleCompact && !app.canCompactIdle();
        const char* label = queued ? "Compacting\u2026" : "Compact";
        const float w = ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - w);
        if (queued) {
            // The armed look of the selected mode toggle, dimmed by the
            // disabled state below.
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.14f, 0.45f, 0.29f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(225, 230, 255, 255));
        }
        ImGui::BeginDisabled(queued || roleGap || idleEmpty);
        const bool hit = HandButton(label, ImVec2(w, 0));
        ImGui::EndDisabled();
        if (queued)
            ImGui::PopStyleColor(2);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                "%s",
                queued
                    ? (app.agentRunning
                           ? "Compaction queued \u2014 it runs at the run's next turn boundary, "
                             "then the agent continues with the compacted context."
                           : "Compacting now \u2014 the conversation the next prompt continues "
                             "from is being rewritten.")
                : roleGap
                    ? "No role is running right now (verification / bookkeeping); the "
                      "button arms once the next one starts."
                    : app.scaffoldRunActive
                        ? "Queue a compaction of the running role's conversation: it runs "
                          "at the role's next turn boundary and the role continues with "
                          "the compacted context."
                    : idleEmpty
                        ? "Nothing to compact: no session is open."
                    : idleCompact
                        ? "Compact the conversation the next prompt continues from, right "
                          "now (a deterministic summary of the older turns, no model call). "
                          "The next prompt then runs on the smaller context."
                        : "Queue a context compaction: it runs after the current run's "
                          "in-flight work settles and the agent continues with the "
                          "compacted context.");
        if (hit)
            app.queueCompaction();
        compactBtnMin = ImGui::GetItemRectMin();
        compactBtnMax = ImGui::GetItemRectMax();
        if (SDL_getenv("MINIAGENT_DEBUG_COMPACT")) { // TEMP DEBUG
            static int frame = 0;
            ++frame;
            fprintf(stderr,
                    "DEBUG_COMPACT frame=%d queued=%d roleGap=%d btn=[%.0f,%.0f..%.0f,%.0f] "
                    "childRight=%.0f\n",
                    frame,
                    (int)queued,
                    (int)roleGap,
                    compactBtnMin.x,
                    compactBtnMin.y,
                    compactBtnMax.x,
                    compactBtnMax.y,
                    ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x);
            ImGuiIO& io = ImGui::GetIO();
            if (frame == 30 && !queued) { // simulate a click on the button
                const float mx = (compactBtnMin.x + compactBtnMax.x) * 0.5f;
                const float my = (compactBtnMin.y + compactBtnMax.y) * 0.5f;
                const Uint32 wid =
                    (Uint32)(intptr_t)ImGui::GetMainViewport()->PlatformHandle;
                SDL_Event m{};
                m.type = SDL_EVENT_MOUSE_MOTION;
                m.motion.windowID = wid;
                m.motion.x = mx;
                m.motion.y = my;
                SDL_PushEvent(&m);
                SDL_Event d{};
                d.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                d.button.windowID = wid;
                d.button.button = SDL_BUTTON_LEFT;
                d.button.clicks = 1;
                d.button.x = mx;
                d.button.y = my;
                SDL_PushEvent(&d);
            } else if (frame == 31) {
                SDL_Event u{};
                u.type = SDL_EVENT_MOUSE_BUTTON_UP;
                u.button.windowID = (Uint32)(intptr_t)ImGui::GetMainViewport()->PlatformHandle;
                u.button.button = SDL_BUTTON_LEFT;
                u.button.clicks = 1;
                u.button.x = (compactBtnMin.x + compactBtnMax.x) * 0.5f;
                u.button.y = (compactBtnMin.y + compactBtnMax.y) * 0.5f;
                SDL_PushEvent(&u);
            }
            if (frame == 45)
                fprintf(stderr,
                        "DEBUG_COMPACT after click: queued=%d running=%d scaffold=%d role=[%s]%s\n",
                        (int)app.compactQueued.load(),
                        (int)app.agentRunning,
                        (int)app.scaffoldRunActive,
                        app.scaffoldRole.c_str(),
                        roleGap ? " (gap: click must not arm)" : "");
        }
    }
    // No blue focus highlight on the prompt box: the child window's own
    // background is enough, so render its frame (and active frame) transparent.
    if (app.promptFocusPending) {
        // SetKeyboardFocusHere(0) targets the *next* item added, so it must go
        // right before the prompt widget (not at the top of the child, where
        // the next item would be the mode-toggle button).
        ImGui::SetKeyboardFocusHere(0);
        app.promptFocusPending = false;
    }
    const std::string textBefore(app.inputBuf);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
    ImGui::BeginDisabled(sendBlocked);
    ImGui::InputTextMultiline("##prompt",
                               app.inputBuf,
                               sizeof(app.inputBuf),
                               ImVec2(-padX * 2, boxH));
    ImGui::EndDisabled();
    if (app.escPressed) {
        // Esc while the box is active: ImGui's cancel handler reverts the
        // buffer to the text that was there when it got focus (usually
        // empty). Esc here is a global command (stop the agent, open the
        // session tree), not a cancel of the edit -- restore the text the
        // user was typing.
        if (std::string(app.inputBuf) != textBefore) {
            snprintf(app.inputBuf, sizeof(app.inputBuf), "%s", textBefore.c_str());
            app.promptGuardSkip = true; // buffer rewrite must not look like a user edit
            if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetItemID()))
                st->ReloadUserBufAndMoveToEnd();
            app.promptFocusPending = true; // refocus on the next frame
        }
    }
    app.escPressed = false;
    ImGui::PopStyleColor();  // FrameBgActive
    ImGui::PopStyleColor();  // FrameBg
    if (app.inputBuf[0] == '\0' && !ImGui::IsItemActive()) {
        ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        std::string hint = sendBlocked
                               ? ("Scaffold running \u2014 the prompt box is disabled while it runs "
                                  "(queuing is Simple-mode only). Esc stops it.")
                               : ("Type a prompt\u2026 (Enter to send, Enter while running queues it "
                                  "for the next turn, Shift+Enter for new line, " +
                                  app.toolExpandKey + " toggles tool output, Ctrl+T hides thinking, "
                                  "/new or Ctrl+N clears the session)");
        ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(),
                                            ImGui::GetFontSize(),
                                            ImVec2(r.Min.x + padX, r.Min.y + padX),
                                            IM_COL32(110, 114, 126, 255),
                                            hint.c_str(),
                                            nullptr,
                                            r.Max.x - r.Min.x - padX * 2);
    }
    bool active = ImGui::IsItemActive();
    app.promptActive = active;
    if (active) {
        if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetItemID())) {
            // ImGui's multi-line Enter handling (Shortcut-based) requires exact modifier
            // match, so Shift+Enter inserts nothing natively; do it here.
            if (ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_Enter) ||
                ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_KeypadEnter))
                st->OnCharPressed('\n');

            // Prompt history: Up on the first line recalls the last sent
            // message; Down on the last line walks back toward it (and
            // past the end restores the in-progress draft).
            const bool plain =
                !ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && !ImGui::IsKeyDown(ImGuiKey_RightCtrl) &&
                !ImGui::IsKeyDown(ImGuiKey_LeftAlt) && !ImGui::IsKeyDown(ImGuiKey_RightAlt);
            if (st->Stb && plain && !app.history.empty()) {
                const int curLine = curLineBefore;
                const int totalLines = totalLinesBefore;

                auto applyHistory = [&](int index) {
                    snprintf(app.inputBuf, sizeof(app.inputBuf), "%s", app.history[index].c_str());
                    app.historyIndex = index;
                    app.promptGuardSkip = true; // buffer rewrite, not a user edit
                    st->ReloadUserBufAndMoveToEnd();
                };

                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && curLine == 0) {
                    if (app.historyIndex < 0) {
                        app.draftInput = app.inputBuf;
                        applyHistory((int)app.history.size() - 1);
                    } else if (app.historyIndex > 0) {
                        applyHistory(app.historyIndex - 1);
                    }
                } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && curLine == totalLines - 1 &&
                           app.historyIndex >= 0) {
                    if (app.historyIndex + 1 >= (int)app.history.size()) {
                        snprintf(app.inputBuf, sizeof(app.inputBuf), "%s", app.draftInput.c_str());
                        app.historyIndex = -1;
                        st->ReloadUserBufAndMoveToEnd();
                    } else {
                        applyHistory(app.historyIndex + 1);
                    }
                }
            }
        }
    }
    bool enterPressed = active && ImGui::IsKeyPressed(ImGuiKey_Enter) &&
                        !ImGui::IsKeyDown(ImGuiKey_LeftShift) &&
                        !ImGui::IsKeyDown(ImGuiKey_RightShift);
    if (enterPressed) {
        app.sendPrompt();
        // sendPrompt clears inputBuf, but while focused ImGui renders its
        // internal copy of the text; reload it from the (now empty) user buf.
        if (app.inputBuf[0] == '\0')
            if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetItemID()))
                st->ReloadUserBufAndMoveToEnd();
    }
    // Make [image N] tokens atomic: when any single character of a token is
    // deleted (Backspace/Delete on any of its characters), delete the whole
    // token at once, leaving the cursor where the token used to start.
    static std::string prevText;
    {
        std::string cur(app.inputBuf);
        if (active && !app.promptGuardSkip && cur.size() + 1 == prevText.size()) {
            // Locate the deleted byte: cur must match prevText minus exactly
            // one byte (prefix + suffix alignment, no insertion).
            size_t p = 0, suffix = 0;
            while (p < cur.size() && prevText[p] == cur[p])
                ++p;
            while (suffix < cur.size() - p &&
                   prevText[prevText.size() - 1 - suffix] == cur[cur.size() - 1 - suffix])
                ++suffix;
            if (p + suffix == cur.size()) {
                const size_t delPos = p;
                size_t ts = 0, te = 0;
                size_t i = 0;
                while ((i = prevText.find("[image ", i)) != std::string::npos) {
                    size_t j = i + 7;
                    while (j < prevText.size() && isdigit((unsigned char)prevText[j]))
                        ++j;
                    if (j < prevText.size() && prevText[j] == ']' && i <= delPos && delPos < j + 1) {
                        ts = i;
                        te = j + 1;
                        break;
                    }
                    i += 7;
                }
                if (te > ts) {
                    // In cur, the token's remaining bytes span [ts, te-1).
                    std::string result = cur.substr(0, ts) + cur.substr(te - 1);
                    snprintf(app.inputBuf, sizeof(app.inputBuf), "%s", result.c_str());
                    if (ImGuiInputTextState* st = ImGui::GetInputTextState(ImGui::GetItemID())) {
                        st->SetSelection(ts, ts);
                        st->ReloadUserBufAndKeepSelection();
                    }
                    prevText = std::move(result);
                } else
                    prevText = std::move(cur);
            } else
                prevText = std::move(cur);
        } else
            prevText = std::move(cur);
    }
    app.promptGuardSkip = false;
    // Right-aligned pills in the toggle row (top-right of the box): the
    // "N images" attachment chip (how many pasted images would go with the
    // next send, i.e. whose [image N] token is still in the text). The chip
    // is pure draw-list geometry (no hover target), so it stays crisp inside
    // the disabled scope. Semi-transparent so prompt text underneath stays
    // legible.
    {
        int attached = 0;
        const std::string bufText(app.inputBuf);
        for (const auto& p : app.pendingImages) {
            const std::string tok = "[image " + std::to_string(p.id) + "]";
            if (bufText.find(tok) != std::string::npos)
                ++attached;
        }
        char imageLabel[24] = "";
        if (attached > 0)
            snprintf(imageLabel, sizeof(imageLabel), attached == 1 ? "1 image" : "%d images", attached);
        const float dotR = 2.5f * gFontScale;
        const float pad = 5.0f * gFontScale;
        const float gap = 5.0f * gFontScale;
        const float pillGap = 6.0f * gFontScale;
        const ImVec2 winPos = ImGui::GetWindowPos();
        const float pillY = winPos.y + ImGui::GetStyle().WindowPadding.y +
                            ImGui::GetStyle().FramePadding.y;
        // Draws a pill whose right edge is at rx, returns its width.
        auto drawPill = [&](const char* label, ImU32 dotCol, float rx, const char* tooltip) -> float {
            ImFont* f = ImGui::GetFont();
            const float fs = ImGui::GetFontSize();
            const ImVec2 tsize = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, label);
            const float pillH = fs + pad;
            const float pillW = pad * 2 + dotR * 2 + gap + tsize.x;
            ImDrawList* dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(ImVec2(rx - pillW, pillY), ImVec2(rx, pillY + pillH),
                              IM_COL32(10, 14, 22, 190), pillH * 0.5f);
            dl->AddCircleFilled(
                ImVec2(rx - pillW + pad + dotR, pillY + pillH * 0.5f), dotR, dotCol);
            dl->AddText(f,
                        fs,
                        ImVec2(rx - pillW + pad + dotR * 2 + gap, pillY + pad * 0.5f),
                        IM_COL32(225, 228, 235, 255),
                        label);
            if (tooltip) {
                // Hover target over the pill (it is pure draw-list geometry).
                ImGui::SetCursorScreenPos(ImVec2(rx - pillW, pillY));
                ImGui::InvisibleButton(label, ImVec2(pillW, pillH));
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tooltip);
            }
            return pillW;
        };
        float rx = winPos.x + ImGui::GetWindowContentRegionMax().x - 4.0f * gFontScale;
        // The Compact button owns the row's far right while a run is up;
        // pills stack to its left.
        if (compactBtnMax.x > compactBtnMin.x)
            rx = std::min(rx, compactBtnMin.x - 6.0f * gFontScale);
        if (imageLabel[0])
            rx -= drawPill(imageLabel, IM_COL32(255, 196, 110, 255), rx, nullptr) + pillGap;
    }
    ImGui::PopFont();  // prompt mono font
    ImGui::EndChild();
}

void DrawStatusStrip(App& app) {
    ImGui::BeginChild("##statusstrip", ImVec2(0, StatusStripHeight()), ImGuiChildFlags_None);
    const ImVec2 originScreen = ImGui::GetCursorScreenPos();
    ImU32 statusColor =
        app.agentRunning ? IM_COL32(128, 218, 153, 255) : IM_COL32(140, 145, 160, 255);
    // Status bullet before the label.
    // Bullet's circle is centered at FontSize*0.5 with radius FontSize*0.25
    // (ImGui::Bullet/RenderBullet), offset by FramePadding.x; clear its right
    // edge and leave an ItemSpacing gap before the label.
    const float dotW = ImGui::GetStyle().FramePadding.x +
                       ImGui::GetFontSize() * 0.75f +
                       ImGui::GetStyle().ItemSpacing.x;
    const char* label = "Working...";
    char statusBuf[32];
    char verifyBuf[48];
    if (app.agentRunning) {
        // pi-style working indicator: the ellipsis after the label grows.
        static const char* kDots[] = {"", ".", "..", "..."};
        const char* dots = kDots[(int)(ImGui::GetTime() / 0.5f) % 4];
        snprintf(statusBuf, sizeof(statusBuf), "Working%s", dots);
        label = statusBuf;
    } else {
        label = "Idle";
    }
    // The harness gate (a build/test run with no model output at all) is the
    // one wait that reads as a freeze, so it owns the label with its own
    // elapsed time; the command itself is in the log row it opened.
    if (app.agentRunning && app.verifyRunning) {
        snprintf(verifyBuf, sizeof(verifyBuf), "Verifying %s",
                 FormatBusyMs(app.verifyElapsedMs()).c_str());
        label       = verifyBuf;
        statusColor = IM_COL32(232, 180, 96, 255);
    }
    ImFont* statusFont = gFontBold ? gFontBold : gFontDefault;
    const float statusSize = FONT_SIZE_TITLE * gFontScale;
    const ImFontBaked* bakedStatus = statusFont->GetFontBaked(statusSize);
    const float statusH = bakedStatus->Ascent - bakedStatus->Descent;
    const ImVec2 cpos = ImGui::GetWindowPos();
    const ImVec2 csize = ImGui::GetWindowSize();
    // Dot + label, both drawn with the draw list so they share the same
    // optical center (ImGui::Bullet() would center on the default font's
    // line, not the taller baked label font).
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float labelX = originScreen.x + dotW;
        // Center on the strip window (like the right-side stats), not on the
        // default font's line box, so both sides share the same vertical axis.
        const float labelY = cpos.y + (csize.y - statusH) * 0.5f;
        const ImVec2 dotCenter =
            ImVec2(originScreen.x + ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize() * 0.5f,
                   cpos.y + csize.y * 0.5f);
        dl->AddCircleFilled(dotCenter,
                           ImGui::GetFontSize() * 0.20f,
                           statusColor,
                           (ImGui::GetFontSize() < 22) ? 8 : 12);
        dl->AddText(statusFont,
                    statusSize,
                    ImVec2(labelX, labelY),
                    statusColor,
                    label,
                    label + std::strlen(label));
    }
    // Busy timer: ticks while working, freezes at the last AgentEnd; keeps
    // accumulating across prompts in this session. Shown on the right, just
    // left of the token speed text.
    const int64_t elapsed =
        app.busyMsTotal +
        (app.busyActive
             ? std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - app.busyStart)
                   .count()
             : 0);
    char busyStr[32] = "";
    if (elapsed > 0)
        snprintf(busyStr, sizeof(busyStr), "%s", FormatBusyMs(elapsed).c_str());
    const int live =
        app.streamedUsageOutput > 0 ? app.streamedUsageOutput : (int)((app.streamedChars + 3) / 4);
    const int totalCtx = app.contextTokens + live;
    char ctxTok[24] = "";
    if (totalCtx > 0) {
        char tok[12] = "";
        if (totalCtx >= 1000)
            snprintf(tok, sizeof(tok), "%.2fk", totalCtx / 1000.0);
        else
            snprintf(tok, sizeof(tok), "%d", totalCtx);
        if (app.contextWindow > 0) {
            // Known window: show live usage against it (34.48k / 180k).
            char win[12] = "";
            if (app.contextWindow % 1000 == 0)
                snprintf(win, sizeof(win), "%dk", app.contextWindow / 1000);
            else
                snprintf(win, sizeof(win), "%.1fk", app.contextWindow / 1000.0);
            snprintf(ctxTok, sizeof(ctxTok), "%s / %s", tok, win);
        } else {
            snprintf(ctxTok, sizeof(ctxTok), "%s", tok);
        }
    }
    // Output speed: live window rate while working (held while the model is
    // quiet, so it holds during tool runs); falls back to the last finished
    // turn's value so the slot never disappears once a value has been shown.
    char speed[32] = "";
    if (app.agentRunning && app.liveTokPerS > 0.0)
        snprintf(speed, sizeof(speed), "%.1f tok/s", app.liveTokPerS);
    if (speed[0] == 0 && app.lastTokPerS > 0.0)
        snprintf(speed, sizeof(speed), "%.1f tok/s", app.lastTokPerS);
    // Right-side stats, drawn via the draw list (like the status label) so the
    // taller mono font does not advance the cursor and wrap the strip into a
    // second line. Fields (busy timer, speed, ctx tokens) are laid out
    // left-to-right with a uniform two-space gap, right-anchored as a group.
    // The scaffold role and its task-list button are not shown here: the
    // floating scaffold tasks panel already displays them.
    if (busyStr[0] || speed[0] || ctxTok[0]) {
        if (gFontMono)
            ImGui::PushFont(gFontMono);
        const float rightX = cpos.x + csize.x - ImGui::GetStyle().WindowPadding.x;
        const float gap = ImGui::CalcTextSize("  ").x;
        const float y = cpos.y + (csize.y - ImGui::GetFontSize()) * 0.5f;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_TextDisabled);
        const char* fields[3] = {busyStr[0] ? busyStr : nullptr,
                                 speed[0] ? speed : nullptr,
                                 ctxTok[0] ? ctxTok : nullptr};
        float totalW = 0.0f;
        int count = 0;
        for (const char* f : fields)
            if (f) {
                totalW += ImGui::CalcTextSize(f).x;
                if (count > 0)
                    totalW += gap;
                ++count;
            }
        float x = rightX - totalW;
        for (const char* f : fields) {
            if (!f)
                continue;
            dl->AddText(gFontMono,
                        ImGui::GetFontSize(),
                        ImVec2(x, y),
                        col,
                        f,
                        f + std::strlen(f));
            x += ImGui::CalcTextSize(f).x + gap;
        }
        if (gFontMono)
            ImGui::PopFont();
    }
    ImGui::EndChild();
}

// Modal listing the scaffold's ledger tasks, opened from the status strip's
// task button. Re-reads tasks.json on a light throttle so rows update while
// the run is going; closes itself when the scaffold run ends.
void DrawScaffoldTasksModal(App& app) {
    // TEMP DEBUG (screenshot only; remove before shipping): force the modal
    // open with a canned tasks.json + current id from the environment.
    const char* dbgTasks = SDL_getenv("MINIAGENT_DEBUG_TASKS");
    if (dbgTasks) {
        app.scaffoldRunActive = true;
        if (app.scaffoldRole.empty())
            app.scaffoldRole = "worker";
        if (app.scaffoldRoleLog.empty()) {
            const auto now = std::chrono::steady_clock::now();
            int i        = 0;
            for (const char* r : {"manager", "brainstorm", "manager",
                                  "worker", "worker", "worker"})
                app.scaffoldRoleLog.push_back(
                    {r, now, i == 5 ? -1 : 3400 * i + 5000});  // last worker still running
        }
        // Seed a fake transcript so the message-log dispatch view has
        // content: a user prompt, then a page per dispatch (header, thinking,
        // text, tools), tagged with the entry index it belongs to.
        if (app.uiMessages.empty()) {
            App::UiMessage u;
            u.kind = App::UiMessage::User;
            u.text = "Add a hello-world test to the project and make it pass.";
            app.uiMessages.push_back(u);
            auto header = [&](int d, const char* r) {
                App::UiMessage h;
                h.kind = App::UiMessage::Agent;
                h.nestedAgent = r;
                h.subagentHeader = true;
                h.text = std::string("> ") + r;
                h.dispatch = d;
                app.uiMessages.push_back(h);
            };
            auto think = [&](int d, const char* r, const char* t) {
                App::UiMessage m;
                m.kind = App::UiMessage::Thinking;
                m.nestedAgent = r;
                m.dispatch = d;
                m.text = t;
                app.uiMessages.push_back(m);
            };
            auto text = [&](int d, const char* r, const char* t) {
                App::UiMessage m;
                m.kind = App::UiMessage::Agent;
                m.nestedAgent = r;
                m.dispatch = d;
                m.text = t;
                app.uiMessages.push_back(m);
            };
            auto tool = [&](int d, const char* r, const char* name, const char* args,
                            const char* result, bool done) {
                App::UiMessage m;
                m.kind = App::UiMessage::Tool;
                m.nestedAgent = r;
                m.dispatch = d;
                m.toolName = name;
                m.toolArgs = args;
                m.toolResult = result;
                m.toolDone = done;
                m.toolStartMs = nowMs();
                app.uiMessages.push_back(m);
            };
            header(0, "manager");
            think(0, "manager", "The task is small: one test file, one command.");
            text(0, "manager", "Seeding the ledger with the plan and dispatching.");
            tool(0, "manager", "write", "{\"path\":\"tasks.json\"}",
                 "wrote 3 tasks", true);
            header(1, "brainstorm");
            think(1, "brainstorm", "Use the lightest framework already present.");
            text(1, "brainstorm", "Plain assert-style test, run via the existing "
                                  "test target; no new deps.");
            header(2, "manager");
            text(2, "manager", "Curating results: task 1 done, task 2 needs a worker.");
            tool(2, "manager", "edit", "{\"path\":\"tasks.json\"}",
                 "curated", true);
            header(5, "worker");
            text(5, "worker", "Writing the test now, then running it.");
            tool(5, "worker", "read", "{\"path\":\"CMakeLists.txt\"}",
                 "82: add_executable(miniagent ${SOURCES})", true);
            tool(5, "worker", "bash", "{\"command\":\"cmake --build build\"}",
                 "", false);
        }
        if (const char* pin = SDL_getenv("MINIAGENT_DEBUG_PIN"))
            app.scaffoldViewDispatch = atoi(pin);
        // MINIAGENT_DEBUG_PANEL: preview the inline panel (under the message
        // log's session buttons) instead of the modal.
        app.scaffoldTasksOpen = SDL_getenv("MINIAGENT_DEBUG_PANEL") == nullptr;
        if (SDL_getenv("MINIAGENT_DEBUG_FOLD")) // TEMP DEBUG: start folded
            app.scaffoldPanelCollapsed = true;
    }
    if (app.scaffoldTasksOpen && !app.scaffoldRunActive)
        app.scaffoldTasksOpen = false; // the run ended while the modal was open
    if (app.scaffoldTasksOpen && !ImGui::IsPopupOpen("Scaffold Tasks"))
        ImGui::OpenPopup("Scaffold Tasks");

    const bool wasOpen = app.scaffoldTasksOpen;
    if (app.scaffoldTasksOpen)
        refreshScaffoldTasks(app);

    const ImVec2 vpSize = ImGui::GetMainViewport()->Size;
    const ImVec2 vpCenter = ImVec2(vpSize.x * 0.5f, vpSize.y * 0.5f);
    ImGui::SetNextWindowPos(vpCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(640.0f * gFontScale, 460.0f * gFontScale), ImGuiCond_Appearing);
    PushModalTitleStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * gFontScale, 14.0f * gFontScale));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.03f, 0.04f, 1.0f));
    bool open = app.scaffoldTasksOpen;
    if (ImGui::BeginPopupModal("Scaffold Tasks", &open,
                               ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar)) {

        // The same list body as the inline panel (DrawScaffoldTaskPanel); the
        // footer below reserves its height up front so the list fits.
        const float footerH = 3.0f * (ImGui::GetFontSize() + ImGui::GetStyle().ItemSpacing.y) +
                              (ImGui::GetFontSize() + 2.0f * ImGui::GetStyle().FramePadding.y) +
                              2.0f * ImGui::GetStyle().ItemSpacing.y;
        DrawScaffoldTaskBody(app, footerH, true);

        ImGui::Separator();
        ImGui::TextDisabled("Click a task to expand its full text.");
        ImGui::TextDisabled("Source of truth: .miniagent/ledger/tasks.json (the manager updates it).");
        const float closeW =
            ImGui::CalcTextSize("Close").x + 2.0f * ImGui::GetStyle().FramePadding.x;
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - closeW);
        if (HandButton("Close"))
            open = false;

        ImGui::EndPopup();
    }
    app.scaffoldTasksOpen = open;
    if (wasOpen && !open) {
        ScaffoldTaskView& v = scaffoldTaskView();
        v.expandedIds.clear();      // the next run re-seeds tasks.json from id 1
        v.autoExpandedId = -1;
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    PopModalTitleStyle();
}

// Screen rect the inline scaffold tasks panel (or its collapsed pill)
// occupied while it was drawn, stamped with the frame it was drawn in.
// The message log's drag-selection tests it: the panel floats over the
// message text, and without the check a press anywhere on the panel (its
// scrollbar included) would start a selection on the text underneath.
static ImVec2 g_scaffoldPanelMin{0.0f, 0.0f};
static ImVec2 g_scaffoldPanelMax{0.0f, 0.0f};
static int g_scaffoldPanelFrame = -1;

bool MouseOverScaffoldTasksPanel() {
    return g_scaffoldPanelFrame == ImGui::GetFrameCount() &&
           ImGui::IsMouseHoveringRect(g_scaffoldPanelMin, g_scaffoldPanelMax);
}

void DrawScaffoldTaskPanel(App& app, const ImVec2& pos, const ImVec2& size) {
    // The inline twin of DrawScaffoldTasksModal: while a scaffold run is in
    // progress, a floating panel under the message log's New Session /
    // Sessions buttons shows the same live task list. Hidden while the
    // modal itself is open (it shows the same list), and it vanishes with
    // the run (scaffoldRunActive goes false at AgentEnd).
    if (!app.scaffoldRunActive || app.scaffoldTasksOpen)
        return;
    refreshScaffoldTasks(app);
    // Fold state: when collapsed the panel shrinks to a small pill at the
    // same spot; clicking the pill brings the full panel back.
    if (gFontMono)
        ImGui::PushFont(gFontMono);
    const ImVec2 pillPad = ImVec2(10.0f * gFontScale, 6.0f * gFontScale);
    const char* pillLabel = "Scaffold tasks \u25B8";
    const ImVec2 pillText = ImGui::CalcTextSize(pillLabel);
    const ImVec2 pillSize(pillText.x + 2.0f * ImGui::GetStyle().FramePadding.x + 2.0f * pillPad.x,
                          pillText.y + 2.0f * ImGui::GetStyle().FramePadding.y + 2.0f * pillPad.y);
    const ImVec2 boxSz = app.scaffoldPanelCollapsed ? pillSize : size;
    // Pin the pill to the panel's right edge (same right border as the
    // expanded panel) instead of its left edge.
    const ImVec2 boxPos = app.scaffoldPanelCollapsed
                              ? ImVec2(pos.x + size.x - pillSize.x, pos.y)
                              : pos;
    g_scaffoldPanelMin = boxPos;
    g_scaffoldPanelMax = ImVec2(boxPos.x + boxSz.x, boxPos.y + boxSz.y);
    g_scaffoldPanelFrame = ImGui::GetFrameCount();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.03f, 0.04f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.30f, 0.31f, 0.38f, 1.0f));
    if (app.scaffoldPanelCollapsed) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, pillPad);
        ImGui::SetCursorScreenPos(boxPos);
        if (ImGui::BeginChild("##scaffoldTasksCollapsed", pillSize,
                              ImGuiChildFlags_Borders,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
            if (HandButton(pillLabel, ImVec2(-FLT_MIN, 0)))
                app.scaffoldPanelCollapsed = false;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Show the scaffold task panel");
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    } else {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f * gFontScale, 10.0f * gFontScale));
    ImGui::SetCursorScreenPos(pos);
    if (ImGui::BeginChild("##scaffoldTasks", size, ImGuiChildFlags_Borders)) {
        // Nested children draw no background of their own (see above): the
        // outer child's single background layer is the whole backdrop.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        // Two columns: the task list (as wide as the panel allows) and a slim
        // subagent-log column with the roles that have run, newest at the
        // bottom. Each column is its own scrolling child under the shared
        // panel border.
        const float avail = ImGui::GetContentRegionAvail().x;
        const float gapX  = 16.0f * gFontScale;
        // The log column is as wide as its widest row: the header, or a role
        // name plus its right-aligned elapsed time (worst-case duration
        // string "99h 59m"). The inner child inherits the outer 12px padding.
        const float pad   = 2.0f * 12.0f * gFontScale + 2.0f;
        float rightW      = ImGui::CalcTextSize("Subagent Log").x + pad;
        for (const auto& e : app.scaffoldRoleLog) {
            const int64_t ms = e.elapsedMs >= 0
                                   ? e.elapsedMs
                                   : std::max<int64_t>(
                                         0,
                                         (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                             std::chrono::steady_clock::now() - e.start)
                                             .count());
            rightW = std::max(rightW,
                              ImGui::CalcTextSize(e.role.c_str()).x +
                                  ImGui::CalcTextSize(FormatDurationMs(ms).c_str()).x +
                                  12.0f * gFontScale + pad);
        }
        const float leftW = (avail > rightW + gapX + 120.0f * gFontScale)
                                 ? avail - rightW - gapX
                                 : avail;
        // Left column: the live task list (same body as the modal).
        ImGui::BeginChild("##scaffoldTasksLeft", ImVec2(leftW, 0));
        // The modal's title-bar text, as a header line (the live role's
        // accent color, like the role name in the status strip).
        ImGui::PushStyleColor(ImGuiCol_Text, scaffoldRoleColor(app.scaffoldRole));
        ImGui::TextUnformatted("Scaffold tasks");
        ImGui::PopStyleColor();
        // Fold control: right-aligned on the header line, it collapses the
        // panel to a pill so the message log underneath is fully visible.
        const float hideW =
            ImGui::CalcTextSize("Hide \u25BE").x + 2.0f * ImGui::GetStyle().FramePadding.x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - hideW);
        if (HandButton("Hide \u25BE"))
            app.scaffoldPanelCollapsed = true;
        DrawScaffoldTaskBody(app, 0.0f, false);
        ImGui::EndChild();
        if (leftW < avail) {
            ImGui::SameLine();
            // Right column: the subagent log - one row per dispatch with the
            // role name (left, role color) and its elapsed time (right;
            // ticking for the dispatch that is still running). Clicking a row
            // pins the chat area to that dispatch's transcript (a clean page
            // per dispatch while the run goes); clicking the live dispatch,
            // or the pinned one again, returns to following the live
            // messages. Dispatches are identified by their entry here, not
            // the role name: the manager runs several times in one run.
            ImGui::BeginChild("##scaffoldRoleLog", ImVec2(0, 0), ImGuiChildFlags_Borders);
            ImGui::TextDisabled("Subagent Log");
            ImGui::TextDisabled("click to view");
            const int displayDispatch = app.scaffoldDisplayDispatch();
            int liveDispatch = -1;
            for (int i = (int)app.scaffoldRoleLog.size() - 1; i >= 0; --i)
                if (app.scaffoldRoleLog[i].elapsedMs < 0) {
                    liveDispatch = i;
                    break;
                }
            // Follow the newest entry, but only while the column is already
            // at the bottom (don't fight a user who scrolled up for history).
            const bool follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY();
            for (int idx = 0; idx < (int)app.scaffoldRoleLog.size(); ++idx) {
                const auto& e = app.scaffoldRoleLog[idx];
                const int64_t ms = e.elapsedMs >= 0
                                       ? e.elapsedMs
                                       : std::max<int64_t>(
                                             0,
                                             (int64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                                                 std::chrono::steady_clock::now() - e.start)
                                                 .count());
                const std::string dur = FormatDurationMs(ms);
                const bool viewing = idx == displayDispatch;
                ImGui::PushID(idx);
                ImGui::PushStyleColor(ImGuiCol_Text, scaffoldRoleColor(e.role));
                // Full-width row so the whole entry (the overlaid duration
                // included) is one click target; the viewed dispatch stays
                // highlighted while the chat area shows it.
                if (ImGui::Selectable(e.role.c_str(), viewing, ImGuiSelectableFlags_None,
                                      ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                    if (app.scaffoldViewDispatch == idx || idx == liveDispatch) {
                        app.scaffoldViewDispatch = -1;  // follow the live dispatch again
                        app.stickToBottom = true;
                    } else {
                        app.scaffoldViewDispatch = idx;  // review this dispatch
                        app.msgScrollToTop = true;
                    }
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    if (app.scaffoldViewDispatch == idx)
                        ImGui::SetTooltip("Reviewing %s - click again to follow the live run",
                                          e.role.c_str());
                    else if (idx == liveDispatch)
                        ImGui::SetTooltip("This run is going - click to follow it live");
                    else
                        ImGui::SetTooltip("Click to read this %s run's messages",
                                          e.role.c_str());
                }
                ImGui::PopStyleColor();
                ImGui::PopID();
                ImGui::SameLine(ImGui::GetContentRegionMax().x -
                                ImGui::CalcTextSize(dur.c_str()).x);
                ImGui::TextDisabled("%s", dur.c_str());
            }
            if (follow)
                ImGui::SetScrollHereY(1.0f); // this version has no SetScrollMaxY
            ImGui::EndChild();
        }
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    }
    if (gFontMono)
        ImGui::PopFont();
    ImGui::PopStyleColor(2);
}

void DrawStatusPanel(App& app) {
    ImGui::BeginChild("##statuspanel", ImVec2(0, StatusStripHeight()), ImGuiChildFlags_None);

    std::string cwdText = app.cwd;
    if (cwdText.size() > 60)
        cwdText = "..." + cwdText.substr(cwdText.size() - 57);

    // Session info (formerly the top bar's right side), right-aligned.
    char tokStr[16] = "0";
    if (app.sessionUsage.totalTokens >= 1000000)
        snprintf(tokStr, sizeof(tokStr),
                 "%.2fM", app.sessionUsage.totalTokens / 1000000.0);
    else if (app.sessionUsage.totalTokens >= 1000)
        snprintf(tokStr, sizeof(tokStr),
                 "%.2fK", app.sessionUsage.totalTokens / 1000.0);
    else
        snprintf(tokStr, sizeof(tokStr), "%d", app.sessionUsage.totalTokens);
    // Right side: normally "model • tokens •" followed by the backend name
    // as a blue, underlined link (clicking it opens the Backends modal); while
    // the backend config was malformed at startup, a red error message
    // instead.
    const bool backendErr = !app.backendError.empty();
    char prefix[256];
    if (!backendErr) {
        // "model • tokens •" (the model is dropped when it equals the
        // backend name — the name follows as the link anyway);
        const std::string& link = app.backendName;
        if (app.model == link)
            snprintf(prefix, sizeof(prefix), "%s tokens  \u2022  ", tokStr);
        else
            snprintf(prefix,
                     sizeof(prefix),
                     "%s  \u2022  %s tokens  \u2022  ",
                     app.model.c_str(),
                     tokStr);
    }
    if (gFontMono)
        ImGui::PushFont(gFontMono);
    // Both texts are drawn via the draw list and vertically centered in the
    // strip; a TextDisabled widget would top-align on the font line, so the
    // two sides would not share the same vertical axis.
    const float cwdH = ImGui::CalcTextSize(cwdText.c_str()).y;
    const ImVec2 cpos = ImGui::GetWindowPos();
    const ImVec2 csize = ImGui::GetWindowSize();
    const float padX = ImGui::GetStyle().WindowPadding.x;
    const float right = cpos.x + csize.x - padX;
    std::string rightText = backendErr ? app.backendError : std::string();
    // A malformed-config error carries the file path and can run into the
    // cwd text on the left: truncate to half the strip width, ellipsis'd.
    if (backendErr && ImGui::CalcTextSize(rightText.c_str()).x > csize.x * 0.5f) {
        while (rightText.size() > 3 &&
               ImGui::CalcTextSize(rightText.c_str()).x > csize.x * 0.5f)
            rightText.resize(rightText.size() - 32);
        rightText += "\u2026";
    }
    ImGui::GetWindowDrawList()->AddText(gFontMono,
                                        ImGui::GetFontSize(),
                                        ImVec2(cpos.x + padX,
                                               cpos.y + (csize.y - cwdH) * 0.5f),
                                        ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                        cwdText.c_str(),
                                        cwdText.c_str() + cwdText.size());
    if (!backendErr) {
        // "model • tokens •" in the dim color, then the backend name
        // right-aligned in a link color, underlined; its rect is the click
        // target that opens the Backends modal.
        const std::string& link = app.backendName;
        const float urlW = ImGui::CalcTextSize(link.c_str()).x;        const float urlH = ImGui::CalcTextSize(link.c_str()).y;
        const float preW = ImGui::CalcTextSize(prefix).x;
        const float preH = ImGui::CalcTextSize(prefix).y;
        const float urlX = right - urlW;
        const float urlY = cpos.y + (csize.y - urlH) * 0.5f;
        const ImU32 linkCol = IM_COL32(90, 160, 255, 255);
        ImGui::GetWindowDrawList()->AddText(gFontMono,
                                            ImGui::GetFontSize(),
                                            ImVec2(urlX - preW,
                                                   cpos.y + (csize.y - preH) * 0.5f),
                                            ImGui::GetColorU32(ImGuiCol_TextDisabled),
                                            prefix);
        ImGui::GetWindowDrawList()->AddText(gFontMono,
                                           ImGui::GetFontSize(),
                                           ImVec2(urlX, urlY),
                                           linkCol,
                                           link.c_str(),
                                           link.c_str() + link.size());
        // Underline: a hair above the line box's bottom edge.
        const float underY = urlY + urlH - 2.0f * gFontScale;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(urlX, underY),
                                           ImVec2(urlX + urlW, underY),
                                           IM_COL32(90, 160, 255, 200),
                                           1.0f * gFontScale);
        // Click region with a little slack so it is easy to hit. No widget
        // (SetCursorScreenPos here would extend the child's content
        // boundaries and trip ImGui's assertion); a plain hover + click test
        // on the rect.
        const ImVec2 hitMin = ImVec2(urlX - 4.0f, urlY - 4.0f);
        const ImVec2 hitMax = ImVec2(urlX + urlW + 4.0f, urlY + urlH + 4.0f);
        if (ImGui::IsMouseHoveringRect(hitMin, hitMax, false)) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                app.backendModalOpen = true;
        }
    } else {
        const float infoW = ImGui::CalcTextSize(rightText.c_str()).x;
        const float infoH = ImGui::CalcTextSize(rightText.c_str()).y;
        ImGui::GetWindowDrawList()->AddText(gFontMono,
                                           ImGui::GetFontSize(),
                                           ImVec2(cpos.x + csize.x - infoW - padX,
                                                  cpos.y + (csize.y - infoH) * 0.5f),
                                           ImGui::GetColorU32(ImVec4(1.0f, 0.32f, 0.32f, 1.0f)),
                                           rightText.c_str(),
                                           rightText.c_str() + rightText.size());
    }
    if (gFontMono)
        ImGui::PopFont();

    ImGui::EndChild();
}

// Transient toast pill ("Text copied" / "Message copied"): fades in over the
// first 10% of its lifetime and out over the last 25%. Drawn on the
// foreground draw list (the last one rendered), so it floats above every
// window, including the modals and their dim overlays. Centered, with its
// bottom edge a little above the bottom of the message log -- the same
// bottom-stack formula DrawMessageLog uses to size the log, so the toast
// never covers the status strip / input bar / status panel.
void DrawToast(App& app) {
    if (app.toast.text.empty())
        return;
    const double remain =
        std::chrono::duration_cast<std::chrono::duration<double>>(
            app.toast.until - std::chrono::steady_clock::now())
            .count();
    if (remain <= 0.0) {
        app.toast.text.clear();  // expired; the next showToast() re-arms it
        return;
    }
    const double age =
        std::clamp((App::TOAST_DURATION_S - remain) / App::TOAST_DURATION_S, 0.0, 1.0);
    const float a = (float)std::min({1.0, age / 0.10, (1.0 - age) / 0.25});
    if (a <= 0.0f)
        return;

    ImFont* font = gFontBold ? gFontBold : gFontDefault;
    const float fs = FONT_SIZE_DEFAULT * gFontScale;  // the size the font is baked at
    const ImFontBaked* baked = font->GetFontBaked(fs);
    const float textH = baked->Ascent - baked->Descent;
    const float textW = font
                            ->CalcTextSizeA(fs,
                                            FLT_MAX,
                                            0.0f,
                                            app.toast.text.c_str(),
                                            app.toast.text.c_str() + app.toast.text.size())
                            .x;
    const float padX = 14.0f * gFontScale, padY = 8.0f * gFontScale;
    const float w = textW + padX * 2, h = textH + padY * 2;
    const ImVec2 vsize = ImGui::GetMainViewport()->Size;
    const float bottomH = InputBarHeight(app.inputBuf) + StatusStripHeight() * 2 +
                          ImGui::GetStyle().ItemSpacing.y * 3;
    const float x = (vsize.x - w) * 0.5f;
    const float y = vsize.y - bottomH - h - 12.0f * gFontScale;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float r = 8.0f * gFontScale;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(20, 26, 23, (int)(235 * a)), r);
    dl->AddRect(ImVec2(x, y), ImVec2(x + w, y + h), IM_COL32(82, 108, 94, (int)(150 * a)), r, 0, 1.0f);
    dl->AddText(font, fs, ImVec2(x + padX, y + padY), IM_COL32(226, 236, 230, (int)(255 * a)), app.toast.text.c_str());
}
