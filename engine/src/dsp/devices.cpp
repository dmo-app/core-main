// Core Devices: one musical result per effect, a few knobs in real units, no plugin behind them.
// GROWL (talking bass/lead), METAL (tuned metal), PUNCH (kick and bass attack), ZAP (lasers on transients),
// GLITCH (stutters on the grid), PULSE (rhythmic volume cut), BODY (thickness above the sub).
// SPACE (reverb), ECHO (tempo delay), SPIRAL (frequency-shifted echo) and FLUX (phaser, flanger, chorus) are not
// effects of their own any more: they are CORE FX's Reverb, Delay and Modulate (effects.cpp), built here.
// Same rules as effects.cpp: no allocation in process(), denormals flushed, tails kept across edits (adopt),
// a sound change gets a new version. Plan: core-improvements planos/plugins/core-devices.md.
#include "devices.hpp"
#include "registry.hpp"
#include "song_clock.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace core {

namespace {

constexpr double kPi = 3.141592653589793;
double dbToGain(double db) { return std::pow(10.0, db / 20.0); }
inline double flush(double x) { return std::fabs(x) < 1e-30 ? 0.0 : x; }
inline float flushf(float x) { return std::fabs(x) < 1e-15f ? 0.0f : x; }
// Mix for effects that add something (echoes, tails, rings): the effect comes up with Mix, the dry sound stays
// whole up to Mix 0.5, then goes down to nothing at Mix 1 (only the effect: an R-Core / D-Core bus). A plain
// crossfade lowered the dry sound, kick and sub included, on every insert (the sweep: -2 to -4 dB of sub).
inline double dryOf(double mix) { return std::min(1.0, 2.0 * (1.0 - mix)); }
inline double wetOf(double mix) { return mix; }

// State-variable filter (Cytomic/Simper), stable when its frequency moves every few samples.
struct Svf {
  double g = 0, k = 1, a1 = 1, a2 = 0, a3 = 0;
  double ic1[2] = {0, 0}, ic2[2] = {0, 0};
  void set(double sampleRate, double freq, double q) {
    freq = std::clamp(freq, 10.0, sampleRate * 0.45);
    g = std::tan(kPi * freq / sampleRate);
    k = 1.0 / std::max(0.1, q);
    a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2;
  }
  // Returns band-pass; low-pass in lp.
  double run(int c, double x, double& lp) {
    const double v3 = x - ic2[c], v1 = a1 * ic1[c] + a2 * v3, v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
    ic1[c] = flush(2 * v1 - ic1[c]);
    ic2[c] = flush(2 * v2 - ic2[c]);
    lp = v2;
    return v1;
  }
  double low(int c, double x) { double lp; run(c, x, lp); return lp; }
  double high(int c, double x) { double lp; const double bp = run(c, x, lp); return x - k * bp - lp; }
};

// Linkwitz-Riley 4th order crossover: low + high is an all-pass of the input (flat level, both bands clean).
struct Crossover {
  Svf lo[2], hi[2];
  void set(double sampleRate, double freq) { for (int s = 0; s < 2; ++s) { lo[s].set(sampleRate, freq, 0.7071); hi[s].set(sampleRate, freq, 0.7071); } }
  void run(int c, double x, double& low, double& high) {
    low = lo[1].low(c, lo[0].low(c, x));
    high = hi[1].high(c, hi[0].high(c, x));
  }
};

// ---- GROWL: the band above "Body" talks through three formant filters (a-e-i-o-u), "Talk" moves the
// vowel on the beat (1/8 note), "Grit" drives it; below "Body" the bass stays clean (a 4th-order split).
// Mix blends with the untouched input, so Mix 0 is exactly the dry signal.
// params: vowel 0..1, talk 0..1, grit dB, body Hz, mix
class Growl final : public Effect {
public:
  Growl(double sampleRate, double bpm) : Effect(sampleRate), bpm_(bpm) { song_.tempo(bpm, sampleRate); }
  const char* name() const override { return "growl"; }
  void setTempo(double bpm) override { bpm_ = bpm; song_.tempo(bpm, sampleRate_); }
  void setPosition(double beat) override {
    if (song_.at(beat)) { talk_.place(song_.phase(bpm_ / 60.0 * 2.0), song_.smooth); at_ = 0; }
  }
  void process(float* l, float* r, int n) override {
    song_.run(n);
    const double slide = SongPhase::slideCoef(sampleRate_);
    const double grit = dbToGain(params[2]), mix = std::clamp(params[4], 0.0, 1.0);
    if (params[3] != splitAt_) { splitAt_ = params[3]; split_.set(sampleRate_, splitAt_); }
    const double rate = (bpm_ / 60.0) * 2.0 / sampleRate_;  // cycles per sample: one per 1/8 note
    // Output level: tanh of a driven signal grows less than the drive; bring it back near the input.
    const double makeup = 1.0 / std::max(1.0, std::sqrt(grit));
    for (int i = 0; i < n; ++i) {
      const double phase = talk_.next(rate, slide);
      if ((at_++ & 15) == 0) {  // formants every 16 samples
        const double tri = 1 - 4 * std::fabs(phase - 0.5);  // -1..1
        tune(std::clamp(params[0] + 0.5 * std::clamp(params[1], 0.0, 1.0) * tri, 0.0, 1.0));
      }
      float* ch[2] = {l, r};
      for (int c = 0; c < 2; ++c) {
        const double x = ch[c][i];
        double low, high, lp, talk = 0;
        split_.run(c, x, low, high);
        for (int f = 0; f < 3; ++f) talk += gain_[f] * formant_[f].run(c, high, lp);
        // Only the band above Body changes: the dry sound minus that band plus its talking version, so below
        // Body the sound is the input itself (no phase shift from the split, the sub stays exactly as it was).
        (void)low;
        const double talking = 0.5 * std::tanh((0.35 * high + talk) * grit) * makeup;  // 0.5: the sweep keeps peaks 1 dB under the input
        ch[c][i] = (float)(x + mix * (talking - high));
      }
    }
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Growl&>(p);
    split_ = o.split_;
    splitAt_ = o.splitAt_;
    for (int f = 0; f < 3; ++f) formant_[f] = o.formant_[f];
    talk_ = o.talk_; song_ = o.song_; at_ = o.at_;
  }
private:
  void tune(double vowel) {
    // F1, F2, F3 (Hz) and levels of a, e, i, o, u (a low male voice: they sit where a bass talks).
    static const double fr[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {300, 2200, 2900}, {570, 840, 2410}, {325, 700, 2530}};
    static const double lv[3] = {1.0, 0.6, 0.3};
    const double pos = vowel * 4;
    const int a = std::min(3, (int)pos);
    const double t = pos - a;
    for (int f = 0; f < 3; ++f) {
      const double hz = fr[a][f] * std::pow(fr[a + 1][f] / fr[a][f], t);
      formant_[f].set(sampleRate_, hz, 6.0);
      gain_[f] = lv[f] * 2.0;
    }
  }
  double bpm_;
  Crossover split_;
  double splitAt_ = -1;
  Svf formant_[3];
  double gain_[3] = {2, 1.2, 0.6};
  SongPhase talk_;
  SongClock song_;
  unsigned at_ = 0;
};

// ---- METAL: a resonator tuned to a note rings on every hit (comb with damping, -60 dB after "Decay"),
// "Ring" adds a ring modulator at the same note, "Crush" takes bits and rate away.
// params: note (MIDI), ring 0..1, decay ms, crush 0..1, mix
class Metal final : public Effect {
public:
  explicit Metal(double sampleRate) : Effect(sampleRate) {
    const size_t size = (size_t)std::ceil(sampleRate / 20.0) + 4;  // the lowest note: 20 Hz
    buf_[0].assign(size, 0.0f);
    buf_[1].assign(size, 0.0f);
    sub_[0].set(sampleRate, 20.0, 0.5412); sub_[1].set(sampleRate, 20.0, 1.3066);  // 4th-order Butterworth
  }
  const char* name() const override { return "metal"; }
  void setTempo(double bpm) override { song_.tempo(bpm, sampleRate_); }
  // The ring's carrier is placed by the song (on a jump, and when Note changes while playing: slid there in ~30 ms),
  // so Note A -> B -> A rings exactly as before; it used to keep the phase the other note left it at.
  void setPosition(double beat) override {
    const double hz = noteHz();
    const bool jumped = song_.at(beat);
    if (beat >= 0 && (jumped || hz != hzAt_)) { ring_.place(placeOf(hz), jumped ? song_.smooth : true); hzAt_ = hz; }
  }
  void process(float* l, float* r, int n) override {
    song_.run(n);
    const double slide = SongPhase::slideCoef(sampleRate_);
    const double hz = noteHz();
    const double period = std::clamp(sampleRate_ / hz, 2.0, (double)buf_[0].size() - 3);
    const double decay = std::max(5.0, params[2]) * 0.001;
    const double fb = std::pow(0.001, (period / sampleRate_) / decay);  // -60 dB after decay
    const double ring = std::clamp(params[1], 0.0, 1.0), crush = std::clamp(params[3], 0.0, 1.0), mix = std::clamp(params[4], 0.0, 1.0);
    const double bits = 16 - 13 * crush, steps = std::pow(2.0, bits - 1);
    const int hold = 1 + (int)std::lround(crush * crush * 15);  // sample-and-hold, up to 1/16 of the rate
    const double damp = 0.35;
    const double ringStep = hz / sampleRate_;
    const size_t size = buf_[0].size();
    float* ch[2] = {l, r};
    for (int i = 0; i < n; ++i) {
      const double carrier = std::sin(2 * kPi * ring_.next(ringStep, slide));
      const bool take = (holdAt_++ % hold) == 0;
      for (int c = 0; c < 2; ++c) {
        const double x = ch[c][i];
        double read = (double)pos_ - period;
        if (read < 0) read += (double)size;
        const size_t i0 = (size_t)read, i1 = (i0 + 1) % size;
        const double frac = read - (double)i0;
        const double delayed = buf_[c][i0] * (1 - frac) + buf_[c][i1] * frac;
        lp_[c] = flush((1 - damp) * delayed + damp * lp_[c]);
        buf_[c][pos_] = flushf((float)(x * 0.5 + fb * lp_[c]));
        dc_[c] = flush(dc_[c] + 0.0013 * (delayed - dc_[c]));  // ~10 Hz: the comb passes DC, the ring must not
        double wet = delayed - dc_[c];
        wet = (1 - ring) * wet + ring * wet * carrier * 1.4;
        // The ring at the comb's own note folds its fundamental down to 0 Hz (f - f): a 20 Hz high-pass after
        // it (the sweep: -17 dB of the sound under 15 Hz at Ring 1, headphones flap).
        wet = sub_[1].high(c, sub_[0].high(c, wet));
        if (crush > 0) {
          if (take) held_[c] = std::round(wet * steps) / steps;
          wet = held_[c];
        }
        ch[c][i] = (float)(wetOf(mix) * std::tanh(wet) + dryOf(mix) * x);
      }
      pos_ = (pos_ + 1) % size;
    }
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Metal&>(p);
    if (o.buf_[0].size() != buf_[0].size()) return;
    buf_[0].swap(o.buf_[0]); buf_[1].swap(o.buf_[1]);
    pos_ = o.pos_; lp_[0] = o.lp_[0]; lp_[1] = o.lp_[1]; ring_ = o.ring_; song_ = o.song_; hzAt_ = o.hzAt_; dc_[0] = o.dc_[0]; dc_[1] = o.dc_[1];
    sub_[0] = o.sub_[0]; sub_[1] = o.sub_[1];
    // The crush's sample-and-hold goes on in step: started again from 0, every hold after an edit fell on other
    // samples and the sound never came back (knobcheck 2026-11-16: hitech "perc", -13 dB of difference for good).
    held_[0] = o.held_[0]; held_[1] = o.held_[1]; holdAt_ = o.holdAt_;
  }
private:
  std::vector<float> buf_[2];
  size_t pos_ = 0;
  double noteHz() const { return 440.0 * std::pow(2.0, (std::clamp(params[0], 12.0, 96.0) - 69) / 12.0); }
  // Where the carrier is at this point of the song: its cycles since bar 1 (exact, no 4-bar lock: it is a pitch).
  double placeOf(double hz) const { const double p = std::max(0.0, song_.beat) * 60.0 / song_.bpm * hz; return p - std::floor(p); }
  double lp_[2] = {0, 0}, held_[2] = {0, 0}, dc_[2] = {0, 0}, hzAt_ = -1;
  SongPhase ring_;
  SongClock song_;
  Svf sub_[2];
  unsigned holdAt_ = 0;
};

// ---- PUNCH: transient shaper (fast vs slow envelope, linked L/R) and a soft clipper, for kick and bass.
// params: click dB (the attack), tail dB (what follows), clip 0..1, output dB
class Punch final : public Effect {
public:
  explicit Punch(double sampleRate) : Effect(sampleRate) {
    fastA_ = std::exp(-1.0 / (0.0005 * sampleRate)); fastR_ = std::exp(-1.0 / (0.020 * sampleRate));
    slowA_ = std::exp(-1.0 / (0.015 * sampleRate)); slowR_ = std::exp(-1.0 / (0.120 * sampleRate));
    smooth_ = std::exp(-1.0 / (0.001 * sampleRate));
  }
  const char* name() const override { return "punch"; }
  void process(float* l, float* r, int n) override {
    const double click = params[0], tail = params[1], clip = std::clamp(params[2], 0.0, 1.0), out = dbToGain(params[3]);
    const double ceiling = dbToGain(-12.0 * clip);  // clip 1: soft-clips from -12 dBFS
    for (int i = 0; i < n; ++i) {
      const double x = std::max(std::fabs((double)l[i]), std::fabs((double)r[i]));
      fast_ = flush(x > fast_ ? fastA_ * fast_ + (1 - fastA_) * x : fastR_ * fast_ + (1 - fastR_) * x);
      slow_ = flush(x > slow_ ? slowA_ * slow_ + (1 - slowA_) * x : slowR_ * slow_ + (1 - slowR_) * x);
      // > 0 while the attack is ahead of the body, < 0 in the tail; in dB, kept to +-1 (a full step).
      const double diff = std::clamp(20 * std::log10((fast_ + 1e-9) / (slow_ + 1e-9)) / 6.0, -1.0, 1.0);
      const double targetDb = diff > 0 ? click * diff : tail * -diff;
      gainDb_ = smooth_ * gainDb_ + (1 - smooth_) * targetDb;
      const double g = dbToGain(gainDb_);
      double a = l[i] * g, b = r[i] * g;
      if (clip > 0) { a = ceiling * std::tanh(a / ceiling); b = ceiling * std::tanh(b / ceiling); a /= std::sqrt(ceiling); b /= std::sqrt(ceiling); }
      l[i] = (float)(a * out);
      r[i] = (float)(b * out);
    }
  }
  void adopt(Effect& p) override { auto& o = static_cast<Punch&>(p); fast_ = o.fast_; slow_ = o.slow_; gainDb_ = o.gainDb_; }
private:
  double fastA_, fastR_, slowA_, slowR_, smooth_;
  double fast_ = 0, slow_ = 0, gainDb_ = 0;
};

// ---- ZAP: a chain of all-pass filters smears each hit into a chirp (a laser), and on every hit their
// frequency sweeps by "Sweep" octaves over "Length". "Sync" (not Off) starts the sweep on the grid instead
// (every 1/16 to every bar), so the lasers land in time even on a held sound.
// params: pitch Hz, amount 0..1 (stages), sweep oct, length ms, mix, sync 0..5 (Off, 1/16, 1/8, 1/4, 1/2, 1 bar)
class Zap final : public Effect {
public:
  Zap(double sampleRate, double bpm) : Effect(sampleRate) {
    env_ = std::exp(-1.0 / (0.002 * sampleRate)); envR_ = std::exp(-1.0 / (0.050 * sampleRate));
    split_.set(sampleRate, 150.0);
    setTempo(bpm);
  }
  const char* name() const override { return "zap"; }
  void setTempo(double bpm) override { perSample_ = std::max(1.0, bpm) / 60.0 / sampleRate_; }
  void setPosition(double beat) override { if (std::fabs(beat - beat_) > 1e-3) { beat_ = beat; step_ = INT64_MIN; } }
  void process(float* l, float* r, int n) override {
    const int stages = (int)std::lround(std::clamp(params[1], 0.0, 1.0) * kStages);
    const double sweep = params[2], length = std::max(5.0, params[3]) * 0.001 * sampleRate_, mix = std::clamp(params[4], 0.0, 1.0);
    static const double grid[6] = {0, 0.25, 0.5, 1.0, 2.0, 4.0};  // beats per step
    const double every = grid[std::clamp((int)std::lround(params[5]), 0, 5)];
    float* ch[2] = {l, r};
    for (int i = 0; i < n; ++i) {
      // The hits are found the same way whatever the knobs (Mix 0 included), and a hit is the moment the attack
      // rises over the body (armed again once it falls back): before, "a hit only when the last sweep is half
      // done" made which hits sweep depend on Length's past, and Length or Mix A -> B -> A stayed different.
      const double x = std::max(std::fabs((double)l[i]), std::fabs((double)r[i]));
      fast_ = flush(x > fast_ ? env_ * fast_ + (1 - env_) * x : envR_ * fast_ + (1 - envR_) * x);
      slow_ = flush(0.9995 * slow_ + 0.0005 * x);
      const bool hit = fast_ > 2.0 * slow_ + 1e-4;
      if (every > 0) {  // on the grid: a new step starts the sweep
        const int64_t step = (int64_t)std::floor(beat_ / every + 1e-9);
        if (step != step_) { if (step_ != INT64_MIN) since_ = 0; step_ = step; }
      } else if (hit && armed_) {
        since_ = 0;  // a hit: the sweep starts again
      }
      if (hit) armed_ = false; else if (fast_ < 1.5 * slow_) armed_ = true;
      beat_ += perSample_;
      if (mix == 0) { since_ += 1; ++at_; continue; }  // the dry signal, exactly (the split would shift its phase)
      if ((at_++ & 15) == 0) {
        const double t = std::min(1.0, since_ / length);
        const double hz = std::clamp(params[0] * std::pow(2.0, sweep * (1 - t)), 20.0, sampleRate_ * 0.45);
        const double w = std::tan(kPi * hz / sampleRate_);
        coef_ = (w - 1) / (w + 1);
      }
      since_ += 1;
      for (int c = 0; c < 2; ++c) {
        // Only above 150 Hz: the kick and the sub are not smeared (the sweep found the sub 3.6 dB down).
        const double x = ch[c][i];
        double low, high;
        split_.run(c, x, low, high);
        double y = high;
        for (int s = 0; s < stages; ++s) {  // first-order all-pass: y = c*x + z; z = x - c*y
          const double out = coef_ * y + z_[c][s];
          z_[c][s] = flush(y - coef_ * out);
          y = out;
        }
        ch[c][i] = (float)(low + high + mix * (y - high));
      }
    }
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Zap&>(p);
    std::memcpy(z_, o.z_, sizeof z_); fast_ = o.fast_; slow_ = o.slow_; since_ = o.since_; coef_ = o.coef_; beat_ = o.beat_; step_ = o.step_;
    armed_ = o.armed_; at_ = o.at_;
    split_ = o.split_;
  }
private:
  double perSample_ = 0, beat_ = 0;
  int64_t step_ = INT64_MIN;
  Crossover split_;
  static constexpr int kStages = 48;
  double z_[2][kStages] = {};
  double env_, envR_, fast_ = 0, slow_ = 0, since_ = 1e12, coef_ = 0;
  bool armed_ = true;
  unsigned at_ = 0;
};

// ---- Grid clock for GLITCH and PULSE: the song position in beats, given by the graph before each
// sub-block (setPosition) and counted per sample in between. Kept while the graph agrees (within 1/1000 of a
// beat), so a buffer split in real time counts exactly like the export; a jump (loop, seek) takes the new place.
struct BeatClock {
  double beat = 0, perSample = 0;
  bool jumped = true;  // the first block, or after a jump: the effect drops what it was doing
  void tempo(double bpm, double sampleRate) { perSample = std::max(1.0, bpm) / 60.0 / sampleRate; }
  void at(double songBeat) {
    if (std::fabs(songBeat - beat) > 1e-3) { beat = songBeat; jumped = true; }
  }
};

// splitmix64: a fixed "random" number per beat, so the same song plays the same stutters every time
// (real time and export alike).
inline double chanceOf(int64_t beat) {
  uint64_t z = (uint64_t)beat * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

// ---- GLITCH: beat repeat. On each beat, with "Chance", it catches one slice of the grid ("Rate": 1/8 to
// 1/64 of a bar) as it plays and repeats it until "Repeats" slices have sounded, then lets the music through
// again. The first slice is the music itself (nothing is delayed, no latency); each repeat is faded 1.5 ms at
// both ends so it never clicks. Mix blends the repeats with the music (Mix 0 or Chance 0: the dry signal).
// "Reverse" is the chance that a stutter's repeats play backwards (the reverser's swoosh, on the same grid).
// params: rate 0..3 (1/8, 1/16, 1/32, 1/64), repeats 2..16, chance 0..1, mix 0..1, reverse 0..1
class Glitch final : public Effect {
public:
  Glitch(double sampleRate, double bpm) : Effect(sampleRate) {
    // The longest slice: 1/8 at the song's slowest tempo (at least 30 BPM).
    const size_t size = (size_t)std::ceil(0.5 * 60.0 / std::max(30.0, bpm) * sampleRate) + 4;
    buf_[0].assign(size, 0.0f);
    buf_[1].assign(size, 0.0f);
    // The echo's buffers too, here and not when Echo first goes up (audit 2026-11-07 #3: process() allocated
    // them on the audio thread, 79 kB at 145 BPM).
    echoBuf_[0].assign(size, 0.0f);
    echoBuf_[1].assign(size, 0.0f);
    fade_ = std::max(8, (int)std::lround(0.0015 * sampleRate));
    clock_.tempo(bpm, sampleRate);
  }
  const char* name() const override { return "glitch"; }
  void setTempo(double bpm) override { clock_.tempo(bpm, sampleRate_); }
  void setPosition(double beat) override { clock_.at(beat); }
  // More (2026-11-07, ideas from Glitchmachines Fracture and Hysteresis; all off by default, so a song sounds as before):
  // 5 pitch: semitones the repeats glide to by the last one (tape stop down, "pew" up); 6 tone: each repeat darker
  // (< 0, a low-pass closing) or thinner (> 0, a high-pass opening); 7 vary: the chance a stutter takes a slice one
  // step longer or shorter than Rate; 8 length: how much of each repeat sounds (short = a choppy gate); 9 echo: the
  // repeats ring out in a short dark echo after the stutter; 10 spread: the repeats jump left and right.
  void process(float* l, float* r, int n) override {
    static const double sliceBeats[4] = {0.5, 0.25, 0.125, 0.0625};  // 1/8, 1/16, 1/32, 1/64 note
    const int rate = std::clamp((int)std::lround(params[0]), 0, 3);
    const int repeats = std::clamp((int)std::lround(params[1]), 2, 16);
    const double chance = std::clamp(params[2], 0.0, 1.0), mix = std::clamp(params[3], 0.0, 1.0);
    const double pitch = std::clamp(params[5], -24.0, 24.0), tone = std::clamp(params[6], -1.0, 1.0);
    const double vary = std::clamp(params[7], 0.0, 1.0), length = std::clamp(params[8], 0.05, 1.0);
    const double echo = std::clamp(params[9], 0.0, 1.0), spread = std::clamp(params[10], 0.0, 1.0);
    if (clock_.jumped) { left_ = 0; back_ = 0; clock_.jumped = false; lastBeat_ = (int64_t)std::ceil(clock_.beat) - 1; }  // on a beat: it counts; mid-beat: from the next
    const double echoLp = 1 - std::exp(-2 * 3.141592653589793 * 3500 / sampleRate_);
    for (int i = 0; i < n; ++i) {
      const int64_t beatNow = (int64_t)std::floor(clock_.beat);
      if (beatNow != lastBeat_) {  // a new beat: maybe a stutter starts here
        lastBeat_ = beatNow;
        if (left_ <= 0 && chance > 0 && chanceOf(beatNow) < chance) {
          int rt = rate;
          if (vary > 0 && chanceOf(beatNow + 1543) < vary) rt = std::clamp(rt + (chanceOf(beatNow + 2753) < 0.5 ? -1 : 1), 0, 3);
          slice_ = (int)std::clamp(std::lround(sliceBeats[rt] / clock_.perSample), (long)fade_ * 4, (long)buf_[0].size() - 1);
          left_ = slice_ * repeats;
          at_ = 0;
          repeats_ = repeats;
          backwards_ = std::clamp(params[4], 0.0, 1.0) > 0 && chanceOf(beatNow + 7919) < std::clamp(params[4], 0.0, 1.0);
        }
      }
      clock_.beat += clock_.perSample;
      const double x[2] = {l[i], r[i]};
      double wet[2] = {x[0], x[1]};
      bool repeating = false;
      if (left_ > 0) {
        const int k = at_ / slice_, in = at_ - k * slice_;  // which slice, and where in it
        if (k == 0) {  // the first slice plays as it is, and is kept
          buf_[0][(size_t)in] = (float)x[0];
          buf_[1][(size_t)in] = (float)x[1];
        } else {
          repeating = true;
          const double frac = repeats_ > 1 ? (double)k / (repeats_ - 1) : 1.0;  // 0 .. 1 over the repeats
          if (in == 0) setRepeat(k, frac, tone);
          if (pitch == 0) {
            const size_t from = backwards_ ? (size_t)(slice_ - 1 - in) : (size_t)in;
            wet[0] = buf_[0][from];
            wet[1] = buf_[1][from];
          } else {  // read faster or slower: the slice wraps round, at a pitch gliding to Pitch by the last repeat
            const double speed = std::pow(2.0, pitch * frac / 12.0);
            double pos = std::fmod(in * speed, (double)slice_);
            if (backwards_) pos = slice_ - 1 - pos;
            const int i0 = std::clamp((int)pos, 0, slice_ - 1), i1 = std::min(i0 + 1, slice_ - 1);
            const double f = pos - i0;
            for (int c = 0; c < 2; ++c) wet[c] = buf_[c][(size_t)i0] + (buf_[c][(size_t)i1] - buf_[c][(size_t)i0]) * f;
          }
          if (tone != 0)
            for (int c = 0; c < 2; ++c) {
              lp_[c] += toneCoef_ * (wet[c] - lp_[c]);
              wet[c] = tone < 0 ? lp_[c] : wet[c] - lp_[c];
            }
          if (spread > 0) { wet[k % 2 ? 0 : 1] *= 1 - spread; }
          if (in < fade_) { const double g = (double)in / fade_; wet[0] *= g; wet[1] *= g; }
          if (length < 1) {  // only the first part of each repeat sounds, faded out at its end
            const int open = std::max(fade_ * 2, (int)(slice_ * length));
            if (in >= open) { wet[0] = wet[1] = 0; }
            else if (open - in <= fade_) { const double g = (double)(open - in - 1) / fade_; wet[0] *= g; wet[1] *= g; }
          }
        }
        const int toEnd = slice_ - in;
        if (toEnd <= fade_) { const double g = (double)(toEnd - 1) / fade_; wet[0] *= g; wet[1] *= g; }
        ++at_;
        if (--left_ == 0) back_ = fade_;  // the music comes back in 1.5 ms
      } else if (back_ > 0) {
        const double g = 1.0 - (double)back_ / fade_;
        wet[0] *= g; wet[1] *= g;
        --back_;
      }
      if (echo > 0) {  // a dark echo a slice long, fed by the repeats only, that rings on after the stutter
        const size_t size = echoBuf_[0].size();
        const size_t d = (size_t)std::clamp(slice_, 1, (int)size - 1);
        const size_t rd = (echoAt_ + size - d) % size;
        double out[2];
        for (int c = 0; c < 2; ++c) {
          echoLpS_[c] += echoLp * (echoBuf_[c][rd] - echoLpS_[c]);
          out[c] = echoLpS_[c];
          const double in = (repeating ? 0.5 * wet[c] : 0.0) + out[c] * 0.6 * echo;
          echoBuf_[c][echoAt_] = (float)std::clamp(flush(in), -4.0, 4.0);
        }
        echoAt_ = (echoAt_ + 1) % size;
        const double g = 0.7 * echo;
        if (left_ <= 0 && back_ <= 0) { wet[0] = x[0] + out[0] * g; wet[1] = x[1] + out[1] * g; }
        else { wet[0] += out[0] * g; wet[1] += out[1] * g; }
      }
      if (left_ > 0 || back_ > 0 || wet[0] != x[0] || wet[1] != x[1]) {
        l[i] = (float)(mix * wet[0] + (1 - mix) * x[0]);
        r[i] = (float)(mix * wet[1] + (1 - mix) * x[1]);
      }
    }
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Glitch&>(p);
    clock_.beat = o.clock_.beat;
    clock_.jumped = o.clock_.jumped;
    lastBeat_ = o.lastBeat_;
    if (o.buf_[0].size() != buf_[0].size()) return;  // a new tempo range: the stutter in progress ends
    buf_[0].swap(o.buf_[0]); buf_[1].swap(o.buf_[1]);
    echoBuf_[0].swap(o.echoBuf_[0]); echoBuf_[1].swap(o.echoBuf_[1]);
    left_ = o.left_; at_ = o.at_; slice_ = o.slice_; back_ = o.back_; backwards_ = o.backwards_; repeats_ = o.repeats_;
    echoAt_ = o.echoAt_; toneCoef_ = o.toneCoef_;
    for (int c = 0; c < 2; ++c) { lp_[c] = o.lp_[c]; echoLpS_[c] = o.echoLpS_[c]; }
  }
private:
  static double flush(double v) { return std::fabs(v) < 1e-20 ? 0.0 : v; }
  // Each repeat's filter: Tone < 0 closes a low-pass from 18 kHz down to ~400 Hz by the last repeat; > 0 opens a
  // high-pass from 30 Hz up to ~3 kHz.
  void setRepeat(int k, double frac, double tone) {
    if (tone == 0) return;
    if (k == 1) lp_[0] = lp_[1] = 0;
    const double a = std::fabs(tone) * frac;
    const double hz = tone < 0 ? 18000 * std::pow(2.0, -5.5 * a) : 30 * std::pow(2.0, 6.6 * a);
    toneCoef_ = 1 - std::exp(-2 * 3.141592653589793 * std::min(hz, 0.45 * sampleRate_) / sampleRate_);
  }
  BeatClock clock_;
  std::vector<float> buf_[2], echoBuf_[2];
  int64_t lastBeat_ = INT64_MIN;
  int fade_ = 72, slice_ = 1, left_ = 0, at_ = 0, back_ = 0, repeats_ = 2;
  size_t echoAt_ = 0;
  double toneCoef_ = 1, lp_[2] = {0, 0}, echoLpS_[2] = {0, 0};
  bool backwards_ = false;
};

// ---- PULSE: a rhythmic volume cut (trance gate). A bar of 16 sixteenths, each open or closed by
// "Pattern"; "Depth" is how far the closed steps go down (1: silence), "Smooth" softens the edges (from 0.3 ms
// to half a step), "Swing" pushes every second sixteenth late (up to half a step). Depth 0: the dry signal.
// params: pattern 0..7, depth 0..1, smooth 0..1, swing 0..1
class Pulse final : public Effect {
public:
  Pulse(double sampleRate, double bpm) : Effect(sampleRate) { clock_.tempo(bpm, sampleRate); }
  const char* name() const override { return "pulse"; }
  void setTempo(double bpm) override { clock_.tempo(bpm, sampleRate_); }
  void setPosition(double beat) override { clock_.at(beat); }
  static const char* pattern(int i) {
    // Must match the choices in the domain's effect list (edit.cpp), in this order.
    static const char* p[8] = {
        "X.X.X.X.X.X.X.X.",  // Eighths: every 1/8 cut in half
        ".XXX.XXX.XXX.XXX",  // KBBB: closed on the kick, open for the three bass notes
        "..XX..XX..XX..XX",  // Off-beat: open between the kicks
        "X.XXX.XXX.XXX.XX",  // Gallop
        "XX.XX.X.XX.XX.X.",  // Trance gate (3-3-2)
        "XX.X..X.XX.X.XX.",  // Broken
        "XX..XX..XX..XX..",  // Quarters: the first half of each beat
        "XXXXXXXX........"}; // Half bar: open, then a held breath
    return p[std::clamp(i, 0, 7)];
  }
  void process(float* l, float* r, int n) override {
    const char* pat = pattern((int)std::lround(params[0]));
    const double depth = std::clamp(params[1], 0.0, 1.0), smooth = std::clamp(params[2], 0.0, 1.0);
    const double swing = std::clamp(params[3], 0.0, 1.0) * 0.5;  // the late step starts up to half a step later
    const double stepSec = 0.25 / (clock_.perSample * sampleRate_);
    const double tau = 0.0003 + smooth * smooth * 0.5 * stepSec;
    const double coef = std::exp(-1.0 / (tau * sampleRate_));
    clock_.jumped = false;
    if (depth == 0 && gain_ == 1) { clock_.beat += clock_.perSample * n; return; }  // the dry signal, exactly
    for (int i = 0; i < n; ++i) {
      const double s = clock_.beat * 4;  // in sixteenths
      const double pair = std::floor(s / 2), u = s - 2 * pair;
      int64_t step = (int64_t)pair * 2 + (u >= 1 + swing ? 1 : 0);
      step = ((step % 16) + 16) % 16;
      const double target = pat[step] == 'X' ? 1.0 : 1.0 - depth;
      gain_ = target + (gain_ - target) * coef;
      if (std::fabs(gain_ - target) < 1e-9) gain_ = target;
      clock_.beat += clock_.perSample;
      l[i] = (float)(l[i] * gain_);
      r[i] = (float)(r[i] * gain_);
    }
  }
  void adopt(Effect& p) override { auto& o = static_cast<Pulse&>(p); clock_.beat = o.clock_.beat; gain_ = o.gain_; }
private:
  BeatClock clock_;
  double gain_ = 1;
};


// ---- SPACE: a feedback-delay-network reverb (8 lines mixed by a Hadamard matrix every pass, after two
// all-pass diffusers per side), light enough for a send on every song. "Size" sets how far apart the
// reflections are (a room to a hall), "Decay" the time to -60 dB, "Tone" where the tail darkens (damping in
// every pass), "Low cut" keeps the bass out of the tail (so the kick and bass stay dry and clear), "Motion"
// slowly moves four of the lines (a chorus in the tail, the "vintage" shimmer), "Pre-delay" holds the tail back
// so the hit stays in front. Mix 0 is the dry signal; on an R-Core bus it sits at 1 (only the tail).
// "Shimmer" feeds the tail back an octave up (a small two-head pitch shifter in the loop): it rises into a
// glowing, choir-like cloud (the shimmer reverb).
// "Kind" (shown as Type) picks the kind of reverb (2026-11-07, the psychedelic reverb list: spring, plate, hall/cathedral,
// reverse). 0 Hall is the SPACE every song had before, sample for sample:
//   1 Plate: no early echoes, the tail is dense at once (two more diffusers) and short lines (a metal sheet).
//   2 Spring: two springs per side; each pass goes through 40 stretched all-passes (Valimaki et al. 2010),
//     so the highs below ~4.8 kHz arrive later than the lows: the twangy "drip" chirp, band-limited and boingy.
//   3 Cathedral: lines 1.4-3.6x the hall's, six early reflections far apart, the slow build of a huge room.
//   4 Reverse: 64 taps per side over "Decay" (up to 4 s) that grow louder with time, then diffused: the tail
//     swells up after each hit and stops (the gated "reverse" of the studio hardware, live, no look-ahead).
// "Air" lifts the tail above 4.5 kHz (up to +9 dB): the glassy top that the "Tone" knob only takes away.
// params: size 0..1, decay s, predelay ms, tone Hz, lowcut Hz, motion 0..1, mix 0..1, shimmer 0..1, input 0..1, kind 0..4, air 0..1
class Space final : public Effect {
public:
  explicit Space(double sampleRate) : Effect(sampleRate) {
    modRoom_ = (int)std::ceil(0.0008 * sampleRate) + 2;
    for (int k = 0; k < kLines; ++k) line_[k].assign((size_t)std::ceil(kBaseMs[k] * 0.001 * kAllocScale * sampleRate) + (size_t)modRoom_ + 4, 0.0f);
    static const double diffMs[2][2] = {{4.77, 3.59}, {5.13, 3.31}};
    for (int c = 0; c < 2; ++c)
      for (int d = 0; d < 2; ++d) diff_[c][d].assign((size_t)std::max(1L, std::lround(diffMs[c][d] * 0.001 * sampleRate)), 0.0f);  // never empty, at any rate
    // Plate and cathedral: two longer diffusers per side (Dattorro's input lengths), a cloud from the first ms.
    static const double denseMs[2][2] = {{12.73, 9.31}, {13.27, 8.87}};
    for (int c = 0; c < 2; ++c)
      for (int d = 0; d < 2; ++d) dense_[c][d].assign((size_t)std::max(1L, std::lround(denseMs[c][d] * 0.001 * sampleRate)), 0.0f);
    pre_.assign((size_t)std::ceil(0.45 * sampleRate) + 2, 0.0f);
    for (int c = 0; c < 2; ++c) earlyAp_[c].assign((size_t)std::lround((c ? 7.9 : 7.1) * 0.001 * sampleRate), 0.0f);  // pre-delay (250 ms) + the cathedral's echoes
    preR_.assign(pre_.size(), 0.0f);
    shimWin_ = std::max(256, (int)std::lround(0.05 * sampleRate));  // 50 ms grains
    shim_.assign((size_t)shimWin_ * 2 + 4, 0.0f);
    // Spring: 40 stretched all-passes of K samples, K so the chirp band ends near 4.8 kHz at any rate.
    springK_ = std::max(1, (int)std::lround(sampleRate / 9600.0));
    for (int c = 0; c < 2; ++c)
      for (int s = 0; s < 2; ++s) {
        spring_[c][s].assign((size_t)std::ceil(kSpringMs[c][s] * 0.001 * 1.4 * sampleRate) + 4, 0.0f);
        chirp_[c][s].assign((size_t)(kChirp * springK_), 0.0f);
        for (int d = 0; d < 2; ++d) springAp_[c][s][d].assign((size_t)std::lround(kSpringApMs[d] * (1.0 + 0.11 * (2 * c + s)) * 0.001 * sampleRate), 0.0f);
      }
    // Reverse: up to 4 s of the diffused input per side, read by 64 taps spread over the window.
    rev_[0].assign((size_t)std::ceil(kRevMaxS * sampleRate) + 4, 0.0f);
    rev_[1].assign(rev_[0].size(), 0.0f);
    static const double revDiffMs[4] = {7.13, 11.37, 17.21, 23.89};
    for (int c = 0; c < 2; ++c)
      for (int d = 0; d < 4; ++d) revDiff_[c][d].assign((size_t)std::lround(revDiffMs[d] * (c ? 1.07 : 1.0) * 0.001 * sampleRate), 0.0f);
    uint32_t seed = 0x9E3779B9u;
    double sum2 = 0;
    for (int c = 0; c < 2; ++c)
      for (int i = 0; i < kRevTaps; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double jitter = (double)(seed >> 8) / 16777216.0 - 0.5;
        const double t = std::clamp((i + 0.5 + 0.8 * jitter) / kRevTaps, 0.002, 0.998);
        const double fade = t > 0.94 ? (1.0 - t) / 0.06 : 1.0;  // the end of the swell closes in ~6 % of it, no click
        revAt_[c][i] = t;
        revGain_[c][i] = t * t * fade * ((i & 1) ? -1.0 : 1.0);
        if (c == 0) sum2 += revGain_[c][i] * revGain_[c][i];
      }
    revLevel_ = 1.0 / std::sqrt(sum2);
  }
  const char* name() const override { return "space"; }
  void setTempo(double bpm) override { song_.tempo(bpm, sampleRate_); }
  void setPosition(double beat) override {
    if (!song_.at(beat)) return;
    for (int m = 0; m < 4; ++m) { const double p = song_.phase(kModHz[m]) + 0.25 * m; modPhase_[m].place(p - std::floor(p), song_.smooth); }
    at_ = 0;
  }
  void process(float* l, float* r, int n) override {
    song_.run(n);
    const double slide16 = std::pow(SongPhase::slideCoef(sampleRate_), 16.0);
    const double size = std::clamp(params[0], 0.0, 1.0), decay = std::clamp(params[1], 0.1, 60.0);
    const double mix = std::clamp(params[6], 0.0, 1.0), motion = std::clamp(params[5], 0.0, 1.0);
    const double send = std::clamp(params[8], 0.0, 1.0);  // Input: 0 lets the tail ring on with nothing new going in
    const int type = (int)std::lround(std::clamp(params[9], 0.0, 4.0));
    const double air = std::clamp(params[10], 0.0, 1.0);
    if (type != type_) { clearTank(); type_ = type; }  // another kind of room: the old one's tail stops
    const size_t preSize = pre_.size();
    const size_t pre = std::clamp<size_t>((size_t)std::llround(std::max(0.0, params[2]) * 0.001 * sampleRate_), 0, (size_t)(0.25 * sampleRate_));
    // Hall: the scale every song had (0.25 to 2.2); plate a small sheet; cathedral far bigger.
    const double scale = type == kPlate ? 0.3 + 0.7 * size : type == kCathedral ? 1.4 + 2.2 * size : kMinScale + (kMaxScale - kMinScale) * size;
    double gainSum2 = 0;
    for (int k = 0; k < kLines; ++k) {
      len_[k] = kBaseMs[k] * 0.001 * scale * sampleRate_;
      gain_[k] = std::pow(10.0, -3.0 * len_[k] / (decay * sampleRate_));
      gainSum2 += gain_[k] * gain_[k];
    }
    const double damp = 1.0 - std::exp(-2 * kPi * std::clamp(params[3], 200.0, sampleRate_ * 0.45) / sampleRate_);
    if (params[4] != cutAt_) { cutAt_ = params[4]; cut_.set(sampleRate_, std::clamp(cutAt_, 10.0, 2000.0), 0.7071); }
    // The tail's energy grows like 1 / (1 - g^2): divided back out, so a long and a short tail start about as
    // loud as the input (the length changes, not the level). 3.9: measured on noise, mix 1 = input level (within 1.5 dB from 1 s to 8 s).
    const double g2 = gainSum2 / kLines;
    const double level = 3.9 * std::sqrt(std::max(1e-4, 1.0 - g2));
    const double depth = motion * 0.0006 * sampleRate_;
    const double shimmer = std::clamp(params[7], 0.0, 1.0) * 0.45;
    // Air: a shelf above 4.5 kHz on the tail, 0 to +9 dB.
    const double airGain = dbToGain(9.0 * air) - 1.0;
    const double airCoef = 1.0 - std::exp(-2 * kPi * 4500.0 / sampleRate_);
    // Spring: each pass loses what Decay asks for over its length (the delay plus the chirp's ~300 samples).
    const double springScale = 0.6 + 0.8 * size;
    double springLen[2][2], springGain[2][2];
    for (int c = 0; c < 2; ++c)
      for (int s = 0; s < 2; ++s) {
        springLen[c][s] = kSpringMs[c][s] * 0.001 * springScale * sampleRate_;
        springGain[c][s] = std::pow(10.0, -3.0 * (springLen[c][s] + 6.0 * kChirp * springK_ * 0.25) / (decay * sampleRate_));
      }
    const double springDamp = 1.0 - std::exp(-2 * kPi * std::min(std::clamp(params[3], 200.0, sampleRate_ * 0.45), 5000.0) / sampleRate_);
    const double springLevel = 4.0 * std::sqrt(std::max(1e-4, 1.0 - springGain[0][0] * springGain[0][0]));
    if (type == kSpring && !springReady_) { springHp_.set(sampleRate_, 120.0, 0.7071); springLp_.set(sampleRate_, 4500.0, 0.7071); springReady_ = true; }
    // Reverse: the window is Decay, up to 4 s.
    const double revWin = std::clamp(decay, 0.1, kRevMaxS - 0.01) * sampleRate_;
    double modStep[4];
    for (int m = 0; m < 4; ++m) modStep[m] = 16.0 * song_.lock(kModHz[m]) / sampleRate_;
    auto moveOn = [&] {
      if ((at_++ & 15) == 0)
        for (int m = 0; m < 4; ++m) mod_[m] = depth * (0.5 + 0.5 * std::sin(2 * kPi * modPhase_[m].next(modStep[m], slide16)));
    };
    if (mix == 0 && quiet_) { for (int i = 0; i < n; ++i) moveOn(); return; }  // the dry signal, exactly, and nothing ringing to finish
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      moveOn();
      // Pre-delay, then two all-passes per side (the first reflections smear into a cloud, not a flutter).
      pre_[prePos_] = (float)(send * l[i]);
      preR_[prePos_] = (float)(send * r[i]);
      const size_t readPre = (prePos_ + preSize - pre) % preSize;
      double in[2] = {pre_[readPre], preR_[readPre]};
      double early[2] = {0, 0};
      if (type == kCathedral) {
        // Six early reflections from far walls, after the pre-delay, alternating sides and fading.
        const double erScale = (0.6 + 0.8 * size) * 0.001 * sampleRate_;
        for (int e = 0; e < 6; ++e) {
          const size_t at = (size_t)std::llround(kEarlyMs[e] * erScale) + pre;
          const size_t rp = (prePos_ + preSize - std::min(at, preSize - 1)) % preSize;
          const double x = 0.5 * (pre_[rp] + preR_[rp]) * kEarlyGain[e];
          early[e & 1] += x;
          early[(e & 1) ^ 1] += 0.35 * x;
        }
        // Walls are not mirrors: each echo is smeared a little (one all-pass per side).
        for (int c = 0; c < 2; ++c) {
          auto& ab = earlyAp_[c];
          size_t& ap = earlyApPos_[c];
          const double buffered = ab[ap];
          const double yy = buffered - 0.6 * early[c];
          ab[ap] = flushf((float)(early[c] + 0.6 * yy));
          ap = (ap + 1) % ab.size();
          early[c] = yy;
        }
      }
      prePos_ = (prePos_ + 1) % preSize;
      if (type == kSpring) {
        double outS[2] = {0, 0};
        for (int c = 0; c < 2; ++c) {
          const double x = springHp_.high(c, in[c]);
          for (int s = 0; s < 2; ++s) {
            auto& b = spring_[c][s];
            const size_t bs = b.size();
            double rd = (double)springPos_[c][s] - springLen[c][s];
            while (rd < 0) rd += (double)bs;
            const size_t i0 = (size_t)rd % bs, i1 = (i0 + 1) % bs;
            const double fr = rd - std::floor(rd);
            double y = b[i0] + (b[i1] - b[i0]) * fr;
            // The chirp: 40 all-passes (a + z^-K) / (1 + a z^-K), a = 0.6.
            auto& ch = chirp_[c][s];
            const size_t pos = chirpPos_[c][s];
            for (int st = 0; st < kChirp; ++st) {
              float& v = ch[(size_t)st * springK_ + pos];
              const double w = y - 0.6 * v;
              y = 0.6 * w + v;
              v = flushf((float)w);
            }
            chirpPos_[c][s] = (pos + 1) % (size_t)springK_;
            // Two short all-passes in the loop: each pass of the chirp is a smeared cluster, not one clean echo
            // (a real coil scatters as it travels), so it sounds like a spring and not like a flutter.
            for (int d = 0; d < 2; ++d) {
              auto& ab = springAp_[c][s][d];
              size_t& ap = springApPos_[c][s][d];
              const double buffered = ab[ap];
              const double yy = buffered - 0.35 * y;
              ab[ap] = flushf((float)(y + 0.35 * yy));
              ap = (ap + 1) % ab.size();
              y = yy;
            }
            springLpState_[c][s] = flush(springLpState_[c][s] + springDamp * (y - springLpState_[c][s]));
            const double fed = springLpState_[c][s];
            b[springPos_[c][s]] = flushf((float)(x * 0.5 + fed * springGain[c][s] * (s ? -1.0 : 1.0)));
            springPos_[c][s] = (springPos_[c][s] + 1) % bs;
            outS[c] += s ? -y : y;
          }
          outS[c] = springLp_.low(c, outS[c]) * springLevel;
        }
        const double wl = cut_.high(0, outS[0]), wr = cut_.high(1, outS[1]);
        finish(l[i], r[i], wl, wr, mix, airGain, airCoef, peak);
        continue;
      }
      for (int c = 0; c < 2; ++c)
        for (int d = 0; d < 2; ++d) {
          auto& b = diff_[c][d];
          size_t& p = diffPos_[c][d];
          const double buffered = b[p];
          const double y = buffered - 0.6 * in[c];
          b[p] = flushf((float)(in[c] + 0.6 * y));
          p = (p + 1) % b.size();
          in[c] = y;
        }
      if (type == kReverse) {
        // Taps louder the later they read: each hit swells up over the window and stops, then four
        // all-passes per side turn the 64 taps into a smooth wash.
        double out[2] = {0, 0};
        const size_t rs = rev_[0].size();
        for (int c = 0; c < 2; ++c) {
          rev_[c][revPos_] = flushf((float)in[c]);
          double acc = 0;
          for (int k = 0; k < kRevTaps; ++k) {
            double rd = (double)revPos_ - revAt_[c][k] * revWin;
            while (rd < 0) rd += (double)rs;
            const size_t i0 = (size_t)rd % rs, i1 = (i0 + 1) % rs;
            const double fr = rd - std::floor(rd);
            acc += (rev_[c][i0] + (rev_[c][i1] - rev_[c][i0]) * fr) * revGain_[c][k];
          }
          acc *= revLevel_;
          for (int d = 0; d < 4; ++d) {
            auto& b = revDiff_[c][d];
            size_t& p = revDiffPos_[c][d];
            const double buffered = b[p];
            const double y = buffered - 0.7 * acc;
            b[p] = flushf((float)(acc + 0.7 * y));
            p = (p + 1) % b.size();
            acc = y;
          }
          revLp_[c] = flush(revLp_[c] + damp * (acc - revLp_[c]));
          out[c] = revLp_[c] * kRevGain;
        }
        revPos_ = (revPos_ + 1) % rs;
        const double wl = cut_.high(0, out[0]), wr = cut_.high(1, out[1]);
        finish(l[i], r[i], wl, wr, mix, airGain, airCoef, peak);
        continue;
      }
      if (type != kHall) {
        // Plate and cathedral: two more, longer diffusers (0.7): no flutter, the tail is a cloud at once.
        for (int c = 0; c < 2; ++c)
          for (int d = 0; d < 2; ++d) {
            auto& b = dense_[c][d];
            size_t& p = densePos_[c][d];
            const double buffered = b[p];
            const double y = buffered - 0.7 * in[c];
            b[p] = flushf((float)(in[c] + 0.7 * y));
            p = (p + 1) % b.size();
            in[c] = y;
          }
      }
      // Read the 8 lines (four of them moving slowly), damp, scale by their loop gain.
      double v[kLines];
      for (int k = 0; k < kLines; ++k) {
        const auto& b = line_[k];
        const size_t bs = b.size();
        double read = (double)linePos_[k] - len_[k] - (k < 4 ? mod_[k] : 0.0);
        while (read < 0) read += (double)bs;
        const size_t i0 = (size_t)read % bs, i1 = (i0 + 1) % bs;
        const double frac = read - std::floor(read);
        const double y = b[i0] + (b[i1] - b[i0]) * frac;
        lp_[k] = flush(lp_[k] + damp * (y - lp_[k]));
        v[k] = lp_[k] * gain_[k];
      }
      const double outL = (v[0] + v[2] + v[4] + v[6]) * 0.5, outR = (v[1] + v[3] + v[5] + v[7]) * 0.5;
      double up = 0;
      if (shimmer > 0) {
        // Octave up: two read heads run through the last 50 ms twice as fast, each faded in and out (sin^2),
        // half a grain apart, so their sum is steady. What comes out is fed into the tank again.
        const size_t ss = shim_.size();
        shim_[shimPos_] = flushf((float)(0.5 * (outL + outR)));
        for (int h = 0; h < 2; ++h) {
          double ph = shimPhase_ + 0.5 * h;
          ph -= std::floor(ph);
          const double delay = (1.0 - ph) * shimWin_ + 1;
          double rd = (double)shimPos_ - delay;
          while (rd < 0) rd += (double)ss;
          const size_t a = (size_t)rd % ss, b = (a + 1) % ss;
          const double f = rd - std::floor(rd), w = std::sin(kPi * ph);
          up += (shim_[a] + (shim_[b] - shim_[a]) * f) * w * w;
        }
        shimPhase_ += 1.0 / shimWin_;
        if (shimPhase_ >= 1) shimPhase_ -= 1;
        shimPos_ = (shimPos_ + 1) % ss;
        in[0] += shimmer * up;
        in[1] += shimmer * up;
      }
      // Hadamard 8x8 (fast, three butterflies), normalised: energy kept, every line feeds every other.
      for (int h = 1; h < kLines; h <<= 1)
        for (int a = 0; a < kLines; a += h << 1)
          for (int b = a; b < a + h; ++b) { const double x = v[b], y = v[b + h]; v[b] = x + y; v[b + h] = x - y; }
      for (int k = 0; k < kLines; ++k) {
        const double feed = v[k] * 0.35355339059327373 + (k & 1 ? in[1] : in[0]) * 0.5;
        line_[k][linePos_[k]] = flushf((float)feed);
        linePos_[k] = (linePos_[k] + 1) % line_[k].size();
      }
      // The cathedral's early echoes join the tail at their own level (the tail's level depends on Decay, they do not).
      const double wl = type == kCathedral ? cut_.high(0, outL * level + early[0]) : cut_.high(0, outL) * level;
      const double wr = type == kCathedral ? cut_.high(1, outR * level + early[1]) : cut_.high(1, outR) * level;
      finish(l[i], r[i], wl, wr, mix, airGain, airCoef, peak);
    }
    quiet_ = peak < 1e-7;
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Space&>(p);
    if (o.line_[0].size() != line_[0].size()) return;
    for (int k = 0; k < kLines; ++k) { line_[k].swap(o.line_[k]); linePos_[k] = o.linePos_[k]; lp_[k] = o.lp_[k]; }
    for (int c = 0; c < 2; ++c)
      for (int d = 0; d < 2; ++d) {
        diff_[c][d].swap(o.diff_[c][d]); diffPos_[c][d] = o.diffPos_[c][d];
        dense_[c][d].swap(o.dense_[c][d]); densePos_[c][d] = o.densePos_[c][d];
      }
    pre_.swap(o.pre_); preR_.swap(o.preR_); prePos_ = o.prePos_;
    if (o.shim_.size() == shim_.size()) { shim_.swap(o.shim_); shimPos_ = o.shimPos_; shimPhase_ = o.shimPhase_; }
    for (int c = 0; c < 2; ++c) {
      for (int s = 0; s < 2; ++s) {
        spring_[c][s].swap(o.spring_[c][s]); springPos_[c][s] = o.springPos_[c][s];
        chirp_[c][s].swap(o.chirp_[c][s]); chirpPos_[c][s] = o.chirpPos_[c][s]; springLpState_[c][s] = o.springLpState_[c][s];
        for (int d = 0; d < 2; ++d) { springAp_[c][s][d].swap(o.springAp_[c][s][d]); springApPos_[c][s][d] = o.springApPos_[c][s][d]; }
      }
      earlyAp_[c].swap(o.earlyAp_[c]); earlyApPos_[c] = o.earlyApPos_[c];
      rev_[c].swap(o.rev_[c]);
      for (int d = 0; d < 4; ++d) { revDiff_[c][d].swap(o.revDiff_[c][d]); revDiffPos_[c][d] = o.revDiffPos_[c][d]; }
      revLp_[c] = o.revLp_[c];
      airLp_[c] = o.airLp_[c];
    }
    revPos_ = o.revPos_;
    springHp_ = o.springHp_; springLp_ = o.springLp_; springReady_ = o.springReady_;
    type_ = o.type_;
    cut_ = o.cut_; cutAt_ = o.cutAt_;
    for (int m = 0; m < 4; ++m) { modPhase_[m] = o.modPhase_[m]; mod_[m] = o.mod_[m]; }
    song_ = o.song_; at_ = o.at_;
    quiet_ = o.quiet_;
  }
private:
  enum { kHall = 0, kPlate = 1, kSpring = 2, kCathedral = 3, kReverse = 4 };
  // The tail, the Air shelf, the mix: the last step of every type.
  void finish(float& l, float& r, double wl, double wr, double mix, double airGain, double airCoef, double& peak) {
    if (airGain > 0) {
      airLp_[0] = flush(airLp_[0] + airCoef * (wl - airLp_[0]));
      airLp_[1] = flush(airLp_[1] + airCoef * (wr - airLp_[1]));
      wl += airGain * (wl - airLp_[0]);
      wr += airGain * (wr - airLp_[1]);
    }
    peak = std::max(peak, std::fabs(wl) + std::fabs(wr) + std::fabs((double)l) + std::fabs((double)r));
    l = (float)(dryOf(mix) * l + wetOf(mix) * wl);
    r = (float)(dryOf(mix) * r + wetOf(mix) * wr);
  }
  // A new type starts from silence: the lines of the old one would ring with the wrong lengths.
  void clearTank() {
    if (type_ < 0) return;
    for (auto& b : line_) std::fill(b.begin(), b.end(), 0.0f);
    for (auto& lp : lp_) lp = 0;
    for (int c = 0; c < 2; ++c) {
      for (int d = 0; d < 2; ++d) { std::fill(diff_[c][d].begin(), diff_[c][d].end(), 0.0f); std::fill(dense_[c][d].begin(), dense_[c][d].end(), 0.0f); }
      for (int s = 0; s < 2; ++s) { std::fill(spring_[c][s].begin(), spring_[c][s].end(), 0.0f); std::fill(chirp_[c][s].begin(), chirp_[c][s].end(), 0.0f); springLpState_[c][s] = 0;
                                    for (auto& ab : springAp_[c][s]) std::fill(ab.begin(), ab.end(), 0.0f); }
      std::fill(earlyAp_[c].begin(), earlyAp_[c].end(), 0.0f);
      std::fill(rev_[c].begin(), rev_[c].end(), 0.0f);
      for (int d = 0; d < 4; ++d) std::fill(revDiff_[c][d].begin(), revDiff_[c][d].end(), 0.0f);
      revLp_[c] = 0;
    }
    std::fill(shim_.begin(), shim_.end(), 0.0f);
  }
  static constexpr int kLines = 8;
  static constexpr double kMinScale = 0.25, kMaxScale = 2.2, kAllocScale = 3.6;
  // Mutually prime-ish lengths (ms at scale 1), so the echoes never line up into a pitch.
  static constexpr double kBaseMs[kLines] = {29.7, 37.1, 41.1, 43.7, 53.3, 59.9, 67.7, 73.1};
  static constexpr double kModHz[4] = {0.13, 0.21, 0.31, 0.43};
  static constexpr double kEarlyMs[6] = {17, 29, 43, 61, 83, 109}, kEarlyGain[6] = {0.32, 0.28, 0.24, 0.2, 0.16, 0.12};
  static constexpr double kSpringMs[2][2] = {{37.3, 47.9}, {41.1, 53.3}};
  static constexpr double kSpringApMs[2] = {1.1, 2.3};
  static constexpr int kChirp = 40, kRevTaps = 64;
  static constexpr double kRevMaxS = 4.0, kRevGain = 1.3;
  std::vector<float> line_[kLines], diff_[2][2], dense_[2][2], pre_, preR_, shim_;
  std::vector<float> spring_[2][2], chirp_[2][2], springAp_[2][2][2], rev_[2], revDiff_[2][4], earlyAp_[2];
  size_t shimPos_ = 0;
  double shimPhase_ = 0;
  int shimWin_ = 2400;
  size_t linePos_[kLines] = {}, diffPos_[2][2] = {}, densePos_[2][2] = {}, prePos_ = 0;
  size_t springPos_[2][2] = {}, chirpPos_[2][2] = {}, springApPos_[2][2][2] = {}, revPos_ = 0, revDiffPos_[2][4] = {}, earlyApPos_[2] = {};
  int springK_ = 5;
  double springLpState_[2][2] = {};
  Svf springHp_, springLp_;
  bool springReady_ = false;
  double revAt_[2][kRevTaps] = {}, revGain_[2][kRevTaps] = {}, revLevel_ = 1, revLp_[2] = {}, airLp_[2] = {};
  int type_ = -1;
  double len_[kLines] = {}, gain_[kLines] = {}, lp_[kLines] = {};
  SongPhase modPhase_[4] = {{0, 0}, {0.25, 0}, {0.5, 0}, {0.75, 0}};
  double mod_[4] = {};
  SongClock song_;
  int modRoom_ = 40;
  unsigned at_ = 0;
  Svf cut_;
  double cutAt_ = -1;
  bool quiet_ = true;
};


// ---- ECHO: a tempo-synced echo for D-Core (and alone as an insert). "Time" is a note value, "Feedback" how
// many echoes, "Tone" darkens each repeat, "Low cut" keeps the bass out of the echoes (they never muddy the
// kick and bass), "Ping-pong" sends each echo to the other side (0: both sides, 1: hard left-right).
// Mix 0 is the dry signal, 1 only the echoes (on a D-Core bus); in between a crossfade.
// params: time 0..7 (1/16, 1/8, 3/16, 1/4, 3/8, 1/2, 3/4, 1 bar), feedback 0..0.95, tone Hz, lowcut Hz,
// pingpong 0..1, duck 0..1, mix 0..1
class Echo final : public Effect {
public:
  Echo(double sampleRate, double bpm) : Effect(sampleRate), bpm_(bpm) {
    // A whole bar at the slowest tempo the song may reach (40 BPM), so a tempo change keeps the echoes.
    const size_t size = (size_t)std::ceil(4.0 * 60.0 / 40.0 * sampleRate) + 4;
    buf_[0].assign(size, 0.0f);
    buf_[1].assign(size, 0.0f);
    duckA_ = 1.0 - std::exp(-1.0 / (0.005 * sampleRate));
    duckR_ = std::exp(-1.0 / (0.150 * sampleRate));
  }
  const char* name() const override { return "echo"; }
  void setTempo(double bpm) override { bpm_ = bpm; }
  static double beatsOf(int choice) {
    static const double b[8] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0};
    return b[std::clamp(choice, 0, 7)];
  }
  void process(float* l, float* r, int n) override {
    const size_t size = buf_[0].size();
    const double target = std::clamp(beatsOf((int)std::lround(params[0])) * 60.0 / std::max(1.0, bpm_) * sampleRate_, 1.0, (double)size - 3);
    if (time_ <= 0) time_ = target;
    const double fb = std::clamp(params[1], 0.0, 0.95), mix = std::clamp(params[6], 0.0, 1.0);
    const double ping = std::clamp(params[4], 0.0, 1.0), duck = std::clamp(params[5], 0.0, 1.0);
    const double send = std::clamp(params[7], 0.0, 1.0);  // Input: 0 lets the echoes ring on with nothing new going in
    const double damp = 1.0 - std::exp(-2 * kPi * std::clamp(params[2], 200.0, sampleRate_ * 0.45) / sampleRate_);
    if (params[3] != cutAt_) { cutAt_ = params[3]; cut_.set(sampleRate_, std::clamp(cutAt_, 10.0, 2000.0), 0.7071); }
    if (mix == 0 && quiet_) return;
    const double glide = 1.0 - std::exp(-1.0 / (0.05 * sampleRate_));  // a new time glides in 50 ms (no click)
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      time_ += (target - time_) * glide;
      double read = (double)pos_ - time_;
      if (read < 0) read += (double)size;
      const size_t i0 = (size_t)read % size, i1 = (i0 + 1) % size;
      const double frac = read - std::floor(read);
      double d[2];
      for (int c = 0; c < 2; ++c) {
        d[c] = buf_[c][i0] + (buf_[c][i1] - buf_[c][i0]) * frac;
        lp_[c] = flush(lp_[c] + damp * (d[c] - lp_[c]));
        d[c] = cut_.high(c, lp_[c]);
      }
      const double inL = l[i], inR = r[i];
      // Ping-pong: the input enters on the left as mono and each pass crosses over; 0: each side to itself.
      const double mono = (inL + inR) * 0.5;
      const double wL = send * ((1 - ping) * inL + ping * mono), wR = send * (1 - ping) * inR;
      buf_[0][pos_] = flushf((float)(wL + fb * ((1 - ping) * d[0] + ping * d[1])));
      buf_[1][pos_] = flushf((float)(wR + fb * ((1 - ping) * d[1] + ping * d[0])));
      pos_ = (pos_ + 1) % size;
      const double level = std::max(std::fabs(inL), std::fabs(inR));
      env_ = flush(level > env_ ? env_ + (level - env_) * duckA_ : env_ * duckR_);
      const double g = 1.0 - duck * std::min(1.0, env_ * 4.0);  // -12 dBFS and louder: fully ducked
      peak = std::max(peak, std::fabs(d[0]) + std::fabs(d[1]) + std::fabs(inL) + std::fabs(inR));
      l[i] = (float)(dryOf(mix) * inL + wetOf(mix) * g * d[0]);
      r[i] = (float)(dryOf(mix) * inR + wetOf(mix) * g * d[1]);
    }
    quiet_ = peak < 1e-7;
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Echo&>(p);
    if (o.buf_[0].size() != buf_[0].size()) return;
    buf_[0].swap(o.buf_[0]); buf_[1].swap(o.buf_[1]);
    pos_ = o.pos_; time_ = o.time_; lp_[0] = o.lp_[0]; lp_[1] = o.lp_[1]; cut_ = o.cut_; cutAt_ = o.cutAt_; quiet_ = o.quiet_; env_ = o.env_;
  }
