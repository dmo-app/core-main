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

}  // namespace core::ai
