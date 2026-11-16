// Core AI Gate (Diogo, 2026-11-16): the first plugin of Core's own "AI FX" family. A trance gate in time with the
// song (16 steps: on, half or off, at 1/8, 1/16, 1/32 or 1/16T) with a duck on every beat (the pump against the
// kick, with no sidechain routing). Our own code, made to be driven by the AI: few knobs, named patterns.
//
// One header, std only: the plugin (Core AI Gate.vst3, JUCE), the role map, the AI's "gate" call and the tests all
// use the same parameter table, so the AI writes exactly what the plugin reads.
//
// Real-time rules (CLAUDE.md): process() allocates nothing, locks nothing, logs nothing; every change of a knob is
// smoothed (no clicks); a NaN or Inf in the input never reaches the output; no denormals.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace core::aifx {

// ---- the parameters, in the plugin's order. The VST3 ParamID of each is its index here (the plugin is built with
// JUCE_FORCE_USE_LEGACY_PARAM_IDS), so this order NEVER changes: a new parameter goes at the end.
constexpr int kSteps = 16;
enum Param : int {
  kStep1 = 0,  // ... kStep1 + 15: Off / Half / On
  kRate = 16,  // 1/8, 1/16, 1/32, 1/16T
  kDepth,      // 0..1: how far the closed steps go down (0 = the gate does nothing)
  kLength,     // 0.1..1: how much of each step is open
  kSmooth,     // 0.5..30 ms: hard or soft edges
  kDuck,       // 0..1: the dip at every beat (the kick)
  kDuckLength, // 0.1..1 of a beat
  kDuckCurve,  // 0.5..4: how it comes back (higher = longer down)
  kMix,        // 0..1
  kOutput,     // -24..+6 dB
  kAutoGain,   // off / on: the gated sound back near the dry level (up to +6 dB)
  kParamCount
};

struct ParamInfo {
  const char* id;     // the plugin's parameter id
  const char* name;   // as hosts show it (Settings > Roles guesses the macros from it)
  double min, max, def;
  int choices;        // > 0: a choice of that many (normalized = index / (choices - 1))
};

inline const std::array<ParamInfo, kParamCount>& params() {
  static const std::array<ParamInfo, kParamCount> table = [] {
    std::array<ParamInfo, kParamCount> t{};
    static const char* ids[kSteps] = {"step01", "step02", "step03", "step04", "step05", "step06", "step07", "step08",
                                      "step09", "step10", "step11", "step12", "step13", "step14", "step15", "step16"};
    static const char* names[kSteps] = {"Step 01", "Step 02", "Step 03", "Step 04", "Step 05", "Step 06", "Step 07", "Step 08",
                                        "Step 09", "Step 10", "Step 11", "Step 12", "Step 13", "Step 14", "Step 15", "Step 16"};
    for (int i = 0; i < kSteps; ++i) t[(size_t)i] = {ids[i], names[i], 0, 2, 2, 3};
    t[kRate] = {"rate", "Rate", 0, 3, 1, 4};
    t[kDepth] = {"depth", "Depth", 0, 1, 1, 0};
    t[kLength] = {"length", "Length", 0.1, 1, 0.5, 0};
    t[kSmooth] = {"smooth", "Smooth", 0.5, 30, 3, 0};
    t[kDuck] = {"duck", "Duck", 0, 1, 0, 0};
    t[kDuckLength] = {"duckLength", "Duck Length", 0.1, 1, 0.5, 0};
    t[kDuckCurve] = {"duckCurve", "Duck Curve", 0.5, 4, 1.5, 0};
    t[kMix] = {"mix", "Mix", 0, 1, 1, 0};
    t[kOutput] = {"output", "Output", -24, 6, 0, 0};
    t[kAutoGain] = {"autoGain", "Auto Gain", 0, 1, 1, 2};
    return t;
  }();
  return table;
}

// A plain value (a step 0/1/2, a rate index, 0.5 depth, 3 ms) as the host's 0..1, and back.
inline double toNormalized(int p, double v) {
  const ParamInfo& i = params()[(size_t)p];
  return std::clamp((v - i.min) / (i.max - i.min), 0.0, 1.0);
}
inline double fromNormalized(int p, double n) {
  const ParamInfo& i = params()[(size_t)p];
  const double v = i.min + std::clamp(n, 0.0, 1.0) * (i.max - i.min);
  return i.choices > 0 ? std::round(v) : v;
}

