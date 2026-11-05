#include "agent/skills.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace {

// Depth cap for the SKILL.md scan: also the symlink-loop guard, since
// std::filesystem follows directory symlinks.
constexpr int kMaxDepth = 8;
// Per Agent Skills spec.
constexpr size_t kMaxNameLength        = 64;
constexpr size_t kMaxDescriptionLength = 1024;

std::vector<std::string> splitLines(const std::string& s) {
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

std::string trimEnd(const std::string& s) {
    size_t e = s.find_last_not_of(" \t\r\n");
    return e == std::string::npos ? "" : s.substr(0, e + 1);
}

// Strips the enclosing quotes pi's frontmatter may carry ("name: \"x\"").
std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
        return v.substr(1, v.size() - 2);
    return v;
}

bool boolValue(const std::string& v) {
    return v == "true" || v == "yes" || v == "1";
}

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        ok = false;
        return "";
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    ok = true;
    return ss.str();
}

// Name rules per Agent Skills spec: lowercase a-z, 0-9 and hyphens, at most 64
// characters, no leading/trailing/consecutive hyphen.
std::vector<std::string> validateName(const std::string& name) {
    std::vector<std::string> errors;
    if (name.empty()) {
        errors.push_back("name is required");
        return errors;
    }
    if (name.size() > kMaxNameLength)
        errors.push_back("name exceeds " + std::to_string(kMaxNameLength) + " characters (" +
                         std::to_string(name.size()) + ")");
    for (char c : name)
        if (!(std::islower((unsigned char)c) || std::isdigit((unsigned char)c) || c == '-')) {
            errors.push_back("name contains invalid characters (must be lowercase a-z, 0-9, hyphens only)");
            break;
        }
    if (!name.empty() && (name.front() == '-' || name.back() == '-'))
        errors.push_back("name must not start or end with a hyphen");
    if (name.find("--") != std::string::npos)
        errors.push_back("name must not contain consecutive hyphens");
    return errors;
}

std::vector<std::string> validateDescription(const std::string& desc) {
    std::vector<std::string> errors;
    if (desc.empty())
        errors.push_back("description is required");
    else if (desc.size() > kMaxDescriptionLength)
        errors.push_back("description exceeds " + std::to_string(kMaxDescriptionLength) + " characters (" +
                         std::to_string(desc.size()) + ")");
    return errors;
}

std::string escapeXml(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string fileName(const std::string& path) {
    return fs::path(path).filename().string();
}

std::string stem(const std::string& path) {
    return fs::path(path).stem().string();
}

// Loads one skill file (SKILL.md or a standalone .md) into `out`.
void loadSkillFile(const std::string& path, SkillsResult& out) {
    bool ok = false;
    const std::string content = readFile(path, ok);
    if (!ok) {
        out.diagnostics.push_back({"could not read skill file", path});
        return;
    }
    // A SKILL.md is named after its directory; a standalone .md after its stem.
    const std::string nameHint = fileName(path) == "SKILL.md" ? fileName(fs::path(path).parent_path().string())
                                                             : stem(path);
    Skill skill;
    std::vector<std::string> warnings;
    if (!parseSkillContent(nameHint, content, skill, warnings)) {
        for (auto& w : warnings)
            out.diagnostics.push_back({w, path});
        if (warnings.empty())
            out.diagnostics.push_back({"not a skill: no usable frontmatter", path});
        return;
    }
    skill.filePath = fs::absolute(path).lexically_normal().string();
    skill.baseDir  = fs::path(skill.filePath).parent_path().string();
    for (auto& w : warnings)
        out.diagnostics.push_back({w, path});
    out.skills.push_back(std::move(skill));
}

bool isSkillMd(const std::string& path) {
    return fileName(path) == "SKILL.md";
}

void scanDir(const std::string& dir, SkillsResult& out, bool includeRootFiles, int depth) {
    std::error_code ec;
    if (depth > kMaxDepth || !fs::is_directory(dir, ec))
        return;

    // A directory holding SKILL.md is a skill root: nothing below it is scanned.
    const std::string skillMd = (fs::path(dir) / "SKILL.md").string();
    if (fs::is_regular_file(skillMd, ec)) {
        loadSkillFile(skillMd, out);
        return;
    }

    fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    if (ec)
        return;
    std::vector<std::string> files, dirs;
    for (const auto& entry : it) {
        const std::string name = fileName(entry.path().string());
        if (name.empty() || name.front() == '.' || name == "node_modules")
            continue;
        const std::string path = entry.path().string();
        std::error_code sec;
        if (fs::is_directory(path, sec))
            dirs.push_back(path);
        else if (fs::is_regular_file(path, sec) && name.size() > 3 && name.substr(name.size() - 3) == ".md")
            files.push_back(path);
    }
    // Sorted so name collisions resolve the same way on every run.
    std::sort(files.begin(), files.end());
    std::sort(dirs.begin(), dirs.end());
    if (includeRootFiles)
        for (const auto& f : files)
            loadSkillFile(f, out);
    for (const auto& d : dirs)
        scanDir(d, out, false, depth + 1);
}

} // namespace

