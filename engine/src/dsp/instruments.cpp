#include "instruments.hpp"
#include "registry.hpp"
#include "sample_path.hpp"
#include "song_clock.hpp"
#include "wavetables.hpp"
#include "voice_filter.hpp"
#include "wave_mod.hpp"
#include "../io/audio_file.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>

namespace core {
namespace {

constexpr double kTau = 6.283185307179586;
inline double clampd(double x, double lo, double hi) { return std::min(hi, std::max(lo, x)); }
inline double msToCoef(double ms, double sr) { return std::exp(-1.0 / std::max(1.0, ms * 0.001 * sr)); }
inline double midiToHz(double pitch) { return 440.0 * std::pow(2.0, (pitch - 69.0) / 12.0); }

// Deterministic noise: the same in real time and in the export.
struct Noise {
  uint32_t state = 22222;
  inline double next() { state = state * 1664525u + 1013904223u; return (double)state / 2147483648.0 - 1.0; }
};

// TPT state-variable filter.
struct Svf {
  double ic1 = 0, ic2 = 0;
  double g = 0, k = 2, a1 = 0;
  void set(double cutoff, double resonance, double sr) {
    g = std::tan(3.141592653589793 * clampd(cutoff, 20, sr * 0.45) / sr);
    k = 2 - 2 * clampd(resonance, 0, 0.98);
    a1 = 1 / (1 + g * (g + k));
  }
  inline double lp(double x) { tick(x); return v2; }
  inline double hp(double x) { tick(x); return x - k * v1 - v2; }
  inline double bp(double x) { tick(x); return k * v1; }  // unity gain at the cutoff
private:
  double v1 = 0, v2 = 0;
  inline void tick(double x) {
    double v3 = x - ic2;
    v1 = a1 * ic1 + g * a1 * v3;
    v2 = ic2 + g * v1;
    ic1 = 2 * v1 - ic1;
    ic2 = 2 * v2 - ic2;
  }
};

inline double polyBlep(double t, double dt) {
  if (t < dt) { t /= dt; return t + t - t * t - 1; }
  if (t > 1 - dt) { t = (t - 1) / dt; return t * t + t + t + 1; }
  return 0;
}

// Fade applied to a voice that is cut by a new note (3 ms), so retriggers do not click; a note caught
// mid-way (chase) fades in as fast.
constexpr double kStealMs = 3.0;
inline double fadeInStep(double sr) { return 1.0 / (kStealMs * 0.001 * sr); }

// ---------------------------------------------------------------- kick
// Core's one kick (2026-11-07, Diogo: "não deixamos nada antigo"), remade 2026-11-08 against his own kicks
// (Diogo: kick and bass "soam uma merda"). Each sound is a copy of one of his library kicks, fitted by render +
// measure (tools/kick_fit: band energies over time, the 2 ms envelope and the spectrum, against the sample):
// - pitch in two stages: from Start it drops fast (Pitch decay) to the Knee, then slowly (Knee decay) to End;
// - level: a short attack, then the body SWELLS from a lower start to full (his kicks peak at 40-80 ms, not at the
//   first cycle: that is the "power"), Hold, Decay, and a hard end at Length (no 808 tail);
// - click: noise through a band-pass at Click tone;
// - Drive (tanh) shapes the body's harmonics. No hard clip any more: it squared off the first 25 ms (the "tiro de
//   jogo SNES" top) and is not in any of his kicks.
// Like the hats, the kick track holds several sounds, one per key (the piano roll's rows):
//   C2 (36) = the knobs (they start at Psiger's KICK), C#2..F2 = his other kicks, played as fitted (Level still applies).
class Kick final : public Instrument {
public:
  enum { pitchStart, pitchEnd, pitchDecay, ampDecay, click, drive, level, hold, knee, kneeDecay, length, clickTone };
  // One fitted sound: the 12 knobs' values, then the ones fixed per sound.
  struct Sound {
    double fs, fe, tf, de, click, drive, hold, knee, tk, length, ctone;  // as the knobs
    double s0, ts, att, clen, phase, rel;  // swell start (0..1), swell time ms, attack ms, click length ms, phase, release ms
    double gain;  // brings each sound to C2's loudness
    const char* from;  // the library kick it copies
  };
  static const Sound kSounds[];       // C2 (the knobs' defaults) then the variations, key 36 up
  static const int kSoundCount;
private:
  struct Voice {
    bool active = false; double t = 0, phase = 0, velocity = 0, fade = 1, fadeStep = 0; Noise noise; double fadeIn = 1;
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;  // the click's band-pass
    int sound = 0;
  };
  Voice voices_[2];
  int next_ = 0;
  Sound soundOf(int i) const {
    if (i > 0) return kSounds[i];
    Sound s = kSounds[0];  // C2: the knobs
    s.fs = params[pitchStart]; s.fe = params[pitchEnd]; s.tf = params[pitchDecay]; s.de = params[ampDecay];
    s.click = params[click]; s.drive = params[drive]; s.hold = params[hold]; s.knee = params[knee];
    s.tk = params[kneeDecay]; s.length = params[length]; s.ctone = params[clickTone];
    return s;
  }
  static int soundFor(int pitch) { return pitch > 36 && pitch < 36 + kSoundCount ? pitch - 36 : 0; }
  double endSamples(int sound) const { return (std::max(1.0, soundOf(sound).length) + 1) * 0.001 * sampleRate_; }
public:
  explicit Kick(double sr) : Instrument(sr) {
    const Sound& s = kSounds[0];
    const double def[] = {s.fs, s.fe, s.tf, s.de, s.click, s.drive, 0.85, s.hold, s.knee, s.tk, s.length, s.ctone};
    for (int i = 0; i < 12; ++i) params[i] = def[i];
  }
  const char* name() const override { return "kick"; }
  void noteOn(int pitch, double velocity) override {
    for (auto& v : voices_) if (v.active && v.fadeStep == 0) v.fadeStep = 1.0 / (kStealMs * 0.001 * sampleRate_);
    Voice& v = voices_[next_]; next_ = (next_ + 1) % 2;
    v = Voice{};
    v.active = true; v.velocity = velocity; v.sound = soundFor(pitch);
  }
  void chaseNote(int pitch, double velocity, double age) override {
    if (age <= 0) { noteOn(pitch, velocity); return; }
    if (age >= endSamples(soundFor(pitch))) return;  // its tail is already over
    noteOn(pitch, velocity);
    Voice& v = voices_[(next_ + 1) % 2];
    v.t = age; v.fadeIn = 0;
  }
  void noteOff(int) override {}
  void allNotesOff() override { for (auto& v : voices_) if (v.active) v.fadeStep = 1.0 / (kStealMs * 0.001 * sampleRate_); }
  void adopt(const Instrument& previous) override {
    const auto& p = static_cast<const Kick&>(previous);
    for (int i = 0; i < 2; ++i) voices_[i] = p.voices_[i];
    next_ = p.next_;
  }
  void render(float* out, int frames) override {
    const double sr = sampleRate_, msPerSample = 1000.0 / sr;
    for (auto& v : voices_) {
      if (!v.active) continue;
      const Sound k = soundOf(v.sound);
      const double tf = std::max(0.05, k.tf), tk = std::max(1.0, k.tk), ho = std::max(0.0, k.hold), de = std::max(1.0, k.de);
      const double len = std::max(1.0, k.length), att = std::max(0.05, k.att), ts = std::max(0.1, k.ts), rel = std::max(0.5, k.rel);
      const double clen = std::max(0.05, k.clen), clickEnd = clen * 14 + 5;
      const double dr = std::max(0.01, k.drive * 8), drNorm = 1 / std::tanh(dr);
      // RBJ band-pass (0 dB peak), Q 0.7, at the click tone.
      const double w = kTau * clampd(k.ctone, 100, sr * 0.45) / sr, al = std::sin(w) / (2 * 0.7);
      const double b0 = al / (1 + al), b2 = -al / (1 + al), a1 = -2 * std::cos(w) / (1 + al), a2 = (1 - al) / (1 + al);
      const double start = k.phase * 0.25, gain = params[level] * k.gain * v.velocity;
      for (int i = 0; i < frames; ++i) {
        const double t = v.t * msPerSample;
        const double f = k.fe + (k.knee - k.fe) * std::exp(-t / tk) + (k.fs - k.knee) * std::exp(-t / tf);
        v.phase += f / sr; if (v.phase >= 1) v.phase -= 1;
        const double swell = k.s0 + (1 - k.s0) * (1 - std::exp(-t / ts));
        const double env = std::min(1.0, t / att) * swell * (t < ho ? 1.0 : std::exp(-(t - ho) / de)) * clampd((len - t) / rel, 0, 1);
        double s = std::sin(kTau * (v.phase + start)) * env;
        if (t < clickEnd) {
          const double x = v.noise.next() * 1.7320508 * std::exp(-t / clen);  // uniform noise at a Gaussian's level
          const double y = b0 * x + b2 * v.x2 - a1 * v.y1 - a2 * v.y2;
          v.x2 = v.x1; v.x1 = x; v.y2 = v.y1; v.y1 = y;
          s += k.click * y * 2;
        }
        s = std::tanh(dr * s) * drNorm * gain;
        if (v.fadeIn < 1) { v.fadeIn = std::min(1.0, v.fadeIn + fadeInStep(sr)); s *= v.fadeIn; }
        if (v.fadeStep > 0) { s *= v.fade; v.fade -= v.fadeStep; }
        out[i] += (float)s;
        v.t += 1;
        if (t >= len || v.fade <= 0) { v.active = false; break; }
      }
    }
  }
};

// Fitted 2026-11-08 (tools/kick_fit): the sample each copies, then its values. C2 first.
const Kick::Sound Kick::kSounds[] = {
    {1938, 42.13, 3.409, 251.9, 0.1615, 0.09822, 68.98, 144.7, 56.01, 206.6, 6364, 0.9038, 17.72, 0.9277, 2.37, 0.8284, 7.307, 1, "Kick"},  // Kick__Bass_Oneshots_Psiger_Serum_2_KICK
    {3528, 41.41, 2.231, 97.48, 0.4527, 0.1809, 64.92, 166.4, 44.09, 338.4, 7384, 0.8616, 20.69, 2.156, 0.9687, 0.3217, 17.56, 1.043, "Long"},  // Kick__Bass_Oneshots_Bananas_KICK
    {1600, 46.97, 4.637, 122.5, 0.5914, 0.03735, 89.38, 146.1, 54.62, 241.5, 1.199e+04, 0.3001, 21.2, 4.006, 2.738, 0.3033, 25.62, 1.186, "Punchy"},  // Kick__Bass_Oneshots_Vibration_FS1_07_KICK_A
    {520.9, 34.4, 8.503, 239.6, 0.9116, 0.1777, 44.19, 76.14, 174, 204.6, 6489, 0.8048, 58.27, 1.423, 0.3281, 0.2279, 25.26, 0.9956, "Deep"},  // Kick__Bass_Oneshots_Vibration_FS2_02_KICK_D
    {1217, 46.76, 5.886, 106.2, 0.4663, 0.2936, 18.95, 136.1, 50.41, 200.9, 1.171e+04, 0.9149, 27.52, 3.153, 3.772, 0.875, 26.86, 1.046, "Tight"},  // Kick_Oneshots_Function_Loops_FL_PST_Kit03_Kick_One_Shot
    {529.9, 44.46, 6.955, 56.8, 0.321, 0.1085, 116.2, 168.1, 47.99, 248.3, 8144, 0.3312, 26.22, 2.008, 1.136, 0.342, 29.97, 1.113, "Round"},  // Kick__Bass_Oneshots_Vibration_FS1_02_KICK_G
};
const int Kick::kSoundCount = (int)(sizeof(kSounds) / sizeof(kSounds[0]));

// ---------------------------------------------------------------- hats (42 closed, 44 pedal, 46 open, 51 ride, 70 shaker)
// The three hats are one choke group: a new hat cuts the one still ringing (a closed hat stops an open one).
// Ride and shaker are their own voices (a hat does not cut them; a new ride cuts the previous ride).
// The new sounds come from the same four controls: pedal and shaker follow the closed decay, the ride the
// open one, all of them the tone.
class Hats final : public Instrument {
  enum { tone, closedDecay, openDecay, level };
  enum Kind { Closed, Open, Pedal, Ride, Shaker };
  struct Voice {
    bool active = false; Kind kind = Closed;
    double amp = 0, velocity = 0, fade = 1, fadeStep = 0; Svf hp, hp2, lp; Noise noise; double fadeIn = 1;
    double attack = 1;          // the shaker's 6 ms rise (1: none)
    double phase[6] = {};       // the ride's metal partials
  };
  // Two voices per group (hats 0-1, ride 2-3, shaker 4-5): a new note fades the one still sounding in its group.
  static constexpr int kVoices = 6;
  Voice voices_[kVoices];
  static Kind kindOf(int pitch) {
    return pitch == 46 ? Open : pitch == 44 ? Pedal : pitch == 51 || pitch == 59 ? Ride : pitch == 70 || pitch == 82 ? Shaker : Closed;
  }
  static bool hat(Kind k) { return k == Closed || k == Open || k == Pedal; }
  double decayMs(Kind k) const {
    switch (k) {
      case Open: return params[openDecay];
      case Pedal: return params[closedDecay] * 1.4;
      case Ride: return params[openDecay] * 3.0;
      case Shaker: return params[closedDecay] * 1.8;
      default: return params[closedDecay];
    }
  }
public:
  using Instrument::Instrument;
  const char* name() const override { return "hats"; }
  void noteOn(int pitch, double velocity) override {
    // Closed and open hats play exactly as before the new sounds came (the golden export holds them).
    const Kind kind = kindOf(pitch);
    Voice* pair = &voices_[hat(kind) ? 0 : kind == Ride ? 2 : 4];
    for (int i = 0; i < 2; ++i) if (pair[i].active && pair[i].fadeStep == 0) pair[i].fadeStep = 1.0 / (kStealMs * 0.001 * sampleRate_);
    Voice& v = !pair[0].active ? pair[0] : !pair[1].active ? pair[1] : (pair[0].fade < pair[1].fade ? pair[0] : pair[1]);
    v.active = true; v.kind = kind; v.amp = 1; v.velocity = velocity; v.fade = 1; v.fadeStep = 0; v.noise = Noise{}; v.fadeIn = 1;
    v.attack = kind == Shaker ? 0 : 1;
    for (double& ph : v.phase) ph = 0;
    if (kind == Pedal) v.velocity *= 0.7;
    if (kind == Shaker) v.velocity *= 0.8;
    if (kind == Ride) v.velocity *= 0.55;
  }
  void chaseNote(int pitch, double velocity, double age) override {
    if (age <= 0) { noteOn(pitch, velocity); return; }
    const double amp = std::pow(msToCoef(decayMs(kindOf(pitch)), sampleRate_), age);
    if (amp < 1e-5) return;
    noteOn(pitch, velocity);
    for (auto& v : voices_) if (v.active && v.fadeStep == 0 && v.amp == 1) { v.amp = amp; v.fadeIn = 0; v.attack = 1; break; }
  }
  void noteOff(int) override {}
  void allNotesOff() override { for (auto& v : voices_) if (v.active) v.fadeStep = 1.0 / (kStealMs * 0.001 * sampleRate_); }
  void adopt(const Instrument& previous) override {
    const auto& p = static_cast<const Hats&>(previous);
    for (int i = 0; i < kVoices; ++i) voices_[i] = p.voices_[i];
  }
  void render(float* out, int frames) override {
    // The ride's partials: the classic six square waves at inharmonic ratios, an octave up for a bell.
    static constexpr double kMetal[6] = {410.6, 608.8, 739.2, 1045.4, 1080.0, 1600.0};
    for (auto& v : voices_) {
      if (!v.active) continue;
      const double t = params[tone];
      switch (v.kind) {
        case Pedal: v.hp.set(t * 0.8, 0.2, sampleRate_); v.hp2.set(t * 0.5, 0.1, sampleRate_); break;
        case Ride: v.hp.set(t * 0.6, 0.3, sampleRate_); v.hp2.set(t * 0.35, 0.1, sampleRate_); break;
        case Shaker: v.hp.set(t * 0.7, 0.1, sampleRate_); v.hp2.set(t * 0.4, 0.1, sampleRate_); v.lp.set(t * 1.3, 0.1, sampleRate_); break;
        default: v.hp.set(t, 0.2, sampleRate_); v.hp2.set(t * 0.6, 0.1, sampleRate_); break;
      }
      const double d = msToCoef(decayMs(v.kind), sampleRate_);
      const double rise = 1.0 / (0.006 * sampleRate_);
      for (int i = 0; i < frames; ++i) {
        double x = v.noise.next();
        if (v.kind == Ride) {
          double metal = 0;
          for (int k = 0; k < 6; ++k) {
            v.phase[k] += kMetal[k] / sampleRate_;
            if (v.phase[k] >= 1) v.phase[k] -= 1;
            metal += v.phase[k] < 0.5 ? 1 : -1;
          }
          x = metal / 6.0 + x * 0.25;
        }
        double s = v.hp.hp(v.hp2.hp(x));
        if (v.kind == Shaker) s = v.lp.lp(s);
        s = s * v.amp * v.velocity * params[level];
        if (v.attack < 1) { v.attack = std::min(1.0, v.attack + rise); s *= v.attack; }
        if (v.fadeIn < 1) { v.fadeIn = std::min(1.0, v.fadeIn + fadeInStep(sampleRate_)); s *= v.fadeIn; }
        if (v.fadeStep > 0) { s *= v.fade; v.fade -= v.fadeStep; }
        out[i] += (float)s;
        if (v.attack >= 1) v.amp *= d;
        if (v.amp < 1e-5 || v.fade <= 0) { v.active = false; break; }
      }
    }
  }
};

// ---------------------------------------------------------------- W-Core · Wave: Core's lead, pad and drone synth
// docs/historico/w-core-wave.md (core-main#121). Stereo, notes on their exact sample (queueNote/renderStereo, like the sampler).
// One Wave is one layer of W-Core (WaveStack holds four). Sources, then the filter, then the voice's volume:
// 1. Shape: five band-limited shapes (Round, Warm, Hollow, Buzzy, Hard), morphing between two neighbours,
//    or a wavetable (osc1Table) at a position.
// 2. Spread: up to 7 oscillators per voice, the centre one plus 6 detuned at the JP-8000 "supersaw" ratios; the detune
//    curve and the centre/side mix follow Adam Szabo's measurements ("How to Emulate the Super Saw", 2010). The side
//    oscillators are panned alternately left and right; the centre and the Stack go to both sides, so the mono sum stays.
// 3. Stack: the same wave one or two octaves below (-1, -2) or one above (+1), one oscillator, centre.
//    Osc 2 and Osc 3: more waves (shape or table) at their own interval, mixed in before the filter; they share Spread.
//    Noise, Grain (short windows of a sound of the library, or of the wave) and FM (every oscillator's phase moved by
//    a sine at the note times fmRatio) as before.
// 4. Tone / Bite: the filter (20 types, Drive and Shape for the newer ones), then Drive (a soft clip) and Crush.
// 5. Mode: 0 Lead (mono, the last note wins, Glide ms between legato notes), 1 Pad (8 voices, the oldest is taken with
//    a 3 ms fade). Every oscillator restarts at its own fixed phase, so the export is the same as real time.
// 6. Modulation (version 2, 2026-11-08, wave_mod.hpp): ENV 1 (the volume), ENV 2 and ENV 3 per voice (attack, hold,
//    decay, sustain, release); LFO 1..4 (a shape or drawn points; in time with the song, or again on each note, or
//    once); Velocity, Note and four Macros; and up to 32 links from a source to a knob or to pitch, volume, pan and
//    the detune, each evaluated per voice and per sample. The fixed movements of version 1 (Contour/Bloom, Swell/Fade,
//    Zap, Drift, Key follow, Velocity, LFO 1/2 with one destination) are links now (wave_legacy.hpp converts them).
class Wave final : public Instrument {
public:
  enum { wave, uniDetune, subOct, octave, tone, bite, glide, mode, level, noise, noiseColor, fm, fmRatio, tune,
         grain, grainSize, grainDensity, grainPos, grainScan, grainScatter,
         osc2, osc2Wave, osc2Semi, osc2Fine, filterMode, drive,
         osc1Table, osc1Pos, osc2Table, osc2Pos, osc1Semi, crush,
         filterDrive, filterShape,
         env1Attack, env1Hold, env1Decay, env1Sustain, env1Release,
         env2Attack, env2Hold, env2Decay, env2Sustain, env2Release,
         env3Attack, env3Hold, env3Decay, env3Sustain, env3Release,
         lfo1Rate, lfo1Hz, lfo1Phase, lfo1Smooth, lfo2Rate, lfo2Hz, lfo2Phase, lfo2Smooth,
         lfo3Rate, lfo3Hz, lfo3Phase, lfo3Smooth, lfo4Rate, lfo4Hz, lfo4Phase, lfo4Smooth,
         macro1, macro2, macro3, macro4,
         unison, uniBlend, uniWidth, subLevel, subShape,
         osc1Warp, osc1WarpAmt, osc2Warp, osc2WarpAmt,
         bend, bendRange, modWheel, polyVoices, legato, glideAlways,
         filter2Mode, tone2, bite2, filter2Drive, filter2Shape, filterSerial,
         osc1Route, osc2Route, subRoute, noiseRoute,
         osc1Level, subPan, filterMix, filter2Mix, driveColour, driveMix,
         lfo1Depth, lfo2Depth, lfo3Depth, lfo4Depth, outPan, grainPitch, kCount };
  static int envSlot(int e) { return env1Attack + 5 * e; }  // + 0 attack, 1 hold, 2 decay, 3 sustain, 4 release
  static int lfoSlot(int k) { return lfo1Rate + 4 * k; }    // + 0 rate (beats), 1 Hz, 2 phase, 3 smooth (ms)
  // Grain (the plan's granular source): short windowed grains, each played at the note's pitch (the source's root is
  // C4), from a sound of the library (the track's first sample zone, given by the graph) or, without one, from the
  // wave itself. Size = each grain's length, Density = grains per second, Position = where in the sound they start,
  // Scan = how fast that place moves on (0 freezes it), Scatter = random spread of place, pitch (up to a fifth) and
  // pan. Random choices come from each voice's own seed, so a render is the same every time (export = real time).
private:
  using Src = wmod::Source;
  static constexpr int kGrains = 16;
  struct Grain { bool on = false; double pos = 0, inc = 0, age = 0, len = 1, l = 0.7071, r = 0.7071; };
  static constexpr int kVoices = 8, kOsc = 16, kQueue = 256, kHeld = 16, kEnvs = wmod::kEnvs, kLfos = wmod::kLfos;
  // Two more slots only for the ghosts of cut notes (start()): with all 8 voices sounding, the ghost used to take
  // the quietest other voice and silence it in one sample (audit 2026-11-07 #5: a step of 0.098, 37 dB over the sound).
  static constexpr int kGhosts = 2;
  static constexpr int kWarps = 12;  // off, sync, bend+, bend-, PWM, asym, mirror, flip, quantize, FM, AM, RM
  // Unison: up to 16 copies of each oscillator, detuned and spread left and right. Seven is the classic supersaw
  // (Szabo's measured detune offsets); the other counts spread their copies evenly over the same width.
  static constexpr double kDetune7[7] = {-0.11002313, -0.06288439, -0.01952356, 0, 0.01991221, 0.06216538, 0.10745242};
  static constexpr double kStart[kOsc] = {0.13, 0.71, 0.37, 0.0, 0.53, 0.89, 0.29, 0.61, 0.07, 0.83, 0.47, 0.19, 0.97, 0.33, 0.77, 0.41};
  // Each count's copies, from the lowest detune to the highest: their phase slot (the centre copy is slot 3, so one
  // voice and seven voices start as before), detune (x the Detune curve), left gain (right = 1.25 - left) and centre.
  struct UniLayout { int n = 1; int slot[kOsc] = {}; double det[kOsc] = {}, panL[kOsc] = {}; bool centre[kOsc] = {}; };
  static const UniLayout& uniLayout(int n) {
    static const auto table = [] {
      std::array<UniLayout, kOsc + 1> t{};
      for (int n = 1; n <= kOsc; ++n) {
        UniLayout& u = t[n];
        u.n = n;
        static const int order[kOsc] = {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 3};
        int next = 0;
        for (int k = 0; k < n; ++k) {
          const bool odd = n % 2 == 1;
          u.centre[k] = odd ? k == (n - 1) / 2 : (k == n / 2 - 1 || k == n / 2);
          u.slot[k] = odd && u.centre[k] ? 3 : order[next++];
          u.det[k] = n == 7 ? kDetune7[k] : n == 1 ? 0.0 : 0.11 * (2.0 * k / (n - 1) - 1);
          u.panL[k] = u.centre[k] ? 0.7071 : k < n / 2 ? (k % 2 == 0 ? 1.0 : 0.25) : ((n - 1 - k) % 2 == 0 ? 0.25 : 1.0);
        }
      }
      return t;
    }();
    return table[std::clamp(n, 1, kOsc)];
  }
  // An envelope's stages. Plateau is its Hold (the time it stays at the top); Sustain is where a held key waits.
  enum Stage : uint8_t { Off, Attack, Plateau, Decay, Sustain, Release };
  // released: ENV 2 and 3 let go during their attack finish it first (as version 1's filter envelope did), then release.
  struct Env { Stage stage = Off; double v = 0; double left = 0; bool released = false; };
  // An LFO that starts on each note (Note, Once): each voice its own.
  struct VoiceLfo { double phase = 0, last = 0, held = 0, smooth = 0; uint32_t cycle = 0; bool done = false; };
  // A stereo filter: the voice's own state-variable filters (the first five types) and the newer types (5 and up:
  // ladder, comb, formant...), left and right.
  struct Filt {
    Svf l1, l2, r1, r2;
    dsp::VoiceFilter more[2];
    void reset() { l1 = l2 = r1 = r2 = Svf{}; more[0].reset(); more[1].reset(); }
    // The first five types are the state-variable filters; Drive and Shape belong to the others (a first-five type
    // with Drive runs through the VoiceFilter too, so it is clipped inside, oversampled).
    void set(int mode, double fc, double res, double sr, double drive, double shape) {
      l1.set(fc, 0, sr); r1.set(fc, 0, sr); l2.set(fc, res, sr); r2.set(fc, res, sr);
      if (mode > dsp::VoiceFilter::Notch || drive > 0) { more[0].set(mode, fc, res, sr, drive, shape); more[1].set(mode, fc, res, sr, drive, shape); }
    }
    void run(int mode, bool drive, double sl, double sr, double& l, double& r) {
      switch (drive ? -1 : mode) {
        case 1: l = l2.lp(sl); r = r2.lp(sr); break;                  // low-pass 12 dB
        case 2: l = l2.bp(sl); r = r2.bp(sr); break;                  // band-pass
        case 3: l = l2.hp(l1.hp(sl)); r = r2.hp(r1.hp(sr)); break;    // high-pass 24 dB
        case 4: l = sl - l2.bp(sl); r = sr - r2.bp(sr); break;        // notch
        case 0: l = l2.lp(l1.lp(sl)); r = r2.lp(r1.lp(sr)); break;    // low-pass 24 dB
        default: l = more[0].run(sl); r = more[1].run(sr); break;     // ladder, comb, formant...
      }
    }
  };
  struct Voice {
    Env env[kEnvs];  // env[0] is the volume: the voice sounds while it is not Off
    int pitch = -1; double velocity = 0, freq = 0, target = 0;
    double phase[kOsc] = {}, phase2[kOsc] = {}, lastB[kOsc] = {}, stackPhase = 0, fade = 1, fadeStep = 0, fadeIn = 1; uint64_t born = 0;
    double modPhase = 0, noiseLp = 0, pmPrev = 0; Noise noise;
    Filt f1, f2;  // Filter 1 and Filter 2
    Grain grains[kGrains]; double grainClock = 1, scan = 0; Noise grainRng;
    VoiceLfo lfo[kLfos]; uint32_t seed = 0;
    Stage stage() const { return env[0].stage; }
    bool down() const { const Stage s = env[0].stage; return s != Off && s != Release; }
  };
  std::shared_ptr<const AudioData> source_;
  struct Queued { int offset; bool on; int pitch; float velocity; double age = 0; int offAt = -1; };
  Voice voices_[kVoices + kGhosts];  // [0, kVoices) play notes, the rest only fade cut notes out
  Queued queue_[kQueue];
  Queued pendingOff_[kQueue];
  int queued_ = 0;
  int held_[kHeld] = {}; int heldCount_ = 0;  // Lead: the keys down, the last one sounding
  uint64_t clock_ = 0;
  int coefCounter_ = 0;
  double lastFreq_ = 0;  // Lead: the last pitch played (Glide always slides from it)
  SongClock song_;  // the song LFOs follow the song: every pass of a loop moves the same way
  // An LFO in time with the song (Song mode). at: the rate it was placed at (beats, or -Hz when free): a rate change
  // while playing places it again (see setPosition).
  struct SongLfo { SongPhase phase; double last = 0, held = 0, smooth = 0, at = 0; uint32_t cycle = 0; };
  SongLfo lfo_[kLfos];
  double lfoNow_[kLfos] = {};  // this sample's value of each song LFO
  // Crush: fewer bits and a lower rate (sample and hold) on the synth's output: the aliasing of a laser stab.
  double crushHeld_[2] = {0, 0};
  unsigned crushAt_ = 0;
  // The modulation (from the track's "synth", set on the control thread before the instrument goes live).
  wmod::RtLayer mod_;
  int modIndex_[kCount];             // a parameter's place among the moved ones, or -1
  int modSlot_[wmod::kLinks] = {};   // and back: the parameter of each moved place
  int moved_ = 0;
  // Section lights (WaveStack): a source switched off, or one that no longer reaches the filter.
  bool muteSrc_[wmod::SrcCount] = {}, muteToTone_[wmod::SrcCount] = {};
  // The per-voice, per-sample result of the links.
  struct Mod { double pitch = 0, volume = 1, pan = 0, detune = 0, tone = 0, tone2 = 0; double off[wmod::kLinks] = {}; };
public:
  explicit Wave(double sr) : Instrument(sr) { std::fill(modIndex_, modIndex_ + kCount, -1); }
  const char* name() const override { return "wave"; }
  bool stereo() const override { return true; }
  // The layer's links, with the parameter names already turned into slots (control thread).
  void setLayer(const wmod::Layer& layer) {
    mod_ = wmod::RtLayer{};
    std::fill(modIndex_, modIndex_ + kCount, -1);
    moved_ = 0;
    for (int k = 0; k < kLfos; ++k) mod_.lfo[k] = layer.lfo[k];
    const auto& specs = specList();
    for (const auto& l : layer.links) {
      if (!l.on || mod_.links >= wmod::kLinks) continue;
      wmod::RtLink r;
      r.source = l.source; r.aux = l.aux; r.auxDepth = l.auxDepth; r.amount = l.amount; r.unipolar = l.unipolar;
      if (const int v = wmod::virtualFromName(l.target)) r.dst = v;
      else if (l.target == "tone") { r.dst = tone; r.octaves = true; }
      else if (l.target == "tone2") { r.dst = tone2; r.octaves = true; }
      else {
        int slot = -1;
        for (size_t i = 0; i < specs.size(); ++i) if (l.target == specs[i].name) slot = (int)i;
        if (slot < 0) continue;  // parsePatch has checked the names: never happens
        r.dst = slot;
        if (modIndex_[slot] < 0) { modIndex_[slot] = moved_; modSlot_[moved_++] = slot; }
      }
      mod_.usesSource[l.source] = true;
      if (l.aux != wmod::SrcNone) mod_.usesSource[l.aux] = true;
      mod_.link[mod_.links++] = r;
    }
  }
  void mute(const bool* sources, const bool* toTone) {
    std::copy(sources, sources + wmod::SrcCount, muteSrc_);
    std::copy(toTone, toTone + wmod::SrcCount, muteToTone_);
  }
  // The layer's parameters, in order (the registry lists them under the track's names).
  static const std::vector<ParamSpec>& specList() {
    static const std::vector<ParamSpec> s = {
      {"wave", 3, 0, 4, ""},              {"uniDetune", 0.3, 0, 1, ""},
      {"subOct", -1, -3, 1, "oct"},       {"octave", 0, -2, 2, "oct"},
      {"tone", 1800, 40, 12000, "Hz"},    {"bite", 0.3, 0, 0.95, ""},
      {"glide", 0, 0, 1000, "ms"},        {"mode", 0, 0, 1, ""},
      {"level", 0.7, 0, 1.5, ""},
      {"noise", 0, 0, 1, ""},             {"noiseColor", 0.6, 0, 1, ""},
      {"fm", 0, 0, 10, ""},               {"fmRatio", 2, 0.25, 16, ""},
      {"tune", 0, -100, 100, "ct"},  // fine tune in cents
      // Grain (0 = off: the wave alone)
      {"grain", 0, 0, 1, ""},             {"grainSize", 80, 5, 1000, "ms"},
      {"grainDensity", 20, 1, 200, "/s"}, {"grainPos", 0, 0, 1, ""},
      {"grainScan", 0.2, 0, 2, ""},       {"grainScatter", 0.3, 0, 1, ""},
      {"osc2", 0, 0, 1, ""},              {"osc2Wave", 3, 0, 4, ""},
      {"osc2Semi", 0, -24, 24, "st"},     {"osc2Fine", 7, -100, 100, "ct"},
      {"filterMode", 0, 0, 19, ""},       {"drive", 0, 0, 1, ""},
      // Wavetables and a third oscillator: table 0 = the classic shapes.
      {"osc1Table", 0, 0, 8, ""},         {"osc1Pos", 0, 0, 1, ""},
      {"osc2Table", 0, 0, 8, ""},         {"osc2Pos", 0, 0, 1, ""},
      {"osc1Semi", 0, -24, 24, "st"},  // Osc 1's interval (Osc 2 has its own)
      {"crush", 0, 0, 1, ""},  // fewer bits and a lower rate: aliasing (0 = off)
      // Drive pushes the sound into the filter (0 = clean); Shape moves Morph from low to band to high, spreads the
      // Phaser's stages and sets how far apart Dual Notch / Dual Band's two filters are.
      {"filterDrive", 0, 0, 1, ""},       {"filterShape", 0.5, 0, 1, ""},
      // ENV 1 (the volume), ENV 2, ENV 3. Decay: to -40 dB of the way to the sustain; Release: to -60 dB. The
      // defaults are version 1's: Swell 5, Fade 300; ENV 2 = Bloom 400 ms; ENV 3 = Zap time 80 ms.
      {"env1Attack", 5, 0, 8000, "ms"},   {"env1Hold", 0, 0, 8000, "ms"},
      {"env1Decay", 1000, 1, 20000, "ms"}, {"env1Sustain", 1, 0, 1, ""},
      {"env1Release", 300, 1, 30000, "ms"},
      {"env2Attack", 0, 0, 8000, "ms"},   {"env2Hold", 0, 0, 8000, "ms"},
      {"env2Decay", 1840, 1, 20000, "ms"}, {"env2Sustain", 0, 0, 1, ""},
      {"env2Release", 2763.102111592855, 1, 30000, "ms"},
      {"env3Attack", 0, 0, 8000, "ms"},   {"env3Hold", 0, 0, 8000, "ms"},
      {"env3Decay", 368, 1, 20000, "ms"}, {"env3Sustain", 0, 0, 1, ""},
      {"env3Release", 552.620422318571, 1, 30000, "ms"},
      // LFO 1..4: Rate in beats per cycle (in time) or Hz (free, the LFO's "sync" off); Phase where it starts; Smooth
      // the glide over its jumps. In time with the song a rate is locked so 4 bars hold whole cycles.
      {"lfo1Rate", 1, 0.03125, 64, "beats"}, {"lfo1Hz", 1, 0.005, 40, "Hz"},
      {"lfo1Phase", 0, 0, 1, ""},         {"lfo1Smooth", 1.5, 0, 200, "ms"},
      {"lfo2Rate", 4, 0.03125, 64, "beats"}, {"lfo2Hz", 1, 0.005, 40, "Hz"},
      {"lfo2Phase", 0, 0, 1, ""},         {"lfo2Smooth", 1.5, 0, 200, "ms"},
      {"lfo3Rate", 16, 0.03125, 64, "beats"}, {"lfo3Hz", 0.25, 0.005, 40, "Hz"},
      {"lfo3Phase", 0, 0, 1, ""},         {"lfo3Smooth", 0, 0, 200, "ms"},
      {"lfo4Rate", 2, 0.03125, 64, "beats"}, {"lfo4Hz", 1, 0.005, 40, "Hz"},
      {"lfo4Phase", 0, 0, 1, ""},         {"lfo4Smooth", 1.5, 0, 200, "ms"},
      {"macro1", 0, 0, 1, ""},            {"macro2", 0, 0, 1, ""},
      {"macro3", 0, 0, 1, ""},            {"macro4", 0, 0, 1, ""},
      // Unison (copies of every oscillator: how many, how far apart, how loud the side copies, how wide), and the
      // Sub: one plain wave an octave or more under (shape 0 = Osc 1's own wave, then sine, triangle, saw, square, pulse).
      {"unison", 7, 1, 16, ""},           {"uniBlend", 0.3, 0, 1, ""},
      {"uniWidth", 1, 0, 1, ""},          {"subLevel", 0, 0, 1, ""},
      {"subShape", 0, 0, 5, ""},
      // Warp per oscillator: off, sync, bend+, bend-, PWM, asym, mirror, flip, quantize, FM, AM, RM (the last three
      // from the other oscillator), and how much.
      {"osc1Warp", 0, 0, 11, ""},         {"osc1WarpAmt", 0, 0, 1, ""},
      {"osc2Warp", 0, 0, 11, ""},         {"osc2WarpAmt", 0, 0, 1, ""},
      // Playing: pitch bend (-1..1, x Bend range semitones), the mod wheel (a source of links), how many notes Pad plays,
      // Legato (Lead: a note played over another does not start its envelopes again) and Glide always (Lead: slides
      // into every note, not only the ones that touch).
      {"bend", 0, -1, 1, ""},             {"bendRange", 2, 0, 48, "st"},
      {"modWheel", 0, 0, 1, ""},          {"polyVoices", 8, 1, 8, ""},
      {"legato", 0, 0, 1, ""},            {"glideAlways", 0, 0, 1, ""},
      // Filter 2 (the same types as Filter 1), Serial (Filter 1 goes into Filter 2), and where each source goes:
      // 0 = Filter 1, 1 = Filter 2, 2 = no filter. Everything into Filter 1 = one filter, as before.
      {"filter2Mode", 0, 0, 19, ""},      {"tone2", 3000, 40, 12000, "Hz"},
      {"bite2", 0.3, 0, 0.95, ""},        {"filter2Drive", 0, 0, 1, ""},
      {"filter2Shape", 0.5, 0, 1, ""},    {"filterSerial", 0, 0, 1, ""},
      {"osc1Route", 0, 0, 2, ""},         {"osc2Route", 0, 0, 2, ""},
      {"subRoute", 0, 0, 2, ""},
      {"noiseRoute", 0, 0, 2, ""},
      // The design's knobs (2026-11-09): Osc 1's own level, the Sub's place left/right, how much of each filter is
      // heard (the rest dry), Drive's colour (0 = even, more = uneven: richer, warmer) and how much is driven, how far
      // each LFO moves what it is linked to, the layer's place left/right and the grains' own pitch. At their defaults
      // the sound is as before, to the bit.
      {"osc1Level", 1, 0, 1, ""},         {"subPan", 0, -1, 1, ""},
      {"filterMix", 1, 0, 1, ""},         {"filter2Mix", 1, 0, 1, ""},
      {"driveColour", 0, 0, 1, ""},       {"driveMix", 1, 0, 1, ""},
      {"lfo1Depth", 1, 0, 1, ""},        {"lfo2Depth", 1, 0, 1, ""},
      {"lfo3Depth", 1, 0, 1, ""},        {"lfo4Depth", 1, 0, 1, ""},
      {"outPan", 0, -1, 1, ""},           {"grainPitch", 0, -24, 24, "st"}};
    return s;
  }
  void queueNote(int offset, bool on, int pitch, double velocity) override {
    if (queued_ >= kQueue) {
      if (on) { ++droppedNotes_; return; }
      // A note-off is never lost (a stuck note never stops): the queued press of its pitch carries it (it lets go at
      // the note-off's time), or, with none queued, the note lets go now.
      for (int q = queued_ - 1; q >= 0; --q)
        if (queue_[q].on && queue_[q].pitch == pitch) {
          if (queue_[q].offAt < 0) { queue_[q].offAt = std::max(offset, queue_[q].offset); return; }
          break;
        }
      release(pitch);
      return;
    }
    queue_[queued_++] = Queued{offset, on, pitch, (float)velocity};
  }
  void chaseNote(int pitch, double velocity, double age) override {
    if (queued_ >= kQueue) { ++droppedNotes_; return; }
    queue_[queued_++] = Queued{0, true, pitch, (float)velocity, std::max(0.0, age)};
  }
  void noteOn(int pitch, double velocity) override { queueNote(0, true, pitch, velocity); }
  void noteOff(int pitch) override { queueNote(0, false, pitch, 0); }
  void allNotesOff() override {
    queued_ = 0; heldCount_ = 0;
    for (auto& v : voices_) if (v.stage() != Off) letGo(v);
  }
  // Every voice ends at once (a W-Core layer switched on again: what it was playing when it went off is over).
  void silence() {
    queued_ = 0; heldCount_ = 0;
    for (auto& v : voices_) v = Voice{};
  }
  void setSource(std::shared_ptr<const AudioData> source) override { source_ = std::move(source); }
  void adopt(const Instrument& previous) override {
    const auto* p = dynamic_cast<const Wave*>(&previous);
    if (!p) return;
    for (int i = 0; i < kVoices + kGhosts; ++i) voices_[i] = p->voices_[i];
    for (int i = 0; i < kHeld; ++i) held_[i] = p->held_[i];
    heldCount_ = p->heldCount_; clock_ = p->clock_; lastFreq_ = p->lastFreq_;
    for (int k = 0; k < kLfos; ++k) lfo_[k] = p->lfo_[k];
    song_ = p->song_; coefCounter_ = p->coefCounter_;
    crushHeld_[0] = p->crushHeld_[0]; crushHeld_[1] = p->crushHeld_[1]; crushAt_ = p->crushAt_;  // audit 2026-11-07 #8
  }
  void setPosition(double beat, double bpm) override {
    song_.tempo(bpm, sampleRate_);
    // Placed by the song on a jump, and also when a rate changes while playing: the phase only ran on at the old rate,
    // so A -> B -> A ended somewhere else than never touching it (audit 2026-11-07: 0.23 of difference 7 s later).
    // A rate change slides there (SongPhase, ~30 ms), so it never clicks.
    const bool jumped = song_.at(beat);
    if (beat < 0) return;
    if (jumped) coefCounter_ = 0;
    for (int k = 0; k < kLfos; ++k) {
      if (mod_.lfo[k].mode != wmod::LfoMode::Song) continue;
      SongLfo& o = lfo_[k];
      double cyc, at;
      if (mod_.lfo[k].sync) { at = lfoBeats(k); cyc = beat / at; }
      else { const double hz = lfoHz(k); at = -hz; cyc = std::max(0.0, beat) * 60.0 / song_.bpm * song_.lock(hz); }
      if (!jumped && at == o.at) continue;
      o.phase.place(cyc - std::floor(cyc), jumped ? song_.smooth : true);
      o.cycle = (uint32_t)(int64_t)std::floor(cyc);
      o.held = stepValue(k, o.cycle);
      o.last = cyc - std::floor(cyc);
      o.at = at;
    }
  }
  void render(float* out, int frames) override {  // mono path (never used by the graph for a stereo instrument)
    float r[512];
    for (int done = 0; done < frames;) {
      const int n = std::min(512, frames - done);
      std::fill(r, r + n, 0.0f);
      float l[512] = {};
      renderStereo(l, r, n);
      for (int i = 0; i < n; ++i) out[done + i] += 0.5f * (l[i] + r[i]);
      done += n;
    }
  }
  void renderStereo(float* left, float* right, int frames) override {
    int cursor = 0, pending = 0;  // pending: note-offs carried by their press (a full queue), in time order
    auto letGoUntil = [&](int until) {
      while (pending > 0 && pendingOff_[0].offset <= until) {
        const int at = std::clamp(pendingOff_[0].offset, 0, frames);
        if (at > cursor) { renderVoices(left, right, cursor, at); cursor = at; }
        release(pendingOff_[0].pitch);
        std::copy(pendingOff_ + 1, pendingOff_ + pending, pendingOff_);
        --pending;
      }
    };
    for (int q = 0; q < queued_; ++q) {
      const int at = std::clamp(queue_[q].offset, 0, frames);
      letGoUntil(at);
      if (at > cursor) { renderVoices(left, right, cursor, at); cursor = at; }
      if (queue_[q].on) press(queue_[q].pitch, queue_[q].velocity, queue_[q].age);
      else release(queue_[q].pitch);
      if (queue_[q].offAt >= 0) {
        int k = pending++;
        for (; k > 0 && pendingOff_[k - 1].offset > queue_[q].offAt; --k) pendingOff_[k] = pendingOff_[k - 1];
        pendingOff_[k] = Queued{queue_[q].offAt, false, queue_[q].pitch, 0};
      }
    }
    queued_ = 0;
    letGoUntil(frames);
    if (cursor < frames) renderVoices(left, right, cursor, frames);
  }
private:
  // ---- envelopes: one set of coefficients per block, the state per voice
  struct EnvK { double att = 1, dec = 0, sus = 0, rel = 0; double hold = 0; bool instant = false; };
  EnvK envK(int e) const {
    const double sr = sampleRate_;
    const int s = envSlot(e);
    EnvK k;
    const double a = clampd(params[s], 0, 8000);
    k.instant = a <= 0;
    k.att = 1 / std::max(1.0, a * 0.001 * sr);
    k.hold = std::round(clampd(params[s + 1], 0, 8000) * 0.001 * sr);
    k.dec = msToCoef(clampd(params[s + 2], 1, 20000) / 4.6, sr);  // ~ -40 dB of the way in Decay ms
    k.sus = clampd(params[s + 3], 0, 1);
    k.rel = std::exp(std::log(1e-3) / std::max(1.0, clampd(params[s + 4], 1, 30000) * 0.001 * sr));  // -60 dB in Release ms
    return k;
  }
  // One sample of envelope e. ENV 1 ends (Off) once it is silent, and settles on its sustain; the others glide on.
  static void envStep(Env& n, const EnvK& k, bool volume) {
    switch (n.stage) {
      case Attack:
        n.v += k.att;
        if (n.v >= 1) {
          n.v = 1; n.left = k.hold;
          n.stage = n.released ? Release : k.hold > 0 ? Plateau : (k.sus < 1 ? Decay : Sustain);
        }
        break;
      case Plateau:
        if (--n.left <= 0) n.stage = k.sus < 1 ? Decay : Sustain;
        break;
      case Decay:
        n.v = k.sus + (n.v - k.sus) * k.dec;
        if (volume && n.v - k.sus < 1e-4) { n.v = k.sus; n.stage = Sustain; }
        break;
      case Sustain:
        if (!volume) n.v = k.sus + (n.v - k.sus) * k.dec;  // a sustain turned while held: there, at the decay's speed
        else if (k.sus <= 0) { n.v = 0; n.stage = Off; }
        break;
      case Release:
        n.v *= k.rel;
        if (volume && n.v < 1e-4) { n.v = 0; n.stage = Off; }
        break;
      case Off: break;
    }
  }
  // Where an envelope is `age` samples after its note began (a note caught mid-way).
  static void envChase(Env& n, const EnvK& k, double age, double sr, double attackMs, bool volume) {
    const double attLen = k.instant ? 0.0 : std::max(1.0, attackMs * 0.001 * sr);
    if (age < attLen) { n.stage = Attack; n.v = age / attLen; return; }
    double t = age - attLen;
    if (t < k.hold) { n.stage = Plateau; n.v = 1; n.left = k.hold - t; return; }
    t -= k.hold;
    n.v = k.sus + (1 - k.sus) * std::pow(k.dec, t);
    n.stage = volume && n.v - k.sus < 1e-4 ? Sustain : Decay;
  }
  void envStart(Env& n, const EnvK& k) {
    n.released = false;
    if (k.instant) { n.v = 1; n.stage = k.hold > 0 ? Plateau : (k.sus < 1 ? Decay : Sustain); n.left = k.hold; }
    else { n.v = 0; n.stage = Attack; }
  }

