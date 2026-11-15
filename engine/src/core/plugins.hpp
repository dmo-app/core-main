// Plugins (M5): what the engine core needs from a plugin host, without knowing how plugins are loaded.
// The VST3 host itself (JUCE) lives in src/plugins and is linked only into core-engine; the core, the
// tests and the measurements build without it (plugin tracks and effects are then silent).
#pragma once
#include <atomic>
#include <memory>
#include <string>
#include "project.hpp"

namespace core {

struct MidiNote {
  int offset;       // sample inside the block
  bool on;
  int pitch;        // 0..127, or kAllNotesOff (with on = false): MIDI All Notes Off (CC 123)
  float velocity;   // 0..1
};
constexpr int kAllNotesOff = -1;
// The most events one process() call gets (the host prepares its MIDI buffer for this many).
constexpr int kMaxPluginEvents = 1024;

// One loaded plugin, played by the audio thread (process) and prepared by the host.
class PluginProcessor {
public:
  virtual ~PluginProcessor() = default;
  // Processes frames samples in place (an instrument adds to what is there: its inputs are silent).
  // notes are sorted by offset. Real-time: no allocation, no locks taken by the host.
  virtual void process(float* left, float* right, int frames, const MidiNote* notes, int count) = 0;
  virtual bool isInstrument() const = 0;
  // How late its output is (lookahead): the graph delays the other tracks to line them up.
  virtual int latencySamples() const = 0;
  // Automation of its parameters. parameterIndex: control thread (graph build), the parameter with this
  // id (VST3: its ParamID as text), -1 if none. setParameter: audio thread, 0..1, real-time safe.
  virtual int parameterIndex(const std::string& id) const { (void)id; return -1; }
  virtual void setParameter(int index, float normalized) { (void)index; (void)normalized; }
  // The plugin's version as installed (the host knows it once loaded; "" = unknown). Compared with the
  // version the song was saved with, so a plugin update is never silent.
  virtual std::string installedVersion() const { return ""; }
  // M3b: a plugin that threw, crashed (where the host can catch it) or gave non-finite or runaway audio is
  // never called again while this instance lives: its track goes silent and the rest of the song plays on.
  // The first reason is kept. True the first time only.
  enum class Failure : int { None = 0, Exception = 1, Crash = 2, NonFinite = 3, Runaway = 4 };
  bool markFailed(Failure why = Failure::Exception) {
    int none = 0;
    return failure_.compare_exchange_strong(none, (int)why, std::memory_order_acq_rel);
  }
  bool failed() const { return failure_.load(std::memory_order_acquire) != 0; }
  Failure failure() const { return (Failure)failure_.load(std::memory_order_acquire); }
  static const char* failureName(Failure f) {
    switch (f) {
      case Failure::Exception: return "exception";
      case Failure::Crash: return "crash";
      case Failure::NonFinite: return "non-finite audio (NaN/Inf)";
      case Failure::Runaway: return "runaway level (above +48 dBFS)";
      default: return "";
    }
  }
private:
  std::atomic<int> failure_{0};
};

class PluginProvider {
public:
  virtual ~PluginProvider() = default;
  // The plugin for this place of the song (key: "track:<id>" or "fx:<track id or master>:<effect id>"),
  // described by plugin ({format, id, name, vendor, version, state}). Real time (offline = false): the
  // same instance for as long as the key and the plugin (file, id and stateId) stay the same, so edits never
  // restart it (its state comes from the document only when it is first loaded, or when stateId changes:
  // the document chose another state, e.g. a preset). Offline (export): a new instance with
  // the document's state. Null if it cannot be loaded (why: the reason).
  virtual std::shared_ptr<PluginProcessor> get(const std::string& key, const json& plugin, double sampleRate, bool offline,
                                               std::string* why) = 0;
  // Called after a real-time graph is built: instances whose key is not in use any more are released.
  virtual void keepOnly(const std::vector<std::string>& keys) = 0;
};

} // namespace core
