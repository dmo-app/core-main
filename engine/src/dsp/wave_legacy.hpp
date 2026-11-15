// W-Core version 1 -> 2 (2026-11-08): the fixed movements of the first W-Core become links of the modulation system
// (wave_mod.hpp), and their knobs are gone. "Nada antigo" (Diogo): no old engine is kept next to the new one; a song,
// a template or a saved sound of version 1 is converted when it is read, and plays the same:
//   swell / decay / sustain / fade            -> ENV 1 (the volume)
//   contour, bloom, filterAttack/Sustain       -> ENV 2, linked to Tone by Contour octaves (times the velocity, 0.6..1)
//   pitchEnv, pitchTime (Zap)                  -> ENV 3, linked to the pitch by Zap semitones
//   velocity (how much a harder note is louder) -> a Velocity link to the volume
//   keyTrack                                   -> a Note link to Tone (5 octaves over the keyboard's half = 1 per octave)
//   drift + driftHz                            -> LFO 3, a sine at driftHz (locked to 4 bars), linked to Tone and the detune
//   lfo1/2 Dest Shape Rate Amount              -> LFO 1 and 2, with one link (two to four for "shape") each
//   spread                                     -> Unison 7 voices (1 at 0), Detune and Blend = Spread
//   stack                                      -> the Sub: Osc 1's wave at 0.6, Stack octaves (none at 0)
//   osc3 (Diogo 2026-11-08)                    -> the Sub when it is an octave or two under and the Sub is free,
//                                                 else a wave of its own (the next free layer, a copy of its layer
//                                                 playing Osc 3's wave); the levels are kept (close, not bit-identical)
// Header only: the engine (core-project files) and the document (core-document files, saved sounds) both use it.
#pragma once
#include <cmath>
#include <map>
#include <string>
#include <vector>
#include "../../third_party/json.hpp"

