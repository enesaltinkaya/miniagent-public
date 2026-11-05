#pragma once

// The FTXUI fullscreen frontend (PLAN_TUI.md §4 "Event loop"). Everything
// in here runs on the FTXUI loop thread: the shared App is driven only
// from that thread (tick / sends / toggles), while a jthread ticker just
// posts a custom event every 100 ms to wake the loop for streaming output,
// elapsed timers and redraws.

struct App;

namespace tui {

// Runs the fullscreen loop until the user quits (Ctrl+Q; Ctrl+C follows
// the same confirm-while-running path). Returns the process exit code.
// Shutdown ordering on return: the loop has exited and the ticker is
// stopped and joined; the caller then runs app.shutdown().
int runFrontend(App& app);

} // namespace tui
