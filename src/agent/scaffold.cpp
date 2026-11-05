#include "agent/scaffold.h"

#include "agent/agents.h"
#include "agent/notes_limit.h"
#include "tools/factories.h"
#include "util/process.h"
#include "util/settings.h"
#include "util/speech.h"

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <ctime>
#include <thread>
#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

constexpr int kMaxRounds = 15;

void mkdirP(const std::string& path) {
    std::string cur;
    for (char c : path) {
        cur += c;
        if (c == '/' && cur.size() > 1)
            ::mkdir(cur.c_str(), 0755);
    }
    ::mkdir(path.c_str(), 0755);
}

// Recursive directory removal (best effort; failure just gets redone).
void removeDir(const std::string& path) {
    DIR* d = ::opendir(path.c_str());
    if (!d)
        return;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        std::string child = path + "/" + name;
        struct stat st;
        if (lstat(child.c_str(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            removeDir(child);
        else
            ::unlink(child.c_str());
    }
    ::closedir(d);
    ::rmdir(path.c_str());
}


std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f)
        return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::trunc);
    if (!f)
        return false;
    f << content;
    return (bool)f;
}

int countLines(const std::string& s) {
    int n = 0;
    for (char c : s)
        if (c == '\n')
            ++n;
    return n;
}

std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::string normalizeTaskText(std::string s) {
    for (char& c : s)
        c = (char)std::tolower((unsigned char)c);
    std::string collapsed;
    bool ws = true;
    for (char c : s) {
        if (std::isspace((unsigned char)c)) {
            ws = true;
            continue;
        }
        if (ws && !collapsed.empty())
            collapsed += ' ';
        ws = false;
        collapsed += c;
    }
    const std::string polite = "please ";
    while (collapsed.rfind(polite, 0) == 0)
        collapsed.erase(0, polite.size());
    while (!collapsed.empty() &&
           (collapsed.back() == '.' || collapsed.back() == '!' || collapsed.back() == '?'))
        collapsed.pop_back();
    return collapsed;
}

std::string tailLines(const std::string& out, int maxLines, size_t maxChars) {
    std::vector<std::string> ls;
    std::string cur;
    for (char c : out) {
        if (c == '\n') {
            ls.push_back(cur);
            cur.clear();
        } else if (c != '\r') {
            cur += c;
        }
    }
    if (!cur.empty())
        ls.push_back(cur);
    if ((int)ls.size() > maxLines)
        ls.erase(ls.begin(), ls.end() - maxLines);
    std::string tail;
    for (const auto& l : ls) {
        if (!tail.empty())
            tail += '\n';
        tail += l;
    }
    if (tail.size() > maxChars)
        tail = tail.substr(tail.size() - maxChars);
    return tail;
}

// Manager setup: writes plan.md and seeds tasks.json. Only the ledger is
// written, never the solution.
const char* const kManagerSetupTask =
    "Set up the ledger for the task in .miniagent/ledger/task.md. "
    "If .miniagent/ledger/context.md exists, read it first with the read tool (not bash): it is the "
    "conversation history that led to this task and may contain decisions, "
    "constraints and prior work relevant to the plan. "
    "Write plan.md: a 3-6 sentence strategy plus the approach you intend, under "
    "5,000 characters, ending with exactly one pinned line: `Verification: "
    "<exact command>` - the ONE "
    "build/test command that proves the work is done (write `none found` only if "
    "there is truly nothing to run). "
    "Then write tasks.json: a seed list of 3-6 concrete sub-tasks as JSON: "
    "[{\"id\": 1, \"desc\": \"...\", \"status\": \"pending\", \"result\": null}]. "
    "Number the sub-tasks in intended execution order: the ids are the plan's "
    "sequence (id 1 runs first), not a menu of equals. "
    "Only write files inside .miniagent/ledger/. Do not implement the task.";

const char* const kManagerPreamble =
    "You are the manager role of a manager-worker scaffold. You coordinate through "
    "the shared on-disk ledger at .miniagent/ledger/ (relative to the project root): "
    "task.md (the original task), context.md (the prior conversation that led "
    "to the task, if present), plan.md (strategy), tasks.json (task list "
    "[{id, desc, status, result}]) and notes.md (accumulated findings, capped at "
    "{{NOTES_LIMIT}} characters). The other "
    "roles (brainstorm, worker) run in fresh contexts and exchange "
    "information only through these ledger files. You never implement the task "
    "yourself. After every worker round the harness itself runs the pinned "
    "verification command; a failing run overrides any done claim. If codegraph tools are available, use them directly "
    "before grep/read for structural questions about the codebase - where code lives and how it "
    "fits together.\n\n";

// The preamble with the notes.md cap filled in from the single source of
// truth (kNotesCharLimit), so the number cannot drift from the role files.
std::string managerPreamble() {
    return fillNotesLimit(kManagerPreamble);
}

