// Regression tests for runShellCommand (util/process.cpp).
// Covers the "app gets stuck on bash tool call" bugs:
//   - dup2 success returns the fd, not 0 (command used to never run)
//   - poll only reacted to POLLIN, so pipe EOF (POLLHUP) spun at 100% CPU
//   - a grandchild holding the pipe write end kept the call open until timeout
#include "util/process.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

static ProcessOutput run(const std::string& command, int timeoutMs, std::atomic<bool>* abort = nullptr) {
    static std::atomic<bool> noAbort{false};
    return runShellCommand(command, "/tmp", timeoutMs, abort ? *abort : noAbort);
}

static double secsSince(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

int main() {
    { // simple command with output
        auto t0 = std::chrono::steady_clock::now();
        ProcessOutput out = run("echo hi", 5000);
        double took = secsSince(t0);
        check(out.exitCode == 0, "echo: exit 0");
        check(out.output == "hi\n", "echo: captured stdout");
        check(!out.timedOut && !out.killed, "echo: not timed out/killed");
        check(took < 2.0, "echo: returns fast");
    }
    { // no output at all (was the infinite POLLHUP spin)
        auto t0 = std::chrono::steady_clock::now();
        ProcessOutput out = run("true", 5000);
        double took = secsSince(t0);
        check(out.exitCode == 0, "true: exit 0");
        check(out.output.empty(), "true: empty output");
        check(took < 2.0, "true: returns fast");
    }
    { // stderr is captured
        ProcessOutput out = run("echo to-err >&2", 5000);
        check(out.exitCode == 0, "stderr: exit 0");
        check(out.output == "to-err\n", "stderr: captured");
    }
    { // non-zero exit code
        ProcessOutput out = run("exit 3", 5000);
        check(out.exitCode == 3, "exit 3: propagated");
    }
    { // grandchild holding the pipe must not delay the result
        auto t0 = std::chrono::steady_clock::now();
        ProcessOutput out = run("sleep 300 & echo started", 10000);
        double took = secsSince(t0);
        check(out.exitCode == 0, "bg: exit 0");
        check(out.output == "started\n", "bg: captured stdout");
        check(!out.timedOut, "bg: not reported as timeout");
        check(took < 3.0, "bg: returns while grandchild still runs");
    }
    { // timeout kills the process group, keeps partial output
        auto t0 = std::chrono::steady_clock::now();
        ProcessOutput out = run("echo partial; sleep 30", 1000);
        double took = secsSince(t0);
        check(out.exitCode == 137, "timeout: exit 137");
        check(out.timedOut && out.killed, "timeout: flags set");
        check(out.output.find("partial") != std::string::npos, "timeout: partial output kept");
        check(took < 5.0, "timeout: killed promptly");
    }
    { // stdin is /dev/null, so commands reading stdin cannot hang on the tty
        auto t0 = std::chrono::steady_clock::now();
        ProcessOutput out = run("read x; echo after-read", 5000);
        double took = secsSince(t0);
        check(out.exitCode == 0, "stdin: exit 0");
        check(out.output == "after-read\n", "stdin: got EOF from /dev/null");
        check(took < 2.0, "stdin: does not block on input");
    }
    { // abort kills the command
        std::atomic<bool> abort{false};
        auto t0 = std::chrono::steady_clock::now();
        std::thread t([&abort]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            abort.store(true);
        });
        ProcessOutput out = runShellCommand("sleep 30", "/tmp", 60000, abort);
        t.join();
        double took = secsSince(t0);
        check(out.exitCode == 137 && out.killed, "abort: killed");
        check(took < 5.0, "abort: returns promptly");
    }
    { // large but bounded output is fully captured
        ProcessOutput out = run("seq 1 20000 > /tmp/pt_big.txt && cat /tmp/pt_big.txt", 10000);
        check(out.exitCode == 0, "big: exit 0");
        check(out.output.size() > 100000, "big: >100KB captured");
        check(!out.truncated, "big: not truncated under cap");
    }
    { // output beyond the cap is truncated in memory, command still completes
        ProcessOutput out = run("seq 1 400000 > /tmp/pt_big2.txt && cat /tmp/pt_big2.txt", 10000);
        check(out.exitCode == 0, "huge: exit 0");
        check(out.truncated, "huge: truncated flag set");
        check(out.output.size() <= 1200 * 1024, "huge: bounded in memory");
    }
    { // a still-running command (with a background child sharing its process
        // group) is force-stopped by the stop hook. This is the regression for
        // "stop/close during a bash call left the command's descendants running":
        // without the hook the `sleep 300` grandchild would outlive the call and
        // keep running, and without the in-loop group kill a plain `sleep` would
        // also survive. The call has no abort/timeout, so only the hook stops it.
        std::atomic<bool> done{false};
        ProcessOutput out;
        auto t0 = std::chrono::steady_clock::now();
        std::thread worker([&]() {
            out = run("sh -c 'sleep 300 & wait'", 120000); // would run 300s otherwise
            done.store(true);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        check(!done.load(), "stop-hook: still running before the hook");
        killLiveShellProcesses();
        for (int i = 0; i < 40 && !done.load(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        double took = secsSince(t0);
        check(done.load(), "stop-hook: in-flight call returned after the hook");
        check(out.exitCode == 137, "stop-hook: whole group killed (137)");
        check(took < 5.0, "stop-hook: returned promptly, not after 300s");
        worker.join();
    }
    { // the hook is a safe no-op when nothing is running
        killLiveShellProcesses();
    }

    if (failures == 0)
        std::printf("process_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
