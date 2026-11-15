#include "glitch.hpp"
#include <algorithm>

namespace core::doc {

namespace {
Track& glitchTrack(Document& doc, const std::string& trackId) {
  Track* t = nullptr;
  for (auto& x : doc.tracks) if (x.id == trackId) t = &x;
  if (!t) throw DocumentError("tracks", "no track '" + trackId + "'");
  if (!isGlitch(*t)) throw DocumentError("instrument", "the track '" + t->name + "' is not a Core Glitch");
  return *t;
}
}  // namespace

bool isGlitch(const Track& track) { return track.instrument.kind == "builtin" && track.instrument.name == kGlitchInstrument; }

spath::Patch glitchPatch(const Track& track) {
  if (!isGlitch(track)) throw DocumentError("instrument", "the track '" + track.name + "' is not a Core Glitch");
  try { return spath::parsePatch(track.instrument.synth); }
  catch (const std::exception& e) { throw DocumentError("instrument.synth", e.what()); }
}

void setGlitchPatch(Document& doc, const std::string& trackId, const spath::Patch& patch) {
  Track& t = glitchTrack(doc, trackId);
  // Through JSON and back: the same checks and clamps as a song read from a file.
  try { t.instrument.synth = spath::patchToJson(spath::parsePatch(spath::patchToJson(patch))); }
  catch (const std::exception& e) { throw DocumentError("instrument.synth", e.what()); }
}

void setGlitchSound(Document& doc, const std::string& trackId, int sampler, const SampleRef* zone, double seconds, int slices) {
  Track& t = glitchTrack(doc, trackId);
  if (sampler < 0 || sampler >= kGlitchSamplers) throw DocumentError("instrument.samples", "no such sampler");
  auto& zones = t.instrument.samples;
  zones.erase(std::remove_if(zones.begin(), zones.end(), [&](const SampleRef& z) { return std::clamp(z.loKey, 0, 3) == sampler; }), zones.end());
  if (!zone) return;
  SampleRef z = *zone;
  z.rootKey = 60; z.loKey = sampler; z.hiKey = sampler;  // the lowest key names the sampler (engine graph.cpp)
  zones.push_back(z);
  std::sort(zones.begin(), zones.end(), [](const SampleRef& a, const SampleRef& b) { return a.loKey < b.loKey; });
  spath::Patch patch = glitchPatch(t);
  spath::Layer& L = patch.layers[sampler];
  if (!L.slices.empty() || seconds <= 0.01) return;
  const int n = std::clamp(slices, 1, spath::kMaxSlices);
  if (n == 6) {
    const spath::Layer example = spath::examplePatch(seconds).layers[0];
    L.slices = example.slices;
    L.patterns[0] = example.patterns[0];
  } else {
    L.slices = spath::evenSlices(seconds, n);
    spath::Pattern& p = L.patterns[0];
    p.nodes.clear();
    for (int i = 0; i < n; ++i) {
      spath::Node node;
      node.id = i + 1; node.slice = i;
      node.x = 95 + 200.0 * (i % 6); node.y = (i % 2 ? 165 : 52) + 230.0 * (i / 6);
      p.nodes.push_back(node);
    }
    p.nextId = n + 1;
    spath::applyShape(p, "Loop");
  }
  L.used[0] = true;
  L.pattern = 0;
  setGlitchPatch(doc, trackId, patch);
}

const SampleRef* glitchSound(const Track& track, int sampler) {
  for (const auto& z : track.instrument.samples) if (std::clamp(z.loKey, 0, 3) == sampler) return &z;
  return nullptr;
}

json freshGlitchSynth() { return spath::patchToJson(spath::Patch{}); }

}  // namespace core::doc
