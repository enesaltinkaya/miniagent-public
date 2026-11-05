# TUI Frontend Plan

Replicate the desktop GUI as a terminal UI. Both routes stay first-class and
share everything below the rendering layer. The binary currently starts in the
SDL3/ImGui window by default; `--tui` runs the terminal frontend (this plan
originally shipped TUI as the default; see §9.2).

---

## 1. Why this is feasible (current-state facts)

- `src/agent/`, `src/llm/`, `src/tools/`, `src/sessions/`, `src/mcp/` have
  **zero** ImGui/SDL includes. `agent_test`, `toolrows_test`, `app_test`, …
  already drive `App` headlessly.
- `App` (`src/app.{h,cpp}`) is renderer-neutral except:
  - `App::Frame()` — ImGui draw orchestration (the only `ImGui::` block in
    `app.cpp`).
  - `ChordPressed()` (`src/ui/chords.cpp`) — reads ImGui key state.
  - draw-only fields in `UiMessage`: `richLayout`, `advanceCache`,
    `msgLayoutWidth` (harmless leftovers a TUI ignores).
  - `SDL_Log` (3x), `SDL_getenv` (1x), `SDL_SetClipboardText` (2x in
    copy-helpers).
- Tool-row summaries (`toolFile`/`toolCommand` extraction + live streaming
  scan + `TrimMessage`) live in `draw_message_log.cpp` — must be extracted to
  be shared.
- Slash commands (`/new`, `/skills`, `/skill:name`) are parsed in
  `sendPrompt()` → free in the TUI.

## 2. Target architecture

```
main.cpp                 arg parsing: [--desktop|--gui|-g] [workdir] → dispatch
├── desktop_main.cpp     current SDL3/GL/ImGui loop, unchanged behavior
└── tui_main.cpp         FTXUI fullscreen loop
        │
        └── App (shared): sendPrompt, processEvent, sessions, MCP, skills,
            scaffold, backends, tool registry, uiMessages, history
                │
                ├── src/ui/draw_*.cpp      (desktop renderer, as today)
                ├── src/ui/view_rows.{h,cpp}  NEW renderer-neutral row model
                └── src/tui/*.{h,cpp}      NEW TUI renderer
```

One binary, both frontends linked in.

### Library: FTXUI

- Vendored at `build/_deps/ftxui-src` (fetched
  convention: sources under cpp-thirdparty), via `add_subdirectory`.
- Reasons: pure C++, component/event model, built-in SGR-1006 mouse,
  automatic screen diffing, multiline `Input`, modal support, no ncurses C
  API. Rejected alternatives: raw ANSI (input editor + mouse parsing + wide
  chars is weeks of work), ncurses (C ergonomics, weak C++ widgets),
  Notcurses (heavyweight, media-centric).

## 3. Groundwork refactor (M0 — desktop unchanged, all tests green)

1. **Split `main.cpp`**: keep arg parsing + shared early init
   (`EnsureConfigDir`, workdir `chdir`); move the SDL/GL/ImGui body verbatim
   to `desktop_main.cpp` as `int runDesktop(int argc, char** argv)`;
   `software_render` setting applies there only. New `tui_main.cpp` returns
   `int runTui()`.
   - Move the `MINIAGENT_FAKE_*` / `MINIAGENT_SCREENSHOT` debug-hook seeding
     into a shared `seedDebugHooks(App&)` (both frontends call it; keeps
     verify-ui and enables TUI render tests).
2. **Extract `App::tick()`** from `Frame()`: `pumpEvents()`, `mcpDirty`
   rebuild, 2s session-list refresh, scaffold-resume refresh. `Frame()` calls
   `tick()` then draws; the TUI ticker calls `tick()` directly. Key chords
   stay per-frontend (desktop: ImGui IO; TUI: FTXUI events) calling the
   existing `toggleToolExpansion/toggleThinking/newSession`.
