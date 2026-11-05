# AGENTS.md

Guidance for AI coding agents working in this repository.

## Project

`miniagent` is a C++20 CLI agent app with two frontends over a shared core
(`PLAN_TUI.md` is the authoritative spec). The binary starts in the SDL3 +
Dear ImGui window by default; `--tui` runs the
terminal UI instead. Sources live in
`src/`:

- `src/main.cpp` — arg parsing for both frontends; `src/desktop_main.cpp` —
  SDL/GL/ImGui loop; `src/tui_main.cpp` + `src/tui/` — FTXUI fullscreen loop
  (ticker + event routing in `tui.cpp`, renderer-neutral row/overlay
  builders in `tui_view.cpp`, markdown-lite in `rich_text_tui.cpp`)
- `src/app.{h,cpp}` — the shared, renderer-neutral App (state machine,
  sessions, events); `App::tick()` is the frontend-neutral frame advance.
  The input bar's `Loop` checkbox restarts the last prompt `loopDelayS` (10 s)
  after a run settles, in a fresh session each iteration (context cleared;
  `fireLoop()` calls `newSession()` before the replay): armed at the parent
  `AgentEnd`, fired from `tick()` (no timer thread), cancelled by unchecking /
  Esc / Stop / a session switch, and it
  unchecks itself after `kLoopErrorLimit` error runs in a row. Session only, never
  persisted; covered by `loop_test`
- `src/ui/` — the desktop renderer (`draw_*.cpp`) plus `view_rows.{h,cpp}`,
  the neutral row model both frontends share (tool summaries, trim, elapsed)
- `src/agent/` — agent loop (tools, turns)
- `src/llm/` — OpenAI client + SSE parsing
- `src/tools/` — built-in tools (read, write, edit, grep, bash, web_search,
  web_fetch). The two web tools talk to the local SearXNG instance configured by
  `searxng_url` in `~/.config/miniagent/miniagent.settings` (default
  `http://localhost:7070`; `SEARXNG_URL` overrides, `off` unregisters them) and
  are registered for the main agent only. `apply_patch=1` in the same settings
  file swaps the edit tool for the codex-style `apply_patch` tool
  (`src/tools/apply_patch.cpp`, a port of codex-rs/apply-patch: patch-format
  add/delete/move/update with fuzzy line matching); effective on the next
  prompt.
- `src/util/` — HTTP, process, and screenshot helpers
- `tests/` — standalone test executables (`sse_test`, `llm_test`, `agent_test`,
  `app_test`, `tui_render_test`, …)

`./build/miniagent` and `scripts/run.sh` open the SDL/ImGui window; `--tui`
selects the terminal UI, which takes over the pane and must never be run from
the agent's own terminal (see the `verify-tui` skill for safe workflows;
`scripts/run-desktop.sh` is the same as passing no flag).

## Build & run

```bash
cmake -S . -B build          # configure (one-time; downloads pinned deps)
scripts/build.sh             # incremental build
scripts/run.sh               # build, then run ./build/miniagent
cmake --build build          # plain incremental build
```

`io.IniFilename` is set to `nullptr` in `src/main.cpp`, so no `imgui.ini` is
created or read — window/panel layout does not persist across runs.

## Skills

Implemented in `src/agent/skills.{h,cpp}` (ported from pi's
`packages/coding-agent/src/core/skills.ts`). `buildSystemPrompt()` in
`src/agent/system_prompt.h` appends both the SKILL.md contract (`skillsSection()`)
and the `<available_skills>` block; `App::sendPrompt()` expands
`/skill:name <args>` and `App::listSkills()` backs the `/skills` command. Skills
are re-scanned on every prompt, so editing a `SKILL.md` takes effect on the next
send. Subagent and scaffold roles get the same advertisement (without the
authoring contract) via `roleSystemPrompt()` in `src/tools/subagent.cpp`.
Tests: `skills_test`, `role_prompt_test`.

Project skills: `verify-ui` (screenshot workflow), `imgui-widgets` (demo-file
reference), `build-and-test` (build targets and what each test covers). Keep
the how-to detail in the skill files, not here - AGENTS.md is in context on
every run, a skill body only after it is loaded.

Dependencies (SDL3, Dear ImGui, FTXUI, stb) are fetched automatically at
configure time via CMake `FetchContent` and pinned to known-good commits; see
`CMakeLists.txt` for the pinned versions and the `*ROOT` overrides.