// Parse the manager's decision. The prompt asks for a first line of exactly
// `TASK <id>` or `DONE`, but models wrap it in markdown, prefix it with a
// bullet, or trail it with a summary - so scan every line, strip leading
// markdown decoration, and keep the LAST decision line (the reply puts the
// decision last, and rationale lines may mention other tasks). The tokens
// must be the exact uppercase contract words: "TASKS 2, 3, 4 pending" or a
// lower-case prose reference ("task 4's PS") is not a decision. "TASK" must
// be followed by an optional #, :, - separator and a digit. Unparseable =>
// done=false, id=-1 (guard treats it as no progress).
void parseDecision(const std::string& text, bool& done, int& id) {
    done = false;
    id   = -1;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos)
            continue;
        std::string t = line.substr(b);
        size_t d = 0;
        while (d < t.size() && std::strchr("#*>-`", t[d]) != nullptr)
            ++d;
        t.erase(0, d);
        t = trim(t);
        if (t.size() < 4)
            continue;
        std::string tok = t.substr(0, 4);
        // Word boundary: the 5th character must not be an ASCII letter.
        bool wordEnd = t.size() == 4 || !std::isalpha((unsigned char)t[4]);
        if (tok == "DONE" && wordEnd) {
            done = true;
            id   = -1;
        } else if (tok == "TASK" && wordEnd) {
            size_t p = t.find_first_of("0123456789", 4);
            if (p != std::string::npos && p <= 6) {
                int v = std::atoi(t.c_str() + p);
                if (v > 0) {
                    done = false;
                    id   = v;
                }
            }
        }
    }
}

// Field of task id from tasks.json (fallback when missing/malformed).
std::string taskField(const std::string& tasksJson, int id, const char* field) {
    try {
        auto j = nlohmann::json::parse(tasksJson);
        if (!j.is_array())
            return "";
        for (const auto& t : j) {
            if (t.is_object() && t.value("id", -1) == id)
                return t.value(field, "");
        }
    } catch (...) {
    }
    return "";
}

std::string taskDesc(const std::string& tasksJson, int id) {
    return taskField(tasksJson, id, "desc");
}

bool validTaskList(const std::string& tasksJson, std::string* err = nullptr) {
    auto fail = [&err](const std::string& m) {
        if (err)
            *err = m;
        return false;
    };
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(tasksJson);
    } catch (const nlohmann::json::exception& e) {
        return fail(e.what());
    }
    if (!j.is_array())
        return fail("tasks.json is not a JSON array");
    for (const auto& t : j)
        if (!t.is_object() || !t.contains("id") || !t["id"].is_number_integer())
            return fail("tasks.json entries must be objects with an integer \"id\"");
    return true;
}

// Tasks in tasks.json that are not settled yet (any status but "done", which
// covers pending, in_progress leftovers and entries without a status). Drives
// ledgerStatus()'s continuable flag: a finished run that still owes work.
int countOpenTasks(const std::string& tasksJson) {
    try {
        auto j = nlohmann::json::parse(tasksJson);
        if (!j.is_array())
            return 0;
        int open = 0;
        for (const auto& t : j)
            if (t.is_object() && t.value("status", "") != "done")
                ++open;
        return open;
    } catch (...) {
        return 0;
    }
}

// Flip task id's status in tasks.json ("in_progress" when the scaffold
// dispatches a worker, so the ledger - not just UI state - shows which task
// is live, including across a resume). Atomic; best effort.
bool setTaskStatus(const std::string& ledger, int id, const char* status) {
    std::string json = readFile(ledger + "/tasks.json");
    try {
        auto j = nlohmann::json::parse(json);
        if (!j.is_array())
            return false;
        bool found = false;
        for (auto& t : j)
            if (t.is_object() && t.value("id", -1) == id) {
                t["status"] = status;
                found = true;
            }
        if (!found)
            return false;
        return writeFile(ledger + "/tasks.json.tmp", j.dump()) &&
               ::rename((ledger + "/tasks.json.tmp").c_str(),
                        (ledger + "/tasks.json").c_str()) == 0;
    } catch (...) {
        return false;
    }
}

int verifyTimeoutSec() {
    if (const char* env = ::getenv("MINIAGENT_VERIFY_TIMEOUT")) {
        if (*env) {
            int v = std::atoi(env);
            if (v >= 0)
                return v;
        }
    }
    const auto settings = loadSettings();
    const auto it       = settings.find("verify_timeout");
    if (it != settings.end()) {
        int v = std::atoi(it->second.c_str());
        if (v >= 0)
            return v;
    }
    return 1800;
}

bool settleTaskResult(const std::string& ledger, int id, const std::string& result) {
    std::string json = readFile(ledger + "/tasks.json");
    try {
        auto j = nlohmann::json::parse(json);
        if (!j.is_array())
            return false;
        bool found = false;
        for (auto& t : j)
            if (t.is_object() && t.value("id", -1) == id) {
                t["status"] = "done";
                t["result"] = result;
                found       = true;
            }
        if (!found)
            return false;
        return writeFile(ledger + "/tasks.json.tmp", j.dump()) &&
               ::rename((ledger + "/tasks.json.tmp").c_str(),
                        (ledger + "/tasks.json").c_str()) == 0;
    } catch (...) {
        return false;
    }
}

// Truncated excerpt of a role output for the final report.
std::string excerpt(const std::string& s, size_t max) {
    if (s.size() <= max)
        return s;
    return s.substr(0, max) + " ...";
}

// First line of a role result, trimmed short - used in interrupt reports.
std::string firstLine(const std::string& s, size_t max = 200) {
    size_t e = s.find('\n');
    if (e == std::string::npos)
        e = s.size();
    if (e > max)
        e = max;
    return s.substr(0, e);
}