3. **Extract row model** from `draw_message_log.cpp` into
   `src/ui/view_rows.{h,cpp}` (no ImGui includes): collapsed tool summary
   (`toolFile`/`toolCommand` + live streaming-args scan, already cached in
   `UiMessage`), `TrimMessage`, error/elapsed formatting, duration tags.
   `draw_message_log.cpp` calls these; TUI reuses them 1:1.
4. **`App::sendPromptText(const std::string&)`**: fills `inputBuf` then calls
   `sendPrompt()`. The TUI keeps its own editor buffer (std::string, no 4096
   limit reached in UI).
5. **Clipboard abstraction**: replace direct `SDL_SetClipboardText` in the
   copy helpers with a settable `std::function` (desktop: SDL; TUI: OSC 52
   escape sequence, works in kitty/alacritty/gnome-terminal/via tmux passthru).
6. **Drop SDL from core**: swap `SDL_Log`/`SDL_getenv` in `app.cpp` for a
   `logLine()` helper (stderr) + `getenv` — after this the App link unit no
   longer needs SDL (tests get cheaper to link).

## 4. TUI design

### Layout (fullscreen, top → bottom)

```
┌ cwd/short • mode [Simple|Scaffold] … (S)essions (M)CP (B)ackends (N)ew (H)elp ┐  top bar
│                                                                                │
│  message log viewport (scroll, auto-follow bottom; un-pins on manual scroll)   │
│                                                                                │
├ state ● working (12.4 tok/s) • ctx 45.2K/131K • 00:37 … Compact (C) / Stop (Esc)┤  status strip
│ > input editor (3 rows, grows to ~10; history via Up/Down from empty line)     │
│ [Enter] Send  [Simple] [Scaffold]  hint line                                    │
└ model • 12.3K tokens • backend-name (clickable → Backends) … cwd …              ┘  bottom line
```

### Event loop

- `ScreenInteractive::Fullscreen()` + `Loop` with `CatchEvent`.
- A `std::jthread` ticker posts a custom event every 100 ms → handler runs
  `app.tick()`; redraw on tick (FTXUI diffs, so idle ticks are cheap).
  Streaming updates, elapsed timers and spinners animate on the tick.

### Rendering mapping (GUI → TUI)

| GUI element | TUI equivalent |
|---|---|
| message log + rich text | colored lines; markdown-lite renderer (`src/tui/rich_text_tui.cpp`): fences → indented dim block, `` `code` `` → cyan, `**bold**` → bold; wrap at terminal width |
| row kinds/colors | User white, Agent default, Thinking dim italic (Ctrl+T), Tool cyan header, Error red, System blue, Warning yellow — mirrors `UiMessage::Kind` |
| tool rows | collapsed summary via shared view_rows; expand: click row / Enter on row / Ctrl+O toggles all; `toolPreviewLines` respected; spinner + live elapsed while running; streaming args show live summary (never raw JSON) |
| expanded `read` image | **skipped**: print `path (WxH)`; model still gets bytes via analyze_image |
| modals | FTXUI `Modal` overlays: Sessions (Enter open, `d` delete), Backends (Enter select, `i` image model), MCP (Space toggle), Help, Session tree (rewind) |
| status strip | state, tok/s (reuse `liveTokPerS`/`lastTokPerS`), context tokens, busy time |
| backend link | clickable bottom-line segment → Backends overlay |
| toast | transient status-strip message (same `TOAST_DURATION_S`) |
| scaffold run view | same dispatch filtering as `scaffoldDisplayDispatch()`: clean page per role, role-log column as an overlay list, click/Enter pins a dispatch for review |
| drag text selection | dropped — native terminal selection (Shift while mouse captured) |
| tooltips | status-strip hint line |

### Input / keys

- Editor: multiline (paste-safe), Up/Down history when on first/last line,
  `promptSendBlocked()` disables send during scaffold runs (editor inert).
- Chords reused: Ctrl+O (expand), Ctrl+T (thinking), Ctrl+N (new session).
- Esc: closes overlay → else stops agent if running (double-Esc opens session
  tree, reusing `onEscape()` semantics mapped to TUI events).
- Quit: Ctrl+Q (confirm if a run is active). Ctrl+C copies-if-selection else
  quit-confirm (pi-style).
