// Unit tests for the read and edit tools (tools/read.cpp, tools/edit.cpp).
// Regression tests for the "failed edit tool calls" issue:
//   - read prefixes lines with "NNNNN→" (no trailing space) so copying a
//     line's content can't pick up a phantom leading space
//   - edit's "oldText not found" error detects the per-line extra leading
//     space artifact and tells the model to retry with real indentation
//   - edit's "matches N times" error lists the line numbers of the matches
#include "tools/factories.h"

#include "stb_image_write.h"
#include "util/path.h"

#include <sstream>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;

static void check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

namespace {

std::atomic<bool> noAbort{false};
const char* kFile = "/tmp/tool_test/main.cpp";

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), content.size());
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Build tool args explicitly (initializer-list nesting is ambiguous in nlohmann).
nlohmann::json editArgs(const std::string& path,
                        const std::vector<std::pair<std::string, std::string>>& edits) {
    nlohmann::json a = nlohmann::json::object();
    a["path"] = path;
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : edits) {
        nlohmann::json o = nlohmann::json::object();
        o["oldText"] = e.first;
        o["newText"] = e.second;
        arr.push_back(o);
    }
    a["edits"] = arr;
    return a;
}

nlohmann::json readArgs(const std::string& path, long long offset = 0, long long limit = -1) {
    nlohmann::json a = nlohmann::json::object();
    a["path"] = path;
    if (offset > 0)
        a["offset"] = offset;
    if (limit >= 0)
        a["limit"] = limit;
    return a;
}

nlohmann::json grepArgs(const std::string& pattern,
                         const std::string& path = "",
                         int limit = -1,
                         int timeout = -1) {
    nlohmann::json a = nlohmann::json::object();
    a["pattern"] = pattern;
    if (!path.empty())
        a["path"] = path;
    if (limit >= 0)
        a["limit"] = limit;
    if (timeout >= 0)
        a["timeout"] = timeout;
    return a;
}

} // namespace

