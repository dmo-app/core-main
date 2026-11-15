#include "core_plugins.hpp"
#include <algorithm>
#include <cctype>

namespace core::doc {

namespace {

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

}  // namespace

const std::vector<CorePluginInfo>& corePlugins() {
  using K = CorePluginKind;
  static const std::vector<CorePluginInfo> list = {
      {"score", "S-Core", "Stage", "Stage", 3, K::Song, "score", {"s-core", "stage core (s-core)", "stage", "stages", "scores"},
       {"fcore", "pcore"}, "builds each part of the song (intro, build-up, drop...) and says which tracks play there"},
      {"wcore", "W-Core", "Wave", "Synth", 2, K::Instrument, "instrument", {"w-core", "wave", "wave core", "bass", "b-core", "bcore", "elastic"},
       {}, "Core's own synth for bass, lead, pad and drone, with Grain and moods made of Core's effects", "wave"},
      {"coreglitch", "Core Glitch", "Sample Path", "Core Glitch", 2, K::Instrument, "instrument",
       {"core glitch", "sample path", "samplepath", "glitch machine"},
       {}, "cuts a sound into parts and walks them as a path of nodes, in time with the song", "samplepath"},
      {"pcore", "P-Core", "Patterns", "Patterns", 1, K::Notes, "patterns", {"p-core", "pattern core (p-core)", "patterns", "pattern core"},
       {}, "writes rhythms and notes in layers: part, shape, grid, changes"},
      {"fcore", "F-Core", "FX", "FX", 1, K::Effects, "effects", {"f-core", "fx core (f-core)", "fx core"},
       {}, "effects by intent (build-up, intro, impact...) with four knobs", "", "", "fcore", 1},
      {"rcore", "R-Core", "Reverb", "Reverb", 1, K::Effects, "effects", {"r-core", "reverb core (r-core)", "reverb core"},
       {}, "the selected tracks share one reverb on a bus that follows the sections"},
      {"dcore", "D-Core", "Delay", "Delay", 1, K::Effects, "effects", {"d-core", "delay core (d-core)", "delay core"},
       {}, "the selected tracks share one echo on a bus that follows the sections"},
      {"stutter", "STUTTER", "", "STUTTER", 1, K::Notes, "patterns", {},
       {}, "turns notes into repeats inside each note's length", "", "the piano roll: Edit > STUTTER", "stutter", 1},
      {"voicestutter", "VOICE STUTTER", "", "VOICE STUTTER", 1, K::Clips, "", {"voice stutter", "voice-stutter"},
       {}, "turns a recorded word into fragments at a rhythm, then the whole word", "", "the clip editor: More > VOICE STUTTER", "voiceStutter", 1},
  };
  return list;
}

const CorePluginInfo* findCorePlugin(const std::string& idOrName) {
  const std::string want = lower(idOrName);
  if (want.empty()) return nullptr;
  for (const auto& p : corePlugins()) {
    if (p.id == want || lower(p.shortName) == want || lower(p.fullName) == want) return &p;
    for (const auto& a : p.aliases)
      if (a == want) return &p;
  }
  return nullptr;
}

std::string corePluginCallName(const CorePluginInfo& p) {
  return p.word.empty() ? p.shortName : p.shortName + " · " + p.word;
}

std::vector<std::string> corePluginLayerProblems() {
  std::vector<std::string> out;
  for (const auto& p : corePlugins()) {
    if (p.layer < 0 || p.layer > 3) out.push_back(p.id + ": layer " + std::to_string(p.layer) + " is not 0..3");
    for (const auto& c : p.calls) {
      const CorePluginInfo* q = findCorePlugin(c);
      if (!q) out.push_back(p.id + " calls \"" + c + "\", which is not a Core plugin");
      else if (q == &p) out.push_back(p.id + " calls itself");
      else if (q->layer >= p.layer)
        out.push_back(p.id + " (layer " + std::to_string(p.layer) + ") calls " + q->id + " (layer " + std::to_string(q->layer) +
                      "): a plugin only calls lower layers");
    }
  }
  for (size_t i = 0; i < corePlugins().size(); ++i)
    for (size_t j = i + 1; j < corePlugins().size(); ++j)
      if (corePlugins()[i].id == corePlugins()[j].id) out.push_back(corePlugins()[i].id + " is listed twice");
  return out;
}

json corePluginState(const std::string& pluginId, json state) {
  const CorePluginInfo* p = findCorePlugin(pluginId);
  if (p && p->stateVersion > 0 && state.is_object()) state["version"] = p->stateVersion;
  return state;
}

int corePluginStateVersion(const json& state) {
  return state.is_object() && state.contains("version") && state["version"].is_number_integer() ? state["version"].get<int>() : 1;
}

bool corePluginStateReadable(const std::string& pluginId, const json& state) {
  const CorePluginInfo* p = findCorePlugin(pluginId);
  return state.is_object() && (!p || corePluginStateVersion(state) <= std::max(1, p->stateVersion));
}

}  // namespace core::doc
