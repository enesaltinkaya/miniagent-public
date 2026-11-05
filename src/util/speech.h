#pragma once

#include <string>

// Fire and forget: speak() never blocks. Phrases are queued and spoken in
// order by a single background speaker, so overlapping calls never overlap
// the audio. Each phrase is preceded by a silent `speak ' '` warm-up that
// wakes the audio device - without it the first phoneme of the real phrase
// is eaten. Best effort - a missing `speak` (espeak-ng) or a failed play is
// silently ignored.
//
//   MINIAGENT_NO_SPEECH=1  disable all speech
void speak(const std::string& text);
