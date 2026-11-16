// The VST3 host (M5), built on JUCE's plugin hosting. Linked into core-engine and the plugin tests only.
// Plugins are created, given their state, opened and closed on the JUCE message thread; the audio
// thread only calls PluginProcessor::process.
#pragma once
#include <functional>
#include <memory>
#include <string>
#include "core/plugins.hpp"

namespace core::plugins {

class JuceHost final : public PluginProvider {
public:
  JuceHost();
  ~JuceHost() override;
  std::shared_ptr<PluginProcessor> get(const std::string& key, const json& plugin, double sampleRate, bool offline, std::string* why) override;
  void keepOnly(const std::vector<std::string>& keys) override;
  // Another song: every live instance goes (with its window), so the next song's plugins start from
  // that song's own states.
  void releaseAll();
  // Stop: every live instance drops what still rings (its delay and reverb tails, its voices).
  void resetAll();

  // {key: base64 state} of every loaded plugin: what the document saves.
  json captureStates();
  // Opens (or closes) the plugin's own editor in a window of this process.
  bool showEditor(const std::string& key, bool open, std::string* why);
  // True once after any plugin's parameter changed from its own editor (the document needs its state).
  bool takeChanged();
  // A parameter of a loaded plugin, 0..1 (as its editor would set it). False if there is no such plugin.
  bool setParameter(const std::string& key, int index, float normalized);
  // The parameters that can be automated: [{id, name, value, default, steps, text}] (empty if not loaded).
  json parameters(const std::string& key);
  // The loaded plugin's state as a .vstpreset file, in base64 (what a VST3 host such as Bitwig loads), from
  // JUCE's VST3 client extension. Empty if the plugin is not loaded or is not a VST3.
  std::string vstPreset(const std::string& key);
  // The same written to a file (UTF-8 path); returns its size in bytes, 0 if nothing was written.
  size_t saveVstPreset(const std::string& key, const std::string& fileUtf8);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Runs after each piece of plugin code the host calls on the message thread (loading, state, editor).
// The engine puts its crash handler back there: a plugin may install its own when it loads, and then a
// crash leaves no dump.
void setAfterPluginCode(std::function<void()> f);

// JUCE ready on the thread that creates it (that thread becomes the message thread), while it lives.
class Runtime {
public:
  Runtime();
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;
};

// Runs body on a thread of its own while the calling thread runs the JUCE message loop (where plugins
// are created and their editors live); returns body's result when it ends.
int runWithMessageLoop(const std::function<int()>& body);

// What one plugin file holds: [{format, id, name, vendor, version, file, category, instrument, inputs, outputs}].
// Run in its own process (core-engine scan <file>): a plugin that crashes while scanned takes only that
// process down.
json scanFile(const std::string& path);

} // namespace core::plugins