  // ---- LFOs
  // A rate in beats, in time with the song: 4 bars hold a whole number of cycles (1/32 note .. 16 bars).
  double lfoBeats(int k) const {
    const double b = clampd(params[lfoSlot(k)], 0.03125, 64);
    return b <= SongClock::kBeats ? SongClock::kBeats / std::max(1.0, std::round(SongClock::kBeats / b))
                                  : SongClock::kBeats * std::round(b / SongClock::kBeats);
  }
  double lfoHz(int k) const { return clampd(params[lfoSlot(k) + 1], 0.005, 40); }
  double lfoSlew(int k) const {
    const double ms = clampd(params[lfoSlot(k) + 3], 0, 200);
    return ms > 0 ? 1 - std::exp(-1.0 / (ms * 0.001 * sampleRate_)) : 1.0;
  }
  // A random step for cycle n of LFO k: the same for the same cycle of the song every time.
  static double stepValue(int k, uint32_t n) {
    static constexpr uint32_t kSeed[4] = {0xC2B2AE35u, 0x85EBCA6Bu, 0x27D4EB2Fu, 0x165667B1u};
    uint32_t h = n * 2654435761u ^ kSeed[k & 3];
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    return (double)h / 2147483648.0 - 1.0;
  }
  double lfoValue(int k, double p, double held) const {
    double q = p + clampd(params[lfoSlot(k) + 2], 0, 1);
    q -= std::floor(q);
    return wmod::lfoShape(mod_.lfo[k], q, held);
  }
  // Song LFO k's value now (-1..1), then one step on.
  double songLfoNext(int k, double step, double slide, double slew) {
    SongLfo& o = lfo_[k];
    const double p = o.phase.next(step, slide);
    if (p < o.last - 0.5) { ++o.cycle; o.held = stepValue(k, o.cycle); }  // a new cycle
    o.last = p;
    // The jumps (square, saw, random steps) glide over Smooth ms (1.5 by default), so a gate on the volume never clicks
    // (the sweep measured +10 dB clicks on Gated Choir). Smooth shapes pass almost unchanged.
    o.smooth += (lfoValue(k, p, o.held) - o.smooth) * slew;
    return o.smooth;
  }
  double voiceLfoStep(int k) const {  // a note LFO's step per sample
    return mod_.lfo[k].sync ? song_.perSample / clampd(params[lfoSlot(k)], 0.03125, 64) : lfoHz(k) / sampleRate_;
  }
  double voiceLfoNext(Voice& v, int k, double step, double slew) {
    VoiceLfo& o = v.lfo[k];
    const bool once = mod_.lfo[k].mode == wmod::LfoMode::Once;
    const double p = o.done ? 1.0 - 1e-9 : o.phase;
    if (!o.done) {
      o.phase += step;
      if (o.phase >= 1) {
        if (once) { o.done = true; }
        else { o.phase -= std::floor(o.phase); ++o.cycle; o.held = stepValue(k, o.cycle ^ v.seed); }
      }
    }
    o.smooth += (lfoValue(k, p, o.held) - o.smooth) * slew;
    return o.smooth;
  }
  void voiceLfoStart(Voice& v, double age) {
    for (int k = 0; k < kLfos; ++k) {
      if (mod_.lfo[k].mode == wmod::LfoMode::Song || !mod_.usesSource[wmod::SrcLfo1 + k]) continue;
      VoiceLfo& o = v.lfo[k];
      o = VoiceLfo{};
      const double run = age * voiceLfoStep(k);
      if (mod_.lfo[k].mode == wmod::LfoMode::Once && run >= 1) { o.done = true; o.phase = 1; }
      else { o.cycle = (uint32_t)std::floor(run); o.phase = run - std::floor(run); }
      o.held = stepValue(k, o.cycle ^ v.seed);
      o.smooth = lfoValue(k, o.done ? 1.0 - 1e-9 : o.phase, o.held);
    }
  }

