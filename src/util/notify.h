#pragma once

#include <cstddef>
#include <string>

// Desktop notification + sound when the agent finishes or errors (modeled on
// pi's notify extension). Linux: a notify-send toast (urgency/icon by
// outcome) plus a canberra-gtk-play sound; without a desktop notification
// daemon, an OSC 777 sequence asks the terminal (Ghostty, iTerm2, WezTerm,
// rxvt-unicode) to show the toast instead. Fire and forget: a missing
// command must never block or crash the app.
//
// Set MINIAGENT_NO_NOTIFY=1 to suppress all notifications.
void notifyDesktop(const std::string& title, const std::string& text, bool isError);

// Keep the notification body on one reasonably short line: whitespace
// collapsed, truncated at maxLen bytes (with a trailing ellipsis, never
// splitting a UTF-8 character).
std::string compactNotifyText(const std::string& text, size_t maxLen = 180);