private:
  double bpm_;
  std::vector<float> buf_[2];
  size_t pos_ = 0;
  double time_ = 0, lp_[2] = {0, 0}, env_ = 0, duckA_ = 0, duckR_ = 0;
  Svf cut_;
  double cutAt_ = -1;
  bool quiet_ = true;
};


// Hilbert pair (Olli Niemitalo's 2 x 4 all-pass design): two outputs 90 degrees apart from 20 Hz to 20 kHz at
// 44.1-48 kHz, for the frequency shifter. Each stage: y[n] = a^2 (x[n] + y[n-2]) - x[n-2].
struct Hilbert {
  double x1[4][2] = {}, y1[4][2] = {};
  double x2[4][2] = {}, y2[4][2] = {};
  double delayed = 0;
  static double stage(double a2, double in, double (&x)[2], double (&y)[2]) {
    const double out = a2 * (in + y[1]) - x[1];
    x[1] = x[0]; x[0] = in; y[1] = y[0]; y[0] = flush(out);
    return out;
  }
  void run(double in, double& re, double& im) {
    static const double a[4] = {0.6923878 * 0.6923878, 0.9360654322959 * 0.9360654322959, 0.9882295226860 * 0.9882295226860,
                                0.9987488452737 * 0.9987488452737};
    static const double b[4] = {0.4021921162426 * 0.4021921162426, 0.8561710882420 * 0.8561710882420, 0.9722909545651 * 0.9722909545651,
                                0.9952884791278 * 0.9952884791278};
    double p = in, q = in;
    for (int k = 0; k < 4; ++k) { p = stage(a[k], p, x1[k], y1[k]); q = stage(b[k], q, x2[k], y2[k]); }
    re = delayed;  // the first path one sample late
    delayed = p;
    im = q;
  }
};

