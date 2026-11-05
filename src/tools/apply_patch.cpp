// apply_patch: codex-style patch editing (port of codex-rs/apply-patch).
//
// Replaces the edit tool when apply_patch=1 is set in
// ~/.config/miniagent/miniagent.settings (see App::rebuildTools). The patch
// format:
//
//   *** Begin Patch
//   *** Add File: <path>
//   +<line>
//   *** Delete File: <path>
//   *** Update File: <path>
//   *** Move to: <path>
//   @@ <context line>
//    <context>
//   -<removed>
//   +<added>
//   *** End of File
//   *** End Patch
//
// Matching (seekSequence) tries exact line equality first, then tolerates
// trailing whitespace, then leading+trailing whitespace, then normalises
// common Unicode punctuation - so slightly stale context still applies.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "tools/tools.h"
#include "util/path.h"

namespace {

constexpr const char* kBeginPatch = "*** Begin Patch";
constexpr const char* kEndPatch = "*** End Patch";
constexpr const char* kAddFile = "*** Add File: ";
constexpr const char* kDeleteFile = "*** Delete File: ";
constexpr const char* kUpdateFile = "*** Update File: ";
constexpr const char* kMoveTo = "*** Move to: ";
constexpr const char* kEndOfFile = "*** End of File";
constexpr const char* kChangeContext = "@@ ";
constexpr const char* kEmptyChangeContext = "@@";

std::string trimStr(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n\f\v");
    if (b == std::string::npos)
        return {};
    size_t e = s.find_last_not_of(" \t\r\n\f\v");
    return s.substr(b, e - b + 1);
}

std::string trimEndStr(const std::string& s) {
    size_t e = s.find_last_not_of(" \t\r\n\f\v");
    return e == std::string::npos ? std::string() : s.substr(0, e + 1);
}

bool hasPrefix(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i)
            out += '\n';
        out += lines[i];
    }
    return out;
}

// ---------------------------------------------------------------- hunks

struct UpdateChunk {
    bool hasContext = false;
    std::string changeContext;
    std::vector<std::string> oldLines;
    std::vector<std::string> newLines;
    std::vector<std::pair<size_t, size_t>> contextIndices;
    bool isEndOfFile = false;

    void pushContextLine(const std::string& line) {
        contextIndices.emplace_back(oldLines.size(), newLines.size());
        oldLines.push_back(line);
        newLines.push_back(line);
    }

    bool isEmpty() const { return oldLines.empty() && newLines.empty(); }
};

struct Hunk {
    enum class Kind { Add, Delete, Update } kind = Kind::Add;
    std::string path;
    std::string contents;              // Add
    bool hasMove = false;              // Update
    std::string movePath;              // Update
    std::vector<UpdateChunk> chunks;   // Update

    const std::string& affectedPath() const {
        return kind == Kind::Update && hasMove ? movePath : path;
    }
};

// ------------------------------------------------------------- parser
// Port of streaming_parser.rs: a per-line state machine. `fail()` wraps each
// error in codex's "Invalid patch hunk on line N: <msg>" shape.

class PatchParser {
public:
    explicit PatchParser(std::vector<Hunk>& hunks) : hunks_(hunks) {}

    // Returns "" on success, an error message otherwise.
    std::string processLine(const std::string& line) {
        ++lineNumber_;
        const std::string trimmed = trimStr(line);
        switch (mode_) {
            case Mode::NotStarted:
                if (trimmed == kBeginPatch) {
                    mode_ = Mode::StartedPatch;
                    return {};
                }
                return fail("The first line of the patch must be '*** Begin Patch'");
            case Mode::StartedPatch:
            case Mode::AddFile:
            case Mode::DeleteFile: {
                std::string err = handleHeaders(trimmed, line);
                if (err == kHandled)
                    return {};
                if (!err.empty())
                    return err;
                if (mode_ == Mode::AddFile && !line.empty() && line[0] == '+') {
                    hunks_.back().contents += line.substr(1);
                    hunks_.back().contents += '\n';
                    return {};
                }
                return fail("'" + trimmed + "' is not a valid hunk header. Valid hunk headers: "
                            "'*** Add File: {path}', '*** Delete File: {path}', "
                            "'*** Update File: {path}'");
            }
            case Mode::UpdateFile:
                return processUpdateLine(line);
            case Mode::EndedPatch:
                if (trimmed.empty())
                    return {};
                return fail("The last line of the patch must be '*** End Patch'");
        }
        return {};
    }

