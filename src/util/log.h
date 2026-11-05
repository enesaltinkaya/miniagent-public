#pragma once

#include <cstdarg>
#include <cstdio>

// Line-oriented stderr logging for the renderer-neutral core (App and both
// frontends). Replaces SDL_Log so app.cpp carries no SDL dependency; output
// format matches SDL_Log's plain unformatted variant (message + newline).
inline void logLine(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}
