// Unit test for App::onEscape: double-Esc opens the session tree, Esc closes
// it again, single Esc (while idle) does nothing. No LLM or window required.
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <thread>

#include "app.h"

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("%s: %s\n", cond ? "  ok" : "  FAIL", what);
    if (!cond)
        failures++;
}

int main() {
    setenv("MINIAGENT_NO_MCP", "1", 1);  // never start the user's real MCP servers in tests
    setenv("MINIAGENT_NO_PROBE", "1", 1);  // no real network probe from a headless App
    App app;
    check(!app.treeOpen, "tree starts closed");

    // Single Esc while idle: no tree.
    app.onEscape();
    check(!app.treeOpen, "single Esc (idle) does not open tree");

    // Second Esc within the 400ms window: opens the tree.
    app.onEscape();
    check(app.treeOpen, "double Esc opens the tree");
    check(app.treeSelection == -1, "tree starts with no selection");

    // Esc while the tree is up: closes it (and resets any selection).
    app.treeSelection = 2;
    app.onEscape();
    check(!app.treeOpen, "Esc while tree is open closes it");
    check(app.treeSelection == -1, "Esc resets tree selection");

    // The close-Esc must not count toward a double-Esc: a single press right
    // after closing must not immediately reopen the tree.
    app.onEscape();
    check(!app.treeOpen, "single Esc right after close does not reopen");

    // The press above armed the 400ms window, so this one (within 400ms of it)
    // reopens the tree.
    app.onEscape();
    check(app.treeOpen, "Esc within 400ms of previous press reopens the tree");

    // Slower than the 400ms window: presses with a long gap stay "single".
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    app.onEscape();
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    app.onEscape();
    check(!app.treeOpen, "two Escs more than 400ms apart do not open");

    // The sessions modal closes on a single Esc, and that close-Esc (like the
    // tree's) resets the double-Esc timer, so a later single press stays
    // single instead of reopening the tree.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    app.sessionsModalOpen = true;
    app.onEscape();
    check(!app.sessionsModalOpen, "Esc while the sessions modal is open closes it");
    app.onEscape();
    check(!app.treeOpen, "single Esc right after closing the modal stays single");

    if (failures == 0)
        printf("escape_test: all passed\n");
    else
        printf("escape_test: %d failures\n", failures);
    return failures ? 1 : 0;
}