    bool ended() const { return mode_ == Mode::EndedPatch; }

private:
    enum class Mode { NotStarted, StartedPatch, AddFile, DeleteFile, UpdateFile, EndedPatch };

    std::string fail(const std::string& msg) const {
        return "Invalid patch hunk on line " + std::to_string(lineNumber_) + ": " + msg;
    }

    // "" = the line was a hunk header / End Patch (possibly with an error, in
    // error_); nullopt-like sentinel not needed: callers check error_ too.
    std::string ensureUpdateHunkNotEmpty(const std::string& line) {
        if (hunks_.empty() || hunks_.back().kind != Hunk::Kind::Update)
            return {};
        const Hunk& hunk = hunks_.back();
        if (hunk.chunks.empty())
            return fail("Update file hunk for path '" + hunk.path + "' is empty");
        if (hunk.chunks.back().isEmpty()) {
            if (line == kEndPatch)
                return fail("Update hunk does not contain any lines");
            return fail("Unexpected line found in update hunk: '" + line +
                        "'. Every line should start with ' ' (context line), '+' (added line), "
                        "'-' (removed line)");
        }
        return {};
    }

    std::string handleHeaders(const std::string& trimmed, const std::string& raw) {
        if (trimmed == kEndPatch) {
            if (std::string err = ensureUpdateHunkNotEmpty(trimmed); !err.empty())
                return err;
            mode_ = Mode::EndedPatch;
            return kHandled;
        }
        if (hasPrefix(trimmed, kAddFile)) {
            if (std::string err = ensureUpdateHunkNotEmpty(trimmed); !err.empty())
                return err;
            Hunk h;
            h.kind = Hunk::Kind::Add;
            h.path = trimmed.substr(strlen(kAddFile));
            hunks_.push_back(std::move(h));
            mode_ = Mode::AddFile;
            return kHandled;
        }
        if (hasPrefix(trimmed, kDeleteFile)) {
            if (std::string err = ensureUpdateHunkNotEmpty(trimmed); !err.empty())
                return err;
            Hunk h;
            h.kind = Hunk::Kind::Delete;
            h.path = trimmed.substr(strlen(kDeleteFile));
            hunks_.push_back(std::move(h));
            mode_ = Mode::DeleteFile;
            return kHandled;
        }
        if (hasPrefix(trimmed, kUpdateFile)) {
            if (std::string err = ensureUpdateHunkNotEmpty(trimmed); !err.empty())
                return err;
            Hunk h;
            h.kind = Hunk::Kind::Update;
            h.path = trimmed.substr(strlen(kUpdateFile));
            hunks_.push_back(std::move(h));
            mode_ = Mode::UpdateFile;
            return kHandled;
        }
        return {};
    }

    static constexpr const char* kHandled = "\x01"; // sentinel: line consumed

    std::string processUpdateLine(const std::string& line) {
        const std::string updateLine = trimEndStr(line);
        std::string err = handleHeaders(updateLine, line);
        if (err == kHandled)
            return {};
        if (!err.empty())
            return err;
        Hunk& hunk = hunks_.back();
        if (!hunk.chunks.empty() && hunk.chunks.back().isEndOfFile) {
            if (updateLine.empty())
                return {};
            if (updateLine != kEmptyChangeContext && !hasPrefix(updateLine, kChangeContext))
                return fail("Expected update hunk to start with a @@ context marker, got: '" +
                            line + "'");
        }
        if (hunk.chunks.empty() && !hunk.hasMove && hasPrefix(updateLine, kMoveTo)) {
            hunk.hasMove = true;
            hunk.movePath = updateLine.substr(strlen(kMoveTo));
            return {};
        }
        bool isContextMarker =
            updateLine == kEmptyChangeContext || hasPrefix(updateLine, kChangeContext);
        if (isContextMarker && !hunk.chunks.empty() && hunk.chunks.back().isEmpty())
            return fail("Unexpected line found in update hunk: '" + line +
                        "'. Every line should start with ' ' (context line), '+' (added line), "
                        "'-' (removed line)");
        if (updateLine == kEmptyChangeContext) {
            hunk.chunks.emplace_back();
            return {};
        }
        if (hasPrefix(updateLine, kChangeContext)) {
            UpdateChunk chunk;
            chunk.hasContext = true;
            chunk.changeContext = updateLine.substr(strlen(kChangeContext));
            hunk.chunks.push_back(std::move(chunk));
            return {};
        }
        if (updateLine == kEndOfFile) {
            if (!hunk.chunks.empty() && hunk.chunks.back().isEmpty())
                return fail("Update hunk does not contain any lines");
            if (!hunk.chunks.empty())
                hunk.chunks.back().isEndOfFile = true;
            return {};
        }
        if (line.empty()) {
            if (hunk.chunks.empty())
                hunk.chunks.emplace_back();
            hunk.chunks.back().pushContextLine(std::string());
            return {};
        }
        if (line[0] == ' ') {
            if (hunk.chunks.empty())
                hunk.chunks.emplace_back();
            hunk.chunks.back().pushContextLine(line.substr(1));
            return {};
        }
        if (line[0] == '+') {
            if (hunk.chunks.empty())
                hunk.chunks.emplace_back();
            hunk.chunks.back().newLines.push_back(line.substr(1));
            return {};
        }
        if (line[0] == '-') {
            if (hunk.chunks.empty())
                hunk.chunks.emplace_back();
            hunk.chunks.back().oldLines.push_back(line.substr(1));
            return {};
        }
        if (!hunk.chunks.empty() && !hunk.chunks.back().isEmpty())
            return fail("Expected update hunk to start with a @@ context marker, got: '" +
                        line + "'");
        return fail("Unexpected line found in update hunk: '" + line +
                    "'. Every line should start with ' ' (context line), '+' (added line), "
                    "'-' (removed line)");
    }