  bool lead() const { return params[mode] < 0.5; }
  double hz(int pitch) const { return midiToHz(pitch + 12 * std::round(clampd(params[octave], -2, 2))); }
  void start(Voice& v, int pitch, double velocity, double age) {
    if (v.stage() != Off && v.env[0].v > 1e-3) {
      // Cutting a sounding note: it goes on in another slot as a ghost that fades out over 3 ms, and the new note fades
      // in over 3 ms. (Before 2026-11-07 the old note's level dropped to 0 in one sample: the sweep measured a click
      // 22-26 dB above the sound on Rusted Hoover and Static Riser.) The ghost takes a free ghost slot, else the
      // ghost nearest its end (never a voice that plays a note).
      Voice* ghost = nullptr;
      for (int g = kVoices; g < kVoices + kGhosts && !ghost; ++g) if (voices_[g].stage() == Off) ghost = &voices_[g];
      if (!ghost)
        for (int g = kVoices; g < kVoices + kGhosts; ++g)
          if (!ghost || voices_[g].env[0].v * voices_[g].fade < ghost->env[0].v * ghost->fade) ghost = &voices_[g];
      if (ghost) {
        *ghost = v;
        ghost->pitch = -1;  // no key releases or retakes it
        ghost->fadeStep = fadeInStep(sampleRate_) * std::max(1e-3, ghost->fade);
      }
      v.fadeIn = 0;
    }
    if (v.stage() == Off) { v.f1.reset(); v.f2.reset(); }
    // The centre wave starts at the same place on every note (the same attack, a tight lead); the detuned copies start
    // somewhere else on each note, as free-running oscillators do (Serum's random phase): with the same start every
    // time, every note of a riff was the same sample played again, the machine-gun sound of an old console. The place
    // comes from the note's place in its 4 bars and its key, so a render is the same every time (export = real time) and
    // every 4 bars repeat (Diogo 2026-11-06).
    uint32_t seed = (uint32_t)(int64_t)std::llround(std::fmod(std::max(0.0, song_.beat), SongClock::kBeats) * 960.0) * 2654435761u ^ (uint32_t)(pitch * 40503u + 0x9E3779B9u);
    v.seed = seed;
    auto rnd = [&seed]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return (double)(seed & 0xFFFFFF) / 16777216.0; };
    for (int k = 0; k < kOsc; ++k) {  // k: the phase slot (3 = the centre copy)
      const double j1 = k == 3 ? 0 : rnd(), j2 = k == 3 ? 0 : rnd(), j3 = k == 3 ? 0 : rnd();
      v.phase[k] = std::fmod(kStart[k] + j1, 1.0);
      v.phase2[k] = std::fmod(kStart[k < 7 ? (k + 3) % 7 : (k + 3) % kOsc] + j2, 1.0);
      (void)j3;  // drawn as before, so every copy starts where it did
      v.lastB[k] = 0;
    }
    v.stackPhase = 0; v.fade = 1; v.fadeStep = 0;
    v.pitch = pitch; v.velocity = velocity; v.freq = v.target = hz(pitch);
    for (int e = 0; e < kEnvs; ++e) envStart(v.env[e], envK(e));
    v.env[0].v = 0; v.env[0].stage = Attack;  // the volume always rises from silence (Attack 0: in one sample)
    v.modPhase = 0; v.pmPrev = 0; v.born = clock_++;
    for (auto& g : v.grains) g.on = false;
    v.grainClock = 1; v.scan = 0; v.grainRng.state = 0x9E3779B9u ^ (uint32_t)(pitch * 2654435761u);  // the same every time
    coefCounter_ = 0;
    if (age > 0) {  // caught mid-way: where the envelopes would be now
      for (int e = 0; e < kEnvs; ++e) {
        EnvK k = envK(e);
        if (e == 0) k.instant = false;
        envChase(v.env[e], k, age, sampleRate_, clampd(params[envSlot(e)], 0, 8000), e == 0);
      }
      v.fadeIn = 0;
    }
    voiceLfoStart(v, age);
  }
  void press(int pitch, double velocity, double age) {
    // The key goes on top of the held list, in both modes: Pad -> Lead finds the keys really down (A -> B -> A = A).
    {
      int w = 0;
      for (int i = 0; i < heldCount_; ++i) if (held_[i] != pitch) held_[w++] = held_[i];
      heldCount_ = w;
      if (heldCount_ < kHeld) held_[heldCount_++] = pitch;
    }
    if (lead()) {
      Voice& v = voices_[0];
      const bool glides = params[glide] > 0.5;
      // Over a held note: Glide or Legato carry on the same envelopes (a slide, or a jump with Legato alone).
      if (v.down() && (glides || params[legato] >= 0.5) && age <= 0) {
        v.target = hz(pitch); v.pitch = pitch; v.velocity = velocity;
        if (!glides) v.freq = v.target;
        lastFreq_ = v.target;
        return;
      }
      const double from = v.stage() != Off ? v.freq : lastFreq_;
      start(v, pitch, velocity, age);
      if (glides && params[glideAlways] >= 0.5 && from > 0 && age <= 0) v.freq = from;  // slides into every note
      lastFreq_ = v.target;
      return;
    }
    // Pad: a free voice among the first Voices, else the oldest one fades out over 3 ms and the note starts on another
    const int count = (int)std::lround(clampd(params[polyVoices], 1, kVoices));
    Voice* free = nullptr; Voice* oldest = &voices_[0];
    for (int i = 0; i < count; ++i) {
      Voice& v = voices_[i];
      if (v.stage() == Off && !free) free = &v;
      if (v.born < oldest->born) oldest = &v;
    }
    if (!free) {
      for (int i = 0; i < count; ++i) {
        Voice& v = voices_[i];
        if (v.stage() == Release && (!free || v.env[0].v < free->env[0].v)) free = &v;
      }
      if (!free) free = oldest;
    }
    start(*free, pitch, velocity, age);
  }
  static void letGo(Voice& v) {
    for (int e = 0; e < kEnvs; ++e) {
      Env& n = v.env[e];
      if (e > 0 && n.stage == Attack) n.released = true;
      else if (n.stage != Off) n.stage = Release;
    }
  }
  void release(int pitch) {
    int w = 0;
    for (int i = 0; i < heldCount_; ++i) if (held_[i] != pitch) held_[w++] = held_[i];
    const bool wasTop = heldCount_ > 0 && held_[heldCount_ - 1] == pitch;
    heldCount_ = w;
    if (lead()) {
      // A Pad note still down when the mode went back to Lead: it ends with its key, as it would have.
      for (int i = 1; i < kVoices; ++i) if (voices_[i].pitch == pitch && voices_[i].down()) letGo(voices_[i]);
      Voice& v = voices_[0];
      if (v.stage() == Off || v.pitch != pitch) { if (!wasTop) return; }
      if (heldCount_ > 0 && v.down()) {  // back to the key still down
        v.pitch = held_[heldCount_ - 1]; v.target = hz(v.pitch);
        if (params[glide] <= 0.5) v.freq = v.target;
        return;
      }
      if (v.stage() != Off) letGo(v);
      return;
    }
    for (auto& v : voices_) if (v.pitch == pitch && v.down()) letGo(v);
  }
  // Szabo's detune curve (the knob in, the detune amount out) and mix curves for the side and centre oscillators.
  static double detuneCurve(double x) {
    return ((((((((((10028.7312891634 * x - 50818.8652045924) * x + 111363.4808729368) * x - 138150.6761080548) * x +
                106649.6679158292) * x - 53046.9642751875) * x + 17019.9518580080) * x - 3425.0836591318) * x +
             404.2703938388) * x - 24.1878824391) * x + 0.6717417634) * x + 0.0030115596;
  }
  // A source's value for voice v now (the song LFOs' values are this sample's, in lfoNow_).
  double source(const Voice& v, const double* voiceLfo, Src s) const {
    switch (s) {
      case wmod::SrcLfo1: case wmod::SrcLfo2: case wmod::SrcLfo3: case wmod::SrcLfo4: {
        const int k = s - wmod::SrcLfo1;
        return mod_.lfo[k].mode == wmod::LfoMode::Song ? lfoNow_[k] : voiceLfo[k];
      }
      case wmod::SrcEnv1: case wmod::SrcEnv2: case wmod::SrcEnv3: return v.env[s - wmod::SrcEnv1].v;
      case wmod::SrcVelocity: return v.velocity;
      case wmod::SrcNote: return (v.pitch - 60) / 60.0;
      case wmod::SrcMacro1: case wmod::SrcMacro2: case wmod::SrcMacro3: case wmod::SrcMacro4:
        return clampd(params[macro1 + (s - wmod::SrcMacro1)], 0, 1);
      case wmod::SrcModWheel: return clampd(params[modWheel], 0, 1);
      default: return 0;
    }
  }
  void evalLinks(const Voice& v, const double* voiceLfo, Mod& m) const {
    // Two passes: first the links that move an LFO's Depth (as they are), then the others, each LFO's links scaled by
    // its Depth for this voice now (0 = the LFO moves nothing, as if unlinked).
    auto isDepth = [](int dst) { return dst >= lfo1Depth && dst <= lfo4Depth; };
    double depth[kLfos];
    for (int pass = 0; pass < 2; ++pass) {
      if (pass == 1)
        for (int k = 0; k < kLfos; ++k) {
          const int i = modIndex_[lfo1Depth + k];
          depth[k] = clampd(params[lfo1Depth + k] + (i < 0 ? 0.0 : m.off[i]), 0, 1);
        }
      for (int i = 0; i < mod_.links; ++i) {
        const wmod::RtLink& l = mod_.link[i];
        if (isDepth(l.dst) != (pass == 0)) continue;
        if (muteSrc_[l.source] || (l.aux != wmod::SrcNone && muteSrc_[l.aux])) continue;
        if (l.dst == tone && muteToTone_[l.source]) continue;
        double x = source(v, voiceLfo, l.source);
        if (l.unipolar && wmod::bipolar(l.source)) x = 0.5 + 0.5 * x;
        if (l.aux != wmod::SrcNone) x *= 1 - l.auxDepth + l.auxDepth * source(v, voiceLfo, l.aux);
        double amount = l.amount;
        if (pass == 1 && l.source >= wmod::SrcLfo1 && l.source <= wmod::SrcLfo4) {
          const double k = depth[l.source - wmod::SrcLfo1];
          if (k != 1) amount *= k;
        }
        switch (l.dst) {
          case wmod::DstPitch: m.pitch += amount * x; break;
          // Amount 1 = the source's lowest point is silence, its highest the voice's level (a tremolo, the velocity);
          // a negative amount the other way round.
          case wmod::DstVolume: m.volume *= std::max(0.0, amount >= 0 ? 1 - amount * (1 - x) : 1 + amount * x); break;
          case wmod::DstPan: m.pan += amount * x; break;
          case wmod::DstDetune: m.detune += amount * x; break;
          default:
            if (l.octaves) (l.dst == tone ? m.tone : m.tone2) += amount * x;
            else m.off[modIndex_[l.dst]] += amount * x;
        }
      }
    }
  }
  void renderVoices(float* left, float* right, int from, int to) {
    bool any = false;
    for (const auto& v : voices_) if (v.stage() != Off) { any = true; break; }
    const double sr = sampleRate_;
    const double slide = SongPhase::slideCoef(sr);
    song_.run(to - from);
    // The song LFOs: their steps per sample (beats per sample / beats per cycle, or the locked Hz) and smoothing.
    double lfoStep[kLfos], lfoSlewK[kLfos];
    bool songLfo[kLfos], voiceLfo[kLfos], useLfo[kLfos];
    for (int k = 0; k < kLfos; ++k) {
      songLfo[k] = mod_.lfo[k].mode == wmod::LfoMode::Song;
      useLfo[k] = mod_.usesSource[wmod::SrcLfo1 + k] && !muteSrc_[wmod::SrcLfo1 + k];
      voiceLfo[k] = !songLfo[k] && useLfo[k];
      lfoStep[k] = songLfo[k] ? (mod_.lfo[k].sync ? song_.perSample / lfoBeats(k) : song_.lock(lfoHz(k)) / sr) : voiceLfoStep(k);
      lfoSlewK[k] = lfoSlew(k);
    }
    auto songLfos = [&]() {
      for (int k = 0; k < kLfos; ++k)
        if (songLfo[k]) {
          if (useLfo[k]) lfoNow_[k] = songLfoNext(k, lfoStep[k], slide, lfoSlewK[k]);
          else lfo_[k].phase.next(lfoStep[k], slide);
        }
    };
    if (!any) {
      for (int i = from; i < to; ++i) songLfos();
      return;
    }
    // The knobs (a moved one: its value per voice, below).
    const double* P = params;
    auto at = [&](const Mod& m, int slot, double lo, double hi) {
      const int i = modIndex_[slot];
      const double base = clampd(P[slot], lo, hi);
      return i < 0 ? base : clampd(base + m.off[i], lo, hi);
    };
    auto moved = [&](int slot) { return modIndex_[slot] >= 0; };
    const bool anyMoved = moved_ > 0;
    const int t1 = (int)std::lround(clampd(P[osc1Table], 0, Wavetables::kTables));
    const int t2 = (int)std::lround(clampd(P[osc2Table], 0, Wavetables::kTables));
    const Wavetables& wt = Wavetables::get();
    auto ratio = [](double semi, double fine) { return std::pow(2.0, (std::round(clampd(semi, -24, 24)) + clampd(fine, -100, 100) / 100.0) / 12.0); };
    const double o2RatioBase = ratio(P[osc2Semi], P[osc2Fine]);
    const bool ratioMoved = moved(osc2Semi) || moved(osc2Fine);
    const double o1Ratio = std::pow(2.0, std::round(clampd(P[osc1Semi], -24, 24)) / 12.0);
    const int fMode = (int)std::lround(clampd(P[filterMode], 0, dsp::VoiceFilter::kCount - 1));
    const double crushAmt = clampd(P[crush], 0, 1);
    const double crushSteps = std::pow(2.0, 15 - 12 * crushAmt);
    // Held for the same time at any rate (W-Core runs at 2x: the hold is counted in 48 kHz samples).
    const unsigned crushHold = std::max(1u, (unsigned)std::lround((1 + crushAmt * crushAmt * 11) * sr / 48000.0));
    const double stackRatio = std::pow(2.0, (int)std::round(clampd(P[subOct], -3, 1)));
    const int subWave = (int)std::lround(clampd(P[subShape], 0, 5));
    static constexpr int kSubBasic[6] = {0, 0, 1, 3, 2, 4};  // sine, triangle, saw, square, pulse as basic()'s shapes
    const UniLayout& uni = uniLayout((int)std::lround(clampd(P[unison], 1, kOsc)));
    const int warpMode[2] = {(int)std::lround(clampd(P[osc1Warp], 0, kWarps - 1)), (int)std::lround(clampd(P[osc2Warp], 0, kWarps - 1))};
    const bool needB = warpMode[0] >= 9;  // Osc 1's FM / AM / RM read Osc 2
    const double bendSt = clampd(P[bend], -1, 1) * clampd(P[bendRange], 0, 48);
    // Where each source goes (Osc 1, Osc 2, Sub, Noise): 0 = Filter 1, 1 = Filter 2, 2 = past the filters.
    int route[4];
    for (int i = 0; i < 4; ++i) route[i] = (int)std::lround(clampd(P[osc1Route + i], 0, 2));
    const bool split = route[1] != route[0];
    const bool serial = P[filterSerial] >= 0.5;
    bool useF2 = serial, anyDirect = false;
    for (int r : route) { useF2 = useF2 || r == 1; anyDirect = anyDirect || r == 2; }
    const int f2Mode = (int)std::lround(clampd(P[filter2Mode], 0, dsp::VoiceFilter::kCount - 1));
    const double cutoff2 = clampd(P[tone2], 40, 12000);
    const double glideCoef = P[glide] > 0.5 ? msToCoef(clampd(P[glide], 1, 1000) / 4.6, sr) : 0.0;  // ~99 % in glide ms
    const double cutoff = clampd(P[tone], 40, 12000);
    // Pad plays up to 8 notes at once and they add up: -6 dB of headroom, so a 3-4 note chord sits near one
    // Lead note instead of above the kick (engine render 2026-11-06: a 3-note pad at the defaults was -6.5 LUFS
    // and 0 dBFS, the loudest sound in Core; with it, about -12.5 LUFS and -6 dBFS).
    const double gainK = 0.5 * (lead() ? 1.0 : 0.5);
    const double steal = fadeInStep(sr);
    EnvK ek[kEnvs];
    for (int e = 0; e < kEnvs; ++e) ek[e] = envK(e);
    const double grainLen = clampd(P[grainSize], 5, 1000) * 0.001 * sr, grainRate = clampd(P[grainDensity], 1, 200) / sr;
    const double gScan = clampd(P[grainScan], 0, 2), gScatter = clampd(P[grainScatter], 0, 1);
    const AudioData* src = source_ && source_->frames() > 16 ? source_.get() : nullptr;
    const double srcFrames = src ? (double)src->frames() : 0;
    // What only a moved knob recomputes per voice and sample.
    auto noiseCoefOf = [sr](double c) { return 1 - std::exp(-kTau * (200 + 15800 * c) / sr); };
    const double noiseCoefBase = noiseCoefOf(clampd(P[noiseColor], 0, 1));
    const double tuneBase = std::pow(2.0, clampd(P[tune], -100, 100) / 1200.0);
    // Unison gains: Blend sets the side copies against the centre (Szabo's mix curves), the sum stays at one level.
    const int nSide = uni.n % 2 ? uni.n - 1 : uni.n - 2;
    struct SpreadK { double side, centre, norm; };
    auto blendOf = [&](double b) {
      SpreadK k;
      k.side = -0.73764 * b * b + 1.2841 * b + 0.044372;
      k.centre = -0.55366 * b + 0.99785;
      if (uni.n % 2 == 0) k.centre *= 0.7071;  // an even count: two centre copies
      const double c2 = uni.n % 2 ? k.centre * k.centre : 2 * k.centre * k.centre;
      k.norm = 1.0 / std::sqrt(c2 + (double)nSide * k.side * k.side);
      return k;
    };
    const SpreadK spreadBase = blendOf(clampd(P[uniBlend], 0, 1));
    const double detuneBase = detuneCurve(clampd(P[uniDetune], 0, 1));
    const double widthBase = clampd(P[uniWidth], 0, 1);
    const double drvBase = clampd(P[drive], 0, 1), drvNormBase = 1 / std::tanh(1 + 6 * drvBase);
    for (int i = from; i < to; ++i) {
      songLfos();
      const bool coefs = coefCounter_++ % 16 == 0;
      double outL = 0, outR = 0;
      for (auto& v : voices_) {
        if (v.stage() == Off) continue;
        envStep(v.env[0], ek[0], true);
        if (v.stage() == Off) continue;
        envStep(v.env[1], ek[1], false);
        envStep(v.env[2], ek[2], false);
        double vl[kLfos] = {};
        for (int k = 0; k < kLfos; ++k) if (voiceLfo[k]) vl[k] = voiceLfoNext(v, k, lfoStep[k], lfoSlewK[k]);
        Mod m;
        if (anyMoved) std::fill(m.off, m.off + moved_, 0.0);
        evalLinks(v, vl, m);
        // This voice's knobs, with what the links add.
        const double w = at(m, wave, 0, 4);
        const int a = std::min(3, (int)std::floor(w));
        const double mix = w - a;
        const double o2 = at(m, osc2, 0, 1);
        const double w2 = at(m, osc2Wave, 0, 4);
        const int a2 = std::min(3, (int)std::floor(w2));
        const double mix2 = w2 - a2;
        const double o2Ratio = ratioMoved ? ratio(at(m, osc2Semi, -24, 24), at(m, osc2Fine, -100, 100)) : o2RatioBase;
        const double o2Norm = 1.0 / std::sqrt(1 + o2 * o2);
        const double q1 = at(m, osc1Pos, 0, 1), q2 = at(m, osc2Pos, 0, 1);
        const SpreadK spk = moved(uniBlend) ? blendOf(at(m, uniBlend, 0, 1)) : spreadBase;
        const double sideGain = spk.side, centreGain = spk.centre, norm = spk.norm;
        const double detune = (moved(uniDetune) ? detuneCurve(at(m, uniDetune, 0, 1)) : detuneBase) * (1 + m.detune);
        const double width = moved(uniWidth) ? at(m, uniWidth, 0, 1) : widthBase;
        const double wAmt[2] = {warpMode[0] ? at(m, osc1WarpAmt, 0, 1) : 0, warpMode[1] ? at(m, osc2WarpAmt, 0, 1) : 0};
        const double fmNow = at(m, fm, 0, 10) / kTau, fmRat = at(m, fmRatio, 0.25, 16);
        const double tuneMul = moved(tune) ? std::pow(2.0, at(m, tune, -100, 100) / 1200.0) : tuneBase;  // fine tune, live on held notes
        const double grainAmt = at(m, grain, 0, 1), gPos = at(m, grainPos, 0, 1);
        const double gPitch = at(m, grainPitch, -24, 24);
        const double noiseAmt = at(m, noise, 0, 1);
        const double drv = at(m, drive, 0, 1);
        const double o1Lvl = at(m, osc1Level, 0, 1);
        const double fDrive = at(m, filterDrive, 0, 1);
        const double pitchSt = m.pitch + bendSt;
        const double bend = tuneMul * (pitchSt != 0 ? std::pow(2.0, pitchSt / 12.0) : 1.0);
        double pm = 0;
        if (fmNow > 0) {
          pm = fmNow * std::sin(kTau * v.modPhase);
          v.modPhase += v.freq * bend * fmRat / sr; if (v.modPhase >= 1) v.modPhase -= std::floor(v.modPhase);
        }
        if (v.freq != v.target) {
          v.freq = glideCoef > 0 ? v.target + (v.freq - v.target) * glideCoef : v.target;
          if (std::fabs(v.freq - v.target) < 1e-3) v.freq = v.target;
        }
        if (coefs) {
          const double fc = cutoff * std::pow(2.0, m.tone);
          const double res = at(m, bite, 0, 0.95);
          const bool more = fMode > dsp::VoiceFilter::Notch || fDrive > 0;
          v.f1.set(fMode, fc, res, sr, fDrive, more ? at(m, filterShape, 0, 1) : 0.0);
          if (useF2) {
            const double f2Drive = at(m, filter2Drive, 0, 1);
            v.f2.set(f2Mode, cutoff2 * std::pow(2.0, m.tone2), at(m, bite2, 0, 0.95), sr, f2Drive, at(m, filter2Shape, 0, 1));
          }
        }
        // The three buses: into Filter 1, into Filter 2, past the filters.
        double bl[3] = {0, 0, 0}, br[3] = {0, 0, 0};
        double& sl = bl[route[0]];
        double& sr2 = br[route[0]];
        // FM moves the phase: the wave's real step this sample is its own step plus the change of the modulation. The
        // band-limiting (polyBLEP, the wavetables' level) must use that step, else every FM tone aliases.
        const double dpm = pm - v.pmPrev;
        v.pmPrev = pm;
        auto step = [dpm](double d) { return std::min(0.5, std::max(1e-7, std::fabs(d + dpm))); };
        auto read1 = [&](double ph, double d) { return t1 ? wt.read(t1, q1, ph, d) : shape(a, mix, ph, d); };
        auto read2 = [&](double ph, double d) { return t2 ? wt.read(t2, q2, ph, d) : shape(a2, mix2, ph, d); };
        for (int u = 0; u < uni.n; ++u) {
          const int k = uni.slot[u];
          const double dtNote = v.freq * bend * (1 + uni.det[u] * detune) / sr;
          const double dt = dtNote * o1Ratio;
          double ph = v.phase[k] + pm; ph -= std::floor(ph);
          double s = warpMode[0] ? warped(warpMode[0], wAmt[0], ph, step(dt), v.lastB[k], read1) : read1(ph, step(dt));
          v.phase[k] += dt; if (v.phase[k] >= 1) v.phase[k] -= 1;
          double x2 = 0;  // Osc 2 at its level
          if (o2 > 0 || needB) {
            double sum = s;
            {
              const double dt2 = dtNote * o2Ratio;
              double ph2 = v.phase2[k] + pm; ph2 -= std::floor(ph2);
              const double b = warpMode[1] ? warped(warpMode[1], wAmt[1], ph2, step(dt2), s, read2) : read2(ph2, step(dt2));
              v.lastB[k] = b;
              if (o1Lvl != 1) { s *= o1Lvl; sum = s; }  // Osc 1's level (after it moved Osc 2's warp)
              if (o2 > 0) { x2 = o2 * b; sum += x2; }
              v.phase2[k] += dt2; if (v.phase2[k] >= 1) v.phase2[k] -= std::floor(v.phase2[k]);
            }
            if (split) {  // each oscillator to its own bus
              const double gu = (uni.centre[u] ? centreGain : sideGain) * o2Norm;
              const double pl = uni.centre[u] ? 0.7071 : width >= 1 ? uni.panL[u] : 0.625 + width * (uni.panL[u] - 0.625);
              const double pr = uni.centre[u] ? 0.7071 : 1.25 - pl;
              const double xs[2] = {s, x2};
              for (int o = 0; o < 2; ++o) { bl[route[o]] += xs[o] * gu * pl; br[route[o]] += xs[o] * gu * pr; }
              continue;
            }
            s = sum * o2Norm;
          } else if (o1Lvl != 1) s *= o1Lvl;
          s *= uni.centre[u] ? centreGain : sideGain;
          if (uni.centre[u]) { sl += s * 0.7071; sr2 += s * 0.7071; }
          else {
            const double pl = width >= 1 ? uni.panL[u] : 0.625 + width * (uni.panL[u] - 0.625);
            sl += s * pl; sr2 += s * (1.25 - pl);
          }
        }
        for (int b = 0; b < 3; ++b) { bl[b] *= norm; br[b] *= norm; }
        const double subAmt = at(m, subLevel, 0, 1);
        if (subAmt > 0) {  // the Sub: one copy, in the middle, into the filter
          const double dt = v.freq * bend * stackRatio / sr;
          const double s = subAmt * (subWave == 0 ? read1(v.stackPhase, dt) : basic(kSubBasic[subWave], v.stackPhase, dt)) * 0.7071;
          v.stackPhase += dt; if (v.stackPhase >= 1) v.stackPhase -= 1;
          const double sp = at(m, subPan, -1, 1);
          if (sp != 0) { bl[route[2]] += s * std::min(1.0, 1 - sp); br[route[2]] += s * std::min(1.0, 1 + sp); }
          else { bl[route[2]] += s; br[route[2]] += s; }
        }
        if (grainAmt > 0) {
          // A new grain every 1/Density s (with scatter); each one a Hann window over Size, at the note's pitch.
          v.grainClock += grainRate;
          if (v.grainClock >= 1) {
            v.grainClock -= 1 + gScatter * 0.5 * v.grainRng.next();
            for (auto& g : v.grains) {
              if (g.on) continue;
              const double semis = gScatter * 7.0 * v.grainRng.next();  // up to a fifth either way
              const double gRatio = std::pow(2.0, (semis + gPitch) / 12.0);
              const double panAt = 0.5 + 0.5 * gScatter * v.grainRng.next();
              g.on = true; g.age = 0; g.len = std::max(16.0, grainLen);
              g.l = std::cos(panAt * kTau / 4); g.r = std::sin(panAt * kTau / 4);
              if (src) {
                double pos = (gPos + v.scan + gScatter * 0.1 * v.grainRng.next()) * srcFrames;
                pos = std::fmod(pos, srcFrames); if (pos < 0) pos += srcFrames;
                g.pos = pos;
                g.inc = v.freq * bend / midiToHz(60) * gRatio * (src->sampleRate / sr);
              } else {
                g.pos = 0.5 + 0.5 * v.grainRng.next();
                g.inc = v.freq * bend * gRatio / sr;
              }
              break;
            }
          }
          v.scan += gScan / sr * (src ? sr / std::max(1.0, srcFrames) : 0.25);  // Scan 1 = the sound's own speed
          if (v.scan >= 1) v.scan -= std::floor(v.scan);
          double gl = 0, gr = 0;
          for (auto& g : v.grains) {
            if (!g.on) continue;
            const double gw = std::sin(kTau * 0.5 * g.age / g.len);
            const double win = gw * gw;
            double s;
            if (src) {
              const int64_t i0 = (int64_t)g.pos;
              const double f = g.pos - (double)i0;
              const int64_t a0 = ((i0 % (int64_t)srcFrames) + (int64_t)srcFrames) % (int64_t)srcFrames, a1 = (a0 + 1) % (int64_t)srcFrames;
              const double m0 = src->channels > 1 ? 0.5 * (src->L[(size_t)a0] + src->R[(size_t)a0]) : src->L[(size_t)a0];
              const double m1 = src->channels > 1 ? 0.5 * (src->L[(size_t)a1] + src->R[(size_t)a1]) : src->L[(size_t)a1];
              s = m0 + (m1 - m0) * f;
              g.pos += g.inc;
              if (g.pos >= srcFrames) g.pos -= srcFrames;
            } else {
              s = shape(a, mix, g.pos, g.inc);
              g.pos += g.inc; if (g.pos >= 1) g.pos -= 1;
            }
            gl += s * win * g.l; gr += s * win * g.r;
            g.age += 1;
            if (g.age >= g.len) g.on = false;
          }
          // Overlapping grains add up: keep the level near the wave's, whatever Size x Density is.
          const double overlap = std::max(1.0, std::sqrt(grainLen * grainRate));
          // The grains take Osc 1's place (and its bus); the other sources fade as before.
          for (int b = 0; b < 3; ++b) if (b != route[0]) { bl[b] *= 1 - grainAmt; br[b] *= 1 - grainAmt; }
          sl = sl * (1 - grainAmt) + grainAmt * gl * 0.9 / overlap;
          sr2 = sr2 * (1 - grainAmt) + grainAmt * gr * 0.9 / overlap;
        }
        if (noiseAmt > 0) {  // the same noise on both sides, coloured by a one-pole low-pass, into the filter
          const double noiseCoef = moved(noiseColor) ? noiseCoefOf(at(m, noiseColor, 0, 1)) : noiseCoefBase;
          v.noiseLp += (v.noise.next() - v.noiseLp) * noiseCoef;
          const double n = noiseAmt * v.noiseLp * 0.8;
          bl[route[3]] += n; br[route[3]] += n;
        }
        double l, r;
        v.f1.run(fMode, fDrive > 0, bl[0], br[0], l, r);
        // Mix: how much of each filter is heard, the rest of its input dry.
        const double f1Mix = at(m, filterMix, 0, 1);
        if (f1Mix != 1) { l = bl[0] + (l - bl[0]) * f1Mix; r = br[0] + (r - br[0]) * f1Mix; }
        if (useF2) {  // Filter 2: its own sources, and Filter 1's output too when Serial
          const bool f2Drive = at(m, filter2Drive, 0, 1) > 0;
          const double f2Mix = at(m, filter2Mix, 0, 1);
          const double inL = serial ? bl[1] + l : bl[1], inR = serial ? br[1] + r : br[1];
          double l2, r2;
          v.f2.run(f2Mode, f2Drive, inL, inR, l2, r2);
          if (f2Mix != 1) { l2 = inL + (l2 - inL) * f2Mix; r2 = inR + (r2 - inR) * f2Mix; }
          if (serial) { l = l2; r = r2; }
          else { l += l2; r += r2; }
        }
        if (anyDirect) { l += bl[2]; r += br[2]; }
        if (drv > 0) {
          const double drvK = 1 + 6 * drv, drvNorm = drv == drvBase ? drvNormBase : 1 / std::tanh(drvK);
          const double dMix = at(m, driveMix, 0, 1), col = at(m, driveColour, 0, 1);
          double dl, dr;
          if (col > 0) {  // Colour: the curve pushed off centre (even harmonics), its offset taken back out
            const double c = 0.8 * col, tc = std::tanh(c);
            dl = (std::tanh(l * drvK + c) - tc) * drvNorm; dr = (std::tanh(r * drvK + c) - tc) * drvNorm;
          } else { dl = std::tanh(l * drvK) * drvNorm; dr = std::tanh(r * drvK) * drvNorm; }
          if (dMix != 1) { l += (dl - l) * dMix; r += (dr - r) * dMix; }
          else { l = dl; r = dr; }
        }
        double g = v.env[0].v * at(m, level, 0, 1.5) * gainK * m.volume;
        if (v.fadeStep > 0) {  // a ghost of a cut note, fading out
          v.fade -= v.fadeStep;
          if (v.fade <= 0) { v.env[0] = Env{}; v.fade = 1; v.fadeStep = 0; continue; }
          g *= v.fade;
        }
        const double layerPan = at(m, outPan, -1, 1);
        if (layerPan != 0) m.pan += layerPan;
        if (m.pan != 0) { const double pn = clampd(m.pan, -1, 1); l *= std::min(1.0, 1 - pn); r *= std::min(1.0, 1 + pn); }
        if (v.fadeIn < 1) { v.fadeIn = std::min(1.0, v.fadeIn + steal); g *= v.fadeIn; }
        outL += l * g; outR += r * g;
      }
      if (crushAmt > 0) {
        if (crushAt_++ % crushHold == 0) {
          crushHeld_[0] = std::round(outL * crushSteps) / crushSteps;
          crushHeld_[1] = std::round(outR * crushSteps) / crushSteps;
        }
        outL = crushHeld_[0]; outR = crushHeld_[1];
      }
      left[i] += (float)outL; right[i] += (float)outR;
    }
  }
  static double shape(int a, double mix, double p, double dt) {
    const double s = basic(a, p, dt);
    return mix > 0 ? s * (1 - mix) + basic(a + 1, p, dt) * mix : s;
  }
  // One sample of an oscillator through its Warp. read(phase, step) is the oscillator; other is Osc 2's last sample
  // (for Osc 1) or Osc 1's (for Osc 2 and 3).
  template <class Read> static double warped(int mode, double amt, double p, double dt, double other, const Read& read) {
    switch (mode) {
      case 1: { const double k = 1 + 15 * amt; double q = p * k; q -= std::floor(q); return read(q, std::min(0.5, dt * k)); }  // sync
      case 2: return read(std::pow(p, 1 / (1 + 4 * amt)), dt);                       // bend+: the wave pushed early
      case 3: return read(std::pow(p, 1 + 4 * amt), dt);                             // bend-: pushed late
      case 4: { const double w = 1 - 0.98 * amt; return p >= w ? 0.0 : read(p / w, std::min(0.5, dt / w)); }  // PWM
      case 5: {  // asym: the first half squeezed, the second stretched
        const double w = 0.5 * (1 - 0.98 * amt);
        return p < w ? read(0.5 * p / w, std::min(0.5, dt * 0.5 / w)) : read(0.5 + 0.5 * (p - w) / (1 - w), dt);
      }
      case 6: { const double q = p < 0.5 ? 2 * p : 2 - 2 * p; return (1 - amt) * read(p, dt) + amt * read(q, std::min(0.5, 2 * dt)); }  // mirror
      case 7: { const double s = read(p, dt); return p >= 1 - amt ? -s : s; }       // flip: the end of the cycle turned over
      case 8: { const double n = std::round(64 - 60 * amt); return read(std::floor(p * n) / n, dt); }  // quantize
      case 9: { double q = p + amt * other; q -= std::floor(q); return read(q, dt); }  // FM from the other oscillator
      case 10: return read(p, dt) * (1 - amt + amt * (0.5 + 0.5 * other));           // AM
      case 11: return read(p, dt) * (1 - amt + amt * other);                         // ring
      default: return read(p, dt);
    }
  }
  static double basic(int shape, double p, double dt) {  // the five shapes, band-limited
    switch (shape) {
      case 0: return std::sin(kTau * p);
      case 1: { double q = p + 0.25; if (q >= 1) q -= 1; return 1.1 * (1 - 2 * std::fabs(2 * q - 1)); }
      case 2: { double q = p + 0.5; if (q >= 1) q -= 1; return 0.7 * ((p < 0.5 ? 1.0 : -1.0) + polyBlep(p, dt) - polyBlep(q, dt)); }
      case 3: return 0.95 * (1 - 2 * p + polyBlep(p, dt));
      default: { double q = p + 0.75; if (q >= 1) q -= 1; return 0.8 * ((p < 0.25 ? 1.5 : -0.5) + polyBlep(p, dt) - polyBlep(q, dt)); }
    }
  }
};

// ---------------------------------------------------------------- W-Core layers (Diogo 2026-11-07)
// "Melhor fazer de forma que posso criar várias waves com diferentes filters": the "wave" instrument is up to four
// complete W-Core voices (layers) played by the same notes and added together, like the generator lanes of a
// modular synth. Layer 1 is the classic W-Core (its parameters keep their names, so every older song is the same);
// layers 2..4 have the same parameters under "l2_", "l3_", "l4_" and an on switch ("l2_on"...), off by default.
// Lead/Pad and Glide are layer 1's for all of them (one keyboard). Each layer has its own modulation (setWavePatch). A layer's Grain reads its own sound when it has
// one (setLayerSource), else layer 1's.
class WaveStack final : public Instrument {
public:
  static constexpr int kLayers = 4, kPer = Wave::kCount;  // Wave's parameter count
  static constexpr int onSlot(int layer) { return kPer + (layer - 1) * (kPer + 1); }   // layer 1..3 (0-based 1..3)
  static constexpr int firstSlot(int layer) { return layer == 0 ? 0 : onSlot(layer) + 1; }
  // The section lights (Diogo 2026-11-07: "verde on, vermelho off"): one switch per card and layer, after the layers.
  // Off = that card's part is left out (its knobs are kept): each card has the values that leave it out.
  static constexpr int kSections = 9;
  static constexpr const char* kSectionIds[kSections] = {"colour", "grain", "filter", "motion", "osc2", "tables", "shape", "lfo1", "lfo2"};
  static constexpr int sectionSlot(int layer, int s) { return onSlot(kLayers - 1) + 1 + kPer + layer * kSections + s; }
  // The layers run at twice the rate and are brought back down by a half-band filter (2026-11-07, Diogo: "parece
  // sempre tiros num jogo SNES"): FM, Drive and the screaming filter make harmonics above the Nyquist frequency, which
  // at 1x fold back as inharmonic, metallic noise (measured on a held E6: FM 3 at -13 dB of the sound, Drive 0.6 at
  // -35 dB). At 2x they land above 24 kHz and the filter takes them away.
  static constexpr int kOver = 2;
  explicit WaveStack(double sr) : Instrument(sr) {
    // The wavetables are built here, on the thread that builds the graph (audit 2026-11-07 #2: the first note built
    // them on the audio thread, 131 allocations and an FFT, 58 ms in one 32-frame block). The render only reads them.
    (void)Wavetables::get();
    for (auto& l : layers_) l = std::make_unique<Wave>(sr * kOver);
  }
  const char* name() const override { return "wave"; }
  bool stereo() const override { return true; }
  bool on(int layer) const { return layer == 0 || params[onSlot(layer)] >= 0.5; }
  void queueNote(int offset, bool on_, int pitch, double velocity) override {
    if (queued_ >= kQueue) {
      if (on_) { ++droppedNotes_; return; }
      // A note-off is never lost: the queued press of its pitch carries it (it lets go at the note-off's time), or
      // the layers let go now.
      for (int q = queued_ - 1; q >= 0; --q)
        if (queue_[q].on && queue_[q].pitch == pitch) {
          if (queue_[q].offAt < 0) { queue_[q].offAt = std::max(offset, queue_[q].offset); return; }
          break;
        }
      for (int i = 0; i < kLayers; ++i) layers_[i]->noteOff(pitch);
      return;
    }
    queue_[queued_++] = Queued{offset, on_, pitch, velocity};
  }
  void chaseNote(int pitch, double velocity, double age) override {
    for (int i = 0; i < kLayers; ++i) if (on(i)) layers_[i]->chaseNote(pitch, velocity, age * kOver);
  }
  void noteOn(int pitch, double velocity) override { queueNote(0, true, pitch, velocity); }
  void noteOff(int pitch) override { queueNote(0, false, pitch, 0); }
  void allNotesOff() override { queued_ = 0; for (auto& l : layers_) l->allNotesOff(); }
  void setWavePatch(const wmod::Patch& patch) override { for (int i = 0; i < kLayers; ++i) layers_[i]->setLayer(patch.layer[i]); }
  void setSource(std::shared_ptr<const AudioData> source) override { setLayerSource(0, std::move(source)); }
  void setLayerSource(int layer, std::shared_ptr<const AudioData> source) override {
    if (layer < 0 || layer >= kLayers) return;
    own_[layer] = source != nullptr;
    if (layer == 0) for (int i = 1; i < kLayers; ++i) if (!own_[i]) layers_[i]->setSource(source);
    layers_[layer]->setSource(std::move(source));
  }
  void adopt(const Instrument& previous) override {
    if (const auto* p = dynamic_cast<const WaveStack*>(&previous)) {
      for (int i = 0; i < kLayers; ++i) { layers_[i]->adopt(*p->layers_[i]); wasOn_[i] = p->wasOn_[i]; }
      primed_ = p->primed_;
      down_[0] = p->down_[0]; down_[1] = p->down_[1]; sumGain_ = p->sumGain_;
    }
  }
  void setPosition(double beat, double bpm) override {
    sync();
    for (int i = 0; i < kLayers; ++i) if (on(i)) layers_[i]->setPosition(beat, bpm);
  }
  void render(float* out, int frames) override {
    float l[512], r[512];
    for (int done = 0; done < frames;) {
      const int n = std::min(512, frames - done);
      std::fill(l, l + n, 0.0f); std::fill(r, r + n, 0.0f);
      renderStereo(l, r, n);
      for (int i = 0; i < n; ++i) out[done + i] += 0.5f * (l[i] + r[i]);
      done += n;
    }
  }
  void renderStereo(float* left, float* right, int frames) override {
    sync();
    if (!primed_) {  // the first block (no adopt): the layers start as they are; a chased note stays
      for (int i = 0; i < kLayers; ++i) wasOn_[i] = on(i);
      primed_ = true;
    }
    for (int i = 0; i < kLayers; ++i) {
      if (!on(i) && wasOn_[i]) layers_[i]->allNotesOff();  // switched off: its notes end (and its queue stays empty)
      // Switched on again: a layer that is off is not rendered, so its releases stood still; they never come back
      // (audit of main fa3b428, F03: a note let go while the layer was off sounded again when it was switched on).
      if (on(i) && !wasOn_[i]) layers_[i]->silence();
      wasOn_[i] = on(i);
    }
    // Layers add up: each one is a whole synth, so two or more get headroom (-3 dB per doubling), the sum of four
    // full layers stays near one (audit 2026-11-07 #13: four layers were +12 dB over one).
    int layersOn = 0;
    for (int i = 0; i < kLayers; ++i) layersOn += on(i) ? 1 : 0;
    const double sumGain = 1.0 / std::sqrt((double)std::max(1, layersOn));
    int q = 0, pending = 0;  // pending: note-offs carried by their press (a full queue), in time order
    auto send = [&](const Queued& e, int done) {
      for (int i = 0; i < kLayers; ++i)
        if (on(i)) layers_[i]->queueNote(std::max(0, e.offset - done) * kOver, e.on, e.pitch, e.velocity);
    };
    for (int done = 0; done < frames;) {
      const int n = std::min(kChunk, frames - done);
      // The notes of this chunk, at the doubled rate.
      auto letGoUntil = [&](int until) {
        for (; pending > 0 && pendingOff_[0].offset <= until; --pending) {
          send(pendingOff_[0], done);
          std::copy(pendingOff_ + 1, pendingOff_ + pending, pendingOff_);
        }
      };
      for (; q < queued_ && queue_[q].offset < done + n; ++q) {
        letGoUntil(queue_[q].offset);
        send(queue_[q], done);
        if (queue_[q].offAt >= 0) {
          int k = pending++;
          for (; k > 0 && pendingOff_[k - 1].offset > queue_[q].offAt; --k) pendingOff_[k] = pendingOff_[k - 1];
          pendingOff_[k] = Queued{queue_[q].offAt, false, queue_[q].pitch, 0};
        }
      }
      letGoUntil(done + n >= frames ? std::numeric_limits<int>::max() : done + n - 1);  // the last chunk: every one
      std::fill(upL_, upL_ + n * kOver, 0.0f); std::fill(upR_, upR_ + n * kOver, 0.0f);
      for (int i = 0; i < kLayers; ++i) {
        if (on(i)) layers_[i]->renderStereo(upL_, upR_, n * kOver);
        droppedNotes_ += layers_[i]->takeDroppedNotes();
      }
      for (int k = 0; k < n; ++k) {
        sumGain_ += (sumGain - sumGain_) * gainSlew_;  // a layer switched on or off glides over ~5 ms
        const double xl = sumGain_ * down_[0].run(upL_[2 * k], upL_[2 * k + 1]);
        const double xr = sumGain_ * down_[1].run(upR_[2 * k], upR_[2 * k + 1]);
        left[done + k] += (float)xl;
        right[done + k] += (float)xr;
      }
      done += n;
    }
    queued_ = 0;
  }
private:
  void sync() {  // each layer's knobs from its slice of params; Lead/Pad, Glide and the playing knobs from layer 1
    std::copy(params, params + kPer, layers_[0]->params);
    leaveOut(0);
    for (int i = 1; i < kLayers; ++i) {
      std::copy(params + firstSlot(i), params + firstSlot(i) + kPer, layers_[i]->params);
      layers_[i]->params[Wave::mode] = params[Wave::mode];
      layers_[i]->params[Wave::glide] = layers_[0]->params[Wave::glide];
      // One keyboard: the bend, the mod wheel and how notes are played are layer 1's.
      for (int k : {Wave::bend, Wave::modWheel, Wave::polyVoices, Wave::legato, Wave::glideAlways})
        layers_[i]->params[k] = params[k];
      leaveOut(i);
    }
  }
  // The sections switched off (red light) get the values that leave them out, and their movements are muted: the
  // filter's (the envelopes and the key on Tone), Motion's (Glide and LFO 3, version 1's Drift), LFO 1's and LFO 2's.
  void leaveOut(int layer) {
    double* p = layers_[layer]->params;
    auto off = [&](int s) { return params[sectionSlot(layer, s)] < 0.5; };
    bool src[wmod::SrcCount] = {}, toTone[wmod::SrcCount] = {};
    if (off(0)) { p[Wave::noise] = 0; p[Wave::fm] = 0; }
    if (off(1)) p[Wave::grain] = 0;
    if (off(2)) {
      p[Wave::tone] = 12000; p[Wave::bite] = 0; p[Wave::filterMode] = 0; p[Wave::filterDrive] = 0;
      toTone[wmod::SrcEnv1] = toTone[wmod::SrcEnv2] = toTone[wmod::SrcEnv3] = toTone[wmod::SrcNote] = true;
    }
    if (off(3)) { p[Wave::glide] = 0; src[wmod::SrcLfo3] = true; }
    if (off(4)) p[Wave::osc2] = 0;
    if (off(5)) { p[Wave::osc1Table] = 0; p[Wave::osc2Table] = 0; }
    if (off(6)) {
      p[Wave::filterMode] = 0; p[Wave::env2Attack] = 0; p[Wave::env2Sustain] = 0; p[Wave::drive] = 0; p[Wave::crush] = 0;
      p[Wave::filterDrive] = 0; toTone[wmod::SrcNote] = true;
    }
    if (off(7)) src[wmod::SrcLfo1] = true;
    if (off(8)) src[wmod::SrcLfo2] = true;
    layers_[layer]->mute(src, toTone);
  }
  std::unique_ptr<Wave> layers_[kLayers];
  bool own_[kLayers] = {}, wasOn_[kLayers] = {true, false, false, false};
  bool primed_ = false;  // wasOn_ taken from the knobs at the first block, or from the instrument adopted
  struct Queued { int offset; bool on; int pitch; double velocity; int offAt = -1; };
  static constexpr int kQueue = 256, kChunk = 256;
  Queued queue_[kQueue];
  Queued pendingOff_[kQueue];
  int queued_ = 0;
  float upL_[kChunk * kOver] = {}, upR_[kChunk * kOver] = {};
  // A 63-tap half-band low-pass (Kaiser, beta 8): flat to 20 kHz (-0.001 dB), -80 dB from 28 kHz at the doubled rate,
  // so what folds back stays above 20 kHz. Every other tap is zero; the 16 odd taps on each side, then the centre.
  struct HalfBand {
    static constexpr int kTaps = 63, kHalf = 16;
    static constexpr double kOdd[kHalf] = {0.31707287210093404, -0.10244250878154293, 0.057724044394320502, -0.037489381567611592,
      0.025637685282126059, -0.017804700211773501, 0.012315583034667371, -0.0083762104292271784, 0.0055436470171553454,
      -0.0035344143020879315, 0.0021459085710929925, -0.0012220760031961134, 0.00063810637543991835, -0.00029356008100292686,
      0.00010903623056181318, -2.4015252436811136e-05};
    double x[2 * kTaps] = {};  // the last kTaps inputs, twice (no wrap in the sum)
    int at = 0;
    void push(double v) { at = at == 0 ? kTaps - 1 : at - 1; x[at] = x[at + kTaps] = v; }
    // Two inputs at the doubled rate in, one output out.
    double run(double a, double b) {
      push(a); push(b);
      const double* h = x + at;  // h[0] = newest
      double y = 0.4999999672451619 * h[31];
      for (int k = 0; k < kHalf; ++k) y += kOdd[k] * (h[30 - 2 * k] + h[32 + 2 * k]);
      return y;
    }
  };
  HalfBand down_[2];
  double sumGain_ = 1;
  const double gainSlew_ = 1 - std::exp(-1.0 / (0.005 * sampleRate_));
};

// ---------------------------------------------------------------- snare (38, 40) and clap (39)
// Core's own snare and clap, so a template needs no sample for them (CORE-MANUAL core-plugins-ui-and-w-core-review.md:
// "native drum sounds"). The recipes are the classic analogue ones, as Sound On Sound "Synth Secrets" describes them
// (soundonsound.com/techniques/practical-snare-drum-synthesis, soundonsound.com/techniques/synthesizing-drums-snare-drum):
// - snare: two sine "drum head" modes about 1.74x apart (SOS: ~180 and ~330 Hz) that fall a little in pitch on the
//   hit, plus high-passed noise (the snares) with its own, longer envelope. Tone moves the head, Snap the noise.
// - clap (as the TR-808's): band-passed noise struck three times about 10 ms apart (several hands), then a longer
//   tail (the room). Clap tone is the band's centre.
// Decay is the time to about -40 dB. One voice per sound; a new hit fades the old one in 3 ms.
class Snare final : public Instrument {
  enum { tone, snap, decay, clapTone, level };
  struct Voice {
    bool active = false, clap = false;
    double age = 0, velocity = 0, fade = 1, fadeStep = 0, fadeIn = 1, ph1 = 0, ph2 = 0;
    Svf hp, hp2, bp; Noise noise;
  };
  Voice voices_[4];  // two per sound: the old one fades while the new one starts
  static bool clapPitch(int p) { return p == 39; }
public:
  using Instrument::Instrument;
  const char* name() const override { return "snare"; }
  static bool plays(int p) { return p == 38 || p == 40 || p == 39; }
  void noteOn(int pitch, double velocity) override {
    if (!plays(pitch)) return;
    const bool clap = clapPitch(pitch);
    Voice* pair = &voices_[clap ? 2 : 0];
    for (int i = 0; i < 2; ++i) if (pair[i].active && pair[i].fadeStep == 0) pair[i].fadeStep = fadeInStep(sampleRate_);
    Voice& v = !pair[0].active ? pair[0] : !pair[1].active ? pair[1] : (pair[0].fade < pair[1].fade ? pair[0] : pair[1]);
    v = Voice{};
    v.active = true; v.clap = clap; v.velocity = velocity;
    v.noise.state = clap ? 33333u : 44444u;
  }
  void chaseNote(int pitch, double velocity, double age) override {
    if (!plays(pitch)) return;
    if (age > clampd(params[decay], 40, 2000) * 0.001 * sampleRate_) return;  // already gone
    noteOn(pitch, velocity);
    for (auto& v : voices_) if (v.active && v.age == 0 && v.fadeStep == 0) { v.age = age; v.fadeIn = 0; break; }
  }
  void noteOff(int) override {}
  void allNotesOff() override { for (auto& v : voices_) if (v.active) v.fadeStep = fadeInStep(sampleRate_); }
  void adopt(const Instrument& previous) override {
    const auto& p = static_cast<const Snare&>(previous);
    for (int i = 0; i < 4; ++i) voices_[i] = p.voices_[i];
  }
  void render(float* out, int frames) override {
    const double sr = sampleRate_;
    const double f = clampd(params[tone], 120, 400), sn = clampd(params[snap], 0, 1);
    const double decMs = clampd(params[decay], 40, 2000), gain = clampd(params[level], 0, 1.5);
    const double bodyTau = decMs * 0.35 / 4.6 * 0.001 * sr, noiseTau = decMs / 4.6 * 0.001 * sr;
    const double burstTau = 0.0025 * sr, gap = 0.010 * sr, tailTau = decMs / 4.6 * 0.001 * sr;
    for (auto& v : voices_) {
      if (!v.active) continue;
      if (v.clap) { v.bp.set(clampd(params[clapTone], 500, 4000), 0.55, sr); v.hp.set(400, 0, sr); }
      else { v.hp.set(1800, 0.1, sr); v.hp2.set(900, 0, sr); }
      for (int i = 0; i < frames; ++i) {
        const double t = v.age;
        double s;
        if (v.clap) {
          // three hands, then the room
          double env = 0;
          for (int k = 0; k < 3; ++k) { const double a = t - k * gap; if (a >= 0 && a < gap * 1.2) env = std::max(env, std::exp(-a / burstTau)); }
          if (t >= 2 * gap) env = std::max(env, 0.55 * std::exp(-(t - 2 * gap) / tailTau));
          s = 2.2 * v.bp.bp(v.hp.hp(v.noise.next())) * env;
          if (t > 2 * gap && env < 1e-4) v.active = false;
        } else {
          const double bend = 1 + 0.5 * std::exp(-t / (0.012 * sr));  // the head falls into its pitch
          v.ph1 += f * bend / sr; v.ph1 -= std::floor(v.ph1);
          v.ph2 += f * 1.74 * bend / sr; v.ph2 -= std::floor(v.ph2);
          const double body = (std::sin(kTau * v.ph1) + 0.6 * std::sin(kTau * v.ph2)) * std::exp(-t / bodyTau);
          const double nEnv = std::exp(-t / noiseTau);
          const double snares = v.hp.hp(v.hp2.hp(v.noise.next())) * nEnv;
          s = (1 - 0.5 * sn) * body * 0.7 + sn * snares * 1.6;
          if (nEnv < 1e-4 && t > bodyTau * 9.2) v.active = false;
        }
        s *= v.velocity * gain;
        if (v.fadeIn < 1) { v.fadeIn = std::min(1.0, v.fadeIn + fadeInStep(sr)); s *= v.fadeIn; }
        if (v.fadeStep > 0) { s *= v.fade; v.fade -= v.fadeStep; if (v.fade <= 0) v.active = false; }
        out[i] += (float)s;
        v.age += 1;
        if (!v.active) break;
      }
    }
  }
};

// ---------------------------------------------------------------- drum kit: kick (36) + hats (42 closed, 46 open)
// Same shape as the JS "drums" track. Parameters: the 7 kick ones, then the 4 hats ones.
class Drums final : public Instrument {
  Kick kick_; Hats hats_;
  // 42 closed, 44 pedal, 46 open, 51 ride, 70 shaker (other pitches stay silent, as before)
  static bool hatsPitch(int p) { return p == 42 || p == 44 || p == 46 || p == 51 || p == 70; }
public:
  explicit Drums(double sr) : Instrument(sr), kick_(sr), hats_(sr) {}  // the kick's other knobs: its own sound
  const char* name() const override { return "drums"; }
  // The kick and the hats always see the drums' knobs: copied before every call that reaches them.
  void shareParams() {
    for (int i = 0; i < 7; ++i) kick_.params[i] = params[i];
    for (int i = 0; i < 4; ++i) hats_.params[i] = params[7 + i];
  }
  void noteOn(int pitch, double velocity) override {
    shareParams();
    if (pitch == 36) kick_.noteOn(pitch, velocity);
    else if (hatsPitch(pitch)) hats_.noteOn(pitch, velocity);
  }
  void chaseNote(int pitch, double velocity, double age) override {
    shareParams();
    if (pitch == 36) kick_.chaseNote(pitch, velocity, age);
    else if (hatsPitch(pitch)) hats_.chaseNote(pitch, velocity, age);
  }
  void noteOff(int) override {}
  void allNotesOff() override { kick_.allNotesOff(); hats_.allNotesOff(); }
  void adopt(const Instrument& previous) override {
    const auto& p = static_cast<const Drums&>(previous);
    kick_.adopt(p.kick_); hats_.adopt(p.hats_);
  }
  void render(float* out, int frames) override {
    shareParams();
    kick_.render(out, frames);
    hats_.render(out, frames);
  }
};

// The "instrument" of an audio or a bus track: no notes of its own, silence (the clips and the inputs
// are played by the graph).
class Silent final : public Instrument {
public:
  using Instrument::Instrument;
  const char* name() const override { return "silent"; }
  void noteOn(int, double) override {}
  void noteOff(int) override {}
  void allNotesOff() override {}
  void render(float*, int) override {}
  void adopt(const Instrument&) override {}
};

} // namespace

