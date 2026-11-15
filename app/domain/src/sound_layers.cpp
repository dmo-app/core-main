#include "sound_layers.hpp"
#include <algorithm>
#include <cmath>
#include "edit.hpp"

namespace core::doc {

const std::vector<WaveShape>& waveShapes() {
  static const std::vector<WaveShape> waves = {
      {"Round", 0, "Soft and pure, almost only the deep part: a smooth sub bass."},
      {"Warm", 1, "Round with a little more body: still soft, a bit fuller."},
      {"Hollow", 2, "Hollow and woody, like a clarinet: a plonk with character."},
      {"Buzzy", 3, "Bright and rich, the classic rolling psy bass: lots for the filter to open up."},
      {"Hard", 4, "Thin and nasal, cuts through: aggressive, hi-tech."}};
  return waves;
}

std::string waveShapeName(double wave) {
  const auto& w = waveShapes();
  wave = std::clamp(wave, 0.0, 4.0);
  const int a = std::min(3, (int)std::floor(wave));
  const double frac = wave - a;
  if (frac < 0.08) return w[a].name;
  if (frac > 0.92) return w[a + 1].name;
  return std::string(w[a].name) + " \xE2\x86\x92 " + w[a + 1].name;  // →
}

void setMoodKnobs(Document& doc, const std::string& trackId, const std::string& moodKey, const std::map<std::string, double>& knobs) {
  // Diogo, 2026-11-06: "everything I change stacks on what was already changed". A mood's knobs are a layer: the
  // previous mood's knobs go back to what they were before it (unless turned since), then the new mood's are set.
  const std::string key = moodKey + "Knobs";
  Track& t = trackById(doc, trackId);
  if (t.extra.is_object() && t.extra.contains(key) && t.extra[key].is_object()) {
    const json before = t.extra[key];
    for (const auto& [param, was] : before.items()) {
      if (!was.is_object()) continue;
      const auto now = t.instrument.params.find(param);
      const double set = was.value("set", 0.0);
      if (now == t.instrument.params.end() || std::fabs(now->second - set) > 1e-9) continue;  // turned by hand since: it stays
      if (was.contains("was") && was["was"].is_number()) t.instrument.params[param] = was["was"].get<double>();
      else t.instrument.params.erase(param);  // it was the default
    }
    t.extra.erase(key);
  }
  if (knobs.empty()) return;
  json record = json::object();
  for (const auto& [k, v] : knobs) {
    const auto now = trackById(doc, trackId).instrument.params.find(k);
    json r = json::object();
    if (now != trackById(doc, trackId).instrument.params.end()) r["was"] = now->second;
    setInstrumentParam(doc, trackId, k, v);
    r["set"] = trackById(doc, trackId).instrument.params[k];
    record[k] = r;
  }
  Track& tt = trackById(doc, trackId);
  if (!tt.extra.is_object()) tt.extra = json::object();
  tt.extra[key] = record;
}

}  // namespace core::doc