// ---- rates and patterns
inline const std::vector<std::string>& rateNames() {
  static const std::vector<std::string> r = {"1/8", "1/16", "1/32", "1/16T"};
  return r;
}
inline double stepBeats(int rate) {
  switch (rate) {
    case 0: return 0.5;
    case 2: return 0.125;
    case 3: return 1.0 / 6.0;
    default: return 0.25;
  }
}
inline int rateIndex(const std::string& name) {
  const auto& r = rateNames();
  for (size_t i = 0; i < r.size(); ++i) {
    std::string a = r[i], b = name;
    for (auto& c : a) c = (char)std::tolower((unsigned char)c);
    for (auto& c : b) c = (char)std::tolower((unsigned char)c);
    if (a == b) return (int)i;
  }
  return -1;
}

// A pattern as text, one character per step: x = on, h = half, . = off ("x.xx" repeats to 16 steps; 1, 2, 4, 8 or
// 16 characters; spaces and | are ignored). Returns false when it is not one.
inline bool parsePattern(const std::string& text, std::array<int, kSteps>& steps) {
  std::vector<int> v;
  for (char c : text) {
    const char l = (char)std::tolower((unsigned char)c);
    if (l == ' ' || l == '|') continue;
    if (l == 'x' || l == '1') v.push_back(2);
    else if (l == 'h' || l == '-') v.push_back(1);
    else if (l == '.' || l == '0' || l == '_') v.push_back(0);
    else return false;
  }
  if (v.empty() || v.size() > (size_t)kSteps || kSteps % v.size() != 0) return false;
  for (int i = 0; i < kSteps; ++i) steps[(size_t)i] = v[(size_t)i % v.size()];
  return true;
}
inline std::string patternText(const std::array<int, kSteps>& steps) {
  std::string s;
  for (int v : steps) s += v >= 2 ? 'x' : v == 1 ? 'h' : '.';
  return s;
}

// Everything the gate does, in plain values.
struct Settings {
  std::array<int, kSteps> steps{2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2};
  int rate = 1;
  double depth = 1, length = 0.5, smoothMs = 3, duck = 0, duckLength = 0.5, duckCurve = 1.5, mix = 1, outputDb = 0;
  bool autoGain = true;
};

// The named patterns (what the AI picks instead of 16 values).
struct Preset {
  const char* id;
  const char* what;
  const char* pattern;
  int rate;
  double depth, length, smoothMs, duck, duckLength;
};
inline const std::vector<Preset>& presets() {
  static const std::vector<Preset> p = {
      {"trance-16", "the classic trance gate: every 1/16, half open", "x", 1, 1.0, 0.5, 3, 0, 0.5},
      {"psy-bass-gap", "the rolling bass with the first 1/16 of each beat cut (room for the kick)", ".xxx", 1, 1.0, 1.0, 1, 0, 0.5},
      {"gallop", "gallop: off, on, on on each beat", "x.xx", 1, 1.0, 0.8, 2, 0, 0.5},
      {"offbeat", "only the offbeat 1/16 of each beat: stabs", "..x.", 1, 1.0, 0.7, 2, 0, 0.5},
      {"hitech-32", "fast hi-tech chop at 1/32", "x", 2, 1.0, 0.5, 1, 0, 0.5},
      {"triplet", "1/16 triplets, a soft step every fourth: a stagger", "xxxh", 3, 1.0, 0.7, 2, 0, 0.5},
      {"half-time", "slow 1/8 chop, soft edges", "x.", 0, 0.9, 0.8, 8, 0, 0.5},
      {"pump-soft", "no chop: a soft dip at every beat (a pad breathing with the kick)", "x", 1, 0.0, 1.0, 3, 0.6, 0.6},
      {"pump-hard", "no chop: a hard short duck at every beat", "x", 1, 0.0, 1.0, 1, 1.0, 0.35},
  };
  return p;
}
inline const Preset* findPreset(const std::string& id) {
  for (const auto& p : presets()) {
    std::string a = p.id, b = id;
    for (auto& c : b) c = (char)std::tolower((unsigned char)c);
    if (a == b) return &p;
  }
  return nullptr;
}
inline std::vector<std::string> presetIds() {
  std::vector<std::string> out;
  for (const auto& p : presets()) out.push_back(p.id);
  return out;
}
inline Settings fromPreset(const Preset& p) {
  Settings s;
  parsePattern(p.pattern, s.steps);
  s.rate = p.rate;
  s.depth = p.depth;
  s.length = p.length;
  s.smoothMs = p.smoothMs;
  s.duck = p.duck;
  s.duckLength = p.duckLength;
  return s;
}

