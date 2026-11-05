#include "util/speech.h"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>

namespace {

    constexpr size_t kMaxQueued = 4;

    // The worker is detached and stays parked in cv.wait whenever it has
    // nothing to say, so its state must outlive exit(): exit() runs the static
    // destructors and pthread_cond_destroy() blocks forever while a waiter is
    // parked there, which hung app shutdown after any run that spoke. It is
    // allocated once and never freed, so no destructor ever runs.
    struct Speaker {
        std::mutex mtx;
        std::condition_variable cv;
        std::deque<std::string> queue;
        std::atomic<bool> threadStarted{false};
        void loop();
    };

    Speaker& speaker() {
        static Speaker* inst = new Speaker();
        return *inst;
    }

    bool disabled() {
        static bool off = [] {
            const char* env = std::getenv("MINIAGENT_NO_SPEECH");
            return env && (env[0] == '1' || env[0] == 'y' || env[0] == 'Y');
        }();
        return off;
    }

    int runSync(const std::string& cmd) {
        const int devnull = ::open("/dev/null", O_WRONLY);
        const pid_t pid   = ::fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            if (devnull >= 0) {
                ::dup2(devnull, STDOUT_FILENO);
                ::dup2(devnull, STDERR_FILENO);
                if (devnull > 2) ::close(devnull);
            }
            ::execlp("bash", "bash", "-c", cmd.c_str(), (char*)nullptr);
            _exit(127);
        }
        if (devnull >= 0) ::close(devnull);
        int status = 0;
        if (::waitpid(pid, &status, 0) != pid) return -1;
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        return -1;
    }

    std::string shellQuote(const std::string& s) {
        std::string q = "'";
        for (char c : s) {
            if (c == '\'')
                q += "'\\''";
            else
                q += c;
        }
        return q + "'";
    }

    void Speaker::loop() {
        for (;;) {
            std::string text;
            {
                std::unique_lock l(mtx);
                cv.wait(l, [this] { return !queue.empty(); });
                text = std::move(queue.front());
                queue.pop_front();
            }
            // The silent warm-up utterance wakes the audio device; without it
            // the first phoneme of the real phrase is swallowed.
            const std::string cmd = "speak ' ' && speak " + shellQuote(text);
            // Cross-process lock: multiple miniagent instances (or tests) must
            // not play over each other. One shared lock file serializes all.
            const std::string lockPath = "/tmp/miniagent_speech.lock";
            const int lockFd = ::open(lockPath.c_str(), O_CREAT | O_WRONLY, 0600);
            if (lockFd >= 0) {
                ::flock(lockFd, LOCK_EX);  // blocks until no other process plays
                (void)runSync(cmd);
                ::flock(lockFd, LOCK_UN);
                ::close(lockFd);
            } else {
                (void)runSync(cmd);
            }
        }
    }

}  // namespace

void speak(const std::string& text) {
    if (text.empty() || disabled()) return;
    Speaker& sp = speaker();
    {
        std::lock_guard l(sp.mtx);
        sp.queue.push_back(text);
        if (sp.queue.size() > kMaxQueued) sp.queue.pop_front();
        if (!sp.threadStarted.exchange(true)) std::thread(&Speaker::loop, &sp).detach();
    }
    sp.cv.notify_one();
}
