#include <SDL3/SDL.h>

#include <GL/gl.h>

#include "app.h"
#include "debug_hooks.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl3.h"
#include "ui/fonts.h"
#include "util/screenshot.h"
#include "util/settings.h"

#include <chrono>
#include <cstdlib>
#include <climits>
#include <string>
#include <unistd.h>

// The SDL3/OpenGL/ImGui frontend (the classic desktop window). main.cpp
// parses args (flags + optional workdir), creates the config dir and chdirs
// into the workdir before dispatching here.

int runDesktop(int argc, char** argv) {
    (void)argc;
    (void)argv;

    // Optional: software_render=1 in miniagent.settings forces Mesa's
    // llvmpipe by exporting LIBGL_ALWAYS_SOFTWARE before SDL creates a GL
    // context (saves VRAM; needs a restart to take effect). An already-set
    // environment variable wins.
    {
        auto earlySettings = loadSettings();
        if (earlySettings["software_render"] == "1" || earlySettings["software_render"] == "true") {
            setenv("LIBGL_ALWAYS_SOFTWARE", "1", 0);
            fprintf(stderr, "software rendering enabled (LIBGL_ALWAYS_SOFTWARE)\n");
        }
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    std::string workDir;
    char dirBuf[PATH_MAX] = {0};
    if (getcwd(dirBuf, sizeof(dirBuf)))
        workDir = dirBuf;
    else
        workDir = ".";

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    // Match game-001's window system: half of the primary display width
    // (capped at 1280px) at a 16:9 aspect ratio, centered horizontally and
    // pinned to the top.
    int windowWidth = 1280, windowHeight = 720;
    int primaryDisplayId               = SDL_GetPrimaryDisplay();
    const SDL_DisplayMode* displayMode = SDL_GetCurrentDisplayMode(primaryDisplayId);
    if (displayMode) {
        windowWidth  = std::min(1800, static_cast<int>(displayMode->w * 0.75F));
        windowHeight = (displayMode->w * 0.75F) / 1.77F;
    }

    int windowX = -1, windowY = -1;
    bool restoreMaximized = false;
    {
        auto settings = loadSettings();
        auto num      = [&](const char* key) {
            auto it = settings.find(key);
            return it != settings.end() ? std::atoi(it->second.c_str()) : -1;
        };
        int x = num("window_x"), y = num("window_y");
        int w = num("window_w"), h = num("window_h");
        if (w >= 400 && h >= 300) {
            int displayCount       = 0;
            SDL_DisplayID* displays = SDL_GetDisplays(&displayCount);
            for (int i = 0; i < displayCount; i++) {
                SDL_Rect b;
                SDL_GetDisplayBounds(displays[i], &b);
                if (x + w > b.x && x < b.x + b.w && y + h > b.y && y < b.y + b.h) {
                    windowWidth  = w;
                    windowHeight = h;
                    windowX      = x;
                    windowY      = y;
                    break;
                }
            }
            SDL_free(displays);
        }
        auto maxIt = settings.find("window_maximized");
        restoreMaximized = maxIt != settings.end() && maxIt->second == "1";
    }

    std::string windowTitle = workDir;
    size_t slash            = windowTitle.rfind('/');
    if (windowTitle.size() > 40 && slash != std::string::npos && slash > 8)
        windowTitle = "..." + windowTitle.substr(slash);
    std::string title  = "MiniAgent \u2014 " + windowTitle;
    SDL_Window* window = SDL_CreateWindow(title.c_str(),
                                          windowWidth,
                                          windowHeight,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!window) {
        SDL_Log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    if (windowX >= 0 && windowY >= 0)
        SDL_SetWindowPosition(window, windowX, windowY);
    else {
        SDL_Rect bounds = {};
        SDL_GetDisplayBounds(SDL_GetDisplayForWindow(window), &bounds);
        SDL_SetWindowPosition(window, bounds.x + (bounds.w - windowWidth) / 2, bounds.y);
    }
    if (restoreMaximized) SDL_MaximizeWindow(window);

    void* glContext = SDL_GL_CreateContext(window);
    if (!glContext) {
        SDL_Log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_SetSwapInterval(1);

    float displayScale = SDL_GetWindowDisplayScale(window);
    SDL_Log("display scale: %.1fx", displayScale);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().LogFilename = nullptr;
    SetupFonts(ImGui::GetIO(), displayScale);
    ImGui::StyleColorsDark();
    // Dark green backdrop: dark enough for contrast with the light text and
    // saturated kind colors.
    ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = ImVec4(0.02f, 0.06f, 0.035f, 1.0f);
    // No scrollbar track: let the window background show through instead of
    // StyleColorsDark's near-black band behind the grab.
    ImGui::GetStyle().Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    // Darker backdrop behind modal windows than the default (0.35 alpha).
    ImGui::GetStyle().Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.78f);
    // Default button style: the dark-green trio of the Simple/Scaffold toggles
    // and the floating Sessions buttons, with their 4px frame rounding, so
    // every button matches them without local color pushes.
    ImGui::GetStyle().Colors[ImGuiCol_Button]        = ImVec4(0.07f, 0.31f, 0.19f, 1.0f);
    ImGui::GetStyle().Colors[ImGuiCol_ButtonHovered] = ImVec4(0.11f, 0.39f, 0.25f, 1.0f);
    ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]  = ImVec4(0.14f, 0.45f, 0.29f, 1.0f);
    ImGui::GetStyle().FrameRounding                  = 4.0f;
    // And the green buttons' generous frame padding (CSS-`padding`
    // equivalent) becomes the app-wide default, so every framed widget —
    // buttons, input fields, checkboxes — gets the same padded look.
    ImGui::GetStyle().FramePadding = ImVec2(12.0f * gFontScale, 7.0f * gFontScale);
    if (!ImGui_ImplSDL3_InitForOpenGL(window, glContext) || !ImGui_ImplOpenGL3_Init(nullptr)) {
        SDL_Log("ImGui init failed");
        return 1;
    }

    // Optional: if MINIAGENT_SCREENSHOT is set, wait (MINIAGENT_SCREENSHOT_AFTER
    // seconds, default 3) after the window appears, save the rendered frame to
    // that path, and exit.
    const char* screenshotPath = SDL_getenv("MINIAGENT_SCREENSHOT");

    App app;

    // Clipboard: the App core is renderer-neutral, so the frontend installs
    // the implementation — SDL here, an OSC 52 escape writer in the TUI.
    app.setClipboardText = [](const std::string& text) {
        SDL_SetClipboardText(text.c_str());
    };

    // Startup MINIAGENT_* debug hooks (shared with the TUI frontend, see
    // src/debug_hooks.cpp).
    seedDebugHooks(app);

    bool running   = true;
    auto loopStart = std::chrono::steady_clock::now();
    // Optional: MINIAGENT_FPS logs the average frame time every second.
    const bool logFps = SDL_getenv("MINIAGENT_FPS") != nullptr;
    double frameAccum = 0.0;
    int frameCount    = 0;
    auto fpsClock     = std::chrono::steady_clock::now();
    // Frame pacing: with software rendering (LIBGL_ALWAYS_SOFTWARE) the GL
    // swap does not block on vsync, so an uncapped loop spins at full speed
    // and burns every core. Cap it at a steady ~30fps.
    const std::chrono::milliseconds kFrame = std::chrono::milliseconds(33);
    while (running) {
        auto frameStart = std::chrono::steady_clock::now();
        // Poll (never block) and render every frame.
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_E &&
                 (event.key.mod & SDL_KMOD_ALT)))
                running = false;
            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                app.onEscape();
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Temporary verification hook: fake a left-drag over the white agent
        // line and print the clipboard one frame after release.
        if (SDL_getenv("MINIAGENT_FAKE_DRAG")) {
            static int step = 0;
            if (step == 5) {
                char* t = SDL_GetClipboardText();
                fprintf(stderr, "FAKE_DRAG step=%d clipboard=[%s]\n", step, t ? t : "(null)");
                SDL_free(t);
            } else if (step < 4) {
                ImGuiIO& io = ImGui::GetIO();
                // Partial drag across a body line: from a mid-word
                // start to a later word, so the byte offsets must be partial.
                // MINIAGENT_FAKE_DRAG=2 targets the thinking line below.
                const float y =
                    SDL_getenv("MINIAGENT_FAKE_DRAG") && SDL_getenv("MINIAGENT_FAKE_DRAG")[0] == '2'
                        ? 314.0f
                        : 276.0f;
                const float xs[4] = {220.0f, 300.0f, 500.0f, 620.0f};
                // const float y = 276.0f;
                if (step == 0) {
                    io.AddMousePosEvent(xs[0], y);
                    io.AddMouseButtonEvent(0, true);
                } else if (step < 3) {
                    io.AddMousePosEvent(xs[step], y);
                } else {
                    io.AddMousePosEvent(xs[step], y);
                    io.AddMouseButtonEvent(0, false);
                }
                step++;
            }
        }

        app.Frame();

        if (logFps) {
            frameAccum += std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - frameStart)
                              .count();
            frameCount++;
            if (std::chrono::steady_clock::now() - fpsClock >= std::chrono::seconds(1)) {
                SDL_Log("avg frame: %.1f ms over %d frames", frameAccum / frameCount, frameCount);
                frameAccum = 0.0;
                frameCount = 0;
                fpsClock   = std::chrono::steady_clock::now();
            }
        }

        ImGui::Render();
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.02f, 0.06f, 0.035f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        const long shotAfter = SDL_getenv("MINIAGENT_SCREENSHOT_AFTER")
                                   ? std::atol(SDL_getenv("MINIAGENT_SCREENSHOT_AFTER"))
                                   : 3;
        if (screenshotPath &&
            std::chrono::steady_clock::now() - loopStart >= std::chrono::seconds(shotAfter)) {
            saveGlFramebufferPng(screenshotPath, w, h);
            running = false;
        }
        SDL_GL_SwapWindow(window);

        // Pace the loop at a steady ~30fps (see above).
        const auto elapsed = std::chrono::steady_clock::now() - frameStart;
        if (elapsed < kFrame)
            // kFrame is milliseconds but elapsed is nanoseconds; the
            // difference must be cast back to ms or SDL_Delay sleeps for
            // nanoseconds-as-milliseconds (hours). +1ms absorbs SDL_Delay's
            // round-down granularity.
            SDL_Delay(static_cast<Uint32>(
                          std::chrono::duration_cast<std::chrono::milliseconds>(kFrame - elapsed)
                              .count()) +
                      1);
    }

    {
        auto settings = loadSettings();
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MAXIMIZED) {
            settings["window_maximized"] = "1";
        } else {
            int px, py, pw, ph;
            SDL_GetWindowPosition(window, &px, &py);
            SDL_GetWindowSize(window, &pw, &ph);
            settings["window_x"]          = std::to_string(px);
            settings["window_y"]          = std::to_string(py);
            settings["window_w"]          = std::to_string(pw);
            settings["window_h"]          = std::to_string(ph);
            settings["window_maximized"] = "0";
        }
        saveSettings(settings);
    }
    app.shutdown();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
