// Core's own plugins, described once (Diogo, 2026-11-06: "the common plug"). Every Core plugin says here who it is:
// its ids, its names, its layer, what kind of thing it is, which window opens it and which other Core plugins it
// may call. The function list for AICORE (control/calls), the "open" aliases and the layer check read this table,
// so a new plugin is one entry here plus its own code, not a hand-made branch in every file.
//
// The sound itself stays in the engine (engine/src/dsp/registry: NativeRegistry); this table is about the plugin as
// Diogo and AICORE see it. Layers (docs, AICORE CORE-PLUGIN-LAYERS.md): 0 pieces (sources, effects), 1 gestures
// (F-Core, P-Core, R-Core, D-Core, STUTTER, VOICE STUTTER), 2 instruments (W-Core), 3 the song (S-Core).
// A plugin only calls plugins of a lower layer.
#pragma once
#include <string>
#include <vector>
#include "json.hpp"

namespace core::doc {

using json = nlohmann::json;

enum class CorePluginKind {
  Instrument,  // replaces the track's instrument (W-Core)
  Effects,     // puts Core's own effects on a track or a shared bus (F-Core, R-Core, D-Core)
  Notes,       // writes notes (P-Core, STUTTER)
  Clips,       // writes audio clips (VOICE STUTTER)
  Song,        // arranges the whole song (S-Core)
};

struct CorePluginInfo {
  std::string id;         // the plugin's id, also its call id for AICORE ("wcore", "fcore", "score")
  std::string shortName;  // "W-Core"
  std::string word;       // "Wave": what the letter stands for
  std::string fullName;   // as the interface shows it: "Wave Core (W-Core)"
  int layer = 1;
  CorePluginKind kind = CorePluginKind::Effects;
  std::string opens;      // the window that shows it: "instrument" | "effects" | "patterns" | "score"
  std::vector<std::string> aliases;  // other names AICORE or a person may use ("w-core", "bass", "stage")
  std::vector<std::string> calls;    // the Core plugins it may call (all of a lower layer)
  std::string about;      // one line, plain words
  std::string instrument; // Instrument kind: the engine instrument it plays ("wave", "kick"; NativeRegistry id)
  std::string where;      // where it opens when it is not a window of its own ("" = the Plugins menu opens it)
  // What it keeps in the song (extra.<stateKey> on a track, a note or a clip), with this version written as "version".
  // An older state is read as it is (a missing "version" is 1); a newer one, from a later Core, is left alone.
  std::string stateKey;   // "" = nothing beyond ordinary notes, effects and instrument knobs
  int stateVersion = 0;
};

// Every Core plugin, the song first, then by layer.
const std::vector<CorePluginInfo>& corePlugins();
// By id, short name or alias, any case ("W-Core", "wcore", "wave"); null when none.
const CorePluginInfo* findCorePlugin(const std::string& idOrName);
// "W-Core · Synth": the name AICORE's function list uses.
std::string corePluginCallName(const CorePluginInfo& p);
// The layer rule: one line per plugin that calls an unknown plugin, itself, or one of its own layer or above.
// Empty when the table is right (domain_tests checks it).
std::vector<std::string> corePluginLayerProblems();

// The common versioned state: `state` with the plugin's "version" set; the version a stored state was written with
// (1 when it says none); whether this Core can read it (not from a later Core).
json corePluginState(const std::string& pluginId, json state);
int corePluginStateVersion(const json& state);
bool corePluginStateReadable(const std::string& pluginId, const json& state);

}  // namespace core::doc