std::string roleSpeech(const std::string& agent) {
    if (agent == "manager")
        return "Manager is working.";
    if (agent == "brainstorm")
        return "Brainstorm is working.";
    if (agent == "worker")
        return "Worker is working.";
    return "";
}

} // namespace

std::string Scaffold::parseVerificationCommand(const std::string& planMd) {
    std::istringstream in(planMd);
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("Verification:", 0) != 0)
            continue;
        std::string rest = trim(line.substr(line.find(':') + 1));
        if (rest.compare(0, 2, "``") == 0) {
            size_t e = rest.find("``", 2);
            rest     = e == std::string::npos ? rest.substr(2) : rest.substr(2, e - 2);
        } else if (!rest.empty() && rest[0] == '`') {
            size_t e = rest.find('`', 1);
            rest     = e == std::string::npos ? rest.substr(1) : rest.substr(1, e - 1);
        } else {
            for (const char* dash : {" \xe2\x80\x94", " \xe2\x80\x93"}) {
                size_t d = rest.find(dash);
                if (d != std::string::npos)
                    rest.resize(d);
            }
            size_t b = rest.find('`');
            if (b != std::string::npos) {
                size_t e = rest.find('`', b + 1);
                rest = e == std::string::npos ? rest.substr(b + 1)
                                              : rest.substr(b + 1, e - b - 1);
            }
        }
        rest = trim(rest);
        if (!rest.empty() && rest.back() == '.')
            rest.pop_back();
        if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"')
            rest = rest.substr(1, rest.size() - 2);
        rest = trim(rest);
        if (!rest.empty() && rest != "none found")
            return rest;
        return "";
    }
    return "";
}

int Scaffold::runVerification(const std::string& cwd,
                              const std::string& cmd,
                              std::string& tail,
                              const std::atomic<bool>& abort,
                              int timeoutSec,
                              bool* timedOut) {
    if (timedOut)
        *timedOut = false;
    ProcessOutput out =
        runShellCommand(cmd, cwd, timeoutSec > 0 ? timeoutSec * 1000 : 0, abort);
    tail = tailLines(out.output, 15, 1200);
    if (out.killed) {
        if (timedOut)
            *timedOut = out.timedOut;
        return 124;
    }
    return out.exitCode;
}

int Scaffold::mechanicalDispatch(const std::string& tasksJson,
                                 int lastTaskId,
                                 const std::string& workerOut,
                                 bool verified,
                                 std::string* settleResult) {
    if (!verified || lastTaskId < 0 || workerOut.rfind("[subagent", 0) == 0)
        return -1;
    size_t h = workerOut.find("## Task result");
    if (h == std::string::npos)
        return -1;
    size_t ls = workerOut.find('\n', h);
    if (ls == std::string::npos)
        return -1;
    size_t b = workerOut.find_first_not_of(" \t\r\n", ls);
    if (b == std::string::npos)
        return -1;
    size_t e = workerOut.find('\n', b);
    std::string line =
        trim(workerOut.substr(b, e == std::string::npos ? std::string::npos : e - b));
    bool doneWord = line.rfind("done", 0) == 0 &&
                    (line.size() == 4 || line[4] == ' ' || line[4] == '-');
    if (!doneWord)
        return -1;
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(tasksJson);
    } catch (...) {
        return -1;
    }
    if (!j.is_array())
        return -1;
    bool stamped = false;
    int next     = -1;
    for (const auto& t : j) {
        if (!t.is_object() || !t.contains("id") || !t["id"].is_number_integer())
            return -1;
        int id = t["id"].get<int>();
        if (id == lastTaskId) {
            if (t.value("status", "") != "in_progress")
                return -1;
            stamped = true;
        }
        if (t.value("status", "") == "pending" && (next < 0 || id < next))
            next = id;
    }
    if (!stamped || next < 0)
        return -1;
    if (settleResult) {
        if (line.size() > 240)
            line.resize(240);
        *settleResult = line;
    }
    return next;
}

std::string Scaffold::taskSlug(const std::string& task) {
    std::string slug;
    for (char c : task) {
        if (slug.size() >= 40)
            break;
        if (c == '/' || c == '\\')
            c = ' ';
        if (std::isalnum((unsigned char)c))
            slug += (char)tolower((unsigned char)c);
        else if (c == ' ')
            slug += '-';
        else if (c == '-' || c == '_')
            slug += c;
    }
    while (!slug.empty() && !std::isalnum((unsigned char)slug.back()))
        slug.pop_back();
    return slug.empty() ? "task" : slug;
}

std::string Scaffold::sessionFile(const std::string& task) const {
    return cwd_ + "/.miniagent/sessions/" + taskSlug(task) + ".md";
}

Scaffold::Scaffold(std::string cwd,
                   OpenAIProvider provider,
                   ToolRegistry* tools,
                   const std::function<void(const AgentEvent&)>& onEvent,
                   const std::function<void(const AgentMessage&)>& onMessage,
                   std::atomic<bool>* manualCompact)
    : cwd_(std::move(cwd)),
      provider_(std::move(provider)),
      tools_(tools),
      onEvent_(onEvent),
      onMessage_(onMessage),
      manualCompact_(manualCompact) {}