    std::vector<Hunk>& hunks_;
    Mode mode_ = Mode::NotStarted;
    int lineNumber_ = 0;
};

struct ParseOutcome {
    bool ok = false;
    std::string error;
    std::vector<Hunk> hunks;
};

ParseOutcome parsePatch(const std::string& patch) {
    ParseOutcome out;
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : patch) {
            if (c == '\n') {
                if (!cur.empty() && cur.back() == '\r')
                    cur.pop_back();
                lines.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        if (!cur.empty())
            lines.push_back(cur);
    }
    while (!lines.empty() && trimStr(lines.front()).empty())
        lines.erase(lines.begin());
    while (!lines.empty() && trimStr(lines.back()).empty())
        lines.pop_back();
    if (lines.empty() || trimStr(lines.front()) != kBeginPatch) {
        out.error = "Invalid patch: The first line of the patch must be '*** Begin Patch'";
        return out;
    }
    if (lines.empty() || trimStr(lines.back()) != kEndPatch) {
        out.error = "Invalid patch: The last line of the patch must be '*** End Patch'";
        return out;
    }

    PatchParser parser(out.hunks);
    for (const std::string& line : lines) {
        std::string err = parser.processLine(line);
        if (!err.empty()) {
            out.error = err;
            return out;
        }
    }
    if (!parser.ended()) {
        out.error = "Invalid patch: The last line of the patch must be '*** End Patch'";
        return out;
    }
    out.ok = true;
    return out;
}

// ------------------------------------------- source file (port of text_file.rs)
// Splits contents into logical lines while retaining each line ending, so
// untouched lines (context) keep their exact terminators; inserted lines use
// the file's first-seen (preferred) ending.

struct SourceFile {
    struct Line {
        std::string text;
        std::string ending; // "", "\n", "\r\n", "\r"
    };
    std::vector<Line> lines;
    std::string preferred = "\n";

    static SourceFile parse(const std::string& contents) {
        SourceFile file;
        bool havePreferred = false;
        size_t lineStart = 0, cursor = 0;
        while (cursor < contents.size()) {
            std::string ending;
            if (contents[cursor] == '\r' && cursor + 1 < contents.size() &&
                contents[cursor + 1] == '\n')
                ending = "\r\n";
            else if (contents[cursor] == '\r' || contents[cursor] == '\n')
                ending = contents[cursor] == '\r' ? "\r" : "\n";
            if (ending.empty()) {
                ++cursor;
                continue;
            }
            if (!havePreferred) {
                file.preferred = ending;
                havePreferred = true;
            }
            file.lines.push_back({contents.substr(lineStart, cursor - lineStart), ending});
            cursor += ending.size();
            lineStart = cursor;
        }
        if (lineStart < contents.size())
            file.lines.push_back({contents.substr(lineStart), std::string()});
        return file;
    }

    std::vector<std::string> lineTexts() const {
        std::vector<std::string> out;
        out.reserve(lines.size());
        for (const Line& l : lines)
            out.push_back(l.text);
        return out;
    }

