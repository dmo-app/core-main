// The Vital presets of Core's AI (Diogo, 2026-11-16): presets/vital/presets.json lists Core's own presets (made for
// dark psycore, hitech and slambient) and a user folder holds Diogo's variants. Every preset has the same 4 macros
// (TOM, SUJO, MOVIMENTO, COMPRIMENTO), so the AI picks a preset by name and only moves 4 numbers.
//
// A .vital file is Vital's preset JSON, and Vital's own state is that JSON as UTF-8 text ending with a NUL
// (getStateInformation writes it with MemoryOutputStream::writeString); that is its VST3 component state. The song
// keeps a plugin's state as JUCE's VST3 host gives it (the engine calls setStateInformation with it): base64 of
// copyXmlToBinary(<VST3PluginState><IComponent>JUCE base64 of the component state</IComponent></VST3PluginState>).
// So putting a preset on a track is writing that wrapping of the preset's JSON as the plugin's "state".
// No JUCE here: the AI, the UI and the tests use these functions.
#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

inline constexpr std::array<const char*, 4> kVitalMacros = {"TOM", "SUJO", "MOVIMENTO", "COMPRIMENTO"};
using VitalMacros = std::array<double, 4>;  // 0..1 each; 0.5 = the sound as designed
inline constexpr VitalMacros kVitalMacrosRest = {0.5, 0.5, 0.5, 0.5};

struct VitalPreset {
  std::string id, name, style, role, description, origin;  // origin: "core" or "user"
  std::vector<std::string> tags;
  int bpm = 0;
  std::filesystem::path file;
};

// Core's index (presets.json in `coreDir`) plus every .vital in `userDir` (origin "user"; style, role and tags read
// from the preset's comments, as saveVitalVariant writes them). A missing user folder is no error. Throws
// DocumentError when the index is missing or malformed.
std::vector<VitalPreset> loadVitalLibrary(const std::filesystem::path& coreDir, const std::filesystem::path& userDir);
// The presets of a style and a role ("" = any), Core's first, then the user's, by name.
std::vector<VitalPreset> findVitalPresets(const std::vector<VitalPreset>& library, const std::string& style,
                                          const std::string& role);
const VitalPreset* findVitalPreset(const std::vector<VitalPreset>& library, const std::string& idOrName);

// The plugin state (base64) of a preset with its macros set; throws DocumentError on a file that is not a Vital
// preset or a macro outside 0..1.
std::string vitalState(const std::string& presetJson, const VitalMacros& macros = kVitalMacrosRest);
// The preset JSON back from a state (as vitalState writes it, or as Vital itself saved it, with JUCE's private data
// after it); throws DocumentError when it is not one.
std::string vitalPresetFromState(const std::string& state);
// The VST3 component state inside a song's plugin state (JUCE host format); throws DocumentError.
std::string vitalComponentState(const std::string& state);

// Is this track's instrument Vital?
bool isVitalTrack(const Document& doc, const std::string& trackId);
// Puts the preset on the track's Vital (the instrument must be Vital: DocumentError otherwise). Records the preset id
// and the macros on the plugin reference ("vitalPreset": {id, macros}) so the AI can read back what it chose.
void setVitalPreset(Document& doc, const std::string& trackId, const VitalPreset& preset,
                    const VitalMacros& macros = kVitalMacrosRest);
// Saves the track's Vital as it is now as a user variant "<base> - <label>" in `userDir` (created if needed) with the
// base preset's style, role and tags; never overwrites (adds " 2", " 3"...). Returns the file.
std::filesystem::path saveVitalVariant(const Document& doc, const std::string& trackId, const VitalPreset& base,
                                       const std::string& label, const std::filesystem::path& userDir);

std::string base64Encode(const std::string& bytes);
std::string base64Decode(const std::string& text);  // throws DocumentError on bad input
std::string juceBase64Encode(const std::string& bytes);  // JUCE's MemoryBlock::toBase64Encoding ("<size>.<chars>")
std::string juceBase64Decode(const std::string& text);   // throws DocumentError on bad input

}  // namespace core::doc
