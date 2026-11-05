// Unit tests for the apply_patch tool (tools/apply_patch.cpp), a port of
// codex-rs/apply-patch. Cases mirror codex's own tests: add/delete/update
// hunks, moves, multi-chunk updates, the *** End of File marker, the fuzzy
// whitespace/unicode matching passes, and the failure messages.
#include "tools/factories.h"

#include "util/path.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

static int failures = 0;

static void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

namespace {

std::atomic<bool> noAbort{false};

void writeFile(const std::string& path, const std::string& content) {
    std::ofstream f(path, std::ios::binary);
    f.write(content.data(), (std::streamsize)content.size());
}

std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

ToolResult run(const Tool& tool, const std::string& patch) {
    nlohmann::json args = nlohmann::json::object();
    args["patch"] = patch;
    return tool.execute(args, noAbort);
}

std::string wrap(const std::string& body) {
    return "*** Begin Patch\n" + body + "\n*** End Patch";
}

std::string cwdOf(const std::string& dir) { return dir; }

void testAddAndDelete() {
    const std::string dir = "/tmp/apply_patch_test/add_del";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);

    ToolResult r = run(tool, wrap("*** Add File: hello.txt\n+hello\n+world"));
    check(!r.isError, "add file succeeds: " + r.content);
    check(readFile(dir + "/hello.txt") == "hello\nworld\n", "add file content");
    check(r.content.find("A hello.txt") != std::string::npos, "add file summary");

    writeFile(dir + "/gone.txt", "bye\n");
    r = run(tool, wrap("*** Delete File: gone.txt"));
    check(!r.isError, "delete file succeeds: " + r.content);
    check(!std::filesystem::exists(dir + "/gone.txt"), "delete file removes it");
    check(r.content.find("D gone.txt") != std::string::npos, "delete file summary");

    r = run(tool, wrap("*** Delete File: missing.txt"));
    check(r.isError, "delete missing file fails");
}

void testUpdateExact() {
    const std::string dir = "/tmp/apply_patch_test/update";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/file.txt", "foo\nbar\n");

    ToolResult r = run(tool, wrap("*** Update File: file.txt\n@@\n foo\n-bar\n+baz"));
    check(!r.isError, "update succeeds: " + r.content);
    check(readFile(dir + "/file.txt") == "foo\nbaz\n", "update result");
    check(r.content.find("M file.txt") != std::string::npos, "update summary");
}

void testMultipleChunks() {
    const std::string dir = "/tmp/apply_patch_test/multi";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/m.txt", "foo\nbar\nbaz\nqux\n");

    ToolResult r = run(tool, wrap("*** Update File: m.txt\n@@\n foo\n-bar\n+BAR\n@@\n baz\n-qux\n+QUX"));
    check(!r.isError, "two chunks succeed: " + r.content);
    check(readFile(dir + "/m.txt") == "foo\nBAR\nbaz\nQUX\n", "two chunk result");
}

void testContextAnchor() {
    const std::string dir = "/tmp/apply_patch_test/context";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/c.txt", "def a():\n    x = 1\ndef b():\n    x = 1\n");

    // The same old line twice: the @@ anchor selects the one after 'def b():'.
    ToolResult r = run(
        tool, wrap("*** Update File: c.txt\n@@ def b():\n-    x = 1\n+    x = 2"));
    check(!r.isError, "anchored update succeeds: " + r.content);
    check(readFile(dir + "/c.txt") == "def a():\n    x = 1\ndef b():\n    x = 2\n",
          "anchored update result");

    r = run(tool, wrap("*** Update File: c.txt\n@@ def missing():\n-    x = 1\n+    x = 3"));
    check(r.isError, "missing context fails");
    check(r.content.find("Failed to find context 'def missing():'") != std::string::npos,
          "missing context error names it: " + r.content);
}

void testFuzzyWhitespace() {
    const std::string dir = "/tmp/apply_patch_test/fuzzy";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/w.txt", "  indented   \nplain\n");

    // Patch omits the trailing spaces of line 1 and the leading spaces of 2.
    ToolResult r = run(tool, wrap("*** Update File: w.txt\n@@\n-  indented\n-plain\n+one\n+two"));
    check(!r.isError, "whitespace-tolerant match succeeds: " + r.content);
    check(readFile(dir + "/w.txt") == "one\ntwo\n", "whitespace-tolerant result");
}

void testFuzzyUnicode() {
    const std::string dir = "/tmp/apply_patch_test/unicode";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/u.txt", "import x  # note \xE2\x80\x93 dash\n");

    // Patch uses an ASCII dash where the file has an EN DASH.
    ToolResult r = run(
        tool, wrap("*** Update File: u.txt\n@@\n-import x  # note - dash\n+import y"));
    check(!r.isError, "unicode-normalised match succeeds: " + r.content);
    check(readFile(dir + "/u.txt") == "import y\n", "unicode-normalised result");
}

void testEndOfFileMarker() {
    const std::string dir = "/tmp/apply_patch_test/eof";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    // The anchor text appears twice; only *** End of File pins the chunk to
    // the last occurrence.
    writeFile(dir + "/e.txt", "append here\na\nappend here\n");

    ToolResult r = run(
        tool,
        wrap("*** Update File: e.txt\n@@\n append here\n+tail\n*** End of File"));
    check(!r.isError, "eof marker succeeds: " + r.content);
    check(readFile(dir + "/e.txt") == "append here\na\nappend here\ntail\n",
          "eof marker anchors the chunk at end of file");
}

