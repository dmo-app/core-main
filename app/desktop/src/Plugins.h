// The VST3 plugins found on this computer (M5). Scanning runs the engine (core-engine scan <file>) once
// per plugin file, in its own process with a time limit: a plugin that hangs or crashes while scanned
// is set aside (quarantine), never taking the window or the engine down. Each new plugin, or new version
// of one, is then qualified (core-engine qualify <file>, also in its own process): played on its own for
// a few seconds; one that crashes, throws or gives NaN/Inf or a runaway level is set aside too, with why.
// What was found is kept in %APPDATA%/Core/plugins.json (each plugin with its "qualified" result).
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <juce_core/juce_core.h>
#include "json.hpp"

namespace core::app {

class PluginCatalog {
public:
  using json = nlohmann::json;
  explicit PluginCatalog(juce::File engine);
  ~PluginCatalog();

  std::vector<json> instruments() const;  // {format, id, name, vendor, version, file, ...}
  std::vector<json> effects() const;
  std::vector<juce::String> folders() const;
  std::vector<juce::String> quarantine() const;  // files that failed to scan or to qualify (left out next time)
  juce::String problem(const juce::String& file) const;  // why that file was set aside ("" = not known)
  void addFolder(const juce::File& folder);
  void clearQuarantine();  // the next scan tries them again

  // In the background; onDone (message thread) gets how many plugins were found and how many files failed.
  void scan(std::function<void(int found, int failed)> onDone);
  // Only these plugin files (the AI's, Settings of the AI panel): the rest of the list stays as it is.
  void scanOnly(const juce::Array<juce::File>& files, std::function<void(int found, int failed)> onDone);
  bool scanning() const { return scanning_; }
  juce::String progress() const;  // "3 / 12: Something.vst3"
  // One file, now (the self-test uses it with its own test plugins).
  static json scanFile(const juce::File& engine, const juce::File& plugin, juce::String* problem);
  // Qualifies the plugins of one file: [{id, verdict: "ok" | "unsafe", problems, warnings, cpuPct, ...}], or
  // null with problem set when the qualifying process itself crashed or hung (the file is then unsafe).
  static json qualifyFile(const juce::File& engine, const juce::File& plugin, juce::String* problem);

private:
  void scanFiles(juce::Array<juce::File> files, bool all, std::function<void(int, int)> onDone);
  void load();
  void store() const;

  juce::File engine_, storeFile_;
  mutable std::mutex mutex_;
  std::vector<json> plugins_;
  std::vector<juce::String> folders_, quarantine_;
  std::map<juce::String, juce::String> problems_;  // quarantined file -> why
  juce::String progress_;
  std::atomic<bool> scanning_{false}, stop_{false};
  std::thread worker_;
};

} // namespace core::app
