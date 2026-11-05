#include "util/utf8.h"

#include <cstdint>
#include <string>

namespace {
const std::string kFffd = "\xEF\xBF\xBD";
}

std::string sanitizeUtf8(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    const unsigned char* p = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        size_t len = 0;
        unsigned cp = 0;
        bool ok = false;
        if (c < 0x80) {
            len = 1;
            cp = c;
            ok = true;
        } else if (c >= 0xC2 && c <= 0xDF) {
            len = 2;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            len = 3;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            len = 4;
            cp = c & 0x07;
        }
        if (len == 0) {
            // Stray continuation byte or invalid start byte (0xC0/0xC1/
            // 0xF5-0xFF): replace it and rescan from the next byte.
            out += kFffd;
            i++;
            continue;
        }
        ok = true;
        for (size_t k = 1; k < len; k++) {
            if (i + k >= n || (p[i + k] & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (p[i + k] & 0x3F);
        }
        if (ok) {
            if (len == 2 && cp < 0x80)
                ok = false;
            if (len == 3 && cp < 0x800)
                ok = false;
            if (len == 4 && cp < 0x10000)
                ok = false;
            if (cp > 0x10FFFF)
                ok = false;
            if (cp >= 0xD800 && cp <= 0xDFFF)
                ok = false;
        }
        if (ok) {
            out.append(in.data() + i, len);
            i += len;
        } else {
            // Reject the maximal candidate and rescan from the next byte, so
            // a stray continuation byte becomes its own replacement char.
            out += kFffd;
            i++;
        }
    }
    return out;
}