    void applyReplacements(const std::vector<std::tuple<size_t, size_t, std::vector<std::string>>>&
                               replacements) {
        std::vector<Line> out;
        size_t sourceIndex = 0;
        for (const auto& [startIdx, oldLen, newSegment] : replacements) {
            for (; sourceIndex < startIdx; ++sourceIndex)
                out.push_back(lines[sourceIndex]);
            sourceIndex = startIdx + oldLen;
            for (const std::string& text : newSegment)
                out.push_back({text, preferred});
        }
        for (; sourceIndex < lines.size(); ++sourceIndex)
            out.push_back(lines[sourceIndex]);
        lines = std::move(out);
        for (Line& l : lines)
            if (l.ending.empty())
                l.ending = preferred;
    }

    std::string contents() const {
        std::string out;
        for (const Line& l : lines) {
            out += l.text;
            out += l.ending;
        }
        return out;
    }
};

// ------------------------------------------ seek_sequence (port of seek_sequence.rs)
// Exact match first, then trailing-whitespace-tolerant, then fully trimmed,
// then with common Unicode punctuation normalised to ASCII.

std::string normaliseLine(const std::string& s) {
    const std::string t = trimStr(s);
    std::string out;
    out.reserve(t.size());
    size_t i = 0;
    while (i < t.size()) {
        const unsigned char b = (unsigned char)t[i];
        if (b < 0x80) {
            out += (char)b;
            ++i;
            continue;
        }
        uint32_t cp = 0;
        size_t len = 0;
        if (b >= 0xF0 && i + 3 < t.size()) {
            cp = ((uint32_t)(b & 0x07) << 18) | ((uint32_t)(t[i + 1] & 0x3F) << 12) |
                 ((uint32_t)(t[i + 2] & 0x3F) << 6) | (uint32_t)(t[i + 3] & 0x3F);
            len = 4;
        } else if (b >= 0xE0 && i + 2 < t.size()) {
            cp = ((uint32_t)(b & 0x0F) << 12) | ((uint32_t)(t[i + 1] & 0x3F) << 6) |
                 (uint32_t)(t[i + 2] & 0x3F);
            len = 3;
        } else if (b >= 0xC0 && i + 1 < t.size()) {
            cp = ((uint32_t)(b & 0x1F) << 6) | (uint32_t)(t[i + 1] & 0x3F);
            len = 2;
        }
        if (len == 0) {
            out += (char)b;
            ++i;
            continue;
        }
        char mapped = 0;
        switch (cp) {
            case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015:
            case 0x2212: mapped = '-'; break;
            case 0x2018: case 0x2019: case 0x201A: case 0x201B: mapped = '\''; break;
            case 0x201C: case 0x201D: case 0x201E: case 0x201F: mapped = '"'; break;
            case 0x00A0: case 0x2002: case 0x2003: case 0x2004: case 0x2005: case 0x2006:
            case 0x2007: case 0x2008: case 0x2009: case 0x200A: case 0x202F: case 0x205F:
            case 0x3000: mapped = ' '; break;
            default: break;
        }
        if (mapped) {
            out += mapped;
            i += len;
        } else {
            out.append(t, i, len);
            i += len;
        }
    }
    return out;
}

std::optional<size_t> seekSequence(const std::vector<std::string>& lines,
                                   const std::vector<std::string>& pattern,
                                   size_t start,
                                   bool eof) {
    if (pattern.empty())
        return start;
    if (pattern.size() > lines.size())
        return std::nullopt;
    size_t searchStart = start;
    if (eof)
        searchStart = std::max(lines.size() - pattern.size(), start);
    if (searchStart > lines.size() - pattern.size())
        return std::nullopt;
    const size_t last = lines.size() - pattern.size();
    for (size_t i = searchStart; i <= last; ++i) {
        bool ok = true;
        for (size_t k = 0; k < pattern.size(); ++k)
            if (lines[i + k] != pattern[k]) { ok = false; break; }
        if (ok)
            return i;
    }
    for (size_t i = searchStart; i <= last; ++i) {
        bool ok = true;
        for (size_t k = 0; k < pattern.size(); ++k)
            if (trimEndStr(lines[i + k]) != trimEndStr(pattern[k])) { ok = false; break; }
        if (ok)
            return i;
    }
    for (size_t i = searchStart; i <= last; ++i) {
        bool ok = true;
        for (size_t k = 0; k < pattern.size(); ++k)
            if (trimStr(lines[i + k]) != trimStr(pattern[k])) { ok = false; break; }
        if (ok)
            return i;
    }
    for (size_t i = searchStart; i <= last; ++i) {
        bool ok = true;
        for (size_t k = 0; k < pattern.size(); ++k)
            if (normaliseLine(lines[i + k]) != normaliseLine(pattern[k])) { ok = false; break; }
        if (ok)
            return i;
    }
    return std::nullopt;
}

