------
### Important Note  
Not a single line was written by me or any other human.  
Pure vibe ai slope.  
After cloning it, ask your ai agent to review it, **check for security issues** and whatnot.  
It is a coding agent, you might want to **instruct your ai agent to place some safe-guards**, for example dont allow certain bash commands like "rm -rf".  

**I dont accept any responsibility if this makes your computer blow up.**  
Licence is whatever you want it to be.  

------

# miniagent

A small, self-contained AI coding agent written in C++20. One core, two frontends:

- **Desktop window** (default) — SDL3 + Dear ImGui GUI
- **Terminal UI** — `--tui`, built with FTXUI, runs fullscreen in your terminal

Both share the same engine: multi-session chat, built-in file/bash/web tools,
streaming responses, subagents, MCP support, markdown rendering, skills, and
automatic context compaction for long sessions. It talks to any
**OpenAI-compatible** chat API (OpenRouter, OpenAI, local LM Studio/llama.cpp,
…).

![status](https://img.shields.io/badge/status-personal%20experiment-orange)

## Requirements

- Linux with a desktop environment (X11)
- C++20 compiler (GCC 12+ or Clang 15+)
- CMake ≥ 3.20 and `git` (dependencies are fetched from GitHub on first
  configure — internet access needed once)
- Dev packages. Debian/Ubuntu:

  ```bash
  sudo apt install build-essential cmake git \
       libcurl4-openssl-dev nlohmann-json3-dev \
       libgl1-mesa-dev libx11-dev libxcursor-dev \
       libxrandr-dev libxi-dev libxext-dev
  ```

  Fedora: `sudo dnf install gcc-c++ cmake git libcurl-devel json-devel \
  mesa-libGL-devel libX11-devel libXcursor-devel libXrandr-devel libXi-devel libXext-devel`

## Build & run

```bash
cmake -S . -B build        # first run downloads SDL3/ImGui/FTXUI/stb (pinned commits)
cmake --build build -j$(nproc)
./build/miniagent          # desktop window
./build/miniagent --tui    # terminal UI instead
```

Or use the convenience wrappers: `scripts/run.sh` (build + desktop),
`scripts/run-desktop.sh`, `scripts/build.sh`.

To build against your own checkouts of the dependencies instead of the
fetched ones:

```bash
cmake -S . -B build \
  -DSDL3_ROOT=/path/to/SDL \
  -DIMGUI_ROOT=/path/to/imgui \
  -DFTXUI_ROOT=/path/to/ftxui \
  -DSTB_ROOT=/path/to/stb
```

## First run: connect a model

On first start the app creates `~/.config/miniagent/backends.json` with a
default local backend pointing at LM Studio (`http://localhost:1234/v1`).
Edit it to point at any OpenAI-compatible endpoint:

```json
[
  {
    "name": "openrouter",
    "url": "https://openrouter.ai/api/v1",
    "apiKey": "sk-or-...",
    "model": "anthropic/claude-sonnet-4.5"
  },
  {
    "name": "local",
    "url": "http://localhost:1234/v1",
    "apiKey": "1234",
    "model": "local"
  }
]
```

Multiple entries are fine — switch between them in the app's backends panel.
The model can also be changed per session from the UI.

## Optional configuration

Everything lives in `~/.config/miniagent/`:

| File / dir              | Purpose |
| ----------------------- | ------- |
| `backends.json`         | LLM endpoints (see above) |
| `miniagent.settings`    | `key=value` lines (see table below) |
| `fonts/`                | Optional custom fonts; copy the `.ttf`s from `data/` for the intended look (falls back to the built-in font) |
| `mcp.json`              | MCP server definitions |
| `sessions/`, `history.txt` | Saved sessions and input history |

Useful `miniagent.settings` keys:

| Key                | Effect |
| ------------------ | ------ |
| `searxng_url`      | SearXNG instance used by the built-in web search/fetch tools (default `http://localhost:7070`; `SEARXNG_URL` env overrides; `off` disables the tools) |
| `apply_patch=1`    | Replace the `edit` tool with a codex-style `apply_patch` tool |
| `file_tools=0`     | Disable the read/write/edit file tools |
| `context_window`   | Override the context window size (tokens) instead of probing the backend |
| `show_thinking=0`  | Hide reasoning/thinking output |
| `send_thinking=0`  | Stop sending thinking blocks back to the model |
| `tool_preview_lines` | Lines shown in collapsed tool-output previews |

## Skills

Drop a `SKILL.md` (YAML frontmatter with `name` + `description`) into
`.miniagent/skills/<name>/` in the project or `skills/` under the config dir.
Skills are re-scanned on every prompt; `/skills` lists them, `/skill:name`
invokes one explicitly.

## Tests

Each test is a standalone executable (no ctest harness):

```bash
cmake --build build -j$(nproc) --target sse_test app_test agent_test
./build/sse_test
./build/app_test
```

Targets include `sse_test`, `llm_test`, `agent_test`, `app_test`,
`tui_render_test`, `tool_test`, `web_test`, `mcp_test`, `backends_test` and
more — see `CMakeLists.txt` for the full list.

## Project layout

```
src/main.cpp          entry point / arg parsing
src/desktop_main.cpp  SDL3 + ImGui main loop
src/tui/              FTXUI terminal frontend
src/app.*             renderer-neutral core (state machine, sessions)
src/agent/            agent loop, tools, turns, compaction, skills
src/llm/              OpenAI-compatible client + SSE parsing
src/tools/            built-in tools (read, write, edit, grep, bash, web, …)
src/ui/               desktop renderer + shared row model
src/util/             HTTP, process, screenshot helpers
tests/                standalone test executables
```

## Notes

- Linux only (X11 linking is unconditional).
- No telemetry, no accounts — your config, your API keys, your SearXNG.
