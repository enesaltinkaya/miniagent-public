// Offline unit test: Scaffold::ledgerStatus() drives the input bar's
// Continue / New Session / Clear Ledger affordance, and Start::ContinueLedger
// decides what a "Continue" run does to the ledger. It reads only ledger files
// and passes no onEvent_ callback to Scaffold (which makes every role call a
// no-op), so no LLM is needed.

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "agent/scaffold.h"

static int failures = 0;
static void check(bool cond, const std::string& what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what.c_str());
    if (!cond)
        failures++;
}

// The mechanical fast-path decision: which rounds may skip the manager
// call entirely. The happy path must fire only when ground truth alone
// settles everything; every judgement case must fall back to the manager.
static void testMechanicalDispatch() {
    const std::string tasks =
        R"([{"id":1,"desc":"a","status":"in_progress","result":null},)"
        R"({"id":2,"desc":"b","status":"pending","result":null},)"
        R"({"id":3,"desc":"c","status":"pending","result":null}])"
        "\n";
    const std::string workerDone =
        "## Task result\n\ndone - landed the knob and its env row\n\n"
        "## Files changed\n\n- x.c\n";

    std::string settle;
    check(Scaffold::mechanicalDispatch(tasks, 1, workerDone, true, &settle) == 2,
          "fast path: dispatches lowest pending id");
    check(settle == "done - landed the knob and its env row",
          "fast path: settle result is the worker's one-line result");

    check(Scaffold::mechanicalDispatch(tasks, 1, workerDone, false, &settle) == -1,
          "fast path: verification not passed goes to the manager");
    check(Scaffold::mechanicalDispatch(
              tasks, 1, "## Task result\n\npartial - wrote the knob, no env row\n",
              true, &settle) == -1,
          "fast path: partial work goes to the manager");
    check(Scaffold::mechanicalDispatch(tasks, 1, "", true, &settle) == -1,
          "fast path: no result line goes to the manager");
    check(Scaffold::mechanicalDispatch(
              tasks, 1, "[subagent stopped]\n## Task result\n\ndone - x\n", true,
              &settle) == -1,
          "fast path: cut-off worker goes to the manager");
    check(Scaffold::mechanicalDispatch("{not json", 1, workerDone, true, &settle) == -1,
          "fast path: malformed tasks.json goes to the manager");
    check(Scaffold::mechanicalDispatch(
              R"([{"id":1,"status":"done"},{"id":2,"status":"pending"}])"
              "\n",
              1, workerDone, true, &settle) == -1,
          "fast path: already-curated task goes to the manager");
    check(Scaffold::mechanicalDispatch(
              R"([{"id":1,"status":"in_progress"}])"
              "\n",
              1, workerDone, true, &settle) == -1,
          "fast path: no pending task left goes to the manager (sign-off)");
    check(Scaffold::mechanicalDispatch(tasks, 1, workerDone, true, nullptr) == 2,
          "fast path: null settleResult tolerated");

    // The result line is capped: a runaway summary cannot re-bloat
    // tasks.json through the settle path.
    std::string longDone = "done - ";
    longDone += std::string(500, 'x');
    check(Scaffold::mechanicalDispatch(tasks, 1,
                                       "## Task result\n\n" + longDone + "\n", true,
                                       &settle) == 2,
          "fast path: over-long result still dispatches");
    check(settle.size() <= 240, "fast path: settle result capped at 240 chars");
}

