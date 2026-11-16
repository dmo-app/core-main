// Export for the Bitwig (Diogo, 2026-11-16: "exportar midi, ler no bitwig como foi exportado, plugins etc").
// A folder with the song as a .mid (one MIDI track per Core track that plays notes, rolls expanded, the tempo) and,
// for every plugin the song uses, a .vstpreset with its sound as it is now (the engine writes the preset; this file
// says which ones and their names), plus a text file that says which preset goes on which track.
// In the Bitwig: drag the .mid in (one track per MIDI track, in the same order), then drop each track's .vstpreset
// on it: the same plugin opens with the same sound.
//
// No JUCE: tested on its own (tests/bitwig_tests.cpp).
#pragma once
#include <string>
#include <vector>
#include "document.hpp"
#include "midi.hpp"

namespace core::doc {

struct BitwigPreset {
  std::string key;   // the engine's plugin key ("track:<id>" or "fx:<track or master>:<effect id>")
  std::string file;  // the .vstpreset's file name in the folder
  std::string role;  // "instrument" or "effect"
  json plugin;       // {format, id, name, vendor, file...} as the document has it
};
struct BitwigTrack {
  std::string trackId, name;
  int midiTrack = 0;  // 1, 2...: its place among the .mid's note tracks (the Bitwig's track order); 0 = not in it
  std::vector<BitwigPreset> presets;
};
struct BitwigPlan {
  std::string midiFile = "song.mid";
  std::vector<BitwigTrack> tracks;   // the tracks with notes or plugins, in the song's order
  std::vector<BitwigPreset> master;  // the master's plugin effects
};

// What goes in the folder. Bypassed effects are listed too (the preset keeps their sound; they come in on).
BitwigPlan bitwigPlan(const Document& doc, const MidiExportOptions& options = {});
// The text file for the folder: tempo, then track by track the MIDI track, the plugins and their presets.
// `missing`: keys whose preset the engine could not give (not loaded, not VST3): said so in the text.
std::string bitwigReadme(const Document& doc, const BitwigPlan& plan, const std::vector<std::string>& missing = {});
// A file name part with nothing Windows refuses (\ / : * ? " < > | and control characters), at most 60 bytes,
// cut on a whole UTF-8 character.
std::string safeFilePart(const std::string& text);

}  // namespace core::doc
