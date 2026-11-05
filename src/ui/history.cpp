#include "ui/history.h"

#include "util/settings.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

std::string HistoryPath() {
    return miniagentConfigDir() + "/history.txt";
}

}  // namespace

void SaveHistory(const std::vector<std::string>& hist) {
    std::FILE* f = std::fopen(HistoryPath().c_str(), "w");
    if (!f)
        return;
    for (const std::string& e : hist) {
        for (char c : e) {
            if (c == '\\')
                std::fputs("\\\\", f);
            else if (c == '\n')
                std::fputs("\\n", f);
            else
                std::fputc(c, f);
        }
        std::fputc('\n', f);
    }
    std::fclose(f);
}

void LoadHistory(std::vector<std::string>& hist) {
    std::FILE* f = std::fopen(HistoryPath().c_str(), "r");
    if (!f)
        return;
    char line[8192];
    while (std::fgets(line, sizeof(line), f)) {
        std::string l(line);
        if (!l.empty() && l.back() == '\n')
            l.pop_back();
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
        if (l.empty())
            continue;
        std::string entry;
        for (size_t i = 0; i < l.size(); ++i) {
            if (l[i] == '\\' && i + 1 < l.size()) {
                char n = l[++i];
                entry += (n == 'n') ? '\n' : n;
            } else
                entry += l[i];
        }
        hist.push_back(std::move(entry));
    }
    std::fclose(f);
}