Scaffold::State Scaffold::loadStateFrom(const std::string& ledgerDir) {
    State s;
    try {
        auto j = nlohmann::json::parse(readFile(ledgerDir + "/state.json"));
        if (!j.is_object())
            return s;
        std::string phase = j.value("phase", "");
        const char* const kPhases[] = {"setup", "brainstorm", "curate", "worker",
                                       "finalizer", "done"};
        for (const char* p : kPhases)
            if (phase == p)
                s.phase = phase;
        if (s.phase.empty())
            return s; // unknown phase: not resumable
        s.round        = j.value("round", 1);
        if (s.round < 1) s.round = 1;
        s.lastTaskId   = j.value("lastTaskId", -2);
        s.signedOff    = j.value("signedOff", false);
    } catch (...) {
        return State{};
    }
    return s;
}

Scaffold::LedgerStatus Scaffold::ledgerStatus(const std::string& cwd) {
    const std::string ledger = cwd + "/.miniagent/ledger";
    LedgerStatus st;
    struct stat sb;
    st.exists = ::stat(ledger.c_str(), &sb) == 0;
    const State saved = loadStateFrom(ledger);
    st.phase = saved.phase;
    st.task  = trim(readFile(ledger + "/task.md"));
    st.finished  = st.phase == "done";
    // Same condition run() uses to take the resume path: a saved phase that
    // is neither empty (no/bad state.json) nor "done", plus the task text
    // the resume match compares against.
    st.resumable = !st.phase.empty() && !st.finished && !st.task.empty();
    // A run that ended without the manager's `DONE` sign-off while tasks were
    // still open ran out of rounds: the work is not over, only the budget. The
    // UI offers Continue (Start::ContinueLedger) for exactly that.
    st.openTasks   = countOpenTasks(readFile(ledger + "/tasks.json"));
    st.signedOff   = saved.signedOff;
    st.continuable = st.finished && !st.signedOff && st.openTasks > 0 &&
                     !st.task.empty();
    return st;
}

void Scaffold::discardLedger(const std::string& cwd) {
    removeDir(cwd + "/.miniagent/ledger");
}

bool Scaffold::saveState(const State& s) const {
    nlohmann::json j{{"phase", s.phase},
                     {"round", s.round},
                     {"lastTaskId", s.lastTaskId},
                     {"signedOff", s.signedOff}};
    // Atomic: write to a temp file, then rename over state.json, so an
    // interrupted process never leaves a torn state file behind.
    const std::string path = ledgerDir() + "/state.json";
    if (!writeFile(path + ".tmp", j.dump()))
        return false;
    return ::rename((path + ".tmp").c_str(), path.c_str()) == 0;
}

ToolResult Scaffold::role(const std::string& agent,
                         const std::string& task,
                         const std::atomic<bool>& abort,
                         const std::string& label) {
    speak(roleSpeech(agent));
    if (!onEvent_)
        return ToolResult{"", false};
    // The manager is a built-in role, not discovered; brainstorm/worker
    // come from role files so user overrides apply.
    AgentDef def;
    if (agent == "manager") {
        def.name         = "manager";
        def.description  = "scaffold manager";
        def.systemPrompt = managerPreamble();
    } else {
        bool found = false;
        for (const AgentDef& a : discoverAgents(cwd_))
            if (a.name == agent) {
                def   = a;
                found = true;
                break;
            }
        if (!found)
            return ToolResult{"Unknown scaffold role: " + agent, true};
    }

    // Automatic retry: a transient failure (network hiccup, rate limit,
    // crashed subagent) should not burn the whole run. Retry a few times
    // with a short backoff; only a persistent failure is reported upward,
    // where the resume logic picks it up.
    constexpr int kMaxAttempts = 3;
    ToolResult r{"", false};
    for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        r = runSubagent(cwd_,
                        provider_,
                        {def},
                        agent,
                        task,
                        tools_->all(),
                        abort,
                        onEvent_,
                        label,
                        manualCompact_);
        if (!r.isError || abort.load())
            return r;
        if (attempt < kMaxAttempts) {
            const int kWaitMs = 5000;
            if (onEvent_) {
                // Nested (role-tagged): the note belongs to this role's page in
                // the chat log (the role name is the row's prefix there).
                AgentEvent ev{AgentEventType::Retry,
                              "failed (attempt " + std::to_string(attempt) + "/" +
                                  std::to_string(kMaxAttempts) + "), retrying: " +
                                  firstLine(r.content)};
                ev.nested      = true;
                ev.nestedAgent = agent;
                ev.retryAttempt     = attempt;
                ev.retryMaxAttempts = kMaxAttempts;
                ev.retryDelayMs     = kWaitMs;
                onEvent_(ev);
            }
            for (int i = 0; i < kWaitMs / 100 && !abort.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (abort.load())
                return r;
        }
    }
    return r;
}

