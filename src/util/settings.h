#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <utility>

#include <unistd.h>

// Persistent app settings, stored as a key=value file in
// ~/.config/miniagent (one entry per line).

inline std::string miniagentConfigDir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home)
        home = ".";
    return std::string(home) + "/.config/miniagent";
}

// Creates ~/.config/miniagent (and ~/.config) if they don't exist.
// Idempotent; returns false if the config dir could not be created.
inline bool EnsureConfigDir() {
    const std::string dir       = miniagentConfigDir();
    const std::string dotConfig  = dir.substr(0, dir.rfind('/'));
    if (mkdir(dotConfig.c_str(), 0755) != 0 && errno != EEXIST) return false;
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) return false;
    return true;
}

// Optional user-supplied fonts live in ~/.config/miniagent/fonts.
inline std::string miniagentFontsDir() { return miniagentConfigDir() + "/fonts"; }

inline std::string settingsPath() { return miniagentConfigDir() + "/miniagent.settings"; }

inline std::map<std::string, std::string> loadSettings() {
    std::map<std::string, std::string> out;
    FILE* f = fopen(settingsPath().c_str(), "r");
    if (!f)
        return out;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        char* eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        out[std::string(line)] = std::string(eq + 1);
    }
    fclose(f);
    return out;
}

inline bool saveSettings(const std::map<std::string, std::string>& values) {
    std::string path = settingsPath();
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "w");
    if (!f)
        return false;
    for (const auto& [k, v] : values)
        fprintf(f, "%s=%s\n", k.c_str(), v.c_str());
    fclose(f);
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}