// ---- SPIRAL: an echo whose every repeat is shifted in frequency (not pitch): it climbs or falls a few Hz
// each time and goes out of tune, the psychedelic spiral of a frequency-shifted delay. "Shift" in Hz (minus
// falls), "Time" a note value, "Feedback" how many turns, "Tone" darkens each turn, "Low cut" keeps the bass out.
// Mix 0 is the dry signal, 1 only the spiral.
// params: time 0..7 (as ECHO), shift Hz -50..50, feedback 0..0.95, tone Hz, lowcut Hz, mix 0..1
class Spiral final : public Effect {
public:
  Spiral(double sampleRate, double bpm) : Effect(sampleRate), bpm_(bpm) {
    const size_t size = (size_t)std::ceil(4.0 * 60.0 / 40.0 * sampleRate) + 4;
    buf_[0].assign(size, 0.0f);
    buf_[1].assign(size, 0.0f);
  }
  const char* name() const override { return "spiral"; }
  void setTempo(double bpm) override { bpm_ = bpm; song_.tempo(bpm, sampleRate_); }
  void setPosition(double beat) override {
    // As Flux: placed on a jump and when Shift changes while playing, so Shift A -> B -> A is A again.
    const double shift = std::clamp(params[1], -200.0, 200.0);
    const bool jumped = song_.at(beat);
    if (beat >= 0 && (jumped || shift != shiftAt_)) { shift_.place(song_.phase(shift), jumped ? song_.smooth : true); shiftAt_ = shift; }
  }
  void process(float* l, float* r, int n) override {
    song_.run(n);
    const double slide = SongPhase::slideCoef(sampleRate_);
    static const double beats[8] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0};
    const size_t size = buf_[0].size();
    const double target = std::clamp(beats[std::clamp((int)std::lround(params[0]), 0, 7)] * 60.0 / std::max(1.0, bpm_) * sampleRate_, 1.0, (double)size - 3);
    if (time_ <= 0) time_ = target;
    const double step = song_.lock(std::clamp(params[1], -200.0, 200.0)) / sampleRate_;  // repeats every 4 bars
    const double fb = std::clamp(params[2], 0.0, 0.95), mix = std::clamp(params[5], 0.0, 1.0);
    const double send = std::clamp(params[6], 0.0, 1.0);  // Input: 0 lets the spiral turn on with nothing new going in
    const double damp = 1.0 - std::exp(-2 * kPi * std::clamp(params[3], 200.0, sampleRate_ * 0.45) / sampleRate_);
    if (params[4] != cutAt_) { cutAt_ = params[4]; cut_.set(sampleRate_, std::clamp(cutAt_, 10.0, 2000.0), 0.7071); }
    if (mix == 0 && quiet_) { for (int i = 0; i < n; ++i) shift_.next(step, slide); return; }  // it keeps its place in the song
    const double glide = 1.0 - std::exp(-1.0 / (0.05 * sampleRate_));
    double peak = 0;
    for (int i = 0; i < n; ++i) {
      time_ += (target - time_) * glide;
      double read = (double)pos_ - time_;
      if (read < 0) read += (double)size;
      const size_t i0 = (size_t)read % size, i1 = (i0 + 1) % size;
      const double frac = read - std::floor(read);
      const double phase = shift_.next(step, slide);
      const double c = std::cos(2 * kPi * phase), sn = std::sin(2 * kPi * phase);
      const double in[2] = {l[i], r[i]};
      double out[2];
      for (int ch = 0; ch < 2; ++ch) {
        const double d = buf_[ch][i0] + (buf_[ch][i1] - buf_[ch][i0]) * frac;
        lp_[ch] = flush(lp_[ch] + damp * (d - lp_[ch]));
        double re, im;
        hilbert_[ch].run(cut_.high(ch, lp_[ch]), re, im);
        out[ch] = re * c + im * sn;  // the whole spectrum moved up by Shift Hz (down when below 0)
        buf_[ch][pos_] = flushf((float)(send * in[ch] + fb * out[ch]));
      }
      pos_ = (pos_ + 1) % size;
      peak = std::max(peak, std::fabs(out[0]) + std::fabs(out[1]) + std::fabs(in[0]) + std::fabs(in[1]));
      l[i] = (float)(dryOf(mix) * in[0] + wetOf(mix) * out[0]);
      r[i] = (float)(dryOf(mix) * in[1] + wetOf(mix) * out[1]);
    }
    quiet_ = peak < 1e-7;
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Spiral&>(p);
    if (o.buf_[0].size() != buf_[0].size()) return;
    buf_[0].swap(o.buf_[0]); buf_[1].swap(o.buf_[1]);
    pos_ = o.pos_; time_ = o.time_; shift_ = o.shift_; shiftAt_ = o.shiftAt_; song_ = o.song_; lp_[0] = o.lp_[0]; lp_[1] = o.lp_[1];
    hilbert_[0] = o.hilbert_[0]; hilbert_[1] = o.hilbert_[1]; cut_ = o.cut_; cutAt_ = o.cutAt_; quiet_ = o.quiet_;
  }