// ------------------------------- replacements (port of file_update.rs compute_replacements)
// Returns "" on success; on failure the error text mirrors codex's.

struct Replacement {
    size_t start = 0;
    size_t oldLen = 0;
    std::vector<std::string> newLines;
};

std::string computeReplacements(const std::vector<std::string>& originalLines,
                                const std::string& path,
                                const std::vector<UpdateChunk>& chunks,
                                std::vector<Replacement>& replacements) {
    size_t lineIndex = 0;
    for (const UpdateChunk& chunk : chunks) {
        if (chunk.hasContext) {
            auto idx = seekSequence(originalLines, {chunk.changeContext}, lineIndex, false);
            if (!idx)
                return "Failed to find context '" + chunk.changeContext + "' in " + path;
            lineIndex = *idx + 1;
        }
        if (chunk.oldLines.empty()) {
            replacements.push_back({originalLines.size(), 0, chunk.newLines});
            continue;
        }
        // A patch chunk's old_lines often end with an empty string that stands
        // for the final newline of the region; SourceFile keeps terminators on
        // the preceding line, so retry without that sentinel element.
        std::vector<std::string> shorterPattern;
        std::vector<std::string> shorterNew;
        const std::vector<std::string>* pattern = &chunk.oldLines;
        const std::vector<std::string>* newSlice = &chunk.newLines;
        auto found = seekSequence(originalLines, *pattern, lineIndex, chunk.isEndOfFile);
        if (!found && pattern->back().empty()) {
            shorterPattern.assign(pattern->begin(), pattern->end() - 1);
            pattern = &shorterPattern;
            if (!newSlice->empty() && newSlice->back().empty()) {
                shorterNew.assign(newSlice->begin(), newSlice->end() - 1);
                newSlice = &shorterNew;
            }
            found = seekSequence(originalLines, *pattern, lineIndex, chunk.isEndOfFile);
        }
        if (!found)
            return "Failed to find expected lines in " + path + ":\n" +
                   joinLines(chunk.oldLines);
        const size_t startIdx = *found;
        // Context lines occur on both sides of the chunk: keep the original
        // lines in place (exact contents and terminators survive) and only
        // replace the runs between them.
        size_t oldStart = 0, newStart = 0;
        for (const auto& [oldContext, newContext] : chunk.contextIndices) {
            if (oldContext >= pattern->size() || newContext >= newSlice->size())
                break;
            if (oldStart != oldContext || newStart != newContext)
                replacements.push_back(
                    {startIdx + oldStart, oldContext - oldStart,
                     std::vector<std::string>(newSlice->begin() + newStart,
                                              newSlice->begin() + newContext)});
            oldStart = oldContext + 1;
            newStart = newContext + 1;
        }
        if (oldStart != pattern->size() || newStart != newSlice->size())
            replacements.push_back(
                {startIdx + oldStart, pattern->size() - oldStart,
                 std::vector<std::string>(newSlice->begin() + newStart, newSlice->end())});
        lineIndex = startIdx + pattern->size();
    }
    std::stable_sort(replacements.begin(), replacements.end(),
                     [](const Replacement& a, const Replacement& b) { return a.start < b.start; });
    return {};
}

// ------------------------------------------------------------------ fs helpers

bool readFileText(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

void createParentDirs(const std::string& path) {
    const size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0)
        return;
    std::error_code ec;
    std::filesystem::create_directories(path.substr(0, slash), ec);
}

bool writeFileText(const std::string& path, const std::string& content, std::string& error) {
    createParentDirs(path);
    const std::string tmp = path + ".minipatch.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            error = "Failed to write file " + path;
            return false;
        }
        f.write(content.data(), (std::streamsize)content.size());
        f.close();
        if (!f) {
            error = "Failed to write file " + path;
            remove(tmp.c_str());
            return false;
        }
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        error = "Failed to write file " + path;
        return false;
    }
    return true;
}

struct UpdateResult {
    bool ok = false;
    std::string error;
    std::string newContents;
};

