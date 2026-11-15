// Style FX ("core-fx-rack", version 1): a style's sound for a track in one step, made of Core's own presets: a
// Core Device preset, an R-Core or D-Core place, or an F-Core build-up. The files live in the
// app's racks folder (and the user's). Core has no VST (2026-11-08): the plugin slots, macros and rises the racks
// once had are gone, only the native steps play.
// Contract: docs/contracts/core-fx-rack.md.
#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

constexpr const char* kRackFormat = "core-fx-rack";
constexpr const char* kRackExtension = ".core-fx-rack.json";

// One step of a Style FX (Diogo, 2026-11-05: racks become Core's own presets).
struct NativeStep {
  std::string kind;    // "effect" | "rcore" | "dcore" | "instrument" | "call"
  std::string name;    // the effect ("space"), the instrument ("wave") or the call ("fcore:build-up")
  std::string preset;  // the preset's or the place's name ("" for a call)
};

struct RackDef {
  std::string id;           // "slambient/haunted": [a-z0-9-]+(/[a-z0-9-]+)?
  int version = 1;
  std::string name, role, kind = "effects", description, status;  // kind: "effects" | "instrument"
  std::vector<std::string> styles, tags;
  std::vector<NativeStep> native;
};

// Validates a definition; throws DocumentError("rack...").
RackDef parseRack(const json& value);
std::string styleLabel(const std::string& style);  // "darkpsycore" -> "Dark psycore"
const std::vector<std::string>& rackStyles();      // the styles in menu order

struct RackEntry { RackDef def; std::filesystem::path file; bool builtin = false; };
struct RackProblem { std::filesystem::path file; std::string message; };
// The app's racks folder (read only), then the user's: a user file with a built-in id is refused (never in
// place of a built-in one: a new id is needed), and so is a file with an id already read.
std::vector<RackEntry> loadRacks(const std::filesystem::path& builtinFolder, const std::filesystem::path& userFolder,
                                 std::vector<RackProblem>* problems = nullptr);

// The Style FX on the selected tracks, one edit: Core Device presets go on each track, an R-Core / D-Core place is
// shared by all of them, and "fcore:build-up" puts
// F-Core's build-up on them (in its first style, over the song's build-up sections on tracks that are not
// samplers). Steps this Core cannot do are left out and returned by name. Throws DocumentError when it has no
// steps, a named preset does not exist, or a track refuses a step.
// The effects a Style FX added carry its id under this key: the next Style FX on the track replaces them.
constexpr const char* kStyleFxKey = "styleFx";
std::vector<std::string> applyNativeRack(Document& doc, const std::vector<std::string>& trackIds, const RackDef& def);
// True when at least one step can be done here.
bool nativeRackReady(const RackDef& def);

}  // namespace core::doc
