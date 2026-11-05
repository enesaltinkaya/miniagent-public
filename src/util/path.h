#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include <sys/stat.h>

// Resolve a (possibly relative) path against cwd. Supports leading "/".
// Normalizes "." and ".." segments lexically. Does not touch the filesystem.
inline std::string resolvePath(const std::string& cwd, const std::string& path) {
    if (path.empty())
        return cwd;
    std::string p = path;
    if (p[0] != '/') {
        p = cwd + "/" + p;
    }

    std::vector<std::string> parts;
    std::string current;
    for (char c : p) {
        if (c == '/') {
            if (!current.empty() && current != ".") {
                if (current == "..") {
                    if (!parts.empty())
                        parts.pop_back();
                } else {
                    parts.push_back(current);
                }
            }
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty() && current != ".") {
        if (current == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else {
            parts.push_back(current);
        }
    }

    std::string result = "/";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0)
            result += "/";
        result += parts[i];
    }
    return result;
}

inline bool isDir(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

inline bool isFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// Truncate to at most maxLines lines and maxBytes bytes (whichever first).
// Returns truncated text plus a trailing note if truncation happened.
// cutOut (optional) receives the byte offset the text was cut at: the
// content before it is what the caller actually got.
inline std::string truncateToLimit(const std::string& text, size_t maxLines = 2000, size_t maxBytes = 50 * 1024,
                                   size_t* cutOut = nullptr) {
    size_t newlineCount = 0;
    size_t lastNewline = std::string::npos;
    bool hitLineLimit = false;
    size_t i = 0;
    for (; i < text.size() && i < maxBytes; i++) {
        if (text[i] == '\n') {
            newlineCount++;
            lastNewline = i;
            if (newlineCount >= maxLines) {
                hitLineLimit = true;
                break;
            }
        }
    }
    size_t cut = text.size();
    if (hitLineLimit || (i < text.size() && cut == text.size())) {
        // cut at the last complete line within the limits (mid-line cut if there is none)
        cut = (lastNewline != std::string::npos) ? lastNewline : i;
    }
    if (cutOut)
        *cutOut = cut;
    bool truncated = (cut < text.size());
    if (truncated) {
        std::string out = text.substr(0, cut);
        char note[128];
        snprintf(note, sizeof(note), "\n... [truncated: showing %zu bytes of %zu]", cut, text.size());
        out += note;
        return out;
    }
    return text;
}
