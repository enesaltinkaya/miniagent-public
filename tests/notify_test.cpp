#include "util/notify.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/types.h>

namespace {

const char* const kBinDir = "/tmp/miniagent_notify_test_bin";
const char* const kLog    = "/tmp/miniagent_notify_test_log";

void makeFake(const char* name, const char* body) {
    std::ofstream f(std::string(kBinDir) + "/" + name);
    f << body;
    f.close();
    ::chmod((std::string(kBinDir) + "/" + name).c_str(), 0755);
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// The detached fakes write asynchronously; poll briefly for the line.
bool logContains(const std::string& needle) {
    for (int i = 0; i < 100; ++i) {
        if (readFile(kLog).find(needle) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return readFile(kLog).find(needle) != std::string::npos;
}

} // namespace

int main() {
    // --- compactNotifyText ----------------------------------------------------
    assert(compactNotifyText("  a   b\nc\t\t d ") == "a b c d");
    assert(compactNotifyText("   ") == "");
    assert(compactNotifyText("short").size() == 5);
    {
        std::string long1(200, 'x');
        std::string c = compactNotifyText(long1);
        assert(c == std::string(180, 'x') + "\xE2\x80\xA6"); // 180 chars + ellipsis
    }
    // Truncation must not split a multi-byte character.
    {
        std::string utf8;
        for (int i = 0; i < 100; ++i)
            utf8 += "\xC3\xA9"; // 100 x "e-acute" = 200 bytes
        std::string t = compactNotifyText(utf8);
        assert(t.compare(t.size() - 3, 3, "\xE2\x80\xA6") == 0);
        assert((static_cast<unsigned char>(t[t.size() - 4]) & 0xC0) != 0x80); // no dangling continuation
    }

    // --- notifyDesktop (fake commands on PATH) --------------------------------
    ::unlink(kLog);
    ::mkdir(kBinDir, 0755);
    makeFake("notify-send",
             "#!/bin/sh\nfor a in \"$@\"; do printf '[%s]\\n' \"$a\" >> "
             "/tmp/miniagent_notify_test_log; done\n");
    makeFake("canberra-gtk-play",
             "#!/bin/sh\nfor a in \"$@\"; do printf '[%s]\\n' \"$a\" >> "
             "/tmp/miniagent_notify_test_log; done\n");

    const std::string oldPath = std::string(getenv("PATH") ? getenv("PATH") : "");
    setenv("PATH", (std::string(kBinDir) + ":" + oldPath).c_str(), 1);
    unsetenv("MINIAGENT_NO_NOTIFY");

    notifyDesktop("miniagent", "Done \u2014 waiting for input", false);
    assert(logContains("[--urgency=normal]"));
    assert(logContains("[--icon=dialog-information]"));
    assert(logContains("[miniagent]"));
    assert(logContains("[Done \u2014 waiting for input]"));
    assert(logContains("[complete]")); // canberra sound icon

    notifyDesktop("miniagent \u2014 error", "some   long\nerror  text", true);
    assert(logContains("[--urgency=critical]"));
    assert(logContains("[--icon=dialog-error]"));
    assert(logContains("[miniagent \u2014 error]"));
    assert(logContains("[some long error text]")); // whitespace collapsed
    assert(logContains("[dialog-warning]"));

    // MINIAGENT_NO_NOTIFY=1 suppresses everything.
    setenv("MINIAGENT_NO_NOTIFY", "1", 1);
    const std::string logBefore = readFile(kLog);
    notifyDesktop("miniagent", "should not appear", false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(readFile(kLog) == logBefore);

    // Without notify-send the OSC fallback must be a silent no-op
    // (no crash, nothing logged).
    unsetenv("MINIAGENT_NO_NOTIFY");
    setenv("PATH", "/nonexistent", 1);
    notifyDesktop("miniagent", "osc fallback", false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(readFile(kLog) == logBefore);

    setenv("PATH", oldPath.c_str(), 1);
    printf("notify_test: ok\n");
    return 0;
}
