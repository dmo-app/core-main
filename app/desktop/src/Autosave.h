// Autosave (M6): where the copies go, and whether the session that wrote one is still running.
#pragma once
#include <cstdint>
#include <juce_core/juce_core.h>

namespace core::app {

// %LOCALAPPDATA%\Core\Autosave: the user's own, outside the program and the songs' folders.
juce::File autosaveFolder();
int64_t currentProcessId();
// True while that process is running (another window of Core: its autosave is not from a crash).
bool processRunning(int64_t processId);

} // namespace core::app
