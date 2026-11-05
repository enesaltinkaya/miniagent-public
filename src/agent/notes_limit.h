#pragma once

#include <cstddef>
#include <string>

// Hard cap (in characters) for the scaffold ledger's notes.md. One number for
// the whole app: the role prompts (agents/*.md, via the {{NOTES_LIMIT}}
// placeholder) and the scaffold's compaction prompt both read it, so the cap
// cannot drift apart.
inline constexpr size_t kNotesCharLimit = 15000;

// The cap as it appears in prompt text: "15,000".
inline std::string notesCharLimitText() {
    const std::string digits = std::to_string(kNotesCharLimit);
    std::string out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return out;
}

// Replaces every {{NOTES_LIMIT}} placeholder in a prompt with the cap.
inline std::string fillNotesLimit(std::string s) {
    const std::string limit = notesCharLimitText();
    size_t p = 0;
    while ((p = s.find("{{NOTES_LIMIT}}", p)) != std::string::npos) {
        s.replace(p, 15, limit);
        p += limit.size();
    }
    return s;
}