- Mode toggle: click, or `Alt+S`? → decide: buttons row is clickable; key
  shortcut optional.

### Mouse

- FTXUI enables SGR-1006 tracking automatically: clickable top-bar buttons,
  bottom-line backend link, tool rows (expand), overlay rows, mode/Compact
  buttons; wheel scrolls the log (un-pins auto-follow).
- Documented caveats: native selection needs Shift held; right/middle-click
  belong to the terminal (Sessions middle-click-delete → `d` key instead).

### Images into the model (parity item, small)

GUI pastes clipboard images as `[image N]`. TUI adds an `/image <path>`
command (or drag-drop path detection) appending the same `PendingImage`
entry + token; `sendPrompt()` needs no change (it already attaches by token).

## 5. CMake / build

- `set(FTXUI_ROOT /path/to/ftxui)` +
  `add_subdirectory(${FTXUI_ROOT} ftxui EXCLUDE_FROM_ALL)`, guarded by
  `if(EXISTS …)` with a configure-time error message otherwise.
- Add `src/tui/*.cpp` + `src/ui/view_rows.cpp` to the `miniagent` target and
  link `ftxui::component`. Desktop-only files stay as-is.
- Tests: new `tui_render_test` (see §6); existing tests unchanged except
  `view_rows` extraction (they keep linking the same sources).

## 6. Testing

- `tests/tui_render_test.cpp`: build an `App`, drive it with synthetic
  `AgentEvent`s (same pattern as `toolrows_test`) or `seedDebugHooks`,
  render through the TUI row/markdown builders into a string buffer
  (FTXUI `Screen` can render off-screen), assert on output: collapsed tool
  summaries, expand toggle, markdown spans, dispatch filtering, blocked
  input state.
- Manual matrix: kitty, gnome-terminal, alacritty, tmux (`set -g mouse on`) —
  click targets, wheel, resize, streaming flicker, Ctrl-chords.
- Update `.miniagent/skills/`: extend `build-and-test` (new target), add
  `verify-tui` (render-to-string / tmux capture workflow mirroring
  `verify-ui`).

## 7. Milestones (each keeps the desktop build green)

- [x] **M0 — groundwork** (§3): split main, `App::tick()`, view_rows,
      `sendPromptText`, clipboard fn, SDL-free core. All existing tests pass,
      desktop pixel-identical (verify-ui skill).
- [x] **M1 — skeleton**: FTXUI loop + ticker, top bar, log viewport (plain
      text), input editor, send → run → stream, quit, Esc stop.
- [x] **M2 — rendering parity**: colors, markdown-lite, thinking toggle,
      tool rows (collapsed summary, expand, streaming, elapsed), trim rules,
      status strip + bottom line.
- [x] **M3 — overlays + commands**: Sessions, Backends (incl. image-model),
      MCP, Help; `/image` attach; toast; history Up/Down.
- [x] **M4 — mouse**: clickable everything, wheel scroll, auto-follow
      un/pin; SGR verification across the terminal matrix.
- [x] **M5 — scaffold parity**: dispatch-filtered view, role-log overlay,
      pin/unpin review, Continue/New-Session buttons, Compact; blocked-input
      states.
- [x] **M6 — tests + docs**: tui_render_test, skills updates, AGENTS.md
      (flag, layout), `scripts/run.sh` stays (runs TUI), optional
      `scripts/run-desktop.sh`.

## 8. Accepted losses / non-goals

- In-log image pixels (kitty/sixel possible later; not planned).
- Pixel-precise drag selection & partial-word copy (native selection only).
- Tooltips-on-hover, hand/text cursors, proportional fonts, pixel layout.
- Clipboard image paste (replaced by `/image <path>`).

## 9. Decisions

1. Desktop mode flags: `--desktop` and `--gui` (both accepted; `-g` short
   alias).
2. Default flipped to TUI immediately — no `MINIAGENT_UI` transition gate.
   (Since reverted: the window is the default again, `--tui` opts in.)
3. FTXUI is fetched automatically at configure time.
