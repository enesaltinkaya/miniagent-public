#pragma once

#include <atomic>
#include <functional>
#include <string>

#include "agent/types.h"
#include "llm/openai.h"
#include "tools/tools.h"

// Deterministic manager-worker scaffold (ledger-based orchestration; GVS5H
// v2 scaffold - Gao et al., arXiv:2608.26480). The loop structure is code:
// round budget, no-progress guard, cut-off summarizer, verification,
// compaction and finalizer. Every role is a fresh-context LLM call
// coordinating only through the .miniagent/ledger/ directory:
//
//   manager (setup)  -> writes plan.md + seeds tasks.json
//   brainstorm       -> ideation pass, appends to notes.md
//   loop 1..15:      curate (a manager LLM call, or the mechanical fast
//                    path below) -> worker (one task; targeted self-checks)
//                    -> verification (harness runs the pinned command, no
//                       model call; exit status is ground truth, a fail
//                       overrides DONE and forces a fix-or-switch task). The
//                       gate runs the way the bash tool does - stdin off the
//                       terminal, capped output, a deadline (settings
//                       `verify_timeout` seconds, default 1800, 0 = none;
//                       MINIAGENT_VERIFY_TIMEOUT overrides) and a whole-process
//                       group kill on timeout or Esc - so an endless gate
//                       wedges neither the loop nor the app.
//                    Fast path: when the previous round's verification
//                    passed, the worker reported done and a next pending
//                    task exists, the harness settles the finished task in
//                    tasks.json and dispatches the lowest-numbered pending
//                    one directly - no manager call for the rounds whose
//                    decision is fully determined by ground truth.
//   finalizer        -> only when the loop ends without a clean sign-off
//
// The finished run's result is saved to .miniagent/sessions/<task-slug>.md.
//
// A role whose reply does not follow the contract is handled fail-safe (an
// unparseable decision gets one corrective re-ask, then stops the loop into
// the finalizer). Resuming a run saved in the finalizer phase re-curates
// first: the manager may now sign off cleanly instead of re-running the
// finalizer.
//
// Resumability: the loop's progress (phase, round, last task) is persisted
// to .miniagent/ledger/state.json at every step boundary. A run
// interrupted by Esc or a failed role call keeps the ledger and stops; a
// later run with the same task picks up exactly where it left off (Start::Fresh
// wipes and starts over). The UI surfaces an interrupted ledger through
// ledgerStatus(): Continue re-runs the stored task.md text (which matches the
// resume check by construction), New Session discards the ledger outright.
// A run that used up its round budget without a clean sign-off ends (phase
// "done") with tasks still open in tasks.json; ledgerStatus() reports that as
// continuable, and its Continue button starts another round budget on the same
// ledger (Start::ContinueLedger) instead of wiping the plan.
class Scaffold {
public:
    Scaffold(std::string cwd,
             OpenAIProvider provider,
             ToolRegistry* tools,
             // Forwards the nested (role) events to the UI, marked nested.
             const std::function<void(const AgentEvent&)>& onEvent,
             // Receives the final report as an AssistantMessage (session).
             const std::function<void(const AgentMessage&)>& onMessage = {},
             // Shared manual-compaction request flag (the UI's Compact
             // button): handed to every role agent, so whichever role is
             // live when the button is pressed compacts its OWN conversation
             // at its next turn boundary and carries on.
             std::atomic<bool>* manualCompact = nullptr);

    // How a run starts (see run()).
    enum class Start {
        // Resume the ledger's interrupted run when it holds this exact task,
        // otherwise wipe it and start over.
        Resume,
        // Always wipe the ledger and start a new run.
        Fresh,
        // Keep the ledger of a FINISHED run that ran out of rounds with open
        // tasks, and run another round budget on it: plan.md / tasks.json /
        // notes.md stay as they are, the loop restarts at curate.
        ContinueLedger
    };

    // Blocking; run on the agent thread. Emits AgentStart/AgentEnd itself.
    // Resume semantics: if the ledger already holds this exact task and a
    // resumable state.json (from an interrupted run), the run continues where
    // it left off instead of starting over. Start::Fresh always wipes the
    // ledger and starts a new run; Start::ContinueLedger keeps it (see above).
    void run(const std::string& task, const std::atomic<bool>& abort,
             Start start = Start::Resume);

    // Prior conversation transcript (markdown) written to ledger/context.md
    // on a fresh run, so fresh-context roles see the chat that led to the
    // task. No-op when empty.
    void setContext(std::string history) { context_ = std::move(history); }

    // Ledger directory for this cwd (.miniagent/ledger).
    std::string ledgerDir() const { return cwd_ + "/.miniagent/ledger"; }

