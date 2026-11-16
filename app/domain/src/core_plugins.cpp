#include "core_plugins.hpp"

#include <cctype>
#include <set>
#include "roles.hpp"

namespace core::doc {

namespace {
std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
}  // namespace

const std::vector<CorePluginInfo>& corePlugins() {
  static const std::vector<CorePluginInfo> list = {
      {"pcore", "P-Core", "Patterns", "writes rhythms and notes in layers: part, shape (KBB...), scale, grid, changes", {}, "window",
       {"patterns", "pattern core", "pattern maker", "channel rack"}},
      {"wcore", "W-Core", "Instrument", "the track plays the plugin of a role: bass, lead, pad, pluck or drums", {"bass", "lead", "pad", "pluck", "drums"},
       "tool", {"instrument", "synth", "role instrument"}},
      {"fcore", "F-Core", "FX", "effects by intent over a section: build-up, suck, intro, outro, impact, break", {"filter", "reverb", "delay", "drive"},
       "tool", {"fx", "fx core", "intent"}},
      {"rcore", "R-Core", "Reverb", "the selected tracks share one reverb on a bus that follows the sections", {"reverb"}, "tool",
       {"reverb core", "shared reverb", "space"}},
      {"dcore", "D-Core", "Delay", "the selected tracks share one echo on a bus that follows the sections", {"delay"}, "tool",
       {"delay core", "shared delay", "echo"}},
      {"suck", "SUCK", "Into the drop", "the sound is sucked into the drop: the reverb opens, the filter closes, back on the drop",
       {"reverb", "filter"}, "tool", {"suck into the drop"}},
      {"pum", "PUM", "Sidechain", "the volume dips at every kick and comes back", {"sidechain"}, "tool", {"pump", "sidechain pump"}},
      {"buildup", "Build-up", "Up to the drop", "filter opens, reverb and delay grow, drive pushes, back on the drop",
       {"filter", "reverb", "delay", "drive"}, "tool", {"build up", "buildup fx"}},
      {"roll", "ROLL", "Roll", "a roll that speeds up into the drop, in a new pattern on the track", {}, "tool", {"snare roll", "kick roll"}},
  };
  return list;
}

const CorePluginInfo* findCorePlugin(const std::string& idOrName) {
  const std::string want = lower(idOrName);
  if (want.empty()) return nullptr;
  for (const auto& p : corePlugins()) {
    if (p.id == want || lower(p.shortName) == want) return &p;
    for (const auto& a : p.aliases) if (a == want) return &p;
  }
  return nullptr;
}

std::vector<std::string> corePluginListProblems() {
  std::vector<std::string> out;
  std::set<std::string> names;
  for (const auto& p : corePlugins()) {
    if (p.id.empty() || p.shortName.empty() || p.word.empty() || p.about.empty()) out.push_back(p.id + ": an empty field");
    if (p.opens != "window" && p.opens != "tool") out.push_back(p.id + ": opens is window or tool");
    for (const auto& n : std::vector<std::string>{p.id, lower(p.shortName)})
      if (!names.insert(n).second && n != p.id) out.push_back(p.id + ": '" + n + "' is used twice");
    for (const auto& a : p.aliases) {
      if (a != lower(a)) out.push_back(p.id + ": alias '" + a + "' is not lower case");
      if (!names.insert(a).second) out.push_back(p.id + ": alias '" + a + "' is used twice");
    }
    for (const auto& r : p.roles) if (!findRole(r)) out.push_back(p.id + ": unknown role '" + r + "'");
  }
  return out;
}

}  // namespace core::doc