UpdateResult deriveNewContents(const std::string& path,
                               const std::vector<UpdateChunk>& chunks) {
    UpdateResult result;
    std::string original;
    if (!readFileText(path, original)) {
        result.error = "Failed to read file to update " + path;
        return result;
    }
    SourceFile file = SourceFile::parse(original);
    std::vector<Replacement> replacements;
    std::string err =
        computeReplacements(file.lineTexts(), path, chunks, replacements);
    if (!err.empty()) {
        result.error = err;
        return result;
    }
    std::vector<std::tuple<size_t, size_t, std::vector<std::string>>> tupleReplacements;
    tupleReplacements.reserve(replacements.size());
    for (const Replacement& r : replacements)
        tupleReplacements.emplace_back(r.start, r.oldLen, r.newLines);
    file.applyReplacements(tupleReplacements);
    result.ok = true;
    result.newContents = file.contents();
    return result;
}

} // namespace

Tool makeApplyPatchTool(const std::string& cwd) {
    Tool t;
    t.name = "apply_patch";
    t.description =
        "Apply a patch that creates, updates, moves, or deletes files. Format:\n"
        "*** Begin Patch\n"
        "*** Add File: <path>\n"
        "+<content line>\n"
        "*** Delete File: <path>\n"
        "*** Update File: <path>\n"
        "*** Move to: <path> (optional, inside an update hunk, before the chunk)\n"
        "@@ <line of the file the chunk sits after> (optional anchor)\n"
        " <context line> (leading space: unchanged line, must match the file)\n"
        "-<line to remove>\n"
        "+<line to add>\n"
        "*** End of File (optional: the chunk matches at end of file)\n"
        "*** End Patch\n"
        "A file may have several chunks (repeat @@ sections); a patch may touch several "
        "files. Read the file first and copy context and '-' lines from its current "
        "content. Matching is tolerant of whitespace and unicode punctuation, but a chunk "
        "that cannot be located fails the patch with the region that was expected.";
    t.paramsSchema = {
        {"type", "object"},
        {"properties", {
            {"patch", {{"type", "string"}, {"description", "The patch text (apply_patch format)"}}},
        }},
        {"required", {"patch"}},
    };
    t.execute = [cwd](nlohmann::json args, const std::atomic<bool>& /*abort*/) -> ToolResult {
        if (!args.contains("patch") || !args["patch"].is_string())
            return {"Missing 'patch' argument", true};
        const std::string patch = args["patch"].get<std::string>();

        ParseOutcome parsed = parsePatch(patch);
        if (!parsed.ok)
            return {parsed.error, true};
        if (parsed.hunks.empty())
            return {"No files were modified.", true};

        std::vector<std::string> added, modified, deleted;
        for (const Hunk& hunk : parsed.hunks) {
            const std::string path = resolvePath(cwd, hunk.path);
            switch (hunk.kind) {
                case Hunk::Kind::Add: {
                    std::string err;
                    if (!writeFileText(path, hunk.contents, err))
                        return {err, true};
                    added.push_back(hunk.path);
                    break;
                }
                case Hunk::Kind::Delete: {
                    struct stat st;
                    if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
                        return {"Failed to delete file " + path, true};
                    if (remove(path.c_str()) != 0)
                        return {"Failed to delete file " + path, true};
                    deleted.push_back(hunk.path);
                    break;
                }
                case Hunk::Kind::Update: {
                    if (hunk.chunks.empty())
                        return {"Update file hunk for path '" + hunk.path + "' is empty", true};
                    UpdateResult updated = deriveNewContents(path, hunk.chunks);
                    if (!updated.ok)
                        return {updated.error, true};
                    std::string err;
                    if (hunk.hasMove) {
                        const std::string dest = resolvePath(cwd, hunk.movePath);
                        if (!writeFileText(dest, updated.newContents, err))
                            return {err, true};
                        if (remove(path.c_str()) != 0) {
                            return {"Failed to remove original " + path +
                                        " (destination was written)", true};
                        }
                    } else {
                        if (!writeFileText(path, updated.newContents, err))
                            return {err, true};
                    }
                    modified.push_back(hunk.affectedPath());
                    break;
                }
            }
        }

        std::string out = "Success. Updated the following files:";
        for (const std::string& p : added)
            out += "\nA " + p;
        for (const std::string& p : modified)
            out += "\nM " + p;
        for (const std::string& p : deleted)
            out += "\nD " + p;
        return {out, false};
    };
    return t;
}