bool parseSkillContent(const std::string& nameHint,
                       const std::string& content,
                       Skill& out,
                       std::vector<std::string>& warnings) {
    const std::vector<std::string> ls = splitLines(content);
    size_t first = 0;
    while (first < ls.size() && trim(ls[first]).empty())
        ++first;
    if (first >= ls.size() || trim(ls[first]) != "---") {
        warnings.push_back("no frontmatter block");
        return false;
    }

    Skill skill;
    bool nameOk = false;
    bool descOk = false;
    bool closed  = false;
    size_t i    = first + 1;
    for (; i < ls.size(); ++i) {
        const std::string t = trim(ls[i]);
        if (t == "---") {
            closed = true;
            break;
        }
        if (t.empty() || t[0] == '#')
            continue;
        const size_t colon = t.find(':');
        if (colon == std::string::npos)
            continue;
        const std::string key   = trim(t.substr(0, colon));
        const std::string value = unquote(t.substr(colon + 1));
        if (key == "name") {
            skill.name = value;
            nameOk     = !value.empty();
        } else if (key == "description") {
            skill.description = value;
            descOk            = !value.empty();
        } else if (key == "disable-model-invocation") {
            skill.disableModelInvocation = boolValue(value);
        }
        // Other keys (license, compatibility, metadata, allowed-tools) are
        // part of the Agent Skills spec but unused here; ignored.
    }
    if (!closed) {
        warnings.push_back("frontmatter is not closed with ---");
        return false;
    }

    std::string body;
    for (size_t j = i + 1; j < ls.size(); ++j)
        body += ls[j] + "\n";
    body = trimEnd(body);

    if (!nameOk)
        skill.name = nameHint;
    for (const auto& e : validateName(skill.name))
        warnings.push_back(e);
    for (const auto& e : validateDescription(skill.description))
        warnings.push_back(e);
    // A missing description kills the routing signal, so the skill is dropped;
    // everything else is a warning and the skill still loads.
    if (!descOk)
        return false;
    if (skill.name.empty())
        return false;

    skill.body = std::move(body);
    out        = std::move(skill);
    return true;
}

std::string userSkillsDir() {
    const char* home = std::getenv("HOME");
    if (!home || !*home)
        home = ".";
    return std::string(home) + "/.config/miniagent/skills";
}

void loadSkillsFromDir(const std::string& dir, SkillsResult& out) {
    scanDir(dir, out, true, 0);
}

SkillsResult loadSkills(const std::string& cwd, const std::string& userDir) {
    SkillsResult user, project, out;
    loadSkillsFromDir(userDir, user);
    loadSkillsFromDir(cwd + "/.miniagent/skills", project);

    std::unordered_set<std::string> names, realPaths;
    // User first, then project: the first skill with a name wins, so a
    // user-level skill shadows a same-named project one (pi's precedence).
    for (SkillsResult* src : {&user, &project}) {
        for (auto& d : src->diagnostics)
            out.diagnostics.push_back(std::move(d));
        for (auto& skill : src->skills) {
            std::error_code ec;
            std::string realPath = fs::canonical(skill.filePath, ec).string();
            if (!ec && realPaths.count(realPath))
                continue; // same file reached through a symlink
            if (names.count(skill.name)) {
                out.diagnostics.push_back({"name \"" + skill.name + "\" collides with an earlier skill",
                                           skill.filePath});
                continue;
            }
            names.insert(skill.name);
            if (!ec)
                realPaths.insert(realPath);
            out.skills.push_back(std::move(skill));
        }
    }
    return out;
}

std::string formatSkillsForPrompt(const std::vector<Skill>& skills) {
    std::vector<const Skill*> visible;
    for (const auto& s : skills)
        if (!s.disableModelInvocation)
            visible.push_back(&s);
    if (visible.empty())
        return "";

    std::string s =
        "\n\nThe following skills provide specialized instructions for specific tasks.\n"
        "Use the read tool to load a skill's file when the task matches its description; the file "
        "holds the full instructions.\n"
        "When a skill file references a relative path, resolve it against the skill directory (the "
        "parent of its SKILL.md) and use that absolute path in tool commands.\n\n"
        "<available_skills>\n";
    for (const Skill* sk : visible) {
        s += "  <skill>\n    <name>" + escapeXml(sk->name) + "</name>\n    <description>" +
             escapeXml(sk->description) + "</description>\n    <location>" + escapeXml(sk->filePath) +
             "</location>\n  </skill>\n";
    }
    s += "</available_skills>\n";
    return s;
}

std::string expandSkillCommand(const std::vector<Skill>& skills, const std::string& text) {
    if (text.rfind("/skill:", 0) != 0)
        return "";
    const std::string rest = text.substr(7);
    const size_t space     = rest.find(' ');
    const std::string name = space == std::string::npos ? rest : rest.substr(0, space);
    std::string args       = space == std::string::npos ? "" : trim(rest.substr(space + 1));
    if (name.empty())
        return "";

    const Skill* match = nullptr;
    for (const auto& s : skills)
        if (s.name == name) {
            match = &s;
            break;
        }
    if (!match)
        return "";

    std::string block = "<skill name=\"" + match->name + "\" location=\"" + match->filePath + "\">\n"
                        "References are relative to " + match->baseDir + ".\n\n" + match->body + "\n</skill>";
    if (!args.empty())
        block += "\n\n" + args;
    return block;
}
