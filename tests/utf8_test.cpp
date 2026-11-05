#include "util/utf8.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <string>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

int main() {
    // Valid UTF-8 passes through unchanged (ASCII, multibyte, combining).
    std::string clean = "hello \xC3\xA9 \xE2\x86\x92 \xF0\x9F\x98\x80";
    CHECK(sanitizeUtf8(clean) == clean);
    CHECK(sanitizeUtf8("") == "");
    CHECK(sanitizeUtf8("plain ascii") == "plain ascii");

    const std::string fffd = "\xEF\xBF\xBD";

    // 0xFF (the byte from the crash) becomes U+FFFD.
    std::string one = "\xFF";
    CHECK(sanitizeUtf8(one) == fffd);

    // Binary garbage: every bad byte replaced, valid neighbours kept.
    {
        std::string in = "a";
        in += "\xFF\xFE";
        in += "b\x00c";
        std::string out = sanitizeUtf8(in);
        // a + FFFD + FFFD + b + (NUL is valid ASCII) + c
        CHECK(out == std::string("a") + fffd + fffd + "b\x00c");
    }

    // Lone continuation byte.
    CHECK(sanitizeUtf8(std::string("\x80")) == fffd);

    // Truncated multibyte sequence at end of input.
    {
        std::string in = "x\xC3";
        std::string out = sanitizeUtf8(in);
        CHECK(out == std::string("x") + fffd);
    }

    // Overlong encoding of 'A' (0xC0 0x81) and surrogate range (ED A0 80).
    {
        std::string in("\xC0\x81");
        CHECK(sanitizeUtf8(in) == fffd + fffd);
        std::string surr("\xED\xA0\x80");
        CHECK(sanitizeUtf8(surr) == fffd + fffd + fffd);
    }

    // Max valid code point U+10FFFF.
    {
        std::string in("\xF4\x8F\xBF\xBF");
        CHECK(sanitizeUtf8(in) == in);
        std::string bad("\xF4\x90\x80\x80");
        std::string out = sanitizeUtf8(bad);
        CHECK(out.size() == 12); // 4 x U+FFFD
    }

    // Result is always valid UTF-8 and dumpable under strict nlohmann rules.
    {
        std::string dirty = "ok \xFF mid \xE2\x65\x90 ok";
        std::string out = sanitizeUtf8(dirty);
        // \xE2\x65\x90 is an invalid 3-byte sequence: E2 must be followed by
        // 80-BF, so it is replaced; 0x65 is valid ASCII and passes through;
        // 0x90 is a stray continuation byte.
        CHECK(out == "ok " + fffd + " mid " + fffd + "e" + fffd + " ok");
    }

    // Regression for the 0xFF crash: strict dump() throws type_error.316 on
    // raw bytes; the replace handler must not, and sanitizeUtf8 must make the
    // strict path safe too.
    {
        std::string dirty = "tool output \xFF at byte 8481";
        nlohmann::json j{{"content", dirty}};
        bool threw = false;
        try {
            j.dump();
        } catch (const nlohmann::json::exception&) {
            threw = true;
        }
        CHECK(threw);
        std::string replaced = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
        CHECK(replaced.find(fffd) != std::string::npos);
        nlohmann::json safe{{"content", sanitizeUtf8(dirty)}};
        std::string strictDump = safe.dump();
        std::string refDump = nlohmann::json{{"content", dirty}}.dump(
            -1, ' ', false, nlohmann::json::error_handler_t::replace);
        CHECK(strictDump == refDump);
    }

    if (failures == 0)
        std::printf("utf8_test: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