void Scaffold::run(const std::string& task, const std::atomic<bool>& abort, Start start) {
    const std::string ledger = ledgerDir();
    auto emit = [this](AgentEvent ev) {
        if (onEvent_)
            onEvent_(ev);
    };
    emit(AgentEvent{AgentEventType::AgentStart, {}, {}, {}, {}, {}, false, 0, 0});

    // Resume detection: the ledger is the single source of truth. When it
    // holds this exact task and a resumable state.json (a run interrupted by
    // Esc or a failed role call), continue from the saved phase instead of
    // wiping and starting over. A finished ("done") or mismatched ledger
    // starts fresh - unless this is a continuation run, which keeps it.
    State st = start == Start::Fresh ? State{} : loadState();
    const std::string storedTask =
        start == Start::Fresh ? "" : readFile(ledger + "/task.md");
    bool resumed = start != Start::Fresh && !st.phase.empty() &&
                   st.phase != "done" && !storedTask.empty() &&
                   normalizeTaskText(storedTask) == normalizeTaskText(task);
    // Continuation of a finished run that ran out of rounds: the plan, the
    // task list and the findings stay exactly as the last run left them, only
    // the loop restarts (at curate, with a fresh round budget). The stored
    // task.md is kept, so the objective the roles read is unchanged and a run
    // interrupted this way still resumes normally afterwards.
    bool continued = false;
    if (!resumed && start == Start::ContinueLedger &&
        ::access((ledger + "/tasks.json").c_str(), F_OK) == 0) {
        continued = true;
        if (storedTask.empty())
            writeFile(ledger + "/task.md", task + "\n");
        st       = State{};
        st.phase = "curate";
    }
    if (!resumed && !continued) {
        // 1. Ledger setup (the manager owns plan.md / tasks.json). Remove the
        //    ledger from any earlier session first: stale files would be read
        //    as findings by the fresh-context roles.
        removeDir(ledger);
        mkdirP(ledger);
        for (const char* f : {"task.md", "notes.md", "tasks.json", "plan.md"}) {
            std::ofstream o(ledger + "/" + f, std::ios::trunc);
            if (o) {
                if (std::string(f) == "task.md")
                    o << task << "\n";
                else if (std::string(f) == "notes.md")
                    o << "# notes\n";
                else if (std::string(f) == "tasks.json")
                    o << "[]\n";
            }
        }
        // Prior chat history, when handed over from Simple mode: fresh-context
        // roles read it like any other ledger file.
        if (!context_.empty())
            writeFile(ledger + "/context.md", context_);
        st = State{};
    } else if (resumed)
        speak("Resuming scaffold from " + st.phase + ".");
    else
        speak("Continuing with the existing ledger.");
    if (st.phase.empty())
        st.phase = "setup";

    bool interrupted = false; // a role call failed (network/local), not Esc
    std::string stopReason;

    // 2. Setup + brainstorm (skipped when the saved state is past them).
    if (st.phase == "setup") {
        saveState(st);
        if (!abort.load()) {
            ToolResult r = role("manager", kManagerSetupTask, abort);
            if (!abort.load()) {
                if (r.isError) {
                    interrupted = true;
                    stopReason  = "manager setup failed: " + firstLine(r.content);
                } else {
                    std::string err;
                    if (!validTaskList(readFile(ledger + "/tasks.json"), &err)) {
                        ToolResult fix =
                            role("manager",
                                 "Your tasks.json seed is not valid JSON (" + err +
                                     "). Re-read .miniagent/ledger/tasks.json with the "
                                     "read tool (not bash) and rewrite the whole file as "
                                     "valid JSON: an array of {\"id\", \"desc\", "
                                     "\"status\", \"result\"} objects, same tasks in the "
                                     "same order, only the syntax corrected.",
                                 abort);
                        if (!abort.load()) {
                            if (fix.isError) {
                                interrupted = true;
                                stopReason  = "manager setup tasks.json fix failed: " +
                                             firstLine(fix.content);
                            } else if (!validTaskList(readFile(ledger + "/tasks.json"),
                                                      &err)) {
                                interrupted = true;
                                stopReason = "manager setup left tasks.json malformed: " + err;
                            }
                        }
                    }
                    if (!interrupted && !abort.load())
                        st.phase = "brainstorm";
                }
            }
        }
    }
    if (!interrupted && !abort.load() && st.phase == "brainstorm") {
        // Brainstorm (ideation pass, appends to notes.md). A re-run (resume)
        // appends another ideation pass - harmless, notes.md is digested by
        // the manager, not replayed verbatim.
        saveState(st);
        ToolResult r = role("brainstorm",
                           "Read the ledger in .miniagent/ledger/ with the read tool (not bash) and produce the ideation pass for the task.",
                           abort);
        if (!abort.load()) {
            if (r.isError) {
                interrupted = true;
                stopReason  = "brainstorm step failed: " + firstLine(r.content);
            } else {
                st.phase = "curate";
                st.round = 1;
                saveState(st);
            }
        }
    }

    // 3. Loop: curate -> worker.
    int lastTaskId = st.lastTaskId;
    bool signedOff = st.signedOff;
    int round = st.round;
    std::string verifyCmd = parseVerificationCommand(readFile(ledger + "/plan.md"));
    bool verifyRan = false;
    int lastVerifyExit = -1;
    int lastVerifyRound = 0;
    std::string lastVerifyTail;
    // Worker of the current round already dispatched before the interruption
    // (false = re-run curate, true = re-run only the worker). Everything is
    // idempotent: the roles are fresh contexts that re-read the ledger, and a
    // re-dispatched worker simply continues the partial work.
    bool workerDispatched = false;
    bool toFinalizer = false;
    if (resumed && st.phase == "finalizer")
        st.phase = "curate"; // re-curate instead of re-running the finalizer
    if (resumed && st.phase == "worker")
        workerDispatched = true;
    std::string workerOut;
    std::string finalizerWhy;

    for (; !toFinalizer && round <= kMaxRounds && !abort.load() && !interrupted; ++round) {
        bool skipCurate = workerDispatched;
        workerDispatched = false;

        if (!skipCurate) {
            st.phase = "curate";
            st.round = round;
            saveState(st);
            std::string fastSettle;
            const int fastId =
                (verifyRan && lastVerifyExit == 0)
                    ? mechanicalDispatch(readFile(ledger + "/tasks.json"), lastTaskId,
                                         workerOut, true, &fastSettle)
                    : -1;
            if (fastId >= 0) {
                settleTaskResult(ledger, lastTaskId, fastSettle);
                lastTaskId = fastId;
            } else {
                std::string curateTask =
                    "Round " + std::to_string(round) + " of " + std::to_string(kMaxRounds) + ". "
                    "Read the ledger files (.miniagent/ledger/) fresh with the read tool (not bash) - the ledger, "
                    "not memory, is the source of truth. First curate tasks.json: mark "
                    "finished tasks done with a one-line result, merge duplicates, add "
                    "only genuinely new sub-tasks from the latest worker "
                    "findings in notes.md. A task still marked `in_progress` is the one "
                    "the harness dispatched last round (it stamps the status at "
                    "dispatch): settle it to done or pending before deciding. Then "
                    "decide the single next action. Your "
                    "final reply must start with exactly one of: `TASK <id>` - the "
                    "next task to dispatch (the id must exist in tasks.json), "
                    "or `DONE` - only if the task is complete. Follow with one or two "
                    "lines of rationale. Task ids are an execution order, not a menu: "
                    "default to the lowest-numbered pending task and pick a later id "
                    "only when every earlier pending task is blocked on it or already "
                    "settled by findings - say so in the rationale, and mark settled "
                    "tasks done during curation.";
                if (continued) {
                    curateTask +=
                        " This run continues an earlier one that used up its round budget: "
                        "tasks.json already holds that run's finished and open tasks, and "
                        "plan.md still holds the strategy. Do not re-plan from scratch - "
                        "curate as usual and dispatch the lowest-numbered open task. Reply "
                        "DONE only if every open task is genuinely settled by findings.";
                    continued = false; // only the first curation of this run needs it
                }
                if (verifyRan) {
                    curateTask +=
                        " Verification ground truth (harness-run, round " +
                        std::to_string(lastVerifyRound) + ": `" + verifyCmd + "` -> exit " +
                        std::to_string(lastVerifyExit) + " (" +
                        (lastVerifyExit == 0 ? "pass" : "FAIL") + ")." +
                        (lastVerifyTail.empty() ? ""
                                                : " Last output lines:\n" + lastVerifyTail) +
                        " The exit status overrides any worker's done claim: while the "
                        "last run failed you may not reply DONE - name a fix-or-switch "
                        "task instead.";
                }
                ToolResult curate = role("manager", curateTask, abort);
                if (abort.load() || interrupted)
                    break;
                if (curate.isError) {
                    interrupted = true;
                    stopReason  = "manager curation failed: " + firstLine(curate.content);
                    break;
                }
                std::string jerr;
                if (!validTaskList(readFile(ledger + "/tasks.json"), &jerr)) {
                    ToolResult fix =
                        role("manager",
                             "Your tasks.json curation is not valid JSON (" + jerr + "). "
                             "Re-read .miniagent/ledger/tasks.json with the read tool (not "
                             "bash) and rewrite the whole file as valid JSON - same tasks, "
                             "ids, statuses and results, only the syntax corrected. Then "
                             "reply again with your decision: a first line that is exactly "
                             "`TASK <id>` or `DONE` (plain text, nothing else on the line), "
                             "then one line of rationale.",
                             abort);
                    if (abort.load())
                        break;
                    if (fix.isError) {
                        interrupted = true;
                        stopReason  = "manager tasks.json fix re-ask failed: " + firstLine(fix.content);
                        break;
                    }
                    if (!validTaskList(readFile(ledger + "/tasks.json"), &jerr)) {
                        interrupted = true;
                        stopReason  = "manager left tasks.json malformed: " + jerr;
                        break;
                    }
                    curate = fix;
                }
                bool done = false;
                int    id = -1;
                parseDecision(curate.content, done, id);
                // A re-dispatch of the same task is only a stuck-manager signal
                // when the task is already marked done. A still-pending task was
                // simply not finished by the previous worker - continuing it is
                // legitimate progress (same as the resume path).
                auto rejected = [&](int i) {
                    return i == lastTaskId &&
                           taskField(readFile(ledger + "/tasks.json"), i, "status") == "done";
                };
                if (!done && (id < 0 || rejected(id))) {
                    // One corrective re-ask before giving up: a formatting wobble
                    // in the decision line should not cost a finalizer - the retry
                    // demands exactly the format the parser accepts.
                    ToolResult r2 =
                        role("manager",
                             "Your last reply did not contain a parseable decision. Re-read "
                             "the ledger and reply with a first line that is exactly `TASK <id>` "
                             "or `DONE` (plain text, no markdown, nothing else on the line), "
                             "then one line of rationale.",
                             abort);
                    if (!abort.load() && !r2.isError) {
                        bool d2 = false;
                        int  i2 = -1;
                        parseDecision(r2.content, d2, i2);
                        if (d2) {
                            signedOff = true;
                            break; // clean sign-off
                        }
                        if (i2 >= 0 && !rejected(i2))
                            id = i2;
                    }
                }
                if (done && verifyRan && lastVerifyExit != 0) {
                    ToolResult r2 =
                        role("manager",
                             "The pinned verification command is failing (exit " +
                                 std::to_string(lastVerifyExit) +
                                 "), so DONE is not available. Re-read the ledger and "
                                 "reply with a first line that is exactly `TASK <id>` "
                                 "naming a fix-or-switch task, then one line of rationale.",
                             abort);
                    if (abort.load())
                        break;
                    if (r2.isError) {
                        interrupted = true;
                        stopReason = "manager verify-override re-ask failed: " + firstLine(r2.content);
                        break;
                    }
                    bool d2 = false;
                    int  i2 = -1;
                    parseDecision(r2.content, d2, i2);
                    if (d2 || i2 < 0 || rejected(i2)) {
                        finalizerWhy = "round " + std::to_string(round) +
                                       ": manager signed off while verification fails (exit " +
                                       std::to_string(lastVerifyExit) + ")";
                        break;
                    }
                    done = false;
                    id   = i2;
                }
                if (done) {
                    signedOff = true;
                    break; // clean sign-off
                }
                if (id < 0) {
                    finalizerWhy = "round " + std::to_string(round) +
                                   ": manager decision unparseable (final reply: '" +
                                   firstLine(curate.content, 120) + "')";
                    break;
                }
                if (rejected(id)) {
                    finalizerWhy = "round " + std::to_string(round) +
                                   ": manager re-dispatched finished task " +
                                   std::to_string(id) + " (no progress)";
                    break;
                }
                lastTaskId = id;
            }
        }
        st.lastTaskId = lastTaskId;
        currentTaskId_ = lastTaskId;
        // Stamp the ledger, not just the UI: tasks.json shows which task is
        // live (and survives a resume/interruption). The manager curates it
        // back to done/pending in the next round; a leftover in_progress is
        // what the UI flags as a stale task.
        setTaskStatus(ledger, lastTaskId, "in_progress");
        speak("Task " + std::to_string(lastTaskId) + " started.");

        {
            st.phase = "worker";
            st.round = round;
            saveState(st);
            std::string desc = taskDesc(readFile(ledger + "/tasks.json"), lastTaskId);
            char wTask[1024];
            snprintf(wTask,
                     sizeof(wTask),
                     "Round %d, task id %d: '%s'. Read the ledger first (.miniagent/ledger/) "
                     "with the read tool, not bash.",
                     round,
                     lastTaskId,
                     desc.c_str());
            ToolResult w = role("worker", wTask, abort);
            if (abort.load())
                break;
            if (w.isError) {
                interrupted = true;
                stopReason  = "worker round " + std::to_string(round) + " failed: " + firstLine(w.content);
                break;
            }
            workerOut = w.content;

            // Cut-off summarizer: a stopped worker costs a round, not the
            // answer - get its partial state down first (best effort).
            if (workerOut.rfind("[subagent", 0) == 0) {
                role("worker",
                     "Summarize partial work: read the ledger and the files changed in the "
                     "last round, return a compact summary of what exists now.",
                     abort);
                if (abort.load())
                    break;
            }

            if (!verifyCmd.empty()) {
                lastVerifyExit  = runVerification(cwd_, verifyCmd, lastVerifyTail,
                                                  abort, verifyTimeoutSec());
                lastVerifyRound = round;
                verifyRan       = true;
            }

            // currentTaskId_ deliberately stays on lastTaskId here: the task
            // is still in play while the next round's manager curates it into
            // tasks.json. Clearing it at the worker's return made the row fall
            // back to plain pending grey ("as if it never started") for the
            // whole curation turn. The next dispatch overwrites it; the
            // finalizer and the end of the run clear it.

            // Next round's curate is the next safe resume point; compaction
            // runs after the state save so a failed one just re-curates.
            st.round      = round + 1;
            st.signedOff   = signedOff;
            st.lastTaskId  = lastTaskId;
            st.phase       = "curate";
            saveState(st);

            // Compaction: keep notes.md bounded.
            if (countLines(readFile(ledger + "/notes.md")) > 300) {
                const std::string limit = notesCharLimitText();
                ToolResult c =
                    role("worker",
                         "Compaction: notes.md has grown past 300 lines - a worker "
                         "skipped its curation duty. Rewrite "
                         ".miniagent/ledger/notes.md as a curated whole: merge "
                         "duplicates, drop dead ends already reflected in "
                         "tasks.json, keep invariants, decisions and open "
                         "questions verbatim, and keep the file under " + limit +
                         " characters.",
                         abort);
                if (abort.load())
                    break;
                if (c.isError) {
                    interrupted = true;
                    stopReason  = "compaction failed: " + firstLine(c.content);
                    break;
                }
            }
        }
    }

    // 4. Finalizer: only when the loop did not end with a clean sign-off
    //    and the run was not interrupted (an interrupted run resumes later).
    if (!toFinalizer && !signedOff && !abort.load() && !interrupted) {
        toFinalizer  = true;
        finalizerWhy = finalizerWhy.empty() ? "round budget (" + std::to_string(kMaxRounds) +
                                                ") exhausted"
                                           : finalizerWhy;
    }
    if (toFinalizer && !abort.load() && !interrupted) {
        currentTaskId_ = -1;
        st.phase       = "finalizer";
        saveState(st);
        speak("No clean sign-off; starting finalizer.");
        ToolResult fw =
            role(
                "worker",
                "Read the ledger (.miniagent/ledger/) with the read tool, not bash. The run has no clean sign-off; "
                "finish the task in the best verifiable way, and report. If your work "
                "completes a task listed in tasks.json, mark it done in that file with "
                "a one-line result (keep the JSON valid).",
                abort,
                "finalizer");
        if (!abort.load()) {
            if (fw.isError) {
                interrupted = true;
                stopReason  = "finalizer failed: " + firstLine(fw.content);
            } else {
                workerOut = fw.content;
                if (!verifyCmd.empty()) {
                    lastVerifyExit  = runVerification(cwd_, verifyCmd, lastVerifyTail,
                                                      abort, verifyTimeoutSec());
                    lastVerifyRound = round;
                    verifyRan       = true;
                }
            }
        }
    }

    if (!interrupted && !abort.load()) {
        speak("Scaffold finished.");
        State fin    = st;
        fin.phase    = "done";
        fin.signedOff = signedOff;
        saveState(fin);
    }

    // Final report (a normal, non-nested turn so it reads as the run's answer).
    std::string verifyNote =
        verifyRan ? " Last verification `" + verifyCmd + "` -> exit " +
                       std::to_string(lastVerifyExit) + "."
                  : "";
    // Open tasks after the finalizer: the round budget, not the work, is what
    // ran out. Say so, because that is exactly what the input bar's Continue
    // button (ledgerStatus().continuable) offers a run later.
    const int openTasks = countOpenTasks(readFile(ledger + "/tasks.json"));
    std::string openNote =
        (signedOff || openTasks <= 0)
            ? ""
            : "\n\n" + std::to_string(openTasks) +
                  " task(s) in .miniagent/ledger/tasks.json are still open - the round "
                  "budget (" + std::to_string(kMaxRounds) + " rounds) ran out, not the "
                  "work. Press Continue to run another round budget on this ledger.";
    std::string report;
    if (interrupted) {
        report = "Scaffold interrupted (" + stopReason +
                 "). Progress is saved in .miniagent/ledger/ - rerun the same task to "
                 "continue where it left off.";
    } else if (abort.load()) {
        report = "Scaffold aborted. Progress is saved in .miniagent/ledger/ - rerun the "
                 "same task to continue where it left off.";
    } else if (signedOff) {
        report = "Scaffold finished with a clean sign-off: the manager confirmed the "
                 "task complete." + verifyNote + " Changes live in the repo; the full "
                 "trail is in .miniagent/ledger/.";
    } else {
        report = "Scaffold finished without a clean sign-off (finalizer: " +
                 finalizerWhy + ")." + verifyNote + openNote +
                 " Final worker report:\n\n" + excerpt(workerOut, 4000);
    }
    // Save the run's result to .miniagent/sessions/<task-slug>.md
    // so a long run leaves a durable, task-named record (best effort). A
    // same-named earlier session gets a -2/-3/... suffix instead of being
    // overwritten.
    mkdirP(cwd_ + "/.miniagent/sessions");
    {
        std::string file = sessionFile(task);
        for (int i = 2; ::access(file.c_str(), F_OK) == 0; ++i)
            file = sessionFile(task) + "-" + std::to_string(i);
        time_t now = ::time(nullptr);
        std::string stamp = std::ctime(&now);
        if (!stamp.empty() && stamp.back() == '\n')
            stamp.pop_back();
        std::string outcome = interrupted ? ("interrupted (" + stopReason + ")")
                           : abort.load()  ? "aborted (progress saved)"
                           : signedOff      ? "clean sign-off"
                           : openTasks > 0  ? "no clean sign-off (" +
                                                std::to_string(openTasks) +
                                                " task(s) still open)"
                                            : "no clean sign-off";
        if (writeFile(
                file,
                "# Scaffold session\n\n- Task: " + task + "\n- Date: " + stamp +
                    "\n- Outcome: " + outcome + "\n\n## Final report\n\n" +
                    report + "\n"))
            report += "\n\n(Session result saved to " + file.substr(cwd_.size() + 1) + ")";
    }

    emit(AgentEvent{AgentEventType::TurnStart, {}, {}, {}, {}, {}, false, 1, 0});
    emit(AgentEvent{AgentEventType::TextDelta, report, {}, {}, {}, {}, false, 1, 0});
    if (onMessage_) {
        AssistantMessage a;
        a.text = report;
        onMessage_(a);
    }
    emit(AgentEvent{AgentEventType::TurnEnd, {}, {}, {}, {}, {}, false, 1, 0});
    emit(AgentEvent{AgentEventType::AgentEnd, {}, {}, {}, {}, {}, false, 0, 0});
}
