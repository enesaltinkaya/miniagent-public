cmake_minimum_required(VERSION 3.20)
project(miniagent CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Third-party dependencies (SDL3, Dear ImGui, FTXUI, stb) are downloaded
# automatically on the first configure, pinned to known-good commits. A git
# checkout and internet access are required once; afterwards the populated
# copies live in build/_deps and no network is needed.
#
# To build against local checkouts instead, pass any of:
#   -DSDL3_ROOT=/path/to/SDL  -DIMGUI_ROOT=/path/to/imgui
#   -DFTXUI_ROOT=/path/to/ftxui  -DSTB_ROOT=/path/to/stb
set(SDL3_ROOT "" CACHE PATH "SDL3 source checkout (fetched from GitHub if empty)")
set(IMGUI_ROOT "" CACHE PATH "Dear ImGui source checkout (fetched from GitHub if empty)")
set(FTXUI_ROOT "" CACHE PATH "FTXUI source checkout (fetched from GitHub if empty)")
set(STB_ROOT "" CACHE PATH "stb source checkout (fetched from GitHub if empty)")

# SDL3: compiled from source as a static subproject.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
if(NOT SDL3_ROOT)
    FetchContent_Declare(SDL
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG 6efe0e19a7c9f41988615a84341038de34af674e)
    FetchContent_MakeAvailable(SDL)
    set(SDL3_ROOT ${sdl_SOURCE_DIR})
else()
    add_subdirectory("${SDL3_ROOT}" sdl3 EXCLUDE_FROM_ALL)
endif()

# FTXUI (terminal frontend, PLAN_TUI.md §5), compiled from source.
if(NOT FTXUI_ROOT)
    FetchContent_Declare(ftxui
        GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui.git
        GIT_TAG bec373d68b3f66ca63e8a187bc04489b4c413dad)
    FetchContent_MakeAvailable(ftxui)
    set(FTXUI_ROOT ${ftxui_SOURCE_DIR})
else()
    add_subdirectory("${FTXUI_ROOT}" ftxui EXCLUDE_FROM_ALL)
endif()

# Dear ImGui: header/sources only upstream, compiled here (see NOTE below).
if(NOT IMGUI_ROOT)
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG 1bcc23ef123869240971427902b32109c8157e24)
    FetchContent_MakeAvailable(imgui)
    set(IMGUI_ROOT ${imgui_SOURCE_DIR})
endif()

# stb: header-only.
if(NOT STB_ROOT)
    FetchContent_Declare(stb
        GIT_REPOSITORY https://github.com/nothings/stb.git
        GIT_TAG f0569113c93ad095470c54bf34a17b36646bbbb5)
    FetchContent_MakeAvailable(stb)
    set(STB_ROOT ${stb_SOURCE_DIR})
endif()

set(IMGUI_SOURCES
    ${IMGUI_ROOT}/imgui.cpp
    ${IMGUI_ROOT}/imgui_draw.cpp
    ${IMGUI_ROOT}/imgui_tables.cpp
    ${IMGUI_ROOT}/imgui_widgets.cpp
    ${IMGUI_ROOT}/imgui_demo.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_ROOT}/backends/imgui_impl_opengl3.cpp
)

# NOTE: imgui is compiled from the sources at ${IMGUI_ROOT} (1.92.6 WIP) instead
# of the prebuilt build-linux/libimgui.a, which was built from an older version
# (ABI mismatch: ImFont::FontSize/CalcTextSize missing).

add_library(imgui STATIC ${IMGUI_SOURCES})
target_include_directories(imgui PUBLIC
    ${IMGUI_ROOT}
    ${IMGUI_ROOT}/backends
    ${SDL3_ROOT}/include
)

