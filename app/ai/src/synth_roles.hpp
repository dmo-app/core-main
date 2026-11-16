// The default role map of Core's AI: the instrument roles the synth plays (synth.hpp) go to Vital, as the scan found
// it. Same file format as Settings > Roles (%APPDATA%\Core\roles.json: {version, roles: {id: {plugin, params}}}).
#pragma once
#include "json.hpp"

namespace core::ai {

// The scanned plugin that is the synth (VST3, name "Vital"), or null. `scanned`: the plugin list [{format, id, name,
// vendor, file, ...}].
nlohmann::json findSynthPlugin(const nlohmann::json& scanned);

// `roles` (roles.json, or null) with every synth role that has no plugin yet given to the synth. A role the user
// already chose is never changed. Without the synth in `scanned`, `roles` comes back as it was.
nlohmann::json withSynthRoles(nlohmann::json roles, const nlohmann::json& scanned);

// The scanned VST3 plugin with this name (case ignored), or null.
nlohmann::json findPluginNamed(const nlohmann::json& scanned, const std::string& name);
// The scanned VST3 plugin whose name holds `word` (case ignored), or null.
nlohmann::json findPluginWith(const nlohmann::json& scanned, const std::string& word);
// The whole default map of the AI: the synth roles (above) and each role of aiRoles() whose plugin the scan found
// (with "effect" for Airwindows Consolidated: which of its effects plays the role). A role already chosen is never
// changed. The macros' parameters are left to Settings > Roles to guess (they need the plugin's live parameters).
nlohmann::json withAiRoles(nlohmann::json roles, const nlohmann::json& scanned);

}  // namespace core::ai
