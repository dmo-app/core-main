#include "Autosave.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace core::app {

juce::File autosaveFolder() {
  return juce::File::getSpecialLocation(juce::File::windowsLocalAppData).getChildFile("Core").getChildFile("Autosave");
}

int64_t currentProcessId() {
#ifdef _WIN32
  return (int64_t)GetCurrentProcessId();
#else
  return 0;
#endif
}

bool processRunning(int64_t processId) {
#ifdef _WIN32
  if (processId <= 0) return false;
  HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)processId);
  if (!h) return false;
  DWORD code = 0;
  const bool running = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
  CloseHandle(h);
  return running;
#else
  (void)processId;
  return false;
#endif
}

} // namespace core::app
