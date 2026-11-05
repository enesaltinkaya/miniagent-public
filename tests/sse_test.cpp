#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "llm/sse.h"

static int failures = 0;

static void check(bool cond, const char* what) {
    if (cond) {
        printf("  ok: %s\n", what);
    } else {
        printf("  FAIL: %s\n", what);
        failures++;
    }
}

int main() {
    // 1. Basic single-event stream, split at awkward byte boundaries.
    {
        std::vector<SSEEvent> events;
        SSEParser parser([&](SSEEvent e) {
            events.push_back(std::move(e));
        });
        const std::string stream =
            "data: {\"a\":1}\n"
            "\n"
            ": comment line\n"
            "data: {\"b\":2}\n"
            "\n";
        // Feed one byte at a time to stress boundary handling.
        for (char c : stream)
            parser.feed(&c, 1);
        check(events.size() == 2, "two events parsed");
        if (events.size() == 2) {
            check(events[0].data == "{\"a\":1}", "first event data");
            check(events[1].data == "{\"b\":2}", "second event data");
            check(events[0].name.empty(), "empty event name");
        }
    }

    // 2. Multi-line data field joined with \n.
    {
        std::vector<SSEEvent> events;
        SSEParser parser([&](SSEEvent e) {
            events.push_back(std::move(e));
        });
        parser.feed("event: x\ndata: line1\ndata: line2\n\n");
        check(events.size() == 1, "one event");
        if (!events.empty()) {
            check(events[0].name == "x", "event name captured");
            check(events[0].data == "line1\nline2", "multi-line data joined");
        }
    }

    // 3. CRLF line endings and data without space after colon.
    {
        std::vector<SSEEvent> events;
        SSEParser parser([&](SSEEvent e) {
            events.push_back(std::move(e));
        });
        parser.feed("data:hello\r\n\r\n");
        check(events.size() == 1 && events[0].data == "hello", "CRLF + no-space data");
    }

    // 4. [DONE] sentinel and a blank-data event should not dispatch spurious events.
    {
        std::vector<SSEEvent> events;
        SSEParser parser([&](SSEEvent e) {
            events.push_back(std::move(e));
        });
        parser.feed("\n\ndata: [DONE]\n\n\n");
        check(events.size() == 1, "only [DONE] event");
        if (!events.empty())
            check(events[0].data == "[DONE]", "[DONE] payload");
    }

    // 5. Event name persists across the blank line only for that event.
    {
        std::vector<SSEEvent> events;
        SSEParser parser([&](SSEEvent e) {
            events.push_back(std::move(e));
        });
        parser.feed("event: named\n\ndata: d\n\n");
        check(events.size() == 2, "name-only event dispatches with empty data");
        if (events.size() == 2) {
            check(events[0].name == "named" && events[0].data.empty(), "first event has name, empty data");
            check(events[1].name.empty() && events[1].data == "d", "second event independent");
        }
    }

    if (failures == 0)
        printf("sse_test: all passed\n");
    else
        printf("sse_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
