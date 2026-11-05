// Regression tests for the scaffold harness gate (Scaffold::runVerification).
// The pinned `Verification:` command used to run through popen(): no deadline,
// no abort path, the app's stdin inherited, and completion tied to pipe EOF.
// A gate that never returns - or one that leaves a grandchild holding the pipe
// open, or one that reads the inherited terminal - wedged the whole scaffold
// loop right after a worker's task result: the UI sat at "Working..." with Esc
// doing nothing.
#include "agent/scaffold.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    } else {
        std::printf("ok: %s\n", what);
    }
}

// A gate that outlives any sane deadline, with the marker in the sleep's own
// command line (exec -a) so alive() sees the survivor, not just the shell.
static std::string hangCmd(const std::string& marker, int secs) {
    return "bash -c 'exec -a " + marker + " sleep " + std::to_string(secs) + "'";
}

static double secsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// True while a process whose command line carries `marker` is still alive. The
// pattern's first character goes in brackets so the pgrep command line itself
// (which carries the pattern) does not match it.
static bool alive(const std::string& marker) {
    const std::string pattern = "[" + marker.substr(0, 1) + "]" + marker.substr(1);
    return std::system(("pgrep -f -- '" + pattern + "' > /dev/null 2>&1").c_str()) == 0;
}

int main() {
    static std::atomic<bool> noAbort{false};
    const std::string marker = "gate_test_sleep_" + std::to_string((long)::getpid());

    // A gate that never finishes hits the deadline instead of hanging the run.
    {
        std::string tail;
        bool timedOut = false;
        auto t0       = std::chrono::steady_clock::now();
        int rc        = Scaffold::runVerification("/tmp", hangCmd(marker, 120), tail, noAbort, 1,
                                                  &timedOut);
        double secs   = secsSince(t0);
        check(rc == 124 && timedOut, "endless gate returns 124 and reports the timeout");
        check(secs < 10, "endless gate stops at its deadline, not never");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(!alive(marker), "the timed-out gate's process group was killed");
    }

    // Esc mid-gate: the abort flag stops it and kills the whole group.
    {
        std::atomic<bool> abort{false};
        std::thread stopper([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            abort = true;
        });
        std::string tail;
        bool timedOut = true;
        auto t0       = std::chrono::steady_clock::now();
        int rc        = Scaffold::runVerification("/tmp", hangCmd(marker, 120), tail, abort, 60,
                                                  &timedOut);
        double secs   = secsSince(t0);
        stopper.join();
        check(rc == 124 && !timedOut, "an aborted gate reports 124, not a timeout");
        check(secs < 10, "Esc stops a running gate");
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        check(!alive(marker), "the aborted gate's process group was killed");
    }

    // The popen trap: a background grandchild keeps the output pipe's write end
    // open, so waiting for EOF would hold the call open until the deadline.
    {
        std::string tail;
        bool timedOut = true;
        auto t0       = std::chrono::steady_clock::now();
        const std::string cmd =
            "bash -c 'exec -a " + marker + " sleep 30' & echo gate-output";
        int rc = Scaffold::runVerification("/tmp", cmd, tail, noAbort, 20, &timedOut);
        double secs   = secsSince(t0);
        check(rc == 0 && !timedOut, "a background grandchild does not fail the gate");
        check(secs < 10, "a grandchild holding the pipe does not hold the gate open");
        check(tail.find("gate-output") != std::string::npos, "the gate's output tail is kept");
    }

    // A gate reading stdin must not wait on the app's terminal.
    {
        std::string tail;
        int rc = Scaffold::runVerification("/tmp",
                                           "if [ -t 0 ]; then echo STDIN_IS_TTY; else "
                                           "echo STDIN_NOT_TTY; fi; cat",
                                           tail, noAbort, 20);
        check(rc == 0, "a gate that reads stdin returns");
        check(tail.find("STDIN_NOT_TTY") != std::string::npos,
              "the gate's stdin is not a terminal");
    }

    // A failing gate still reports the real exit status and its tail lines.
    {
        std::string tail;
        bool timedOut = true;
        int rc        = Scaffold::runVerification("/tmp", "echo first; echo last; exit 7", tail,
                                                  noAbort, 20, &timedOut);
        check(rc == 7 && !timedOut, "a failing gate keeps its exit status");
        check(tail.find("last") != std::string::npos, "the tail carries the last lines");
    }

    std::printf(failures ? "gate_test: %d FAILED\n" : "gate_test: all passed\n", failures);
    return failures ? 1 : 0;
}