private:
  double bpm_;
  std::vector<float> buf_[2];
  size_t pos_ = 0;
  double time_ = 0, lp_[2] = {0, 0};
  SongPhase shift_;
  double shiftAt_ = 1e9;
  SongClock song_;
  Hilbert hilbert_[2];
  Svf cut_;
  double cutAt_ = -1;
  bool quiet_ = true;
};

// ---- BODY: thickness above the sub. Below 150 Hz the sound passes untouched (a 4th-order split: kick and
// sub stay mono and clean); above it a warm drive lifted around "Color" and a short moving copy on the sides
// ("Width": an ensemble that makes it wider without changing the middle). Mix 0 is the dry signal.
// params: drive dB 0..24, color Hz, width 0..1, mix 0..1
class Body final : public Effect {
public:
  explicit Body(double sampleRate) : Effect(sampleRate) {
    split_.set(sampleRate, 150.0);
    const size_t size = (size_t)std::ceil(0.03 * sampleRate) + 4;
    buf_.assign(size, 0.0f);
    avg_ = 1.0 - std::exp(-1.0 / (0.3 * sampleRate));
    sub_[0].set(sampleRate, 20.0, 0.5412); sub_[1].set(sampleRate, 20.0, 1.3066);
  }
  const char* name() const override { return "body"; }
  void setTempo(double bpm) override { song_.tempo(bpm, sampleRate_); }
  void setPosition(double beat) override { if (song_.at(beat)) width_.place(song_.phase(kWidthHz), song_.smooth); }
  void process(float* l, float* r, int n) override {
    song_.run(n);
    const double slide = SongPhase::slideCoef(sampleRate_);
    const double drive = dbToGain(std::clamp(params[0], 0.0, 24.0)), width = std::clamp(params[2], 0.0, 1.0), mix = std::clamp(params[3], 0.0, 1.0);
    if (params[1] != colorAt_) { colorAt_ = params[1]; color_.set(sampleRate_, std::clamp(colorAt_, 100.0, 8000.0), 0.8); }
    const double widthStep = song_.lock(kWidthHz) / sampleRate_;  // repeats every 4 bars
    if (mix == 0) { for (int i = 0; i < n; ++i) width_.next(widthStep, slide); return; }  // it keeps its place in the song
    const double makeup = 1.0 / std::max(1.0, std::sqrt(drive));
    const size_t size = buf_.size();
    const double base = 0.012 * sampleRate_, depth = 0.004 * sampleRate_;
    for (int i = 0; i < n; ++i) {
      double hi[2], lo[2];
      const double x[2] = {l[i], r[i]};
      for (int c = 0; c < 2; ++c) {
        split_.run(c, x[c], lo[c], hi[c]);
        double lp;
        const double lifted = hi[c] + color_.run(c, hi[c], lp);  // a bump around Color before the drive
        const double driven = std::tanh(lifted * drive) * makeup;
        // Level match (300 ms): the band above the sub comes out as loud as it went in, thicker, not louder.
        inE_[c] = flush(inE_[c] + avg_ * (hi[c] * hi[c] - inE_[c]));
        outE_[c] = flush(outE_[c] + avg_ * (driven * driven - outE_[c]));
        const double match = outE_[c] > 1e-12 ? std::clamp(std::sqrt(inE_[c] / outE_[c]), 0.25, 2.0) : 1.0;
        // The drive makes difference tones of the band; the ones under 20 Hz only flap a driver (the sweep:
        // -24 dB of the sound under 15 Hz at Drive 24). A 4th-order Butterworth high-pass takes them out.
        hi[c] = sub_[1].high(c, sub_[0].high(c, driven * match));
      }
      // Width: a slowly moving 8-16 ms copy of the middle, added to one side and taken from the other.
      const double mid = 0.5 * (hi[0] + hi[1]);
      buf_[pos_] = flushf((float)mid);
      double read = (double)pos_ - (base + depth * std::sin(2 * kPi * width_.next(widthStep, slide)));
      while (read < 0) read += (double)size;
      const size_t i0 = (size_t)read % size, i1 = (i0 + 1) % size;
      const double fr = read - std::floor(read);
      const double side = (buf_[i0] + (buf_[i1] - buf_[i0]) * fr) * width * 0.7;
      pos_ = (pos_ + 1) % size;
      const double wet[2] = {lo[0] + hi[0] + side, lo[1] + hi[1] - side};
      l[i] = (float)((1 - mix) * x[0] + mix * wet[0]);
      r[i] = (float)((1 - mix) * x[1] + mix * wet[1]);
    }
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Body&>(p);
    split_ = o.split_; color_ = o.color_; colorAt_ = o.colorAt_; width_ = o.width_; song_ = o.song_; sub_[0] = o.sub_[0]; sub_[1] = o.sub_[1];
    for (int c = 0; c < 2; ++c) { inE_[c] = o.inE_[c]; outE_[c] = o.outE_[c]; }
    if (o.buf_.size() == buf_.size()) { buf_.swap(o.buf_); pos_ = o.pos_; }
  }
private:
  Crossover split_;
  Svf color_, sub_[2];
  static constexpr double kWidthHz = 0.37;
  double colorAt_ = -1, avg_ = 0, inE_[2] = {0, 0}, outE_[2] = {0, 0};
  SongPhase width_;
  SongClock song_;
  std::vector<float> buf_;
  size_t pos_ = 0;
};