set(SOURCES
    src/main.cpp
    src/desktop_main.cpp
    src/tui_main.cpp
    src/tui/tui.cpp
    src/tui/tui_view.cpp
    src/tui/rich_text_tui.cpp
    src/debug_hooks.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/notify.cpp
    src/util/speech.cpp
    src/util/utf8.cpp
    src/util/screenshot.cpp
    src/util/image.cpp
    src/util/imagetex.cpp
    src/sessions/sessions.cpp
)

add_executable(miniagent ${SOURCES})

# Embed the bundled agent files (agents/*.md: brainstorm, worker) as
# raw-string constants in generated/assets.h.
# CMAKE_CONFIGURE_DEPENDS makes cmake re-run the configure step (and thus
# re-embed) at build time whenever an agent file changes - plain file(READ)
# is not tracked. New files are picked up too, but discoverAgents() in
# src/agent/agents.cpp only looks up brainstorm/worker by name.
file(GLOB AGENT_MD_FILES ${CMAKE_SOURCE_DIR}/agents/*.md)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${AGENT_MD_FILES}")
set(ASSET_HEADER ${CMAKE_BINARY_DIR}/generated/assets.h)
file(WRITE ${ASSET_HEADER}
    "#pragma once\n\nstruct BuiltinAsset { const char* name; const char* content; };\n\ninline const BuiltinAsset kBuiltinAssets[] = {\n")
foreach(asset ${AGENT_MD_FILES})
    get_filename_component(asset_name ${asset} NAME_WE)
    file(READ ${asset} asset_content)
    file(APPEND ${ASSET_HEADER} "    {\"${asset_name}\", R\"ASSET(\n${asset_content}\n)ASSET\"},\n")
endforeach()
file(APPEND ${ASSET_HEADER} "};\n")

find_package(CURL REQUIRED)

add_executable(sse_test
    tests/sse_test.cpp
    src/llm/sse.cpp
)
target_include_directories(sse_test PRIVATE src/)

add_executable(process_test
    tests/process_test.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/utf8.cpp
)
target_include_directories(process_test PRIVATE src/)

add_executable(llm_test
    tests/llm_test.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
)
target_include_directories(llm_test PRIVATE src/)
target_link_libraries(llm_test PRIVATE CURL::libcurl)

add_executable(http_abort_test
    tests/http_abort_test.cpp
    src/util/http.cpp
)
target_include_directories(http_abort_test PRIVATE src/)
target_link_libraries(http_abort_test PRIVATE CURL::libcurl)

add_executable(toolargs_test
    tests/toolargs_test.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
)
target_include_directories(toolargs_test PRIVATE src/)
target_link_libraries(toolargs_test PRIVATE CURL::libcurl)

add_executable(agent_test
    tests/agent_test.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/utf8.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
)
target_include_directories(agent_test PRIVATE src/ ${STB_ROOT} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(agent_test PRIVATE CURL::libcurl pthread)

add_executable(tool_test
    tests/tool_test.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/utf8.cpp
    src/util/image.cpp
)
target_include_directories(tool_test PRIVATE src/ ${STB_ROOT})
target_link_libraries(tool_test PRIVATE CURL::libcurl)

add_executable(edit_hints_test
    tests/edit_hints_test.cpp
    src/agent/edit_hints.cpp
)
target_include_directories(edit_hints_test PRIVATE src/)

add_executable(apply_patch_test
    tests/apply_patch_test.cpp
    src/tools/apply_patch.cpp
)
target_include_directories(apply_patch_test PRIVATE src/)

# Tool steering: the bash description, the system prompt's tool guidance and
# the read tool's truncation note must all push the agent toward read/grep.
add_executable(tool_steer_test
    tests/tool_steer_test.cpp
    src/agent/skills.cpp
    src/tools/read.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/util/utf8.cpp
    src/util/image.cpp
    src/util/process.cpp
    src/util/http.cpp
)
target_include_directories(tool_steer_test PRIVATE src/ ${STB_ROOT})
target_link_libraries(tool_steer_test PRIVATE pthread CURL::libcurl)

add_executable(agent_edit_test
    tests/agent_edit_test.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/utf8.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
)
target_include_directories(agent_edit_test PRIVATE src/ ${STB_ROOT} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(agent_edit_test PRIVATE CURL::libcurl pthread)

add_executable(scaffold_test
    tests/scaffold_test.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/utf8.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
)
target_include_directories(scaffold_test PRIVATE src/ ${STB_ROOT} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(scaffold_test PRIVATE CURL::libcurl pthread)

add_executable(ledger_status_test
    tests/ledger_status_test.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/utf8.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
)
target_include_directories(ledger_status_test PRIVATE src/ ${STB_ROOT} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(ledger_status_test PRIVATE CURL::libcurl pthread)

add_executable(gate_test
    tests/gate_test.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/utf8.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
)
target_include_directories(gate_test PRIVATE src/ ${STB_ROOT} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(gate_test PRIVATE CURL::libcurl pthread)

add_executable(agents_test
    tests/agents_test.cpp
    src/agent/agents.cpp
)
target_include_directories(agents_test PRIVATE src/ ${CMAKE_BINARY_DIR}/generated)

# Skill discovery: SKILL.md frontmatter rules, one-skill-per-directory
# scanning, name collisions, the <available_skills> prompt block and
# /skill:name expansion.
add_executable(skills_test
    tests/skills_test.cpp
    src/agent/skills.cpp
)
target_include_directories(skills_test PRIVATE src/)

add_executable(subagent_test
    tests/subagent_test.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/utf8.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
)
target_include_directories(subagent_test PRIVATE src/ ${STB_ROOT} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(subagent_test PRIVATE CURL::libcurl pthread)

# Offline checks for the role (subagent/scaffold) system prompt: skill
# advertisement without the authoring contract, tool-guidance flags, AGENTS.md.
add_executable(role_prompt_test
    tests/role_prompt_test.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/utf8.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
)
target_include_directories(role_prompt_test PRIVATE src/ ${STB_ROOT} ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(role_prompt_test PRIVATE CURL::libcurl pthread)

add_executable(sessions_test
    tests/sessions_test.cpp
    src/sessions/sessions.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
)
target_include_directories(sessions_test PRIVATE src/)

# selectHistorySeed: a finished run's in-memory messages (with the image
# attachments the session file drops) seed the next run of the same session,
# so the wire format of earlier turns stays byte-identical and the backend's
# prompt cache survives a follow-up prompt.
add_executable(history_seed_test
    tests/history_seed_test.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/sessions/sessions.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
)
target_include_directories(history_seed_test PRIVATE src/)
target_link_libraries(history_seed_test PRIVATE CURL::libcurl pthread)

add_executable(recall_test
    tests/recall_test.cpp
    src/tools/recall.cpp
)
target_include_directories(recall_test PRIVATE src/)

add_executable(utf8_test
    tests/utf8_test.cpp
    src/util/utf8.cpp
)
target_include_directories(utf8_test PRIVATE src/)

add_executable(slash_command_test
    tests/slash_command_test.cpp
)
target_include_directories(slash_command_test PRIVATE src/)

add_executable(vcc_test
    tests/vcc_test.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
)
target_include_directories(vcc_test PRIVATE src/)

add_executable(notify_test
    tests/notify_test.cpp
    src/util/notify.cpp
)
target_include_directories(notify_test PRIVATE src/)

# Speech worker: fake `speak` on PATH; exiting while the worker is parked is
# the shutdown-hang check.
add_executable(speech_test
    tests/speech_test.cpp
    src/util/speech.cpp
)
target_include_directories(speech_test PRIVATE src/)
target_link_libraries(speech_test PRIVATE pthread)

# Retry + compaction: classifier/cut-point unit tests and end-to-end agent
# runs against an in-process HTTP server (no real LLM needed).
add_executable(retry_compaction_test
    tests/retry_compaction_test.cpp
    src/sessions/sessions.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
)
target_include_directories(retry_compaction_test PRIVATE src/)
target_link_libraries(retry_compaction_test PRIVATE CURL::libcurl pthread)

add_executable(stream_rows_test
    tests/stream_rows_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
)
target_include_directories(stream_rows_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(stream_rows_test PRIVATE
    imgui
    SDL3::SDL3
    CURL::libcurl
    pthread
    GL
)

# tok/s speed accounting (chars/3.5 estimator, usage-chunk rebase, generation
# span at TurnEnd): synthetic streamed deltas, no LLM needed.
add_executable(speed_test
    tests/speed_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
)
target_include_directories(speed_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(speed_test PRIVATE
    imgui
    SDL3::SDL3
    CURL::libcurl
    pthread
    GL
)

add_executable(escape_test
    tests/escape_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
)
target_include_directories(escape_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(escape_test PRIVATE
    imgui
    SDL3::SDL3
    CURL::libcurl
    GL
    pthread
    dl
    m
    X11
    Xcursor
    Xrandr
    Xi
    Xext
)

add_executable(image_test
    tests/image_test.cpp
    src/util/image.cpp
)
target_include_directories(image_test PRIVATE src/ ${STB_ROOT})

add_executable(app_test
    tests/app_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
)
target_include_directories(app_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(app_test PRIVATE
    imgui
    SDL3::SDL3
    CURL::libcurl
    GL
    pthread
    dl
    m
    X11
    Xcursor
    Xrandr
    Xi
    Xext
)

# Desktop-notification branch on run end (done / aborted / error): synthetic
# events through processEvent with a recording notifyRun, no LLM needed.
add_executable(notify_app_test
    tests/notify_app_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
)
target_include_directories(notify_app_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(notify_app_test PRIVATE
    imgui
    SDL3::SDL3
    CURL::libcurl
    GL
    pthread
    dl
    m
    X11
    Xcursor
    Xrandr
    Xi
    Xext
)

# Loop mode (the Loop checkbox): arming at the parent AgentEnd, the countdown
# landing in tick(), the error backoff and every cancel path; loopDispatch and
# the speak/notify seams are recorders, so no LLM and no speech.
add_executable(loop_test
    tests/loop_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
)
target_include_directories(loop_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(loop_test PRIVATE
    imgui
    SDL3::SDL3
    CURL::libcurl
    GL
    pthread
    dl
    m
    X11
    Xcursor
    Xrandr
    Xi
    Xext
)

# MCP client over stdio: a throwaway python3 JSON-RPC server in /tmp is
# spawned as the test server; covers the handshake, tool wrapping, text /
# image / error results, the config file and persistence.
# Web tools: HTML-to-text reduction, entity decoding, SearXNG result
# formatting, and both tools driven end to end against an in-process HTTP
# server (search JSON, redirect chain, byte cap, non-text content type).
add_executable(web_test
    tests/web_test.cpp
    src/tools/web.cpp
    src/util/http.cpp
    src/util/utf8.cpp
)
target_include_directories(web_test PRIVATE src/)
target_link_libraries(web_test PRIVATE CURL::libcurl pthread)

add_executable(mcp_test
    tests/mcp_test.cpp
    src/mcp/mcp.cpp
)
target_include_directories(mcp_test PRIVATE src/)

# backends.json (list of LLM backend URLs): first-run creation with the
# default local backend, array parsing, save round-trip, corrupt-file
# fallback.
add_executable(backends_test
    tests/backends_test.cpp
    src/llm/backends.cpp
)
target_include_directories(backends_test PRIVATE src/)

# Tool-call row lifecycle (streaming deltas -> start -> end, parallel calls,
# nested runs): synthetic events, no LLM needed.
add_executable(toolrows_test
    tests/toolrows_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
)
target_include_directories(toolrows_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(toolrows_test PRIVATE
    imgui
    SDL3::SDL3
    CURL::libcurl
    GL
    pthread
    dl
    m
    X11
    Xcursor
    Xrandr
    Xi
    Xext
)

# Drag-selection regression tests (empty-line hit, reallocation mid-drag).
add_executable(selection_test
    tests/selection_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
)
target_include_directories(selection_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(selection_test PRIVATE
    imgui
    SDL3::SDL3
    CURL::libcurl
    GL
    pthread
    dl
    m
    X11
    Xcursor
    Xrandr
    Xi
    Xext
)

# Scaffold-run message-log view (clean page per role, pinned role review).
add_executable(scaffold_view_test
    tests/scaffold_view_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
)
target_include_directories(scaffold_view_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(scaffold_view_test PRIVATE
    imgui
    SDL3::SDL3
    CURL::libcurl
    GL
    pthread
    dl
    m
    X11
    Xcursor
    Xrandr
    Xi
    Xext
)

target_include_directories(miniagent PRIVATE
    src/
    ${CMAKE_BINARY_DIR}/generated
    ${SDL3_ROOT}/include
    ${IMGUI_ROOT}
    ${IMGUI_ROOT}/backends
    ${STB_ROOT}
)

target_link_libraries(miniagent PRIVATE
    imgui
    ftxui::component
    SDL3::SDL3
)

find_package(CURL REQUIRED)
target_link_libraries(miniagent PRIVATE
    CURL::libcurl
    GL
    pthread
    dl
    m
    X11
    Xcursor
    Xrandr
    Xi
    Xext
)

# TUI render test (PLAN_TUI.md §6): drives the TUI row/element builders
# through an off-screen ftxui::Screen — no terminal attached. Same App
# surface as toolrows_test plus the TUI view sources, linked against
# ftxui::dom.
add_executable(tui_render_test
    tests/tui_render_test.cpp
    src/app.cpp
    src/tools/recall.cpp
    src/llm/backends.cpp
    src/ui/fonts.cpp
    src/ui/rich_text.cpp
    src/ui/style.cpp
    src/ui/history.cpp
    src/ui/chords.cpp
    src/ui/view_rows.cpp
    src/ui/draw_message_log.cpp
    src/ui/draw_panels.cpp
    src/ui/draw_sessions.cpp
    src/ui/draw_mcp.cpp
    src/ui/draw_backends.cpp
    src/mcp/mcp.cpp
    src/agent/agent.cpp
    src/agent/edit_hints.cpp
    src/agent/compaction.cpp
    src/agent/vcc/text.cpp
    src/agent/vcc/tokens.cpp
    src/agent/vcc/normalize.cpp
    src/agent/vcc/brief.cpp
    src/agent/vcc/rank.cpp
    src/agent/vcc/sections.cpp
    src/agent/vcc/compile.cpp
    src/agent/agents.cpp
    src/agent/skills.cpp
    src/agent/scaffold.cpp
    src/llm/openai.cpp
    src/llm/sse.cpp
    src/util/http.cpp
    src/util/process.cpp
    src/util/speech.cpp
    src/util/image.cpp
    src/tools/read.cpp
    src/tools/analyze_image.cpp
    src/tools/write.cpp
    src/tools/edit.cpp
    src/tools/apply_patch.cpp
    src/tools/grep.cpp
    src/tools/bash.cpp
    src/tools/web.cpp
    src/tools/subagent.cpp
    src/sessions/sessions.cpp
    src/util/utf8.cpp
    src/util/imagetex.cpp
    src/util/notify.cpp
    src/tui/tui_view.cpp
    src/tui/rich_text_tui.cpp
)
target_include_directories(tui_render_test PRIVATE src/ ${STB_ROOT} ${SDL3_ROOT}/include ${IMGUI_ROOT} ${IMGUI_ROOT}/backends ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(tui_render_test PRIVATE
    imgui
    ftxui::dom
    SDL3::SDL3
    CURL::libcurl
    GL
    pthread
    dl
    m
    X11
    Xcursor
    Xrandr
    Xi
    Xext
)
