// Diagnostics (M6): local logs and crash dumps, so a problem can be understood without sending the songs
// anywhere. Everything goes to %LOCALAPPDATA%\Core\Logs; only the newest files are kept.
#pragma once
#include <juce_core/juce_core.h>

namespace core::app {

juce::File logsFolder();
// Starts this session's log (core-<date-time>.log, also juce::Logger) with what the app and the PC are,
// removes old logs and dumps (the newest 20 stay), and writes a dump if Core crashes.
void startDiagnostics(const juce::String& commandLine);
void stopDiagnostics();
// This session's file for the engine's own messages (its stderr), kept across engine restarts.
juce::File engineLogFile();
juce::File appLogFile();  // this session's log
void logLine(const juce::String& text);  // timestamped, into this session's log

} // namespace core::app