    // Idle-time ledger status for the UI's Continue / New Session affordance.
    // Static so the UI can call it without a live Scaffold instance; two tiny
    // file reads per call (the caller throttles).
    struct LedgerStatus {
        bool exists = false;   // the .miniagent/ledger/ directory is present
        bool finished = false;  // a completed run: state.json phase "done"
        bool resumable = false; // an interrupted run: phase set, != done, and
                                // task.md present - run(task, fresh=false)
                                // would resume it
        int openTasks = 0;     // tasks.json entries whose status is not "done"
        bool signedOff = false; // state.json: the manager signed off cleanly
        bool continuable = false; // a run that ended WITHOUT a clean sign-off
                                 // (round budget / no progress) while tasks
                                 // were still open: the loop can run another
                                 // round budget on the same ledger
                                 // (Start::ContinueLedger)
        std::string task;       // the stored task text (ledger task.md)
        std::string phase;      // raw saved phase ("", setup, ..., done)
    };
    static LedgerStatus ledgerStatus(const std::string& cwd);

    // Remove the whole ledger directory (.miniagent/ledger/). The "New
    // Session" and "Clear Ledger" buttons use it to abandon a leftover
    // ledger; the next scaffold prompt then starts fresh.
    static void discardLedger(const std::string& cwd);

    // The pinned verification command from plan.md's "Verification:" line,
    // with model-written markdown decoration stripped (backtick code spans,
    // quote pairs, a trailing em/en-dash explanation, sentence punctuation).
    // Public static for tests: the harness runs the pin verbatim through the
    // shell, so a decorated pin executes as command substitution or a syntax
    // error and reads as a failing tree while the tree is green.
    static std::string parseVerificationCommand(const std::string& planMd);

    // Runs the pinned gate (the command above) with the bash tool's hardening:
    // stdin off the terminal, capped output, a deadline (timeoutSec, 0 = none)
    // and a whole-process-group kill on timeout or abort. Returns the exit
    // status, or 124 when the gate was stopped instead of finished (*timedOut
    // reports the deadline case); `tail` gets the last output lines. Public
    // static for tests (gate_test drives the hang scenarios): a gate that
    // never returns, one that leaves a grandchild holding the output pipe, and
    // one that reads the terminal must all come back.
    static int runVerification(const std::string& cwd,
                               const std::string& cmd,
                               std::string& tail,
                               const std::atomic<bool>& abort,
                               int timeoutSec,
                               bool* timedOut = nullptr);

    // The mechanical fast-path decision for a loop round: when the previous
    // round's worker ran, the harness verification of that same round passed
    // (verified), the worker's reply settles its task with a `done` result
    // line under "## Task result", tasks.json is valid, the dispatched task
    // (lastTaskId) still carries its in_progress stamp and a pending task
    // exists in the plan's order, returns the next task id to dispatch and
    // fills settleResult (when non-null) with the one-line result for
    // tasks.json. Returns -1 whenever the round needs the manager instead:
    // the sign-off decision, a failing verification, partial or blocked
    // work, a cut-off worker, a malformed ledger, or no pending task left.
    // Public static for tests (ledger_status_test drives the scenarios).
    static int mechanicalDispatch(const std::string& tasksJson,
                                  int lastTaskId,
                                  const std::string& workerOut,
                                  bool verified,
                                  std::string* settleResult = nullptr);
    // Path where the finished session's result is saved
    // (.miniagent/sessions/<task-slug>.md).
    std::string sessionFile(const std::string& task) const;
    // Id of the task in play (-1 during setup, brainstorm, the finalizer and
    // before the first dispatch). It is set when a worker is dispatched and
    // stays set through the following curation turn - the task is not settled
    // until the manager writes it up in tasks.json - then the next dispatch
    // moves it to the next id. tasks.json carries the same fact as status
    // "in_progress" (stamped at dispatch, curated away by the manager), so a
    // resumed run still shows which task was live.
    int currentTaskId() const { return currentTaskId_.load(std::memory_order_relaxed); }

private:
    static std::string taskSlug(const std::string& task);

private:
    struct Role {
        std::string name;
        std::string systemPrompt; // empty = use the role file from discovery
    };
    // Persisted loop progress (ledger/state.json). phase is one of:
    // setup, brainstorm, curate, worker, finalizer, done.
    struct State {
        std::string phase;
        int round = 1;
        int lastTaskId = -2; // -2 = no task dispatched yet
        bool signedOff = false;
    };
    // Parses ledger/state.json from a ledger directory (zero-initialized
    // State{} when missing/invalid, so an unreadable state is never resumed).
    static State loadStateFrom(const std::string& ledgerDir);
    State loadState() const { return loadStateFrom(ledgerDir()); }
    bool saveState(const State& s) const; // atomic (tmp + rename)
    ToolResult role(const std::string& agent,
                    const std::string& task,
                    const std::atomic<bool>& abort,
                    const std::string& label = "");

    std::string cwd_;
    OpenAIProvider provider_;
    ToolRegistry* tools_;
    std::function<void(const AgentEvent&)> onEvent_;
    std::function<void(const AgentMessage&)> onMessage_;
    std::atomic<bool>* manualCompact_ = nullptr;
    std::string context_; // optional prior-chat transcript -> context.md
    std::atomic<int> currentTaskId_{-1};
};
