#include "util/speech.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

#include <sys/stat.h>

namespace {

const char* const kBinDir = "/tmp/miniagent_speech_test_bin";
const char* const kLog    = "/tmp/miniagent_speech_test_log";

std::string readFile() {
    std::ifstream f(kLog);
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// The worker runs the fake asynchronously; poll briefly for the phrase.
bool logContains(const std::string& needle) {
    for (int i = 0; i < 200; ++i) {
        if (readFile().find(needle) != std::string::npos)
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return false;
}

} // namespace

int main() {
    ::unlink(kLog);
    ::mkdir(kBinDir, 0755);
    {
        std::ofstream f(std::string(kBinDir) + "/speak");
        f << "#!/bin/sh\nprintf '%s\\n' \"$*\" >> " << kLog << "\n";
    }
    ::chmod((std::string(kBinDir) + "/speak").c_str(), 0755);

    const std::string oldPath = getenv("PATH") ? getenv("PATH") : "";
    setenv("PATH", (std::string(kBinDir) + ":" + oldPath).c_str(), 1);
    unsetenv("MINIAGENT_NO_SPEECH");

    speak("miniagent done");
    assert(logContains("miniagent done"));

    setenv("PATH", oldPath.c_str(), 1);

    // The worker is still alive, parked in the queue's condition variable.
    // Returning from main is the regression check: a static condvar is
    // destroyed by exit(), and pthread_cond_destroy() blocks forever while a
    // waiter is parked in it, which used to leave a hung miniagent process
    // behind after closing the app post-run.
    printf("speech_test: ok\n");
    return 0;
}