// ---- FLUX: the moving-sound family in one device, one "Mode" at a time. Phaser: six all-pass stages sweep
// notches up and down (200 Hz - 4 kHz). Flanger: a 0.3-6 ms delay that moves, a jet sweep. Chorus: two voices
// 8-25 ms late that drift, a wider, thicker ensemble. "Rate" is how fast it moves, "Depth" how far,
// "Feedback" how strong (metallic for the flanger, sharper notches for the phaser; the chorus ignores it).
// The two sides move a quarter turn apart, so it is wide. Only above 150 Hz (a 4th-order split): kick and sub
// stay where they are (the sweep found a phaser taking 10 dB from the low end). Mix 0 is the dry signal.
// params: mode 0..2 (Phaser, Flanger, Chorus), rate Hz, depth 0..1, feedback 0..0.9, mix 0..1
class Flux final : public Effect {
public:
  explicit Flux(double sampleRate) : Effect(sampleRate) {
    split_.set(sampleRate, 150.0);
    const size_t size = (size_t)std::ceil(0.04 * sampleRate) + 4;
    buf_[0].assign(size, 0.0f);
    buf_[1].assign(size, 0.0f);
  }
  const char* name() const override { return "flux"; }
  void setTempo(double bpm) override { song_.tempo(bpm, sampleRate_); }
  void setPosition(double beat) override {
    // Placed by the song on a jump, and also when Rate changes while playing (slid there, ~30 ms): otherwise the sweep
    // only ran on at the other rate, and Rate A -> B -> A left it somewhere else for good (Diogo 2026-11-08: back to a
    // value = the sound from before).
    const double rate = std::clamp(params[1], 0.01, 20.0);
    const bool jumped = song_.at(beat);
    if (beat >= 0 && (jumped || rate != rateAt_)) { sweep_.place(song_.phase(rate), jumped ? song_.smooth : true); rateAt_ = rate; }
    if (jumped) at_ = 0;
  }
  void process(float* l, float* r, int n) override {
    song_.run(n);
    const double slide = SongPhase::slideCoef(sampleRate_);
    const int mode = std::clamp((int)std::lround(params[0]), 0, 2);
    const double rate = std::clamp(params[1], 0.01, 20.0), depth = std::clamp(params[2], 0.0, 1.0);
    const double fb = std::clamp(params[3], 0.0, 0.9), mix = std::clamp(params[4], 0.0, 1.0);
    const double step = song_.lock(rate) / sampleRate_;  // repeats every 4 bars
    if (mix == 0) { for (int i = 0; i < n; ++i) sweep_.next(step, slide); at_ += (unsigned)n; return; }  // it keeps its place in the song
    const size_t size = buf_[0].size();
    float* ch[2] = {l, r};
    for (int i = 0; i < n; ++i) {
      const double phase = sweep_.next(step, slide);
      for (int c = 0; c < 2; ++c) {
        const double lfo = 0.5 + 0.5 * std::sin(2 * kPi * (phase + 0.25 * c));  // 0..1
        double low, x;
        split_.run(c, ch[c][i], low, x);
        double wet;
        if (mode == 0) {
          if ((at_ & 7) == 0) {  // the all-pass frequency, every 8 samples
            const double hz = 200.0 * std::pow(20.0, depth * lfo);
            const double w = std::tan(kPi * std::min(hz, sampleRate_ * 0.45) / sampleRate_);
            coef_[c] = (w - 1) / (w + 1);
          }
          double y = x + fb * last_[c];
          for (int s = 0; s < 6; ++s) { const double o = coef_[c] * y + z_[c][s]; z_[c][s] = flush(y - coef_[c] * o); y = o; }
          last_[c] = flush(y);
          wet = y;
        } else {
          const double ms = mode == 1 ? 0.3 + 5.7 * depth * lfo : 8.0 + 17.0 * (0.3 + 0.7 * depth) * lfo;
          double rd = (double)pos_ - ms * 0.001 * sampleRate_;
          while (rd < 0) rd += (double)size;
          const size_t a = (size_t)rd % size, b = (a + 1) % size;
          const double d = buf_[c][a] + (buf_[c][b] - buf_[c][a]) * (rd - std::floor(rd));
          buf_[c][pos_] = flushf((float)(x + (mode == 1 ? fb * d : 0.0)));
          wet = mode == 1 ? 0.5 * (x + d) : d;
        }
        ch[c][i] = (float)(low + (1 - mix) * x + mix * wet);
      }
      ++at_;
      pos_ = (pos_ + 1) % size;
    }
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Flux&>(p);
    std::memcpy(z_, o.z_, sizeof z_); sweep_ = o.sweep_; song_ = o.song_; at_ = o.at_; split_ = o.split_; rateAt_ = o.rateAt_;
    last_[0] = o.last_[0]; last_[1] = o.last_[1]; coef_[0] = o.coef_[0]; coef_[1] = o.coef_[1];
    if (o.buf_[0].size() == buf_[0].size()) { buf_[0].swap(o.buf_[0]); buf_[1].swap(o.buf_[1]); pos_ = o.pos_; }
  }
private:
  std::vector<float> buf_[2];
  size_t pos_ = 0;
  double z_[2][6] = {}, last_[2] = {0, 0}, coef_[2] = {0, 0};
  SongPhase sweep_;
  SongClock song_;
  unsigned at_ = 0;
  double rateAt_ = -1;
  Crossover split_;
};

}  // namespace

