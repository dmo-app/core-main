// What a track's modulators are doing right now, for drawing (Diogo 2026-11-07: "quero ver o efeito dos filtros e
// da modulação a funcionar, uma referência visual"). The same formulas as the engine (Graph::modValue): an LFO or a
// step sequence from the song position alone; an envelope from the last note of its track (or of its trigger track)
// before the playhead. Only for the picture: the sound is the engine's.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include "document.hpp"
#include "edit.hpp"
#include "fcore.hpp"

namespace core::app::live {

// The engine's LFO shapes, -1..1 ("random": held per cycle; the engine's seed comes from the track, so this shows
// the kind of steps, not the exact values).
inline double lfoAt(const std::string& shape, double x) {
  const double cycle = std::floor(x), f = x - cycle;
  if (shape == "sine") return std::sin(2 * 3.141592653589793 * f);
  if (shape == "triangle") return 1 - 4 * std::fabs(f - 0.5);
  if (shape == "square") return f < 0.5 ? 1.0 : -1.0;
  if (shape == "saw") return 1 - 2 * f;
  if (shape == "ramp") return 2 * f - 1;
  uint32_t h = 0x9e3779b9u ^ (uint32_t)(int64_t)cycle * 2654435761u;
  h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
  return h / 2147483647.5 - 1.0;
}
inline double envAt(double attackMs, double decayMs, double ms) {
  if (ms < 0) return 0;
  if (ms < attackMs) return ms / std::max(1e-6, attackMs);
  return std::pow(0.001, (ms - attackMs) / std::max(1.0, decayMs));
}

// A modulator drawn in the Modulation tabs (FCore's own and the Mixer's ducking are not).
inline bool shown(const nlohmann::json& m) {
  if (doc::isFCoreModulator(m) || m.value("id", "") == "duck") return false;
  const std::string s = m.value("shape", "sine");
  return s == "envelope" || s == "steps" || s == "sine" || s == "triangle" || s == "square" || s == "saw" || s == "ramp" || s == "random";
}

// The last note of a track that started at or before `tick` (-1: none yet).
inline double lastNoteTick(const doc::Track& t, double tick) {
  double best = -1;
  for (const auto& c : t.clips) {
    if (tick < (double)c.startTick || tick >= (double)(c.startTick + c.lengthTicks)) continue;
    const doc::Pattern* p = t.findPattern(c.patternId);
    if (!p || p->lengthTicks <= 0 || p->notes.empty()) continue;
    const double pos = tick - (double)c.startTick + (double)c.offsetTicks;
    const double loopStart = std::floor(pos / (double)p->lengthTicks) * (double)p->lengthTicks;
    const double local = pos - loopStart;
    constexpr double kNone = -1e18;
    double at = kNone;
    for (const auto& n : p->notes) if ((double)n.tick <= local) at = std::max(at, (double)n.tick);
    if (at == kNone && loopStart > 0)  // none yet in this round of the pattern: the last one of the round before
      for (const auto& n : p->notes) at = std::max(at, (double)n.tick - (double)p->lengthTicks);
    if (at == kNone) continue;
    const double song = (double)c.startTick + (loopStart + at) - (double)c.offsetTicks;
    if (song <= tick && song >= (double)c.startTick) best = std::max(best, song);
  }
  return best;
}

struct Now {
  double w = 0;      // the modulator's output now: -1..1 (LFO, steps) or 0..1 (envelope)
  double where = 0;  // 0..1 across its picture: the place in the cycle (two cycles shown), or in the envelope's window
  bool active = true;  // an envelope with no note yet is off
};

// The modulator's output at `tick` of the song.
inline Now at(const doc::Document& d, const doc::Track& t, const nlohmann::json& m, double tick) {
  Now n;
  const std::string shape = m.value("shape", "sine");
  const double beats = tick / doc::kPpq;
  if (shape == "envelope") {
    const std::string source = m.value("source", "");
    const doc::Track* from = source.empty() ? &t : d.findTrack(source);
    const double note = from ? lastNoteTick(*from, tick) : -1;
    const double a = m.value("attackMs", 0.0), dcy = m.value("decayMs", 150.0), window = std::max(50.0, (a + dcy) * 1.15);
    if (note < 0) { n.active = false; return n; }
    const double ms = (tick - note) / doc::kPpq * 60000.0 / std::max(1.0, doc::bpmAt(d, (int64_t)note));
    n.w = envAt(a, dcy, ms);
    n.where = std::clamp(ms / window, 0.0, 1.0);
    return n;
  }
  const double rate = std::max(1e-6, m.value("rateBeats", 1.0));
  if (shape == "steps") {
    std::vector<double> v;
    if (m.contains("steps") && m["steps"].is_array()) for (const auto& s : m["steps"]) if (s.is_number()) v.push_back(s.get<double>());
    if (v.empty()) return n;
    const double step = beats / rate;
    const int64_t i = (int64_t)std::floor(step);
    const size_t k = (size_t)(((i % (int64_t)v.size()) + (int64_t)v.size()) % (int64_t)v.size());
    n.w = v[k];
    n.where = (k + (step - std::floor(step))) / (double)v.size();
    return n;
  }
  const double x = beats / rate + m.value("phase", 0.0);
  n.w = lfoAt(shape, x);
  const double inTwo = beats / rate - 2 * std::floor(beats / rate / 2);  // the picture shows two cycles
  n.where = inTwo / 2;
  return n;
}

// A control's value now: its own value moved by every shown modulator on it, one after the other, like the engine.
inline double value(const doc::Document& d, const doc::Track& t, const doc::InstrumentParamInfo& p, double tick) {
  double v = doc::instrumentParamValue(t, p);
  const bool octaves = std::string(p.unit) == "Hz";
  for (const auto& m : t.modulators) {
    if (!shown(m) || m.value("param", "") != std::string(p.name)) continue;
    const Now n = at(d, t, m, tick);
    if (!n.active) continue;
    const double depth = m.value("depth", 0.0);
    v = std::clamp(octaves ? v * std::exp2(depth * n.w) : v + depth * n.w, p.min, p.max);
  }
  return v;
}

}  // namespace core::app::live
