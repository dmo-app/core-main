// Core Glitch · Sample Path (built-in instrument "samplepath", 2026-11-09, Diogo's brief and mockup).
// Up to four samplers, each with one sound. Parts of the sound (slices) become nodes on a map; links join the
// nodes; while a note is held every sampler walks its links, one node per step, and plays each node's part
// (the map and the walk: sample_path_patch.hpp). The walk starts at the note-on and is a pure function of the
// samples since then, the pattern and the seed: a loop, a seek or an export plays the same steps (export = real
// time), and a step starts at the same sample whatever the block sizes are. Pitch moves the speed (pitch and length
// together). Each sampler has two FX slots (Delay, Crush, Filter, Stutter); the instrument a reverb and a limiter.
// The data lives in the track's "synth"; the sounds are the track's samples (loKey = the sampler, 0..3).
// Real-time safe: everything is allocated in the constructor and setPatch (control thread).
#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "instruments.hpp"
#include "sample_path_patch.hpp"

namespace core {

class Effect;

class SamplePath final : public Instrument {
public:
  // Parameter order (instruments.cpp "samplepath"): amp, pitch, size, damp, mix, limiter.
  enum { amp, pitch, size, damp, mix, limiter };
  explicit SamplePath(double sampleRate);
  ~SamplePath() override;
  void setPatch(spath::Patch patch);  // control thread, before it goes live
  void setLayerSource(int layer, std::shared_ptr<const AudioData> source) override;
  const char* name() const override { return "samplepath"; }
  bool stereo() const override { return true; }
  void queueNote(int offset, bool on, int pitch, double velocity) override;
  void noteOn(int p, double v) override { queueNote(0, true, p, v); }
  void noteOff(int p) override { queueNote(0, false, p, 0); }
  void chaseNote(int pitch, double velocity, double age) override;
  void allNotesOff() override;
  void render(float*, int) override {}
  void renderStereo(float* left, float* right, int frames) override;
  void adopt(const Instrument& previous) override;
  void setPosition(double beat, double bpm) override;
  // For tests and the window: the node a sampler plays now (-1: none) and how many steps it has walked.
  int nodeNow(int layer) const { return layers_[layer].walking ? layers_[layer].walk.node : -1; }
  int64_t stepsWalked(int layer) const { return layers_[layer].steps; }

private:
  struct Voice {
    const AudioData* data = nullptr;
    int layer = 0;
    bool active = false;
    double pos = 0, step = 1;         // frames in the sound; frames per output sample (negative: reversed)
    int64_t age = 0, length = 0;      // samples; length = the gated part, then the release
    double attack = 1, release = 1;   // samples
    double gainL = 1, gainR = 1;
    double fade = 1, fadeStep = 0;    // a stolen voice fades out in 2 ms
  };
  struct LayerRt {
    std::shared_ptr<const AudioData> sound;
    spath::Walk walk;
    bool walking = false;
    int64_t elapsed = 0;      // samples since the note-on
    double nextAt = 0;        // the next step starts at the first sample >= nextAt (since the note-on)
    int64_t steps = 0;
    std::unique_ptr<Effect> fx[spath::kFxSlots];
    spath::FxType fxType[spath::kFxSlots] = {};
    float holdL[spath::kFxSlots] = {}, holdR[spath::kFxSlots] = {};  // Crush's sample-and-hold
    int holdLeft[spath::kFxSlots] = {};
  };
  struct Queued { int offset; bool on; int pitch; float velocity; };
  void startWalks(int at);
  void stopWalks();
  void step(int layer, int64_t into);  // plays the walk's node (into: samples already past its start)
  void renderVoices(int from, int to);
  void buildFx(int layer);
  void runFx(int layer, int n);
  double stepSamples(const spath::Pattern& p, const spath::Node& n) const;
  const spath::Pattern& pattern(int layer) const { return patch_.layers[layer].patterns[patch_.layers[layer].pattern]; }
  void applyNote(const Queued& q, int at);

  static constexpr int kVoices = 48, kQueue = 256, kBlock = 512;
  spath::Patch patch_;
  LayerRt layers_[spath::kLayers];
  Voice voices_[kVoices];
  Queued queue_[kQueue];
  int queued_ = 0;
  int held_ = 0;
  double transpose_ = 0, velocity_ = 1;
  double songBpm_ = 145, songBeat_ = -1;
  std::vector<float> bufL_[spath::kLayers], bufR_[spath::kLayers], mixL_, mixR_;
  std::unique_ptr<Effect> reverb_;
  double limGain_ = 1.0, limRelease_ = 0.0;  // the safety limiter (no look-ahead)
};

} // namespace core
