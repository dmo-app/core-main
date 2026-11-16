#include "Diagnostics.h"
#include <memory>
#include "core_version.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#endif

namespace core::app {

namespace {

std::unique_ptr<juce::FileLogger> logger;
juce::String sessionStamp;
constexpr int kKeep = 20;  // logs and dumps

#ifdef _WIN32
wchar_t dumpPath[MAX_PATH] = {};

// Unhandled crash in the window's process: a small dump (threads, stacks) next to the logs, then Windows
// goes on as usual (the crash dialog). The autosave keeps the song.
LONG WINAPI writeDump(EXCEPTION_POINTERS* info) {
  HANDLE file = CreateFileW(dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION e{GetCurrentThreadId(), info, FALSE};
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file, MiniDumpNormal, &e, nullptr, nullptr);
    CloseHandle(file);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// The newest kKeep - 1 of each kind stay (the file names sort by date): with the one this session writes, kKeep
// (deep hunt: the folder held kKeep + 1).
void prune(const juce::File& folder, const juce::String& pattern) {
  juce::Array<juce::File> files = folder.findChildFiles(juce::File::findFiles, false, pattern);
  files.sort();
  for (int i = 0; i + (kKeep - 1) < files.size(); ++i) files[i].deleteFile();
}

}  // namespace

juce::File logsFolder() {
  return juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Core").getChildFile("Logs");
}

juce::File appLogFile() { return logsFolder().getChildFile("core-" + sessionStamp + ".log"); }

juce::File engineLogFile() { return logsFolder().getChildFile("engine-" + sessionStamp + ".log"); }

void startDiagnostics(const juce::String& commandLine) {
  const juce::File folder = logsFolder();
  folder.createDirectory();
  sessionStamp = juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
  for (const char* pattern : {"core-*.log", "engine-*.log", "core-*.dmp", "engine-*.dmp"}) prune(folder, pattern);
  const juce::String welcome = juce::String(JUCE_APPLICATION_NAME_STRING) + " " + CORE_BUILD_ID + " started" +
                               (commandLine.isNotEmpty() ? " (" + commandLine + ")" : juce::String()) + "\n" +
                               juce::SystemStats::getOperatingSystemName() + ", " + juce::SystemStats::getCpuModel() + ", " +
                               juce::String(juce::SystemStats::getNumCpus()) + " cores, " +
                               juce::String(juce::SystemStats::getMemorySizeInMegabytes()) + " MB";
  logger = std::make_unique<juce::FileLogger>(folder.getChildFile("core-" + sessionStamp + ".log"), welcome, 0);
  juce::Logger::setCurrentLogger(logger.get());
#ifdef _WIN32
  folder.getChildFile("core-" + sessionStamp + ".dmp").getFullPathName().copyToUTF16((juce::CharPointer_UTF16::CharType*)dumpPath, sizeof dumpPath);
  SetUnhandledExceptionFilter(writeDump);
#endif
}

void stopDiagnostics() {
  logLine("Core closed");
  juce::Logger::setCurrentLogger(nullptr);
  logger.reset();
}

void logLine(const juce::String& text) {
  if (logger) juce::Logger::writeToLog(juce::Time::getCurrentTime().formatted("%H:%M:%S ") + text);
}

} // namespace core::app
