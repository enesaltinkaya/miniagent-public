#pragma once

#include "app.h"

// Parses "Ctrl+Shift+O" style chord names into a KeyChord.
// Returns false if the key itself is not recognized.
bool ParseKeyChord(const char* name, App::KeyChord& out);

// True if the chord's key is pressed with exactly its modifiers this frame.
bool ChordPressed(const App::KeyChord& c);