int main() {
    std::system("rm -rf /tmp/tool_test && mkdir -p /tmp/tool_test");

    ToolRegistry tools;
    tools.add(makeReadTool("/tmp/tool_test"));
    tools.add(makeWriteTool("/tmp/tool_test"));
    tools.add(makeEditTool("/tmp/tool_test"));
    tools.add(makeGrepTool("/tmp/tool_test"));

    { // --- read: line prefix format ---
        writeFile(kFile, "alpha\nbeta\n\n");
        auto r = tools.execute("read", readArgs(kFile), noAbort);
        check(!r.isError, "read: ok");
        check(r.content.find("1→alpha") != std::string::npos, "read: prefix has no trailing space");
        check(r.content.find("2→beta") != std::string::npos, "read: line 2 prefix");
        check(r.content.find("3→\n") != std::string::npos, "read: empty line renders bare prefix");
        check(r.content.find("→ ") == std::string::npos, "read: no '→ ' (space after arrow) anywhere");
    }
    { // --- read: offset/limit ---
        writeFile(kFile, "l1\nl2\nl3\nl4\n");
        auto r = tools.execute("read", readArgs(kFile, 2, 2), noAbort);
        check(r.content == "    2→l2\n    3→l3\n", "read: offset/limit slice");
        auto bad = tools.execute("read", readArgs(kFile, 99), noAbort);
        check(bad.isError && bad.content.find("past end") != std::string::npos, "read: offset past end");
    }
    { // --- read: error on missing file ---
        auto r = tools.execute("read", readArgs("/tmp/tool_test/nope.cpp"), noAbort);
        check(r.isError && r.content.find("File not found") != std::string::npos, "read: missing file");
    }

    { // --- read: directory lists entries (non-error) ---
        const char* kDir = "/tmp/tool_test/listdir";
        std::filesystem::create_directories(std::string(kDir) + "/nested");
        writeFile(std::string(kDir) + "/b.cpp", "int b;\n");
        writeFile(std::string(kDir) + "/a.txt", "a\n");
        writeFile(std::string(kDir) + "/nested/c.txt", "c\n");
        auto r = tools.execute("read", readArgs(kDir), noAbort);
        check(!r.isError, "read dir: not an error");
        check(r.content.find("Directory listing of " + std::string(kDir)) != std::string::npos,
              "read dir: header names the path");
        check(r.content.find("\na.txt\n") != std::string::npos, "read dir: file entry");
        check(r.content.find("\nb.cpp\n") != std::string::npos, "read dir: file entry 2");
        check(r.content.find("\nnested/") != std::string::npos, "read dir: subdirectory marked with /");
        // relative path works too
        auto r2 = tools.execute("read", readArgs("listdir"), noAbort);
        check(!r2.isError && r2.content.find("b.cpp") != std::string::npos, "read dir: relative path");
    }
    { // --- read: empty directory ---
        const char* kEmpty = "/tmp/tool_test/emptydir";
        std::filesystem::create_directories(kEmpty);
        auto r = tools.execute("read", readArgs(kEmpty), noAbort);
        check(!r.isError && r.content.find("Directory is empty") != std::string::npos, "read: empty dir");
    }
    { // --- read: image attachment, and the vision-hint routing ---
        const char* kPng = "/tmp/tool_test/pix.png";
        unsigned char px[4] = {255, 0, 0, 255};
        if (!stbi_write_png(kPng, 1, 1, 4, px, 4))
            check(false, "read image: failed to write test png");
        // No image model: the result carries the attachment.
        auto r = tools.execute("read", readArgs(kPng), noAbort);
        check(!r.isError, "read image: ok");
        check(r.image.has_value(), "read image: attachment present");
        check(r.image->width == 1 && r.image->height == 1, "read image: dimensions");
        check(!r.imagePath.empty(), "read image: on-disk path set");
        // With a vision hint (image model designated): text only, no
        // attachment, and the model is steered to analyze_image.
        Tool reader = makeReadTool("/tmp/tool_test", "USE THE analyze_image tool.");
        auto h = reader.execute(readArgs(kPng), noAbort);
        check(!h.isError, "read image + hint: ok");
        check(!h.image.has_value(), "read image + hint: no attachment");
        check(h.content.find("USE THE analyze_image tool.") != std::string::npos,
              "read image + hint: steering message present");
        check(h.content.find(kPng) != std::string::npos, "read image + hint: path named");
        check(h.content.find("1x1") != std::string::npos, "read image + hint: dimensions reported");
    }

    { // --- edit: basic unique replacement ---
        writeFile(kFile, "static int x = 1;\n");
        auto r = tools.execute("edit", editArgs(kFile, {{"int x = 1;", "int x = 42;"}}), noAbort);
        check(!r.isError && r.content.find("Applied 1 edit(s)") != std::string::npos, "edit: applied");
        check(readFile(kFile) == "static int x = 42;\n", "edit: file rewritten");
    }
    { // --- edit: multiple edits, applied atomically ---
        writeFile(kFile, "one\ntwo\nthree\n");
        auto r = tools.execute("edit", editArgs(kFile, {{"one", "ONE"}, {"three", "THREE"}}), noAbort);
        check(!r.isError, "edit: multi applied");
        check(readFile(kFile) == "ONE\ntwo\nTHREE\n", "edit: multi result");
        // one bad edit -> nothing applied
        writeFile(kFile, "one\ntwo\nthree\n");
        auto bad = tools.execute(
            "edit", editArgs(kFile, {{"one", "ONE"}, {"does-not-exist", "x"}}), noAbort);
        check(bad.isError, "edit: atomic failure reported");
        check(readFile(kFile) == "one\ntwo\nthree\n", "edit: file untouched on failure");
    }
    { // --- edit: phantom leading space artifact (the session failure mode) ---
        // File uses 4-space indentation; the model copied lines with one extra
        // leading space per line (from the old "NNNNN→ " read prefix).
        writeFile(kFile, "    if (x) {\n        doThing();\n    }\n");
        auto r = tools.execute(
            "edit", editArgs(kFile, {{"     if (x) {\n         doThing();\n     }\n", "y"}}), noAbort);
        check(r.isError, "edit: artifact oldText rejected");
        check(r.content.find("removing one leading space from each line") != std::string::npos,
              "edit: specific per-line space hint");
        check(readFile(kFile) == "    if (x) {\n        doThing();\n    }\n", "edit: file untouched");
        // retry with the exact text works
        auto r2 = tools.execute("edit", editArgs(kFile, {{"    if (x) {", "    if (y) {"}}), noAbort);
        check(!r2.isError, "edit: exact retry succeeds");
    }
    { // --- edit: not found, no artifact -> generic indent hint ---
        writeFile(kFile, "int a;\n");
        auto r = tools.execute("edit", editArgs(kFile, {{"  int b;", "x"}}), noAbort);
        check(r.isError && r.content.find("not found") != std::string::npos, "edit: not-found message");
        check(r.content.find("display-only") != std::string::npos, "edit: generic display-only hint");
    }
    { // --- edit: multiple matches lists line numbers ---
        writeFile(kFile, "foo\nbar\nfoo\nbaz\nfoo\n");
        auto r = tools.execute("edit", editArgs(kFile, {{"foo", "x"}}), noAbort);
        check(r.isError, "edit: multiple matches rejected");
        check(r.content.find("matches 3 times") != std::string::npos, "edit: match count");
        check(r.content.find("lines 1, 3, 5") != std::string::npos, "edit: line numbers listed");
    }
    { // --- edit: overlap detection ---
        writeFile(kFile, "abcd\n");
        auto r = tools.execute("edit", editArgs(kFile, {{"ab", "1"}, {"bc", "2"}}), noAbort);
        check(r.isError && r.content.find("overlap") != std::string::npos, "edit: overlap detected");
    }
    { // --- edit: argument validation ---
        nlohmann::json a1 = nlohmann::json::object();
        a1["edits"] = nlohmann::json::array();
        auto r1 = tools.execute("edit", a1, noAbort);
        check(r1.isError && r1.content.find("Missing 'path'") != std::string::npos, "edit: missing path");
        nlohmann::json a2 = nlohmann::json::object();
        a2["path"] = kFile;
        a2["edits"] = nlohmann::json::array();
        auto r2 = tools.execute("edit", a2, noAbort);
        check(r2.isError && r2.content.find("Missing or empty 'edits'") != std::string::npos,
              "edit: empty edits");
        nlohmann::json a3 = nlohmann::json::object();
        a3["path"] = "/tmp/tool_test/nope.cpp";
        nlohmann::json e3 = nlohmann::json::object();
        e3["oldText"] = "a";
        e3["newText"] = "b";
        nlohmann::json arr3 = nlohmann::json::array();
        arr3.push_back(e3);
        a3["edits"] = arr3;
        auto r3 = tools.execute("edit", a3, noAbort);
        check(r3.isError && r3.content.find("File not found") != std::string::npos, "edit: missing file");
    }
    { // --- end-to-end: copy content straight from read output, then edit ---
        writeFile(kFile, "    ImGui::Text(\"hi\");\n    return;\n");
        auto rr = tools.execute("read", readArgs(kFile), noAbort);
        check(rr.content.find("1→") != std::string::npos, "e2e: prefix present");
        // everything after the arrow on a line is exact file content
        std::string firstLine = rr.content.substr(0, rr.content.find('\n'));
        const std::string arrow = "→";
        std::string content = firstLine.substr(firstLine.find(arrow) + arrow.size());
        check(content.rfind("    ImGui::Text(\"hi\");") == 0, "e2e: content starts at real indent");
        auto er = tools.execute("edit", editArgs(kFile, {{content, "    ImGui::Text(\"bye\");"}}), noAbort);
        check(!er.isError, "e2e: edit copied verbatim from read output succeeds");
        check(readFile(kFile) == "    ImGui::Text(\"bye\");\n    return;\n", "e2e: file content correct");
    }

    { // --- edit: trailing whitespace is significant and preserved ---
        writeFile(kFile, "    x = 1;   \n");
        auto rr = tools.execute("read", readArgs(kFile), noAbort);
        check(rr.content.find("x = 1;   ") != std::string::npos, "read: trailing spaces visible");
        auto r = tools.execute("edit", editArgs(kFile, {{"    x = 1;   ", "    x = 2;   "}}), noAbort);
        check(!r.isError, "edit: oldText with trailing spaces matches");
        check(readFile(kFile) == "    x = 2;   \n", "edit: trailing spaces preserved in file");
    }
    { // --- edit: CRLF files stay CRLF ---
        writeFile(kFile, "int a;\r\nint b;\r\n");
        auto r = tools.execute("edit", editArgs(kFile, {{"int a;\r\nint b;\r\n", "int a;\r\nint c;\r\n"}}), noAbort);
        check(!r.isError, "edit: CRLF oldText matches");
        check(readFile(kFile) == "int a;\r\nint c;\r\n", "edit: CRLF preserved");
    }
    { // --- edit: file without trailing newline ---
        writeFile(kFile, "abc");
        auto r = tools.execute("edit", editArgs(kFile, {{"abc", "xyz"}}), noAbort);
        check(!r.isError, "edit: last line without newline matches");
        check(readFile(kFile) == "xyz", "edit: no newline added");
    }
    { // --- edit: multibyte UTF-8 matched byte-exactly ---
        writeFile(kFile, "h\xc3\xa9llo \xe2\x86\x92 mundo\n");
        auto r = tools.execute("edit", editArgs(kFile, {{"h\xc3\xa9llo", "bye"}}), noAbort);
        check(!r.isError, "edit: UTF-8 oldText matches");
        check(readFile(kFile) == "bye \xe2\x86\x92 mundo\n", "edit: UTF-8 preserved");
    }
    { // --- edit: more than 5 matches caps the line list ---
        std::string many = "f\n";
        for (int i = 0; i < 8; i++)
            many += "x\nf\nx\n";
        writeFile(kFile, many);
        auto r = tools.execute("edit", editArgs(kFile, {{"f", "y"}}), noAbort);
        check(r.isError && r.content.find("matches 9 times") != std::string::npos, "edit: 9 matches");
        check(r.content.find("lines 1, 3, 6, 9, 12, \xe2\x80\xa6") != std::string::npos, "edit: capped at 5 + ellipsis");
    }
    { // --- edit: empty oldText rejected ---
        writeFile(kFile, "a\n");
        auto r = tools.execute("edit", editArgs(kFile, {{"", "x"}}), noAbort);
        check(r.isError && r.content.find("must not be empty") != std::string::npos, "edit: empty oldText");
    }
    { // --- read: truncation past 50KB keeps line boundaries ---
        std::string big;
        for (int i = 0; i < 30000; i++)
            big += "some line of text\n";
        writeFile(kFile, big);
        auto r = tools.execute("read", readArgs(kFile), noAbort);
        check(!r.isError && r.content.find("[truncated") != std::string::npos, "read: truncation note");
        check(r.content.size() < 60 * 1024, "read: result stays near limit");
        check(r.content.find("some line of text\n...") != std::string::npos,
              "read: truncated at line boundary");
    }
    { // --- read: empty file ---
        writeFile(kFile, "");
        auto r = tools.execute("read", readArgs(kFile), noAbort);
        check(!r.isError && r.content.empty(), "read: empty file yields empty output");
    }
    { // --- truncateToLimit: line limit reached before byte limit ---
        std::string manyLines;
        for (int i = 0; i < 5000; i++)
            manyLines += "x\n";
        std::string out = truncateToLimit(manyLines, 10, 1000);
        check(out.find("[truncated") != std::string::npos && out.size() < 1000, "truncate: line limit");
    }
    { // --- truncateToLimit: byte limit reached before line limit ---
        std::string fewLongLines;
        for (int i = 0; i < 100; i++)
            fewLongLines += std::string(100, 'y') + "\n";
        std::string out = truncateToLimit(fewLongLines, 1000, 500);
        check(out.find("[truncated") != std::string::npos, "truncate: byte limit note");
        check(out.size() <= 500 + 64, "truncate: byte limit respected");
        check(out.find("y\n... [truncated") != std::string::npos, "truncate: cut at line boundary");
    }
    { // --- truncateToLimit: under both limits -> untouched ---
        std::string small = "a\nb\nc\n";
        check(truncateToLimit(small, 10, 100) == small, "truncate: no-op when small");
    }

    { // --- read: truncation at 2000 lines ---
        std::string big;
        for (int i = 0; i < 3000; i++)
            big += "x\n";
        writeFile(kFile, big);
        auto r = tools.execute("read", readArgs(kFile), noAbort);
        check(!r.isError, "read: large file ok");
        check(r.content.find("[truncated:") != std::string::npos, "read: truncation note");
        check(r.content.find("3000→") == std::string::npos, "read: line 3000 not shown");
        check(r.content.find("1999→") != std::string::npos, "read: line 1999 shown");
    }
    { // --- read: file without trailing newline ---
        writeFile(kFile, "a\nb");
        auto r = tools.execute("read", readArgs(kFile), noAbort);
        check(r.content.find("1→a") != std::string::npos && r.content.find("2→b") != std::string::npos,
              "read: no trailing newline");
        check(r.content.size() >= 2 && r.content.back() == '\n' && r.content[r.content.size() - 2] != '\n',
              "read: no extra trailing newline");
    }
    { // --- edit: file without trailing newline, no \n added ---
        writeFile(kFile, "a\nb");
        auto r = tools.execute("edit", editArgs(kFile, {{"b", "B"}}), noAbort);
        check(!r.isError, "edit: no-trailing-newline applied");
        check(readFile(kFile) == "a\nB", "edit: no trailing newline added");
    }
    { // --- edit: UTF-8 multibyte oldText/newText ---
        writeFile(kFile, "h\u00e9llo w\u00f6rld\n"); // héllo wörld
        auto r = tools.execute("edit", editArgs(kFile, {{"h\u00e9llo", "bye"}}), noAbort);
        check(!r.isError, "edit: UTF-8 applied");
        check(readFile(kFile) == "bye w\u00f6rld\n", "edit: UTF-8 bytes preserved");
    }
    { // --- write: creates parents, overwrites ---
        const char* kSub = "/tmp/tool_test/sub/dir/f.txt";
        nlohmann::json a = nlohmann::json::object();
        a["path"] = kSub;
        a["content"] = "one\ntwo\n";
        auto r = tools.execute("write", a, noAbort);
        check(!r.isError && r.content.find("Wrote 8 bytes") != std::string::npos, "write: nested path");
        check(readFile(kSub) == "one\ntwo\n", "write: content");
        nlohmann::json b = nlohmann::json::object();
        b["path"] = kSub;
        b["content"] = "three\n";
        auto r2 = tools.execute("write", b, noAbort);
        check(!r2.isError && readFile(kSub) == "three\n", "write: overwrite");
        nlohmann::json c = nlohmann::json::object();
        c["content"] = "x";
        auto r3 = tools.execute("write", c, noAbort);
        check(r3.isError && r3.content.find("Missing 'path'") != std::string::npos, "write: missing path");
    }
    { // --- edit: near miss -> closest-region hint with the actual file content ---
        const std::string nearFile =
            "static void DrawStatusStrip(App& app) {\n"
            "    ImGui::BeginChild(\"##statusstrip\");\n"
            "    if (app.agentRunning)\n"
            "        return;\n"
            "}\n";
        writeFile(kFile, nearFile);
        // The model retyped the block: one word garbled, one line off by one
        // indent space. Nothing matches exactly, but the region is close.
        auto r = tools.execute("edit", editArgs(kFile, {{
            "static void DrawStatusStrp(App& app) {\n"
            "     ImGui::BeginChild(\"##statusstrip\");\n"
            "    if (app.agentRunning)\n"
            "        return;\n"
            "}", "x"}}),
            noAbort);
        check(r.isError, "edit: near miss rejected");
        check(r.content.find("Closest region") != std::string::npos, "edit: closest-region hint shown");
        check(r.content.find("1\u2192static void DrawStatusStrip(App& app) {") != std::string::npos,
              "edit: hint shows actual content with read-style line numbers");
        check(r.content.find("5\u2192}") != std::string::npos, "edit: hint covers the whole region");
        check(r.content.find("do not retype it from memory") != std::string::npos,
              "edit: hint tells the model to copy, not retype");
        check(readFile(kFile) == nearFile, "edit: file untouched on near miss");
    }
    { // --- edit: unrelated oldText -> no closest-region hint (no false positives) ---
        writeFile(kFile, "int a = 1;\nint b = 2;\nint c = 3;\n");
        auto r = tools.execute("edit", editArgs(kFile, {{"void render(ImGuiIO io) { }", "x"}}), noAbort);
        check(r.isError, "edit: unrelated oldText rejected");
        check(r.content.find("Closest region") == std::string::npos,
              "edit: no closest-region hint for unrelated text");
    }
    { // --- edit: artifact hint takes priority over the fuzzy hint ---
        writeFile(kFile, "    if (x) {\n        doThing();\n    }\n");
        auto r = tools.execute(
            "edit",
            editArgs(kFile, {{"     if (x) {\n         doThing();\n     }\n", "y"}}),
            noAbort);
        check(r.isError, "edit: artifact oldText rejected");
        check(r.content.find("removing one leading space") != std::string::npos,
              "edit: artifact hint shown");
        check(r.content.find("Closest region") == std::string::npos,
              "edit: artifact hint instead of fuzzy hint");
    }
    { // --- edit: fuzzy search on a large file stays fast and finds the region ---
        std::string big;
        for (int i = 0; i < 3000; i++)
            big += "    helperFunction" + std::to_string(i) + "(arg" + std::to_string(i) + ");\n";
        // The model's stale copy of a middle region: two lines drift.
        big += "    void uniqueTargetRegionLine(int v) {\n";
        big += "        applyTransformation(v);\n";
        big += "    }\n";
        for (int i = 0; i < 50; i++)
            big += "    tailFunction" + std::to_string(i) + "(w);\n";
        writeFile(kFile, big);
        auto r = tools.execute("edit", editArgs(kFile, {{
            "    void uniqueTargetRegionLine(int v) {\n"
            "        applyTransformaton(v);\n"
            "    }", "x"}}),
            noAbort);
        check(r.isError, "edit: stale region rejected");
        check(r.content.find("Closest region") != std::string::npos, "edit: large file: hint shown");
        check(r.content.find("uniqueTargetRegionLine(int v) {") != std::string::npos,
              "edit: large file: hint names the right region");
    }

    { // --- analyze_image: validation paths (no network reached) ---
        LlmBackend imgBackend{"img", "http://127.0.0.1:1/v1", "", "img-model"};
        Tool atool = makeAnalyzeImageTool("/tmp/tool_test", imgBackend);

        nlohmann::json a = nlohmann::json::object();
        a["images"]   = nlohmann::json::array();
        a["question"] = "what is this?";
        auto r = atool.execute(a, noAbort);
        check(r.isError, "analyze_image: empty images rejected");

        a["images"] = nlohmann::json::array({"/tmp/tool_test/missing.png"});
        r = atool.execute(a, noAbort);
        check(r.isError && r.content.find("not found") != std::string::npos,
              "analyze_image: missing file rejected");

        writeFile("/tmp/tool_test/notes.txt", "not an image\n");
        a["images"] = nlohmann::json::array({"/tmp/tool_test/notes.txt"});
        r = atool.execute(a, noAbort);
        check(r.isError && r.content.find("Unsupported image format") != std::string::npos,
              "analyze_image: non-image file rejected");
    }

    { // --- grep: finds matching lines recursively ---
        const char* kG = "/tmp/tool_test/grepme";
        std::filesystem::create_directories(std::string(kG) + "/sub");
        writeFile(std::string(kG) + "/a.cpp", "int alpha = 1;\nint beta = 2;\n");
        writeFile(std::string(kG) + "/sub/b.cpp", "int alpha = 3;\n");
        auto r = tools.execute("grep", grepArgs("alpha", kG), noAbort);
        check(!r.isError, "grep: ok");
        check(r.content.find("a.cpp:1:int alpha = 1;") != std::string::npos, "grep: match with path:line");
        check(r.content.find("sub/b.cpp:1:int alpha = 3;") != std::string::npos, "grep: recursive into subdir");
        check(r.content.find("beta") == std::string::npos, "grep: non-matching line not shown");
        check(r.content.find("2 matches") != std::string::npos, "grep: match count reported");
    }
    { // --- grep: case-insensitive ---
        auto r = tools.execute("grep", grepArgs("ALPHA", "/tmp/tool_test/grepme"), noAbort);
        check(!r.isError && r.content.find("alpha = 1") != std::string::npos, "grep: case-insensitive");
    }
    { // --- grep: no match, missing pattern, bad path ---
        auto none = tools.execute("grep", grepArgs("zzz-not-there", "/tmp/tool_test/grepme"), noAbort);
        check(!none.isError && none.content.find("No matches") != std::string::npos, "grep: no-match message");
        auto missing = tools.execute("grep", nlohmann::json::object(), noAbort);
        check(missing.isError && missing.content.find("Missing 'pattern'") != std::string::npos,
              "grep: missing pattern rejected");
        auto badPath = tools.execute("grep", grepArgs("x", "/tmp/tool_test/nope-dir"), noAbort);
        check(badPath.isError && badPath.content.find("not found") != std::string::npos,
              "grep: bad root rejected");
    }
    { // --- grep: a single file can be searched directly ---
        auto r = tools.execute("grep", grepArgs("alpha", "/tmp/tool_test/grepme/a.cpp"), noAbort);
        check(!r.isError, "grep: single file ok");
        check(r.content.find("a.cpp:1:int alpha = 1;") != std::string::npos, "grep: single-file match");
        check(r.content.find("sub/b.cpp") == std::string::npos, "grep: single file does not recurse");
    }
    { // --- grep: binary files are skipped ---
        const char* kBin = "/tmp/tool_test/grepme/blob.bin";
        writeFile(kBin, std::string("\x01\x02\x00binary\x00alpha\x00", 17));
        auto r = tools.execute("grep", grepArgs("alpha", "/tmp/tool_test/grepme"), noAbort);
        check(!r.isError, "grep: binary file does not break the search");
        check(r.content.find("blob.bin") == std::string::npos, "grep: binary file skipped");
        check(r.content.find("a.cpp:1:int alpha = 1;") != std::string::npos,
              "grep: text file still matched");
        check(r.content.find("binary file(s) skipped") != std::string::npos,
              "grep: binary skips are reported");
    }
    { // --- grep: gitignored directories are never walked ---
        const char* kI = "/tmp/tool_test/ign";
        std::filesystem::create_directories(std::string(kI) + "/gen");
        std::filesystem::create_directories(std::string(kI) + "/sub/keep");
        std::filesystem::create_directories(std::string(kI) + "/.git");
        writeFile(std::string(kI) + "/.gitignore", "gen/\n*.log\n");
        writeFile(std::string(kI) + "/gen/alpha.cpp", "int alpha = 1;\n");
        writeFile(std::string(kI) + "/run.log", "int alpha = 2;\n");
        writeFile(std::string(kI) + "/.git/alpha.txt", "int alpha = 3;\n");
        writeFile(std::string(kI) + "/sub/alpha.cpp", "int alpha = 4;\n");
        writeFile(std::string(kI) + "/sub/.gitignore", "keep/\n");
        writeFile(std::string(kI) + "/sub/keep/alpha.cpp", "int alpha = 5;\n");
        auto r = tools.execute("grep", grepArgs("alpha", kI), noAbort);
        check(!r.isError, "grep: ignored tree ok");
        check(r.content.find("sub/alpha.cpp") != std::string::npos, "grep: tracked file still matched");
        check(r.content.find("gen/alpha.cpp") == std::string::npos, "grep: gitignored directory pruned");
        check(r.content.find("run.log") == std::string::npos, "grep: gitignored file skipped");
        check(r.content.find(".git/alpha.txt") == std::string::npos, "grep: .git never walked");
        check(r.content.find("keep/alpha.cpp") == std::string::npos,
              "grep: nested .gitignore prunes only its own subtree");
    }
    { // --- grep: an explicitly named ignored directory is still searched ---
        auto r = tools.execute("grep", grepArgs("alpha", "/tmp/tool_test/ign/gen"), noAbort);
        check(!r.isError && r.content.find("alpha.cpp:1:") != std::string::npos,
              "grep: explicit path into an ignored directory works");
    }
    { // --- grep: a negated rule re-includes an otherwise ignored file ---
        const char* kN = "/tmp/tool_test/neg";
        std::filesystem::create_directories(kN);
        writeFile(std::string(kN) + "/.gitignore", "*.txt\n!keep.txt\n");
        writeFile(std::string(kN) + "/drop.txt", "int alpha = 1;\n");
        writeFile(std::string(kN) + "/keep.txt", "int alpha = 2;\n");
        auto r = tools.execute("grep", grepArgs("alpha", kN), noAbort);
        check(r.content.find("keep.txt:1:") != std::string::npos, "grep: negated rule re-includes");
        check(r.content.find("drop.txt") == std::string::npos, "grep: other ignored file still skipped");
    }
    { // --- grep: matches come back in path order regardless of thread count ---
        const char* kS = "/tmp/tool_test/order";
        std::filesystem::create_directories(kS);
        for (int i = 0; i < 31; ++i)
            writeFile(std::string(kS) + "/f" + std::string(2, static_cast<char>('0' + i / 10)) +
                          static_cast<char>('0' + i % 10) + ".txt", "int alpha = 1;\n");
        auto r = tools.execute("grep", grepArgs("alpha", kS, 100), noAbort);
        std::vector<std::string> paths;
        std::istringstream lines(r.content);
        std::string line;
        while (std::getline(lines, line)) {
            size_t p = line.find(":1:");
            if (p != std::string::npos)
                paths.push_back(line.substr(0, p));
        }
        check(paths.size() == 31, "grep: every matching file reported");
        bool sorted = std::is_sorted(paths.begin(), paths.end());
        check(sorted, "grep: results are sorted by path (parallel scan is deterministic)");
    }
    { // --- grep: a raised abort flag stops the walk instead of running to completion ---
        std::atomic<bool> stop{true};
        auto t0 = std::chrono::steady_clock::now();
        auto r = tools.execute("grep", grepArgs("alpha", "/tmp/tool_test/ign"), stop);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
        check(!r.isError && ms < 1000, "grep: abort flag ends the search promptly");
    }
    { // --- grep: relative path resolves against the tool's cwd ---
        auto r = tools.execute("grep", grepArgs("alpha", "grepme"), noAbort);
        check(!r.isError && r.content.find("a.cpp:1:int alpha = 1;") != std::string::npos,
              "grep: relative path");
    }
    { // --- grep: limit caps the number of matches ---
        auto r = tools.execute("grep", grepArgs("alpha", "/tmp/tool_test/grepme", 1), noAbort);
        check(!r.isError, "grep: limited ok");
        check(r.content.find("a.cpp:1:") != std::string::npos, "grep: first match present");
        check(r.content.find("sub/b.cpp:1:") == std::string::npos, "grep: limit drops later matches");
    }
    { // --- read: truncated output points at the next chunk ---
        std::string big;
        for (int i = 0; i < 3000; i++)
            big += "x\n";
        writeFile(kFile, big);
        auto r = tools.execute("read", readArgs(kFile), noAbort);
        check(!r.isError, "read: large file ok");
        check(r.content.find("Re-read with offset=") != std::string::npos,
              "read: truncation suggests the next offset");
        check(r.content.find("Total lines in file: 3000") != std::string::npos,
              "read: truncation reports line count");
        // The suggested offset must be the first line the model has NOT
        // seen: reading from it must continue exactly where the cut stopped.
        size_t p = r.content.find("Re-read with offset=");
        long long nextOffset = 0;
        if (p != std::string::npos)
            sscanf(r.content.c_str() + p + 20, "%lld", &nextOffset);
        check(nextOffset > 0, "read: suggested offset parsed");
        auto tail = tools.execute("read", readArgs(kFile, nextOffset), noAbort);
        check(!tail.isError, "read: continuation ok");
        // The last line shown before the cut must be the line just before
        // the suggested offset.
        char want[64];
        snprintf(want, sizeof(want), "%5lld→", nextOffset - 1);
        check(r.content.find(want) != std::string::npos,
              "read: cut ends on the line before the suggested offset");
        // ...and the continuation must start exactly at that offset.
        char first[64];
        snprintf(first, sizeof(first), "%5lld→", nextOffset);
        check(tail.content.find(first) != std::string::npos,
              "read: continuation starts at the suggested offset");
    }
    { // --- read: no chunk hint when the whole file fits ---
        writeFile(kFile, "one\ntwo\n");
        auto r = tools.execute("read", readArgs(kFile), noAbort);
        check(!r.isError && r.content.find("Re-read with offset=") == std::string::npos,
              "read: no chunk hint for a small file");
    }

    std::system("rm -rf /tmp/tool_test");
    if (failures == 0)
        std::printf("tool_test: all passed\n");
    return failures == 0 ? 0 : 1;
}
