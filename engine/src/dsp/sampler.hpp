// Core Sampler (built-in instrument "sampler", version 4): plays WAV files of the user's library on notes.
// One-shot drums (one zone, pitch ignored) or a pitched multi-sample (zones by key, e.g. a bass recorded
// note by note). Stereo, real-time safe: the audio is loaded on the control thread (AssetStore) and the
// graph keeps it alive; render only reads it.
// 4-point Hermite interpolation between samples, so a sample played away from its rate (a pitched bass, a
// tuned kick) keeps its highs and adds little aliasing; `tune` is followed while a note plays (a modulator can
// glide the pitch of a sounding sample: a riser that climbs, an impact that falls). Played above its rate, it
// is read through a band-limited (windowed sinc) filter that follows the step (version 4, audit #15), so what
// would pass the output's Nyquist is taken away instead of folding back as aliasing; at its rate or below the
// read is the Hermite one. The older versions were removed on 2026-11-09: older songs play this one.
// Sound design (2026-11-07, Diogo: "não dá para recriar sons"; what any DAW sampler has), all off by default so
// they change nothing: Sustain (the level a held note decays to), Reverse (plays the file backwards
// from its end), a filter per note (Filter off / low-pass / band-pass / high-pass, Cutoff, Resonance, and an
// envelope that opens or closes it on each hit by Filter env octaves over Filter decay), and Velocity (how much a
// harder note is louder).
#pragma once
#include <memory>
#include <vector>
#include "instruments.hpp"
#include "../io/audio_file.hpp"

namespace core {

struct SamplerZone {
  std::shared_ptr<const AudioData> data;  // at the engine's sample rate
  int rootKey = 60, loKey = 0, hiKey = 127;
  int64_t loopStart = 0, loopEnd = 0;     // frames; loopEnd 0: no loop. A looping zone releases on note-off,
                                          // one-shot or not (it would play for ever otherwise).
};

class Sampler final : public Instrument {
public:
  // Parameter order (project.cpp kSampler): tune, start, attack, decay, release, pitched, level, retrigger.
  enum { tune, start, attack, decay, release, pitched, level, retrigger,
         sustain, reverse, filterType, cutoff, resonance, filterEnv, filterDecay, velocitySens };
  // The knobs added later start where they change nothing, also for code that sets only the first eight.
  explicit Sampler(double sampleRate) : Instrument(sampleRate) {
    params[velocitySens] = 1; params[cutoff] = 20000; params[resonance] = 0.1; params[filterDecay] = 200;
  }
  void setZones(std::vector<SamplerZone> zones) { zones_ = std::move(zones); }
  const char* name() const override { return "sampler"; }
  bool stereo() const override { return true; }
  void queueNote(int offset, bool on, int pitch, double velocity) override;
  void noteOn(int pitch, double velocity) override { queueNote(0, true, pitch, velocity); }
  void noteOff(int pitch) override { queueNote(0, false, pitch, 0); }
  void chaseNote(int pitch, double velocity, double age) override;
  void allNotesOff() override;
  void render(float*, int) override {}
  void renderStereo(float* left, float* right, int frames) override;
  // Voices that play a file the new instance also has go on sounding; the others stop (their audio
  // belongs to the previous graph and is freed with it).
  void adopt(const Instrument& previous) override;

private:
  struct Voice {
    const AudioData* data = nullptr;
    bool active = false, released = false;
    int pitch = 0;
    double pos = 0, step = 1, velocity = 1;
    double keySemis = 0;     // the key's distance from the zone's root (pitched), semitones; tune is added (v3)
    double env = 0;          // amplitude envelope, 0..1
    double age = 0;          // samples since the note started (attack, decay)
    double fade = 1, fadeStep = 0;  // a stolen voice fades out in 3 ms
    double fadeIn = 1;              // a note caught mid-way (chase) fades in in 3 ms
    int64_t played = 0;             // samples rendered (0: not heard yet)
    double lastL = 0, lastR = 0;    // the last sample it added (taken: its level dies away)
    int64_t loopStart = 0, loopEnd = 0;  // its zone's loop (frames; loopEnd 0: none)
    double crossfade = 0;                // frames before loopEnd blended with the same place before loopStart
    double dir = 1;                      // -1: Reverse
    double fenv = 1;                     // the filter envelope, 1 at the hit, falling over Filter decay
    double ic1L = 0, ic2L = 0, ic1R = 0, ic2R = 0;  // the filter's state (TPT state-variable, one per side)
    double g = 0, k = 2, a1 = 0;                    // its coefficients, every 16 samples
  };
  struct Queued { int offset; bool on; int pitch; float velocity; double age = 0; };
  void trigger(int pitch, double velocity, double age = 0);
  void releaseKey(int pitch);
  void renderVoices(float* left, float* right, int from, int to);
  double minAttackMs() const;  // the shortest fade-in a note gets (attack 0)
  static constexpr int kVoices = 16, kQueue = 512;
  std::vector<SamplerZone> zones_;
  Voice voices_[kVoices];
  Queued queue_[kQueue];
  int queued_ = 0;
  double residualL_ = 0, residualR_ = 0;
};

} // namespace core
