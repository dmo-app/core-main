// W-Core's knob values as words ("1/8 T", "Filter 2", "7 voices"...): the panel and the modular window share them.
#pragma once
#include <algorithm>
#include <cmath>
#include <string>
#include <juce_gui_basics/juce_gui_basics.h>
#include "edit.hpp"
#include "sound_layers.hpp"
#include "wcore.hpp"

namespace core::app {

inline bool isWaveLfoRate(const std::string& name) { return name.size() == 8 && name.rfind("lfo", 0) == 0 && name.substr(4) == "Rate"; }

inline juce::String waveValueText(const doc::InstrumentParamInfo& p, double v) {
  std::string name = p.name;
  if (doc::isWaveLayerParam(name)) name = name.substr(3);  // "l2_tone": wave 2's Tone
  if (name == "wave" || name == "osc2Wave") return juce::String::fromUTF8(doc::waveShapeName(v).c_str());
  const int i = (int)std::lround(v);
  if (name == "osc1Table" || name == "osc2Table") {
    static const char* t[] = {"Classic", "PWM", "Sync", "Vowel", "Organ", "FM", "Fold", "Growl", "Digital"}; return t[std::clamp(i, 0, 8)];
  }
  if (name == "osc1Route" || name == "osc2Route" || name == "subRoute" || name == "noiseRoute") {
    static const char* t[] = {"Filter 1", "Filter 2", "Dry"}; return t[std::clamp(i, 0, 2)];
  }
  if (name == "filterSerial") return i ? juce::String("on") : juce::String("off");
  if (name == "filterMode" || name == "filter2Mode") {
    static const char* t[] = {"Low 24", "Low 12", "Band", "High", "Notch", "Low 6", "High 6", "Band 24", "High 12", "Ladder", "Comb", "Formant",
                              "Diode", "MS-20 Low", "MS-20 High", "Comb -", "Phaser", "Morph", "Dual Notch", "Dual Band"};
    return t[std::clamp(i, 0, 19)];
  }
  if (name == "subShape") { static const char* t[] = {"Osc 1", "Sine", "Triangle", "Saw", "Square", "Pulse"}; return t[std::clamp(i, 0, 5)]; }
  if (name == "osc1Warp" || name == "osc2Warp") {
    static const char* t[] = {"off", "Sync", "Bend+", "Bend-", "PWM", "Asym", "Mirror", "Flip", "Quantize", "FM", "AM", "Ring"};
    return t[std::clamp(i, 0, 11)];
  }
  if (name == "unison") return i <= 1 ? juce::String("1 voice") : juce::String(i) + " voices";
  if (name == "polyVoices") return juce::String(i) + (i == 1 ? " note" : " notes");
  if (name == "legato" || name == "glideAlways") return i ? juce::String("on") : juce::String("off");
  if (name == "subOct") return i == 0 ? juce::String("0") : (i > 0 ? "+" : "") + juce::String(i) + " oct";
  if (name == "bend") return std::fabs(v) < 0.005 ? juce::String("0") : (v > 0 ? "+" : "") + juce::String(v, 2);
  if (name == "bendRange") return juce::String(i) + " st";
  if (isWaveLfoRate(name)) {  // beats per movement, as notes and bars (4/4)
    if (v >= 4 - 1e-6) { const double bars = v / 4; return juce::String(bars, std::fabs(bars - std::round(bars)) < 1e-6 ? 0 : 2) + (bars == 1 ? " bar" : " bars"); }
    const double d = 4 / v;  // 1/d of a bar
    if (std::fabs(d - std::round(d)) < 1e-6) {
      const int n = (int)std::lround(d);  // 1/12 of a bar = an 1/8 triplet
      return n % 3 == 0 ? "1/" + juce::String(n * 2 / 3) + " T" : "1/" + juce::String(n);
    }
    if (std::fabs(d * 1.5 - std::round(d * 1.5)) < 1e-6) return "1/" + juce::String((int)std::lround(d * 1.5)) + " D";
    return juce::String(v, 2) + " beats";
  }
  if (p.unit == std::string("Hz") && v < 10) return juce::String(v, v < 1 ? 3 : 2) + " Hz";
  if (name == "osc1Semi" || name == "osc2Semi") return i == 0 ? juce::String("0 st") : (i > 0 ? "+" : "") + juce::String(i) + " st";
  if (name == "stack") return v < -1.5 ? juce::String("-2 oct") : v < -0.5 ? juce::String("-1 oct") : v > 0.5 ? juce::String("+1 oct") : juce::String("off");
  if (name == "octave") return v == 0 ? juce::String("0") : (v > 0 ? "+" : "") + juce::String((int)std::lround(v));
  const std::string unit = p.unit;
  if (unit == "Hz") return juce::String((int)std::lround(v)) + " Hz";
  if (name == "glide" && v < 0.5) return juce::String("off");
  if (unit == "ms") return v >= 1000 ? juce::String(v / 1000, 1) + " s" : juce::String((int)std::lround(v)) + " ms";
  if (unit == "oct") return juce::String(v, 1) + " oct";
  if (unit == "ct") return std::fabs(v) < 0.5 ? juce::String("0 ct") : (v > 0 ? "+" : "") + juce::String((int)std::lround(v)) + " ct";
  if (unit == "st") return std::fabs(v) < 0.05 ? juce::String("off") : (v > 0 ? "+" : "") + juce::String(v, 1) + " st";
  if (unit == "/s") return juce::String((int)std::lround(v)) + " /s";
  if (name == "grainScan") return v < 0.005 ? juce::String("frozen") : juce::String::fromUTF8("\xc3\x97") + juce::String(v, 2);
  if (name == "fmRatio") return juce::String::fromUTF8("\xc3\x97") + juce::String(v, 2);
  return juce::String((int)std::lround(v / std::max(1e-9, p.max) * 100)) + " %";
}

}  // namespace core::app