void testMoveTo() {
    const std::string dir = "/tmp/apply_patch_test/move";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/src.txt", "line\n");

    ToolResult r = run(
        tool, wrap("*** Update File: src.txt\n*** Move to: dst.txt\n@@\n-line\n+line2"));
    check(!r.isError, "move succeeds: " + r.content);
    check(!std::filesystem::exists(dir + "/src.txt"), "move removes source");
    check(readFile(dir + "/dst.txt") == "line2\n", "move writes destination");
    check(r.content.find("M dst.txt") != std::string::npos, "move summary uses destination");
}

void testCrlfPreserved() {
    const std::string dir = "/tmp/apply_patch_test/crlf";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/r.txt", "one\r\ntwo\r\nthree\r\n");

    ToolResult r = run(tool, wrap("*** Update File: r.txt\n@@\n one\n-two\n+TWO"));
    check(!r.isError, "crlf update succeeds: " + r.content);
    check(readFile(dir + "/r.txt") == "one\r\nTWO\r\nthree\r\n",
          "crlf context lines keep their terminators");
}

void testNoTrailingNewline() {
    const std::string dir = "/tmp/apply_patch_test/nonl";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/n.txt", "a\nb");

    ToolResult r = run(tool, wrap("*** Update File: n.txt\n@@\n-b\n+B"));
    check(!r.isError, "no-trailing-newline update succeeds: " + r.content);
    check(readFile(dir + "/n.txt") == "a\nB\n",
          "final line gains the preferred ending (codex behavior)");
}

void testPureAdditionThenRemoval() {
    // From codex's test_pure_addition_chunk_followed_by_removal.
    const std::string dir = "/tmp/apply_patch_test/pure";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/p.txt", "line1\nline2\nline3\n");

    ToolResult r = run(
        tool,
        wrap("*** Update File: p.txt\n@@\n+after-context\n+second-line\n@@\n line1\n-line2\n"
             "-line3\n+line2-replacement"));
    check(!r.isError, "pure addition chunk succeeds: " + r.content);
    check(readFile(dir + "/p.txt") ==
              "line1\nline2-replacement\nafter-context\nsecond-line\n",
          "pure addition chunk result");
}

void testParseErrors() {
    const std::string dir = "/tmp/apply_patch_test/errors";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);

    ToolResult r = run(tool, "not a patch");
    check(r.isError, "missing Begin Patch fails");
    check(r.content.find("first line of the patch must be") != std::string::npos,
          "missing Begin Patch error");

    r = run(tool, "*** Begin Patch\n*** Update File: x.txt\n-old\n+new");
    check(r.isError, "missing End Patch fails");
    check(r.content.find("last line of the patch must be") != std::string::npos,
          "missing End Patch error");

    r = run(tool, wrap("*** Update File: x.txt\nbad line"));
    check(r.isError, "unprefixed line fails");
    check(r.content.find("start with ' '") != std::string::npos ||
              r.content.find("not a valid hunk header") != std::string::npos,
          "unprefixed line error: " + r.content);

    r = run(tool, wrap("*** Update File: nope.txt\n@@\n-absent\n+new"));
    check(r.isError, "update of missing file fails");
    check(r.content.find("Failed to read file to update") != std::string::npos,
          "update of missing file error: " + r.content);

    writeFile(dir + "/present.txt", "different content\n");
    r = run(tool, wrap("*** Update File: present.txt\n@@\n-absent\n+new"));
    check(r.isError, "missing region fails");
    check(r.content.find("Failed to find expected lines") != std::string::npos,
          "missing region error: " + r.content);
}

void testMultiFilePatch() {
    const std::string dir = "/tmp/apply_patch_test/multifile";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    Tool tool = makeApplyPatchTool(dir);
    writeFile(dir + "/a.txt", "old a\n");
    writeFile(dir + "/b.txt", "old b\n");

    ToolResult r = run(tool,
                       wrap("*** Update File: a.txt\n@@\n-old a\n+new a\n"
                            "*** Add File: sub/c.txt\n+made\n"
                            "*** Delete File: b.txt"));
    check(!r.isError, "multi-file patch succeeds: " + r.content);
    check(readFile(dir + "/a.txt") == "new a\n", "multi-file update");
    check(readFile(dir + "/sub/c.txt") == "made\n", "multi-file add creates parents");
    check(!std::filesystem::exists(dir + "/b.txt"), "multi-file delete");
    check(r.content.find("M a.txt") != std::string::npos &&
              r.content.find("A sub/c.txt") != std::string::npos &&
              r.content.find("D b.txt") != std::string::npos,
          "multi-file summary: " + r.content);
}

void testToolShape() {
    Tool tool = makeApplyPatchTool("/tmp");
    check(tool.name == "apply_patch", "tool name");
    check(tool.paramsSchema.contains("properties") &&
              tool.paramsSchema["properties"].contains("patch"),
          "tool schema has patch param");
    check(tool.description.find("*** Begin Patch") != std::string::npos,
          "description teaches the format");
}

} // namespace

int main() {
    testToolShape();
    testAddAndDelete();
    testUpdateExact();
    testMultipleChunks();
    testContextAnchor();
    testFuzzyWhitespace();
    testFuzzyUnicode();
    testEndOfFileMarker();
    testMoveTo();
    testCrlfPreserved();
    testNoTrailingNewline();
    testPureAdditionThenRemoval();
    testParseErrors();
    testMultiFilePatch();
    if (failures == 0)
        std::printf("apply_patch_test: all passed\n");
    else
        std::printf("apply_patch_test: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