std::unique_ptr<Effect> makeDevice(const std::string& name, double sampleRate, double bpm, int /*version*/) {
  if (name == "growl") return std::make_unique<Growl>(sampleRate, bpm);
  if (name == "metal") return std::make_unique<Metal>(sampleRate);
  if (name == "punch") return std::make_unique<Punch>(sampleRate);
  if (name == "zap") return std::make_unique<Zap>(sampleRate, bpm);
  if (name == "glitch") return std::make_unique<Glitch>(sampleRate, bpm);
  if (name == "pulse") return std::make_unique<Pulse>(sampleRate, bpm);
  if (name == "space") return std::make_unique<Space>(sampleRate);
  if (name == "echo") return std::make_unique<Echo>(sampleRate, bpm);
  if (name == "spiral") return std::make_unique<Spiral>(sampleRate, bpm);
  if (name == "body") return std::make_unique<Body>(sampleRate);
  if (name == "flux") return std::make_unique<Flux>(sampleRate);
  return nullptr;
}

// The knobs of CORE FX's building blocks, in the order each one reads params[] (CoreFx sets them; the tests
// drive the blocks alone with them).
const std::vector<ParamSpec>& coreFxBlockParams(const std::string& name) {
  static const std::vector<ParamSpec> none, space = {{"size", 0.6, 0, 1, ""}, {"decay", 2, 0.2, 30, "s"}, {"predelay", 20, 0, 250, "ms"}, {"tone", 7000, 1000, 18000, "Hz"},
                                                 {"lowcut", 300, 20, 1000, "Hz"}, {"motion", 0.3, 0, 1, ""}, {"mix", 0.25, 0, 1, ""}, {"shimmer", 0, 0, 1, ""}, {"input", 1, 0, 1, ""},
                                                 {"kind", 0, 0, 4, ""}, {"air", 0, 0, 1, ""}},
      echo = {{"time", 2, 0, 7, ""}, {"feedback", 0.4, 0, 0.95, ""}, {"tone", 5000, 1000, 16000, "Hz"}, {"lowcut", 250, 20, 1000, "Hz"},
              {"pingpong", 0.5, 0, 1, ""}, {"duck", 0, 0, 1, ""}, {"mix", 0.25, 0, 1, ""}, {"input", 1, 0, 1, ""}},
      spiral = {{"time", 1, 0, 7, ""}, {"shift", 8, -50, 50, "Hz"}, {"feedback", 0.6, 0, 0.95, ""}, {"tone", 5000, 1000, 16000, "Hz"},
                {"lowcut", 300, 20, 1000, "Hz"}, {"mix", 0.25, 0, 1, ""}, {"input", 1, 0, 1, ""}},
      flux = {{"mode", 0, 0, 2, ""}, {"rate", 0.5, 0.02, 8, "Hz"}, {"depth", 0.6, 0, 1, ""}, {"feedback", 0.4, 0, 0.9, ""}, {"mix", 0.5, 0, 1, ""}};
  if (name == "space") return space;
  if (name == "echo") return echo;
  if (name == "spiral") return spiral;
  if (name == "flux") return flux;
  return none;
}

