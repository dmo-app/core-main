// A drum kit for sfizz made from the user's sample library (Diogo, 2026-11-16: "os dois", with the FreePats kit):
// the library's one-shots by role (kick, snare, clap, hats...) on the General MIDI drum notes, a few per note in
// turn (round robin), each at the level the sampler would give it. The .sfz points at the library's own files
// (default_path): nothing is copied, nothing in the library is written.
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "samples.hpp"

namespace core::doc {

struct SfzKitNote { const char* role; int note; const char* name; };
const std::vector<SfzKitNote>& sfzKitNotes();  // kick 36, perc 37, snare 38, clap 39, hat 42, tom 45, openhat 46, crash 49

struct SfzKitOptions {
  int perNote = 4;      // samples per note at most, played in turn
  std::string style;    // "darkpsycore", "hitech"...: samples tagged with it first ("" = any)
};
struct SfzKit {
  std::string text;                                     // the .sfz
  std::map<int, std::vector<std::string>> samples;      // note -> the library paths it plays
  int regions = 0;
};
// Throws DocumentError when the library has no one-shot of any of the roles.
SfzKit makeSfzDrumKit(const SampleIndex& index, const SfzKitOptions& options = {});
// Written atomically (temp file + rename).
void writeSfzDrumKit(const SfzKit& kit, const std::filesystem::path& file);

}  // namespace core::doc
