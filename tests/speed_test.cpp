// Regression test for the tok/s speed accounting (status strip).
//
// Three fixes are locked in here:
//  1. Output tokens were estimated at chars/4, but real output (prose + code)
//     encodes ~3.2-3.8 chars per token, so the rate read ~10-20% low against
//     the backend's token-accurate figure. The estimator is now chars/3.5.
//  2. The true cumulative token count (server usage chunk) arrived as one late
//     sample; it was either ignored (servers that never stream usage -> the
//     number stayed under-counted all turn) or added a false spike. The retained
//     window samples are now rebased onto the true count -> token-accurate and
//     spike-free.
//  3. The TurnEnd whole-turn average included the time-to-first-token; it now
//     measures the generation span (firstTokenAt -> lastTokenAt).
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "app.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

namespace {
// Mirrors estimateOutputTokens() in app.cpp (ceil(chars / 4)).
int estOut(std::size_t chars) { return static_cast<int>((chars + 3) / 4); }
// Mirrors the app's live estimate: max(delta count, chars/4). Backends that
// batch several tokens per SSE chunk (vLLM) are covered by the chars/4 term.
int estLive(int deltas, std::size_t chars) { return std::max(deltas, estOut(chars)); }
double spanSecs(const App::SpeedSample& a, const App::SpeedSample& b) {
    return std::chrono::duration_cast<std::chrono::duration<double>>(b.t - a.t).count();
}
} // namespace

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1); // never start the user's real MCP servers in tests
    setenv("MINIAGENT_NO_PROBE", "1", 1);  // no real network probe from a headless App
    system("rm -rf /tmp/miniagent_speed_test && mkdir -p /tmp/miniagent_speed_test");
    chdir("/tmp/miniagent_speed_test");
    App app;

    AgentEvent turn;
    turn.type = AgentEventType::TurnStart;
    app.processEventForTest(turn);

    // ---- 1. Live windowed rate while streaming (no server usage yet). ----
    // 8 chunks of 40 chars ~100ms apart -> streamedChars 320, span ~0.7s.
    AgentEvent delta;
    delta.type = AgentEventType::TextDelta;
    delta.text = std::string(40, 'a');
    for (int i = 0; i < 8; ++i) {
        app.processEventForTest(delta);
        usleep(100 * 1000);
    }

    const double live = app.liveTokPerS;
    check(live > 0.0, "live tok/s computed while streaming");
    if (live > 0.0) {
        const auto& first = app.speedSamples.front();
        const auto& last  = app.speedSamples.back();
        const double span = spanSecs(first, last);
        // Exact window math: (last.out - first.out) / span.
        check(std::fabs(live - (last.out - first.out) / span) < 0.001,
              "live rate matches the retained-window math");
        // The live estimate is max(deltas, chars/4): 8 deltas vs
        // ceil(320/4)=80 -> the char estimate dominates (batched chunks).
        int sample = 0;
        for (int i = 1; i <= 8; ++i) sample = estLive(i, (std::size_t)i * 40);
        check(last.out == sample,
              "live estimate is max(delta count, chars/4)");
    }

    // ---- 2. Server usage chunk arrives: window rebased onto true tokens. ----
    const int trueTokens = 120;                  // server's token-accurate count
    const int estNow     = estOut(app.streamedChars); // est(320) = 80
    const int scaledFirst =
        (estOut(40) * trueTokens + estNow / 2) / estNow; // 10 -> 15
    AgentEvent usage;
    usage.type               = AgentEventType::UsageDelta;
    usage.usage.outputTokens = trueTokens;
    app.processEventForTest(usage);

    const double afterUsage = app.liveTokPerS;
    const auto& first2      = app.speedSamples.front();
    const auto& last2       = app.speedSamples.back();
    const double span2      = spanSecs(first2, last2);
    check(afterUsage > live * 1.05,
          "usage chunk lifts the rate toward the server's count");
    check(std::fabs(afterUsage - (trueTokens - scaledFirst) / span2) < 0.001,
          "usage-rebased rate matches the rescaled window, no spike");
    check(last2.out == trueTokens, "last sample carries the true token count");

    // ---- 3. TurnEnd freezes the token-accurate window rate. ----
    AgentEvent end;
    end.type               = AgentEventType::TurnEnd;
    end.usage.outputTokens = trueTokens;
    end.usage.totalTokens  = trueTokens;
    app.processEventForTest(end);
    check(app.lastTokPerS > 0.0 && std::fabs(app.lastTokPerS - afterUsage) < 0.001,
          "TurnEnd freezes the window rate into lastTokPerS");
    check(app.speedSamples.empty() && app.liveTokPerS < 0.0,
          "TurnEnd resets the live window");

    // ---- 4. Short turn: whole-turn fallback excludes time-to-first-token. ----
    // A fresh turn that streams less than 0.5s of output (the window never
    // fills) after a ~300ms "thinking before the first token" pause.
    app.processEventForTest(turn); // second TurnStart resets the speed state
    usleep(300 * 1000);            // TTFT: no output, must not count toward the rate
    for (int i = 0; i < 3; ++i) {  // ~200ms of generation -> span < 0.5s
        app.processEventForTest(delta);
        usleep(100 * 1000);
    }
    check(app.liveTokPerS < 0.0, "short turn does not fill the window");
    const double genS =
        std::chrono::duration_cast<std::chrono::duration<double>>(app.lastTokenAt - app.firstTokenAt)
            .count();
    const int outEst = estLive(3, app.streamedChars); // max(3, ceil(120/4)) = 30
    AgentEvent end2;
    end2.type = AgentEventType::TurnEnd;
    app.processEventForTest(end2);
    check(app.lastTokPerS > 0.0 && std::fabs(app.lastTokPerS - outEst / genS) < 1.0,
          "TurnEnd fallback divides by the generation span (no TTFT)");

    printf(failures ? "FAIL (%d)\n" : "PASS\n", failures);
    return failures ? 1 : 0;
}