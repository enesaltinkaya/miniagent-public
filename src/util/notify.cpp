#include "notify.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>

namespace {

// Locate a program on $PATH ("" when missing).
std::string findProgram(const char* name) {
    const char* path = std::getenv("PATH");
    if (!path)
        return "";
    for (const char* p = path;;) {
        const char* next = strchr(p, ':');
        const std::string dir = next ? std::string(p, next - p) : std::string(p);
        const std::string cand = dir.empty() ? name : dir + "/" + name;
        if (::access(cand.c_str(), X_OK) == 0)
            return cand;
        if (!next)
            return "";
        p = next + 1;
    }
}

// Run a command fully detached (new session, reparented to init), with stdio
// pointed at /dev/null. Never blocks: the intermediate fork exits
// immediately, so the parent never waits for the command itself. Failures
// (missing command, fork errors) are silently ignored - the caller treats
// notification as best effort.
void spawnDetached(const std::vector<std::string>& cmd) {
    if (cmd.empty())
        return;
    std::vector<char*> argv;
    argv.reserve(cmd.size() + 1);
    for (const std::string& s : cmd)
        argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    const pid_t a = ::fork();
    if (a < 0)
        return;
    if (a == 0) {
        ::setsid();
        const pid_t b = ::fork();
        if (b == 0) {
            const int devnull = ::open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                ::dup2(devnull, 0);
                ::dup2(devnull, 1);
                ::dup2(devnull, 2);
                if (devnull > 2)
                    ::close(devnull);
            }
            ::execvp(argv[0], argv.data());
            _exit(127);
        }
        _exit(0);
    }
    // The middle child exits right away; the grandchild (the actual command)
    // is adopted by init and reaped there, so no zombie is ever left here.
    ::waitpid(a, nullptr, 0);
}

// OSC 777 (Ghostty, iTerm2, WezTerm, rxvt-unicode). Written to the
// controlling terminal when one exists; a GUI process without a terminal is
// a silent no-op.
void oscToast(const std::string& title, const std::string& body) {
    // ';' separates the OSC fields: swap it for ',' so the body's own
    // punctuation cannot break the layout.
    auto swapSemi = [](std::string s) {
        for (char& c : s)
            if (c == ';')
                c = ',';
        return s;
    };
    const std::string osc = "\x1b]777;notify;" + swapSemi(title) + ";" + swapSemi(body) + "\x07";
    const int tty = ::open("/dev/tty", O_WRONLY);
    if (tty >= 0) {
        (void)!::write(tty, osc.data(), osc.size());
        ::close(tty);
        return;
    }
    if (::isatty(1))
        (void)!::write(1, osc.data(), osc.size());
}

} // namespace

std::string compactNotifyText(const std::string& text, size_t maxLen) {
    std::string out;
    out.reserve(std::min(text.size(), maxLen) + 3);
    bool space = false; // pending single space (not flushed yet)
    for (char c : text) {
        if (std::isspace((unsigned char)c)) {
            if (!out.empty())
                space = true;
            continue;
        }
        if (space && !out.empty())
            out += ' ';
        space = false;
        out += c;
    }
    if (out.size() <= maxLen)
        return out;
    out.resize(maxLen);
    while (!out.empty() && (out.back() & 0xC0) == 0x80)
        out.pop_back(); // never split a multi-byte character
    out += "\xE2\x80\xA6";
    return out;
}

void notifyDesktop(const std::string& title, const std::string& text, bool isError) {
    if (const char* env = std::getenv("MINIAGENT_NO_NOTIFY"))
        if (env[0] == '1' || env[0] == 'y' || env[0] == 'Y')
            return;
    const std::string body = compactNotifyText(text);

    const std::string notifySend = findProgram("notify-send");
    if (!notifySend.empty()) {
        spawnDetached({notifySend,
                       "--app-name=miniagent",
                       isError ? "--urgency=critical" : "--urgency=normal",
                       isError ? "--icon=dialog-error" : "--icon=dialog-information",
                       "--",
                       title,
                       body});
    } else {
        oscToast(title, body);
    }

    const std::string canberra = findProgram("canberra-gtk-play");
    if (!canberra.empty())
        spawnDetached({canberra, "-i", isError ? "dialog-warning" : "complete"});
}
