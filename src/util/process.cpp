#include "util/process.h"
#include "util/utf8.h"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

// Cap on output kept in memory: infinite-output commands (yes, tail -f, ...)
// must not grow the buffer unboundedly until the timeout fires.
constexpr size_t kMaxOutputBytes = 1 * 1024 * 1024;

// Process groups of every shell command this process has launched, keyed by
// the group id (which equals the forked child's pid, since it calls
// setpgid(0,0)). A command's descendants can outlive runShellCommand (the
// direct shell exits while a background job keeps running), so we keep the
// group here until it has no live members; killLiveShellProcesses() then
// force-stops whatever is still alive on stop/shutdown.
std::mutex g_groupsMutex;
std::vector<pid_t> g_liveGroups;

// Register a launched command's process group for the kill-everything hook.
void registerGroup(pid_t pgid) {
    std::lock_guard l(g_groupsMutex);
    g_liveGroups.push_back(pgid);
}

// Drop groups that have no live members. Caller holds g_groupsMutex.
void purgeDeadGroupsLocked() {
    g_liveGroups.erase(std::remove_if(
                           g_liveGroups.begin(), g_liveGroups.end(),
                           [](pid_t g) { return kill(-g, 0) == -1 && errno == ESRCH; }),
                       g_liveGroups.end());
}

void drainRead(int fd, std::string& dest) {
    char buf[8192];
    while (true) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            if (dest.size() < kMaxOutputBytes) {
                size_t room = kMaxOutputBytes - dest.size();
                size_t take = (size_t)n < room ? (size_t)n : room;
                dest.append(buf, take);
                if (dest.size() >= kMaxOutputBytes)
                    dest += "\n[output truncated]";
            }
        } else {
            break;
        }
    }
}

int waitExitCode(pid_t pid) {
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
}

} // namespace

void killLiveShellProcesses() {
    std::vector<pid_t> groups;
    {
        std::lock_guard l(g_groupsMutex);
        purgeDeadGroupsLocked();
        groups = g_liveGroups;
        g_liveGroups.clear();
    }
    // SIGKILL the whole group so descendants die too. Ignore ESRCH: a group
    // may have exited between the snapshot and the kill.
    for (pid_t g : groups)
        killpg(g, SIGKILL);
}

ProcessOutput runShellCommand(
    const std::string& command,
    const std::string& cwd,
    int timeoutMs,
    const std::atomic<bool>& abortFlag
) {
    ProcessOutput out;

    int pipeFd[2];
    if (pipe(pipeFd) != 0) {
        out.output = "pipe() failed: " + std::string(strerror(errno));
        return out;
    }

    pid_t pid = fork();
    if (pid < 0) {
        out.output = "fork() failed: " + std::string(strerror(errno));
        close(pipeFd[0]);
        close(pipeFd[1]);
        return out;
    }

    if (pid == 0) {
        // Child: own process group so the whole tree can be killed at once.
        if (setpgid(0, 0) != 0)
            _exit(127);
        if (chdir(cwd.c_str()) != 0)
            _exit(127);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            // Non-interactive: never let the command read the terminal.
            if (dup2(devnull, STDIN_FILENO) < 0) { /* non-fatal */ }
            close(devnull);
        }
        if (dup2(pipeFd[1], STDOUT_FILENO) < 0 || dup2(pipeFd[1], STDERR_FILENO) < 0)
            _exit(127);
        close(pipeFd[0]);
        close(pipeFd[1]);

        const char* argv[] = {"bash", "-c", command.c_str(), nullptr};
        execvp("bash", const_cast<char* const*>(argv));
        _exit(127); // exec failed
    }

    // Parent
    close(pipeFd[1]);
    fcntl(pipeFd[0], F_SETFL, O_NONBLOCK);

    // Track this command's process group (id == the child's pid) so that
    // descendants outliving this call can still be force-stopped on
    // stop/shutdown. Purge dead groups opportunistically to keep the list bounded.
    {
        std::lock_guard l(g_groupsMutex);
        purgeDeadGroupsLocked();
        g_liveGroups.push_back(pid);
    }

    std::chrono::steady_clock::time_point deadline;
    bool hasDeadline = timeoutMs > 0;
    if (hasDeadline)
        deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    bool eof = false;
    bool reaped = false;

    // The child's own output is fully in the pipe buffer once it exits, so
    // completion is signaled by the child exiting, not by pipe EOF: a
    // grandchild that keeps the write end open (e.g. a background job) must
    // not hold the call open. Any leftover buffer is drained below.
    while (!reaped) {
        bool stop = abortFlag.load() || (hasDeadline && std::chrono::steady_clock::now() >= deadline);
        if (hasDeadline && std::chrono::steady_clock::now() >= deadline)
            out.timedOut = true;

        if (!stop) {
            if (!eof) {
                struct pollfd pfd{.fd = pipeFd[0], .events = POLLIN};
                int pr = poll(&pfd, 1, 100);
                if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
                    char buf[8192];
                    ssize_t n = read(pipeFd[0], buf, sizeof(buf));
                    if (n > 0) {
                        if (out.output.size() < kMaxOutputBytes) {
                            size_t room = kMaxOutputBytes - out.output.size();
                            size_t take = (size_t)n < room ? (size_t)n : room;
                            out.output.append(buf, take);
                            if (out.output.size() >= kMaxOutputBytes) {
                                out.truncated = true;
                                out.output += "\n[output truncated]";
                            }
                        }
                        // Past the cap we still drain: reading is what
                        // unblocks the writer.
                    } else if (n == 0) {
                        eof = true;
                    }
                }
            } else {
                // Pipe at EOF but child still running: just wait.
                poll(nullptr, 0, 100);
            }
        }

        int status = 0;
        errno = 0;
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            reaped = true;
            if (WIFEXITED(status))
                out.exitCode = WEXITSTATUS(status);
            else if (WIFSIGNALED(status))
                out.exitCode = 128 + WTERMSIG(status);
        } else if (r < 0 && errno != EINTR) {
            reaped = true;
        }

        if (stop) {
            // Abort or timeout: force-stop the whole command. Kill the entire
            // process group, not just the direct shell, so any descendants the
            // command spawned (background jobs, `... &`, long-lived servers)
            // die too. This matters even when the direct shell has already
            // exited and been reaped (`reaped`): without killing the group,
            // those grandchildren would outlive the stop and the command would
            // never really be stopped (this is what made the app hang when it
            // was stopped or closed mid-call).
            killpg(pid, SIGKILL);
            out.killed = true;
            out.exitCode = 137;
            if (!reaped)
                waitExitCode(pid); // reap the now-dead child; no zombie
            break;
        }
    }

    drainRead(pipeFd[0], out.output);
    close(pipeFd[0]);

    if (out.exitCode < 0 && !out.killed)
        out.exitCode = waitExitCode(pid);

    // Command output is untrusted: strip invalid UTF-8 so it is safe to put
    // in JSON (strict dump() would throw) and in the UI.
    out.output = sanitizeUtf8(out.output);

    return out;
}