// Every built-in instrument describes itself here once: id, sound version, parameter catalogue (in the
// order its class reads params[]) and how to build it. This is the single source of truth the validation
// (project.cpp), the graph and the app's catalogue all read.
void registerNativeInstruments(NativeRegistry& reg) {
  const std::vector<ParamSpec> kick = {
    // Core's one kick (see the Kick class): the defaults are C2's sound, fitted to Psiger's KICK (2026-11-08). The
    // first seven are the drums' kick knobs too.
    {"pitchStart", 1938, 60, 4000, "Hz"}, {"pitchEnd", 42.13, 25, 120, "Hz"},
    {"pitchDecay", 3.409, 0.2, 200, "ms"}, {"ampDecay", 251.9, 5, 1500, "ms"},
    {"click", 0.1615, 0, 1, ""},          {"drive", 0.09822, 0, 1, ""},
    {"level", 0.85, 0, 1.5, ""},          {"hold", 68.98, 0, 500, "ms"},
    {"knee", 144.7, 40, 500, "Hz"},       {"kneeDecay", 56.01, 5, 400, "ms"},
    {"length", 206.6, 60, 1500, "ms"},    {"clickTone", 6364, 300, 12000, "Hz"}};
  // W-Core · Wave (docs/historico/w-core-wave.md): Core's lead, pad and drone synth (Wave::specList, one layer's knobs).
  // Version 2 (2026-11-08): the modulation system; version 1's fixed movements are links (wave_legacy.hpp).
  const std::vector<ParamSpec>& wave = Wave::specList();
  const std::vector<ParamSpec> hats = {
    {"tone", 8000, 2000, 16000, "Hz"},  {"closedDecay", 35, 5, 400, "ms"},
    {"openDecay", 220, 20, 2000, "ms"}, {"level", 0.45, 0, 1.5, ""}};
  // Snare (38, 40) and clap (39): Tone = the head's pitch, Snap = how much of the snares (noise), Decay to -40 dB.
  const std::vector<ParamSpec> snare = {
    {"tone", 190, 120, 400, "Hz"},      {"snap", 0.6, 0, 1, ""},
    {"decay", 220, 40, 2000, "ms"},     {"clapTone", 1100, 500, 4000, "Hz"},
    {"level", 0.6, 0, 1.5, ""}};
  // Core Sampler (dsp/sampler.hpp). decay at 10000 ms = off (the sample plays to its end);
  // pitched 1 = the key transposes from the zone's root and the note end releases it (a bass).
  // retrigger 0 = a note that comes while the same zone (pitched: the same key) still sounds is ignored (a riser
  // on a roll plays once; another zone of a multi-sample still plays).
  // start reaches 60 s so a long riser can be started late enough to end on the drop.
  const std::vector<ParamSpec> sampler = {
    {"tune", 0, -24, 24, "st"},         {"start", 0, 0, 60000, "ms"},
    {"attack", 0, 0, 200, "ms"},        {"decay", 10000, 10, 10000, "ms"},
    {"release", 30, 5, 2000, "ms"},     {"pitched", 0, 0, 1, ""},
    {"level", 0, -24, 24, "dB"},        {"retrigger", 1, 0, 1, ""},
    // Sound design (2026-11-07), all as before by default.
    {"sustain", 0, 0, 1, ""},           {"reverse", 0, 0, 1, ""},
    {"filterType", 0, 0, 3, ""},        {"cutoff", 20000, 20, 20000, "Hz"},
    {"resonance", 0.1, 0, 0.95, ""},    {"filterEnv", 0, -5, 5, "oct"},
    {"filterDecay", 200, 5, 4000, "ms"}, {"velocity", 1, 0, 1, ""}};
  // Drums = the kick's 7 parameters then the hats' 4, prefixed. A ParamSpec holds a const char*, so the
  // prefixed names are kept alive for the registry's lifetime; reserve() first so no push_back reallocates.
  static std::vector<std::string> drumNames;
  drumNames.reserve(32);
  std::vector<ParamSpec> drums;
  // The drums keep the kick's first 7 knobs (their order is the drums' own); the others are the kick's sound.
  const std::vector<ParamSpec> drumKick(kick.begin(), kick.begin() + 7);
  for (const auto& s : drumKick) drumNames.push_back(std::string("kick.") + s.name);
  for (const auto& s : hats) drumNames.push_back(std::string("hats.") + s.name);
  size_t i = 0;
  for (const auto& s : drumKick) drums.push_back({drumNames[i++].c_str(), s.def, s.min, s.max, s.unit});
  for (const auto& s : hats) drums.push_back({drumNames[i++].c_str(), s.def, s.min, s.max, s.unit});

  // Version 2 (2026-11-07): the one kick. A version 1 kick's knobs were the old kick's, so it plays this kick's
  // own sound (project.cpp), unless it was already on the shaped engine.
  reg.add({"kick", NativeKind::Instrument, 2, false, kick,
           [](double sr) { return std::make_unique<Kick>(sr); }, nullptr});
  // Layers 2..4 (WaveStack): an on switch, then every Wave parameter again under "lN_".
  static std::vector<std::string> layerNames;
  std::vector<ParamSpec> layered = wave;
  if (layerNames.empty()) {
    layerNames.reserve(3 * (wave.size() + 1));
    for (int n = 2; n <= 4; ++n) {
      layerNames.push_back("l" + std::to_string(n) + "_on");
      for (const auto& p : wave) layerNames.push_back("l" + std::to_string(n) + "_" + p.name);
    }
  }
  // Then the section lights, 10 per layer ("sec_colour", "l2_sec_colour"...), on (1) by default.
  if (layerNames.size() == 3 * (wave.size() + 1)) {
    layerNames.reserve(layerNames.size() + 4 * WaveStack::kSections);
    for (int n = 1; n <= 4; ++n)
      for (const char* s : WaveStack::kSectionIds) layerNames.push_back((n == 1 ? std::string() : "l" + std::to_string(n) + "_") + "sec_" + s);
  }
  size_t at = 0;
  for (int n = 2; n <= 4; ++n) {
    layered.push_back({layerNames[at++].c_str(), 0, 0, 1, ""});
    for (const auto& p : wave) { ParamSpec x = p; x.name = layerNames[at++].c_str(); layered.push_back(x); }
  }
  while (at < layerNames.size()) layered.push_back({layerNames[at++].c_str(), 1, 0, 1, ""});
  reg.add({"wave", NativeKind::Instrument, 2, false, layered,
           [](double sr) { return std::make_unique<WaveStack>(sr); }, nullptr});
  reg.add({"hats", NativeKind::Instrument, 1, false, hats,
           [](double sr) { return std::make_unique<Hats>(sr); }, nullptr});
  reg.add({"snare", NativeKind::Instrument, 1, false, snare,
           [](double sr) { return std::make_unique<Snare>(sr); }, nullptr});
  reg.add({"drums", NativeKind::Instrument, 1, false, drums,
           [](double sr) { return std::make_unique<Drums>(sr); }, nullptr});
  // The sampler is built by the graph (it needs the track's zones and the asset store), so no factory here.
  reg.add({"sampler", NativeKind::Instrument, 4, false, sampler, nullptr, nullptr});
  // Core Glitch · Sample Path (sample_path.hpp): its map is the track's "synth", its sounds the track's samples.
  reg.add({"samplepath", NativeKind::Instrument, 1, false,
           {{"amp", -8, -40, 6, "dB"}, {"pitch", 0, -12, 12, "st"}, {"size", 0.45, 0, 1, ""}, {"damp", 0.58, 0, 1, ""},
            {"mix", 0.12, 0, 1, ""}, {"limiter", 1, 0, 1, ""}},
           [](double sr) { return std::make_unique<SamplePath>(sr); }, nullptr});
  // Tracks with no built-in instrument parameters. A placeholder spec keeps them "known" to validation, and
  // the graph gives them a Silent instrument (an audio or a bus track).
  const std::vector<ParamSpec> none{{"_", 0, 0, 0, ""}};
  const std::function<std::unique_ptr<Instrument>(double)> missing =
      [](double sr) { return std::make_unique<Silent>(sr); };
  reg.add({"audio", NativeKind::Instrument, 1, true, none, missing, nullptr});
  reg.add({"bus", NativeKind::Instrument, 1, true, none, missing, nullptr});
}

std::unique_ptr<Instrument> makeInstrument(const std::string& name, double sampleRate) {
  const NativePluginDesc* d = nativePlugins().find(NativeKind::Instrument, name);
  if (d && d->makeInstrument) return d->makeInstrument(sampleRate);
  return nullptr;  // unknown, or built by the graph (the sampler)
}

} // namespace core