// The verification pin sanitizer. Every shape here is one a real model wrote
// into plan.md, and each used to make the harness execute shell garbage
// (command substitution for backticks, syntax errors for prose) that read as
// a failing tree while the tree was green.
static void testParseVerificationCommand() {
    check(Scaffold::parseVerificationCommand("Verification: bash scripts/verify.sh\n") ==
              "bash scripts/verify.sh",
          "pin: plain command");
    check(Scaffold::parseVerificationCommand(
              "Verification: `bash scripts/verify.sh`\n") == "bash scripts/verify.sh",
          "pin: backticks stripped");
    check(Scaffold::parseVerificationCommand(
              "Verification: ``bash scripts/verify.sh``\n") == "bash scripts/verify.sh",
          "pin: double backticks stripped");
    check(Scaffold::parseVerificationCommand(
              "Verification: `bash scripts/verify.sh` - the pinned steps (`a`, "
              "`b`), then prose.\n") == "bash scripts/verify.sh",
          "pin: code span with trailing prose");
    check(Scaffold::parseVerificationCommand(
              "Verification: bash scripts/verify.sh \xe2\x80\x94 the pinned three "
              "steps (`cmake ...`, then `data.sh`)\n") == "bash scripts/verify.sh",
          "pin: em-dash explanation cut");
    check(Scaffold::parseVerificationCommand("Verification: bash x.sh.\n") == "bash x.sh",
          "pin: sentence period stripped");
    check(Scaffold::parseVerificationCommand("Verification: \"bash x.sh\"\n") == "bash x.sh",
          "pin: quote pair stripped");
    check(Scaffold::parseVerificationCommand("Verification: `none found`\n").empty(),
          "pin: none found (backticked)");
    check(Scaffold::parseVerificationCommand("Verification: none found\n").empty(),
          "pin: none found");
    check(Scaffold::parseVerificationCommand("# plan\nno pin here\n").empty(),
          "pin: absent");
    // A malformed pin that survives sanitizing still comes out verbatim: it
    // fails visibly at run time and the manager re-pins plan.md.
    check(Scaffold::parseVerificationCommand("Verification: run `x` then `y`\n") == "x",
          "pin: first code span of a broken pin");
}

static std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void writeLedger(const std::string& cwd,
                        const std::string& state,
                        const std::string& task,
                        const std::string& tasks = "") {
    std::filesystem::create_directories(cwd + "/.miniagent/ledger");
    std::ofstream(cwd + "/.miniagent/ledger/state.json") << state;
    std::ofstream(cwd + "/.miniagent/ledger/task.md") << task;
    std::filesystem::remove(cwd + "/.miniagent/ledger/tasks.json");
    if (!tasks.empty())
        std::ofstream(cwd + "/.miniagent/ledger/tasks.json") << tasks;
}

