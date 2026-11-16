// Insert effects: stereo, in place, per track and on the master. Plugins only since 2026-11-16 (graph.cpp PluginEffect).
// Real-time safe like the instruments: buffers are allocated in the constructor, never in process().
// Each effect has a version: a change that alters the sound gets a new number, old songs keep theirs.
#pragma once
#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "instruments.hpp"
#include "song_clock.hpp"

namespace core {

constexpr int kMaxEffectParams = 8;  // no built-in effect has parameters any more (a plugin's are its own)
constexpr int kMaxEffects = 16;  // per chain

class Effect {
public:
  explicit Effect(double sampleRate) : sampleRate_(sampleRate) {}
  virtual ~Effect() = default;
  // Parameters in the order of effectParams(name). Read at the start of each process() call.
  double params[kMaxEffectParams] = {};
  std::string id;       // the document's id for this effect (set when built; tails follow it across edits)
  uint64_t watchKey = 0;  // Graph::fxWatchKey(owner, id): the effect whose input a plugin window shows (FxWatch)
  bool bypass = false;  // kept in the chain (its place and its tail stay), but not processed
  // Knob moves (a parameter edited, not automated): the graph glides params toward knob[] once per aligned
  // sub-block (~15 ms, as automation does), so a cutoff jump on a resonant filter does not click. Set by
  // the graph: knobCount (0 for plugins), the knobs that jump (a mode switch) in steppedMask.
  double knob[kMaxEffectParams] = {};
  double glide[kMaxEffectParams] = {};
  int knobCount = 0;
  std::bitset<kMaxEffectParams> stepped;  // switches: they jump, never glide
  bool glideInit = false;
  int64_t glideBlock = INT64_MIN;
  // Processes frames samples of left/right in place.
  virtual void process(float* left, float* right, int frames) = 0;
  // Takes over the sound still ringing in the previous instance (same effect, same place in the chain),
  // so an edit while playing never cuts a delay or a reverb tail. Swaps buffers, never allocates.
  virtual void adopt(Effect& previous) = 0;
  virtual const char* name() const = 0;
  // Samples it delays what passes (look-ahead, oversampling). The graph lines the other tracks up with it.
  virtual int latency() const { return 0; }
  // The song's tempo at this point (a song with tempo changes): tempo-synced effects (delay) follow it.
  virtual void setTempo(double) {}
  // The song position, in beats (quarter notes), at the first sample of the next process() call. The graph
  // calls it before every sub-block; effects that follow the bar (GLITCH, PULSE) read it, the others ignore it.
  virtual void setPosition(double) {}
  // M7c: the key signal for this call (a compressor's sidechain), frames samples; null = its own input.
  const float* keyL = nullptr;
  const float* keyR = nullptr;
  int sidechain = -1;  // the graph's index of the track that keys it (-1: none)
  // The deepest gain reduction of its last process() call, dB (>= 0): compressor and limiter; 0 for others.
  float reductionDb = 0;
  // A native effect that produced non-finite or runaway audio is marked failed and taken out of the chain
  // from then on (the track plays on without it): the same protection a plugin effect gets. Atomic: written
  // on the audio thread, read by the control thread for reporting. True the first time only.
  bool markFailed() { int none = 0; return failure_.compare_exchange_strong(none, 1, std::memory_order_acq_rel); }
  bool failed() const { return failure_.load(std::memory_order_acquire) != 0; }
protected:
  double sampleRate_;
  std::atomic<int> failure_{0};
};

// A parameter that is a switch or a choice (a mode, a type, on/off, a note value): it jumps to a new value,
// never glides through the ones between.
inline bool steppedKnob(std::string_view n) {
  const auto ends = [&](std::string_view s) { return n.size() >= s.size() && n.substr(n.size() - s.size()) == s; };
  if (n == "mode" || n == "pingPong" || n == "note" || n == "rate" || n == "pattern") return true;
  if (n == "fxOnly" || n == "limiter" || n == "solo" || n == "delTime" || n == "modRate") return true;  // CORE FX
  if (n == "autoGain") return true;                                                                       // CORE EQ
  return n.size() > 3 && (ends("On") || ends("Type") || ends("Fit") || ends("Slope") || ends("Chan") || ends("Dyn"));
}

// bpm: the slowest tempo of the song (the delay sizes its buffer for it; its time is in beats, see setTempo).
// version: the effect's version in the song (reverb 2 is level-matched; saturation 2 is oversampled;
// limiter 2 holds the true peak).
std::unique_ptr<Effect> makeEffect(const std::string& name, double sampleRate, double bpm, int version = 1);

} // namespace core
