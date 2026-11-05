#include "app.h"

#include "debug_hooks.h"
#include "tui/tui.h"

// The terminal frontend entry (dispatched from main.cpp; the default since
// M1, PLAN_TUI.md §9). Constructs the shared App, applies the MINIAGENT_*
// debug hooks (same states as the desktop frontend, so render tests can
// drive both identically) and runs the FTXUI fullscreen loop. Everything
// inside tui::runFrontend happens on the FTXUI loop thread; on return the
// ticker is already joined, and app.shutdown() is the last App call.
int runTui() {
    App app;
    seedDebugHooks(app);
    const int rc = tui::runFrontend(app);
    app.shutdown();
    return rc;
}