int main() {
    testParseVerificationCommand();
    testMechanicalDispatch();

    const std::string cwd = "/tmp/miniagent_ledger_status_test";
    std::filesystem::remove_all(cwd);

    // No ledger at all: neither finished nor resumable (the UI shows no
    // buttons; a normal send starts fresh).
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(!st.finished, "missing ledger: not finished");
        check(!st.resumable, "missing ledger: not resumable");
        check(st.task.empty(), "missing ledger: no task");
    }

    // An interrupted run: resumable, with the task text surfaced for the UI
    // (trimmed, like Scaffold::run's resume comparison trims both sides).
    writeLedger(cwd,
                R"({"phase":"worker","round":4,"lastTaskId":2,"signedOff":false})",
                "Refactor the config loader\n");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(!st.finished, "interrupted run: not finished");
        check(st.resumable, "interrupted run: resumable");
        check(st.phase == "worker", "interrupted run: phase read back");
        check(st.task == "Refactor the config loader", "interrupted run: task trimmed");
    }

    // Every non-done phase the scaffold persists is resumable.
    for (const char* phase : {"setup", "brainstorm", "curate", "finalizer"}) {
        writeLedger(cwd, std::string(R"({"phase":")") + phase + R"(","round":1})", "task\n");
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(st.resumable, std::string("phase ") + phase + ": resumable");
    }

    // A finished run: not resumable (buttons hide; a send starts fresh).
    writeLedger(cwd, R"({"phase":"done","round":5,"lastTaskId":3,"signedOff":true})", "task\n");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(st.finished, "finished run: finished");
        check(!st.resumable, "finished run: not resumable");
    }

    // A finished run whose tasks.json still holds open tasks and whose
    // manager never signed off: the round budget, not the work, ran out, so
    // the UI offers Continue (another round budget on the same ledger).
    writeLedger(cwd,
                R"({"phase":"done","round":10,"lastTaskId":10,"signedOff":false})",
                "task\n",
                R"([{"id":1,"status":"done"},{"id":2,"status":"in_progress"},)"
                R"({"id":3,"status":"pending"}])"
                "\n");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(st.finished, "out of rounds: finished");
        check(!st.resumable, "out of rounds: not resumable");
        check(st.openTasks == 2, "out of rounds: 2 open tasks counted");
        check(st.continuable, "out of rounds: continuable");
    }

    // Same, but everything settled: nothing to continue.
    writeLedger(cwd,
                R"({"phase":"done","round":10,"lastTaskId":3,"signedOff":false})",
                "task\n",
                R"([{"id":1,"status":"done"},{"id":2,"status":"done"}])"
                "\n");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(st.openTasks == 0, "all tasks done: none open");
        check(!st.continuable, "all tasks done: not continuable");
    }

    // Clean sign-off with stale pending entries left in tasks.json: the
    // manager said DONE, so the ledger is finished, not continuable.
    writeLedger(cwd,
                R"({"phase":"done","round":4,"lastTaskId":3,"signedOff":true})",
                "task\n",
                R"([{"id":1,"status":"done"},{"id":2,"status":"pending"}])"
                "\n");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(st.signedOff, "sign-off: flag read back");
        check(!st.continuable, "sign-off: not continuable despite a pending task");
    }

    // A finished run without tasks.json (setup never got there): nothing to
    // continue, so the row stays "Clear Ledger".
    writeLedger(cwd, R"({"phase":"done","round":1,"signedOff":false})", "task\n");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(st.openTasks == 0, "no tasks.json: none open");
        check(!st.continuable, "no tasks.json: not continuable");
    }

    // Malformed tasks.json: counted as zero open (never a false Continue).
    writeLedger(cwd,
                R"({"phase":"done","round":10,"signedOff":false})",
                "task\n",
                "{not json");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(!st.continuable, "malformed tasks.json: not continuable");
    }

    // An interrupted run with open tasks stays resumable (resume wins over
    // continuation: it picks up the saved phase instead of restarting).
    writeLedger(cwd,
                R"({"phase":"worker","round":7,"lastTaskId":3,"signedOff":false})",
                "task\n",
                R"([{"id":1,"status":"done"},{"id":2,"status":"pending"}])"
                "\n");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(st.resumable, "interrupted with open tasks: resumable");
        check(!st.continuable, "interrupted with open tasks: not continuable");
    }

    // Corrupt state.json: neither (Scaffold::run treats it as fresh too).
    writeLedger(cwd, "{not json", "task\n");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(!st.finished && !st.resumable, "corrupt state: neither finished nor resumable");
    }

    // Unknown phase value (e.g. older format): not resumable.
    writeLedger(cwd, R"({"phase":"warp","round":1})", "task\n");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(!st.finished && !st.resumable, "unknown phase: neither finished nor resumable");
    }

    // Resumable phase but the task text was lost: not resumable (run()'s
    // exact-match resume check could never pass).
    writeLedger(cwd, R"({"phase":"curate","round":2})", "");
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(!st.resumable, "empty task.md: not resumable");
    }

    // "New Session": discardLedger removes the whole ledger, so the row's
    // source data is gone and the next run starts fresh.
    writeLedger(cwd, R"({"phase":"curate","round":2,"lastTaskId":1})", "task\n");
    Scaffold::discardLedger(cwd);
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(!st.resumable && !st.finished, "discardLedger: not resumable afterwards");
        check(st.phase.empty() && st.task.empty(), "discardLedger: no phase/task left");
        check(!std::filesystem::exists(cwd + "/.miniagent/ledger"),
              "discardLedger: directory removed");
    }

    // The Continue button of a run that ran out of rounds: a
    // Start::ContinueLedger run restarts the loop on the SAME ledger, so
    // plan.md / notes.md / tasks.json survive; Start::Fresh wipes them.
    setenv("MINIAGENT_NO_SPEECH", "1", 1);
    std::atomic<bool> stop{false};
    ToolRegistry tools;
    OpenAIProvider provider;
    Scaffold scaffold(cwd, provider, &tools, nullptr);
    scaffold.run("Refactor the config loader", stop, Scaffold::Start::Fresh);
    {
        Scaffold::LedgerStatus st = Scaffold::ledgerStatus(cwd);
        check(st.finished, "role-less run: ends finished");
        check(st.phase == "done", "role-less run: phase done");
        check(std::filesystem::exists(cwd + "/.miniagent/ledger/plan.md"),
              "role-less run: plan.md seeded");
    }
    // Simulate what a round-budget-limited run leaves behind: open tasks, and
    // markers that tell a kept ledger from a wiped one.
    std::ofstream(cwd + "/.miniagent/ledger/plan.md") << "# plan\nMARKER-PLAN\n";
    std::ofstream(cwd + "/.miniagent/ledger/notes.md") << "# notes\nMARKER-NOTES\n";
    std::ofstream(cwd + "/.miniagent/ledger/tasks.json")
        << "[{\"id\":1,\"status\":\"done\"},{\"id\":2,\"status\":\"pending\"}]\n";
    const std::string planBefore  = slurp(cwd + "/.miniagent/ledger/plan.md");
    const std::string notesBefore = slurp(cwd + "/.miniagent/ledger/notes.md");
    const std::string tasksBefore = slurp(cwd + "/.miniagent/ledger/tasks.json");
    const std::string taskBefore  = slurp(cwd + "/.miniagent/ledger/task.md");
    Scaffold::LedgerStatus leftovers = Scaffold::ledgerStatus(cwd);
    check(leftovers.continuable, "leftovers written: continuable");
    scaffold.run(leftovers.task, stop, Scaffold::Start::ContinueLedger);
    check(slurp(cwd + "/.miniagent/ledger/plan.md") == planBefore,
          "continue: plan.md kept");
    check(slurp(cwd + "/.miniagent/ledger/notes.md") == notesBefore,
          "continue: notes.md kept");
    check(slurp(cwd + "/.miniagent/ledger/tasks.json") == tasksBefore,
          "continue: tasks.json kept (open tasks survive)");
    check(slurp(cwd + "/.miniagent/ledger/task.md") == taskBefore,
          "continue: task.md kept");
    check(Scaffold::ledgerStatus(cwd).continuable,
          "continue: a run that gets nowhere keeps offering Continue");
    scaffold.run(leftovers.task, stop, Scaffold::Start::Fresh);
    check(slurp(cwd + "/.miniagent/ledger/plan.md") != planBefore,
          "fresh: plan.md wiped (the control for the checks above)");

    // Resume matching is noise-tolerant: "please continue on X." must resume
    // the ledger of "Continue on X" instead of wiping it (the old exact-text
    // match re-planned from scratch on every politeness wobble).
    auto seedInterrupted = [&]() {
        std::ofstream(cwd + "/.miniagent/ledger/state.json")
            << R"({"phase":"worker","round":3,"lastTaskId":1,"signedOff":false})";
        std::ofstream(cwd + "/.miniagent/ledger/task.md")
            << "Please continue on the config loader.\n";
        std::ofstream(cwd + "/.miniagent/ledger/plan.md") << "# plan\nMARKER-RESUME\n";
    };
    seedInterrupted();
    scaffold.run("continue on the config loader", stop); // Start::Resume
    check(slurp(cwd + "/.miniagent/ledger/plan.md").find("MARKER-RESUME") !=
              std::string::npos,
          "resume: politeness/punctuation noise does not wipe the ledger");
    // A genuinely different task still starts fresh (wipes).
    seedInterrupted();
    scaffold.run("refactor the save system", stop);
    check(slurp(cwd + "/.miniagent/ledger/plan.md").find("MARKER-RESUME") ==
              std::string::npos,
          "resume: a different task still wipes");

    std::filesystem::remove_all(cwd);
    if (failures == 0)
        printf("ledger_status_test: all passed\n");
    else
        printf("ledger_status_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
