#pragma once

// Save the currently rendered OpenGL back buffer to a PNG file.
// Call after the frame is fully rendered (all draw data issued) and
// before SDL_GL_SwapWindow.
bool saveGlFramebufferPng(const char* path, int width, int height);
