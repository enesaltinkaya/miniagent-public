#pragma once

#include <atomic>
#include <string>

struct ProcessOutput {
    std::string output; // stdout + stderr combined (capped in memory, see runShellCommand)
    int exitCode = -1;
    bool timedOut = false;
    bool killed = false; // killed due to abort or timeout
    bool truncated = false; // output exceeded the in-memory cap
};

// Runs `bash -c command` in `cwd` in its own process group.
// Captures combined output, honoring timeoutMs (0 = no timeout) and
// abortFlag (checked between read polls). Kills the process group on
// timeout/abort and reaps it. Output is truncated by the caller as needed.
ProcessOutput runShellCommand(
    const std::string& command,
    const std::string& cwd,
    int timeoutMs,
    const std::atomic<bool>& abortFlag
);

// Force-stop every shell command still alive, including descendants that
// outlived their runShellCommand call (background jobs, `... &`, dev servers
// the command spawned). Called on stop/shutdown so stopping the agent or
// closing the app actually force-stops the commands it launched, not just the
// one that happened to be running. Safe to call from any thread; a no-op when
// nothing is live.
void killLiveShellProcesses();