// The gain of the gate alone (no smoothing) at a song position in beats.
inline double gateGain(const Settings& s, double ppq) {
  const double sb = stepBeats(s.rate);
  const double pos = ppq / sb;
  const double whole = std::floor(pos);
  const int step = (int)(((int64_t)whole % kSteps + kSteps) % kSteps);
  const double frac = pos - whole;
  const double level = s.steps[(size_t)step] / 2.0;
  const double open = frac < s.length ? level : 0.0;
  double g = 1.0 - s.depth * (1.0 - open);
  if (s.duck > 0) {
    const double beat = ppq - std::floor(ppq);
    if (beat < s.duckLength) g *= 1.0 - s.duck * std::pow(1.0 - beat / s.duckLength, s.duckCurve);
  }
  return std::clamp(g, 0.0, 1.0);
}

// The make-up gain of Auto Gain: 1 / the gate's mean gain over one pattern, at most +6 dB.
inline double autoGainOf(const Settings& s) {
  double open = 0;
  for (int v : s.steps) open += v / 2.0;
  open = open / kSteps * s.length;
  double mean = 1.0 - s.depth * (1.0 - open);
  if (s.duck > 0) mean *= 1.0 - s.duck * s.duckLength / (s.duckCurve + 1.0);  // the duck's mean over a beat
  return std::clamp(1.0 / std::max(mean, 1e-6), 1.0, 2.0);
}

// ---- the sound
class Gate {
public:
  void prepare(double sampleRate) {
    rate_ = sampleRate > 0 ? sampleRate : 48000;
    gain_ = 1;
    mix_ = 1;
    out_ = 1;
    make_ = 1;
  }
  // `ppq`: the song position in beats at the first sample; playing = false: the sound passes as it is (smoothly).
  void process(float* const* channels, int numChannels, int numSamples, const Settings& s, double ppq, double bpm, bool playing) {
    const double perSample = playing && bpm > 0 ? bpm / (60.0 * rate_) : 0.0;
    const double edge = 1.0 - std::exp(-1.0 / (std::clamp(s.smoothMs, 0.5, 30.0) * 0.001 * rate_));
    const double knob = 1.0 - std::exp(-1.0 / (0.02 * rate_));  // 20 ms for mix, output and the make-up
    const double mixTarget = std::clamp(s.mix, 0.0, 1.0);
    const double outTarget = std::pow(10.0, std::clamp(s.outputDb, -24.0, 6.0) / 20.0);
    const double makeTarget = s.autoGain && playing ? autoGainOf(s) : 1.0;
    for (int i = 0; i < numSamples; ++i) {
      const double target = playing ? gateGain(s, ppq + perSample * i) : 1.0;
      gain_ += (target - gain_) * edge;
      mix_ += (mixTarget - mix_) * knob;
      out_ += (outTarget - out_) * knob;
      make_ += (makeTarget - make_) * knob;
      if (std::abs(gain_) < 1e-15) gain_ = 0;  // no denormals
      const double wet = gain_ * make_;
      const double g = ((1.0 - mix_) + mix_ * wet) * out_;
      for (int c = 0; c < numChannels; ++c) {
        float& x = channels[c][i];
        const float y = (float)(x * g);
        x = std::isfinite(y) ? y : 0.0f;
      }
    }
  }
  double currentGain() const { return gain_; }

private:
  double rate_ = 48000, gain_ = 1, mix_ = 1, out_ = 1, make_ = 1;
};

}  // namespace core::aifx