namespace core::wlegacy {

using json = nlohmann::json;

// The knobs of version 1 that version 2 no longer has, with their version-1 defaults.
inline const std::map<std::string, double>& removedV1() {
  static const std::map<std::string, double> m = {
      {"contour", 1.5},      {"bloom", 400},       {"swell", 5},        {"fade", 300},       {"drift", 0.2},
      {"driftHz", 0.25},     {"decay", 1000},      {"sustain", 1},      {"pitchEnv", 0},     {"pitchTime", 80},
      {"filterAttack", 0},   {"filterSustain", 0}, {"keyTrack", 0},     {"velocity", 1},
      {"lfo1Dest", 0},       {"lfo1Shape", 0},     {"lfo1Rate", 4},     {"lfo1Amount", 0.3},
      {"lfo2Dest", 0},       {"lfo2Shape", 0},     {"lfo2Rate", 2},     {"lfo2Amount", 0.3},
      {"spread", 0.3},       {"stack", 0},
      {"osc3", 0},           {"osc3Wave", 3},      {"osc3Table", 0},    {"osc3Pos", 0},      {"osc3Semi", -12},
      {"osc3Fine", 0},       {"sec_osc3", 1}};
  return m;
}

// A knob that only changed its name (an automation on it keeps working): the new name, and the factor its values take.
// Bloom and Zap time were also their envelope's release: they move both.
struct Rename { const char* from; const char* to; double scale; };
inline const std::vector<Rename>& renamedV1() {
  static const std::vector<Rename> r = {{"swell", "env1Attack", 1}, {"decay", "env1Decay", 1}, {"sustain", "env1Sustain", 1},
                                        {"fade", "env1Release", 1}, {"bloom", "env2Decay", 4.6},
                                        {"filterAttack", "env2Attack", 1}, {"filterSustain", "env2Sustain", 1},
                                        {"bloom", "env2Release", 6.907755278982137},
                                        {"pitchTime", "env3Decay", 4.6}, {"pitchTime", "env3Release", 6.907755278982137},
                                        {"driftHz", "lfo3Hz", 1}, {"spread", "uniDetune", 1}, {"spread", "uniBlend", 1}};
  return r;
}

inline std::string layerPrefix(int layer) { return layer == 0 ? std::string() : "l" + std::to_string(layer + 1) + "_"; }

// "l3_bloom" -> layer 2, "bloom"; "tone" -> layer 0, "tone".
inline int splitLayer(const std::string& name, std::string* base) {
  if (name.size() > 3 && name[0] == 'l' && name[1] >= '2' && name[1] <= '4' && name[2] == '_') {
    *base = name.substr(3);
    return name[1] - '1';
  }
  *base = name;
  return 0;
}

// An automation or a track movement on a version-1 knob: the version-2 names and the factors for its values (one or
// two), or none when the knob has no knob now (its movement is a link: kept out, and said so).
inline std::vector<std::pair<std::string, double>> renameParamV1(const std::string& name) {
  std::string base;
  const int layer = splitLayer(name, &base);
  if (!removedV1().count(base)) return {{name, 1.0}};
  std::vector<std::pair<std::string, double>> out;
  for (const auto& r : renamedV1())
    if (base == r.from) out.push_back({layerPrefix(layer) + r.to, r.scale});
  return out;
}

struct Converted {
  json params = json::object();   // the version-2 knobs (every one the version-1 track had, renamed; new ones only when set)
  json synth;                     // the modulation ({"layers": [...]}), null when none
  std::vector<std::string> notes; // what could not be carried over
};

// params: the track's version-1 knobs (all, or only those set: the rest take their version-1 default).
inline Converted convertV1(const json& params) {
  Converted out;
  const json in = params.is_object() ? params : json::object();
  auto has = [&](const std::string& k) { return in.contains(k) && in[k].is_number(); };
  // Every knob that is still there keeps its value; the removed ones are read below.
  for (auto it = in.begin(); it != in.end(); ++it) {
    std::string base;
    splitLayer(it.key(), &base);
    if (!removedV1().count(base)) out.params[it.key()] = it.value();
  }
  // Which layers exist: layer 1 always, the others when added (their switch is there).
  bool layerUsed[4] = {true, false, false, false};
  struct Own3 { int layer; double level, semi, fine, wave, table, pos; };
  std::vector<Own3> own3;
  for (int l = 1; l < 4; ++l) layerUsed[l] = has(layerPrefix(l) + "on");
  const double driftHz0 = has("driftHz") ? in["driftHz"].get<double>() : removedV1().at("driftHz");
  json layers = json::array();
  bool any = false;
  for (int l = 0; l < 4; ++l) {
    const std::string pre = layerPrefix(l);
    auto v = [&](const char* k) { return has(pre + k) ? in[pre + k].get<double>() : removedV1().at(k); };
    auto clamp = [](double x, double lo, double hi) { return std::fmin(hi, std::fmax(lo, x)); };
    json links = json::array();
    json lfos = json::array();
    for (int k = 0; k < 4; ++k) lfos.push_back(json{{"shape", "sine"}, {"mode", "song"}, {"sync", true}});
    int lastLfo = -1, n = 0;
    auto link = [&](const char* src, const std::string& target, double amount, json extra = json::object()) {
      json j = {{"id", "m" + std::to_string(++n)}, {"source", src}, {"target", target}, {"amount", amount}};
      for (auto it = extra.begin(); it != extra.end(); ++it) j[it.key()] = it.value();
      links.push_back(j);
    };
    if (!layerUsed[l]) { layers.push_back(json::object()); continue; }
    // Spread -> Unison; Stack -> the Sub.
    const double sp = clamp(v("spread"), 0, 1);
    // (A version-2 knob given with them wins: a preset written with old words and a new knob.)
    auto put = [&](const char* k, double x) { if (!has(pre + k)) out.params[pre + k] = x; };
    put("unison", sp > 0.001 ? 7.0 : 1.0);
    put("uniDetune", sp);
    put("uniBlend", sp);
    const double st = std::round(clamp(v("stack"), -2, 1));
    put("subLevel", st != 0 ? 0.6 : 0.0);
    put("subOct", st != 0 ? st : -1.0);
    put("subShape", 0.0);
    // Osc 3.
    const double o3 = v("sec_osc3") >= 0.5 ? clamp(v("osc3"), 0, 1) : 0.0;
    if (o3 > 0) {
      const double o2 = (has(pre + "sec_osc2") && in[pre + "sec_osc2"].get<double>() < 0.5) ? 0.0 : clamp(has(pre + "osc2") ? in[pre + "osc2"].get<double>() : 0.0, 0, 1);
      const double norm = 1 / std::sqrt(1 + o2 * o2 + o3 * o3), norm0 = 1 / std::sqrt(1 + o2 * o2);
      const double level = has(pre + "level") ? in[pre + "level"].get<double>() : 0.7;
      const int semi = (int)std::lround(clamp(v("osc3Semi"), -24, 24));
      const double fine = clamp(v("osc3Fine"), -100, 100);
      out.params[pre + "level"] = level * norm / norm0;  // Osc 1 and 2 as loud as they were
      if ((semi == -12 || semi == -24) && st == 0 && v("osc3Table") < 0.5) {
        static const double kSubOf[5] = {1, 2, 4, 3, 5};  // Round, Warm, Hollow, Buzzy, Hard -> the Sub's shapes
        out.params[pre + "subLevel"] = clamp(o3 * norm0, 0, 1);
        out.params[pre + "subOct"] = semi / 12.0;
        out.params[pre + "subShape"] = kSubOf[(int)std::lround(clamp(v("osc3Wave"), 0, 4))];
        if (std::fabs(fine) >= 1) out.notes.push_back("Osc 3 became the Sub (its detune of " + std::to_string((int)std::lround(fine)) + " cents is gone)");
        else out.notes.push_back("Osc 3 became the Sub");
      } else {
        own3.push_back({l, level * o3 * norm, (double)semi, fine, v("osc3Wave"), v("osc3Table"), v("osc3Pos")});
      }
    }
    // ENV 1: the volume.
    out.params[pre + "env1Attack"] = clamp(v("swell"), 0.5, 8000);
    out.params[pre + "env1Decay"] = clamp(v("decay"), 1, 12000);
    out.params[pre + "env1Sustain"] = clamp(v("sustain"), 0, 1);
    out.params[pre + "env1Release"] = clamp(v("fade"), 10, 12000);
    // ENV 2: the filter's envelope. Bloom was its time constant (63 % of the way); ENV times reach -40 dB (decay) and
    // -60 dB (release), so they are Bloom x ln(100) and x ln(1000).
    const double bloom = clamp(v("bloom"), 20, 4000);
    const double fAtt = clamp(v("filterAttack"), 0, 4000);
    out.params[pre + "env2Attack"] = fAtt >= 1 ? fAtt : 0.0;
    out.params[pre + "env2Decay"] = bloom * 4.6;
    out.params[pre + "env2Sustain"] = clamp(v("filterSustain"), 0, 1);
    out.params[pre + "env2Release"] = bloom * 6.907755278982137;
    const double contour = clamp(v("contour"), -5, 5);
    if (contour != 0) link("env2", "tone", contour, {{"aux", "velocity"}, {"auxDepth", 0.4}});
    // ENV 3: Zap.
    const double pt = clamp(v("pitchTime"), 1, 2000), pe = clamp(v("pitchEnv"), -48, 48);
    out.params[pre + "env3Attack"] = 0.0;
    out.params[pre + "env3Decay"] = pt * 4.6;
    out.params[pre + "env3Sustain"] = 0.0;
    out.params[pre + "env3Release"] = pt * 6.907755278982137;
    if (pe != 0) link("env3", "pitch", pe);
    const double vs = clamp(v("velocity"), 0, 1);
    if (vs > 0) link("velocity", "volume", vs);
    const double kt = clamp(v("keyTrack"), 0, 1);
    if (kt > 0) link("note", "tone", kt * 5);
    // LFO 1 and 2.
    static const double kBeats[8] = {16, 8, 4, 2, 1, 0.5, 0.25, 0.125};
    static const char* kShape[5] = {"sine", "triangle", "saw", "square", "random"};
    for (int k = 0; k < 2; ++k) {
      const std::string p = k ? "lfo2" : "lfo1";
      const int dest = (int)std::lround(clamp(v((p + "Dest").c_str()), 0, 6));
      const double d = clamp(v((p + "Amount").c_str()), 0, 1);
      out.params[pre + p + "Rate"] = kBeats[(int)std::lround(clamp(v((p + "Rate").c_str()), 0, 7))];
      out.params[pre + p + "Smooth"] = 1.5;
      lfos[k]["shape"] = kShape[(int)std::lround(clamp(v((p + "Shape").c_str()), 0, 4))];
      if (dest == 0 || d <= 0) continue;
      lastLfo = std::max(lastLfo, k);
      const char* src = k ? "lfo2" : "lfo1";
      switch (dest) {
        case 1: link(src, "pitch", d * d * 12); break;  // squared: the first part of the knob was a vibrato
        case 2: link(src, "tone", d * 4); break;
        case 3: link(src, "volume", d, {{"unipolar", true}}); break;
        case 4:
          link(src, "wave", d * 2);
          for (const char* pos : {"osc1Pos", "osc2Pos"}) link(src, pos, d * 0.5);
          break;
        case 5: link(src, "pan", d); break;
        case 6: link(src, "fm", d * 5, {{"unipolar", true}}); break;
      }
    }
    // Drift: LFO 3, one slow sine for the whole layer at driftHz (the song locks it to whole movements in 4 bars, as
    // before), on Tone and the detune. Layers 2..4 moved at layer 1's speed.
    const double drift = clamp(v("drift"), 0, 1);
    lfos[2]["sync"] = false;
    out.params[pre + "lfo3Hz"] = clamp(driftHz0, 0.005, 8);
    out.params[pre + "lfo3Smooth"] = 0.0;
    if (drift > 0) {
      lastLfo = std::max(lastLfo, 2);
      link("lfo3", "tone", drift);
      link("lfo3", "detune", drift * 0.3);
    }
    while ((int)lfos.size() > lastLfo + 1) lfos.erase(lfos.size() - 1);
    json lj = json::object();
    if (!lfos.empty()) lj["lfos"] = lfos;
    if (!links.empty()) { lj["links"] = links; any = true; }
    if (!lj.empty()) any = true;
    layers.push_back(lj);
  }
  // An Osc 3 that is not a sub: a wave of its own, in the next free layer (a copy of its own layer, playing only Osc 3).
  int before = 0;
  for (bool u : layerUsed) before += u ? 1 : 0;
  int after = before;
  for (const auto& o : own3) {
    int free = -1;
    for (int f = 1; f < 4 && free < 0; ++f) if (!layerUsed[f]) free = f;
    if (free < 0) { out.notes.push_back("Osc 3 of wave " + std::to_string(o.layer + 1) + " was left out: all four waves are in use"); continue; }
    layerUsed[free] = true; ++after;
    const std::string from = layerPrefix(o.layer), to = layerPrefix(free);
    json copy = json::object();
    for (auto it = out.params.begin(); it != out.params.end(); ++it) {
      std::string base;
      if (splitLayer(it.key(), &base) == o.layer && base != "on") copy[to + base] = it.value();
    }
    for (auto it = copy.begin(); it != copy.end(); ++it) out.params[it.key()] = it.value();
    out.params[to + "on"] = 1.0;
    out.params[to + "wave"] = o.wave; out.params[to + "osc1Table"] = o.table; out.params[to + "osc1Pos"] = o.pos;
    out.params[to + "osc1Semi"] = o.semi;
    const double tune = has(from + "tune") ? in[from + "tune"].get<double>() : 0.0;
    out.params[to + "tune"] = std::fmin(100.0, std::fmax(-100.0, tune + o.fine));
    out.params[to + "osc2"] = 0.0; out.params[to + "subLevel"] = 0.0; out.params[to + "noise"] = 0.0; out.params[to + "grain"] = 0.0;
    out.params[to + "level"] = o.level;
    while ((int)layers.size() <= free) layers.push_back(json::object());
    layers[free] = o.layer < (int)layers.size() ? layers[o.layer] : json::object();
    if (!layers[free].empty()) any = true;
    out.notes.push_back("Osc 3 of wave " + std::to_string(o.layer + 1) + " became wave " + std::to_string(free + 1));
  }
  // More waves share the same headroom (1 / sqrt(waves)): each one's level goes up to sound as before.
  if (after > before) {
    const double up = std::sqrt((double)after / before);
    for (int l = 0; l < 4; ++l) {
      if (!layerUsed[l]) continue;
      const std::string k = layerPrefix(l) + "level";
      const double was = out.params.contains(k) ? out.params[k].get<double>() : 0.7;
      out.params[k] = std::fmin(1.5, was * up);
      if (was * up > 1.5) out.notes.push_back("wave " + std::to_string(l + 1) + " is a little quieter (its level was at the top)");
    }
  }
  while (!layers.empty() && layers.back().empty()) layers.erase(layers.size() - 1);
  if (any) out.synth = json{{"layers", layers}};
  return out;
}

}  // namespace core::wlegacy