// One musical result each, a few knobs. The order of each list is the order the device reads params[].
void registerNativeDevices(NativeRegistry& reg) {
  const auto add = [&reg](const char* id, std::vector<ParamSpec> params) {
    const std::string name = id;
    reg.add({name, NativeKind::Effect, 1, false, std::move(params), nullptr,
             [name](double sr, double bpm, int v) { return makeDevice(name, sr, bpm, v); }});
  };
  add("growl", {{"vowel", 0.3, 0, 1, ""}, {"talk", 0.5, 0, 1, ""}, {"grit", 12, 0, 36, "dB"}, {"body", 150, 60, 400, "Hz"}, {"mix", 1, 0, 1, ""}});
  add("metal", {{"note", 30, 24, 84, "note"}, {"ring", 0.2, 0, 1, ""}, {"decay", 120, 10, 2000, "ms"}, {"crush", 0, 0, 1, ""}, {"mix", 0.35, 0, 1, ""}});
  add("punch", {{"click", 6, -12, 18, "dB"}, {"tail", 0, -24, 12, "dB"}, {"clip", 0.3, 0, 1, ""}, {"output", 0, -24, 6, "dB"}});
  add("zap", {{"pitch", 1500, 100, 8000, "Hz"}, {"amount", 0.6, 0, 1, ""}, {"sweep", 2, -4, 4, "oct"}, {"length", 80, 10, 500, "ms"}, {"mix", 1, 0, 1, ""}, {"sync", 0, 0, 5, ""}});
  add("glitch", {{"rate", 2, 0, 3, ""}, {"repeats", 4, 2, 16, ""}, {"chance", 0.3, 0, 1, ""}, {"mix", 1, 0, 1, ""}, {"reverse", 0, 0, 1, ""},
                 {"pitch", 0, -24, 24, "st"}, {"tone", 0, -1, 1, ""}, {"vary", 0, 0, 1, ""}, {"length", 1, 0.05, 1, ""},
                 {"echo", 0, 0, 1, ""}, {"spread", 0, 0, 1, ""}});
  add("pulse", {{"pattern", 1, 0, 7, ""}, {"depth", 0.8, 0, 1, ""}, {"smooth", 0.2, 0, 1, ""}, {"swing", 0, 0, 1, ""}});
  add("body", {{"drive", 6, 0, 24, "dB"}, {"color", 900, 100, 8000, "Hz"}, {"width", 0.4, 0, 1, ""}, {"mix", 1, 0, 1, ""}});
}

}  // namespace core
