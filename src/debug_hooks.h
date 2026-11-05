#pragma once

class App;

// Applies the MINIAGENT_* debug hooks (startup modals, fake tool/scaffold
// rows, startup prompt, toast, ...) to a freshly constructed App. Shared by
// both frontends (desktop_main.cpp / tui_main.cpp) so the verify-ui
// screenshot workflow and the TUI render tests can drive identical states.
void seedDebugHooks(App& app);
