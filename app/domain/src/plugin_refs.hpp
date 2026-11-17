#pragma once
// Plugin references in a song that point at another computer's files (templates, songs from another
// PC). The engine loads a plugin from the exact file and id written in the song; this finds the same
// plugin in this computer's scan and points the reference at it. Never a different plugin: the match
// is the same format, name and plugin uid, only the file (and the path hash in the id) may differ.
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

// The parts of a JUCE plugin id ("VST3-<name>-<path hash>-<uid>") that do not depend on where the file is.
struct PluginKey {
  std::string format, name, uid;
  bool operator==(const PluginKey&) const = default;
};
PluginKey pluginKey(const json& plugin);  // empty fields when the id does not have that shape

// For every plugin track and plugin effect (tracks and master) whose {id, file} is not in `scanned`
// (this computer's scan: {format, id, name, file, ...}): when exactly one scanned plugin has the same
// key, its id and file are written into the reference (state, preset and the rest stay). Returns the
// number of references changed; the ones with no match are left as they are (the UI shows them missing).
int relinkPlugins(Document& d, const std::vector<json>& scanned);

// The scanned plugins grouped by their maker, for the Plugins panel's cards (Diogo 2026-11-17: "um card que ao
// carregar abria os plugins, KiloHearts, Kick3, GlitchMachines... e depois ao carregar num deles as opções que cada
// tem"). One group per vendor, alphabetical (case ignored); a plugin with no vendor goes to "Other". Every plugin in
// `plugins` ({format, id, name, vendor, instrument, ...}) lands in exactly one group, instruments and effects alike.
struct PluginVendorGroup {
  std::string vendor;
  std::vector<json> plugins;
};
std::vector<PluginVendorGroup> pluginsByVendor(const json& plugins);

}  // namespace core::doc
