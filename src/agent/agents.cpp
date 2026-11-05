#include "agent/agents.h"

#include "agent/notes_limit.h"
#include "assets.h"
#include "util/settings.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace {

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::vector<std::string> lines(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty())
        out.push_back(cur);
    return out;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// Splits "a, b, c" respecting nothing else (no quoting needed in practice).
std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ',') {
            if (!trim(cur).empty())
                out.push_back(trim(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!trim(cur).empty())
        out.push_back(trim(cur));
    return out;
}

} // namespace

bool parseAgentContent(const std::string& nameHint, const std::string& content, AgentDef& out) {
    auto ls = lines(content);
    size_t first = 0;
    while (first < ls.size() && trim(ls[first]).empty())
        ++first;
    if (first >= ls.size() || trim(ls[first]) != "---")
        return false;

    AgentDef def;
    def.name = nameHint;
    std::vector<std::string> tools;
    bool nameOk = false, descOk = false;
    bool toolsList = false; // "tools:" with no value -> following "- x" lines are tools
    size_t i = first + 1;
    for (; i < ls.size(); ++i) {
        const std::string line = ls[i];
        const std::string t = trim(line);
        if (t == "---")
            break;
        if (t.empty())
            continue;
        if (!toolsList && t.rfind("name:", 0) == 0) {
            def.name = trim(t.substr(5)); // "name:"
            nameOk = !def.name.empty();
        } else if (!toolsList && t.rfind("description:", 0) == 0) {
            def.description = trim(t.substr(13)); // "description: "
            descOk = !def.description.empty();
        } else if (!toolsList && t.rfind("tools:", 0) == 0) {
            std::string v = trim(t.substr(6)); // "tools:"
            if (!v.empty()) {
                tools = splitCsv(v);
            } else {
                toolsList = true;
            }
        } else if (toolsList) {
            if (t.rfind("- ", 0) == 0)
                tools.push_back(trim(t.substr(2)));
            else if (t.empty())
                continue;
            else
                toolsList = false; // next key: handled by the loop
        }
        // Other keys (model, temperature, ...) are ignored.
    }
    if (i >= ls.size() || !nameOk || !descOk)
        return false; // no closing '---' or missing name/description: skip the file

    // Body: everything after the closing '---'.
    std::string body;
    for (size_t j = i + 1; j < ls.size(); ++j)
        body += ls[j] + "\n";

    // pi-authored files address the ledger at .pi/ledger; miniagent uses
    // .miniagent/ledger. Substitute so both spellings behave identically.
    size_t pos = 0;
    while ((pos = body.find(".pi/ledger", pos)) != std::string::npos) {
        body.replace(pos, 10, ".miniagent/ledger");
        pos += 17;
    }

    // The notes.md cap lives in one place (kNotesCharLimit); role files
    // carry the {{NOTES_LIMIT}} placeholder instead of a hardcoded number.
    body = fillNotesLimit(body);

    def.systemPrompt = trim(body);
    def.tools        = std::move(tools);
    out              = std::move(def);
    return true;
}

bool parseAgentFile(const std::string& path, AgentDef& out) {
    const std::string content = readFile(path);
    if (content.empty())
        return false;
    std::filesystem::path p(path);
    std::string nameHint = p.stem().string();
    if (parseAgentContent(nameHint, content, out)) {
        out.path = path;
        return true;
    }
    return false;
}

std::string userAgentsDir() {
    return miniagentConfigDir() + "/agents";
}

const char* builtinAgentContent(const std::string& name) {
    for (const auto& a : kBuiltinAssets)
        if (name == a.name)
            return a.content;
    return nullptr;
}

namespace {

void loadDir(const std::string& dir, std::vector<AgentDef>& out) {
    std::map<std::string, size_t> index;
    for (size_t i = 0; i < out.size(); ++i)
        index[out[i].name] = i;
    std::error_code ec;
    std::filesystem::directory_iterator it(dir, ec);
    if (ec)
        return;
    for (const auto& entry : it) {
        if (!entry.is_regular_file() && !entry.is_symlink())
            continue;
        const std::string path = entry.path().string();
        if (path.size() < 3 || path.substr(path.size() - 3) != ".md")
            continue;
        AgentDef def;
        if (!parseAgentFile(path, def))
            continue;
        auto found = index.find(def.name);
        if (found != index.end())
            out[found->second] = std::move(def); // override by name
        else {
            index[def.name] = out.size();
            out.push_back(std::move(def));
        }
    }
}

const char* const kBuiltinAgentNames[] = {"brainstorm", "worker"};

} // namespace

std::vector<AgentDef> discoverAgents(const std::string& cwd) {
    std::vector<AgentDef> out;
    for (const char* name : kBuiltinAgentNames) {
        const char* content = builtinAgentContent(name);
        if (!content)
            continue;
        AgentDef def;
        if (parseAgentContent(name, content, def))
            out.push_back(std::move(def));
    }
    if (!cwd.empty())
        loadDir(cwd + "/.miniagent/agents", out);
    loadDir(userAgentsDir(), out);
    return out;
}
