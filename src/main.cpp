#include "util/log.h"
#include "util/settings.h"

#include <string>
#include <sys/stat.h>
#include <unistd.h>

int runDesktop(int argc, char** argv); // src/desktop_main.cpp (SDL3/ImGui window)
int runTui();                          // src/tui_main.cpp (FTXUI terminal frontend)

int main(int argc, char** argv) {
    // Arg parsing for both frontends: [--desktop|--gui|-g|--tui] [workdir].
    // The SDL3/ImGui window is the default; --tui selects the terminal
    // frontend. The first non-flag argument is the working directory.
    bool tui = false;
    std::string workDir;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--desktop" || arg == "--gui" || arg == "-g")
            tui = false;
        else if (arg == "--tui")
            tui = true;
        else if (workDir.empty())
            workDir = arg;
    }

    // Settings and history live in ~/.config/miniagent; make sure it exists.
    if (!EnsureConfigDir()) logLine("could not create %s", miniagentConfigDir().c_str());

    // If a directory is passed as the first argument, work there; otherwise
    // use the current working directory.
    if (!workDir.empty()) {
        struct stat st;
        if (stat(workDir.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
            logLine("not a directory: %s", workDir.c_str());
            return 1;
        }
        if (chdir(workDir.c_str()) != 0) {
            logLine("chdir failed: %s", workDir.c_str());
            return 1;
        }
    }

    return tui ? runTui() : runDesktop(argc, argv);
}
