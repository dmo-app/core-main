// MIDI files (.mid, Standard MIDI File) in and out (Diogo, 2026-11-16: the Bitwig stays an option, by .mid).
//
// Export: what the song plays, note for note, as the engine plays it: every clip and every repeat of a looping
// pattern, the notes that start inside a clip, and each roll expanded into its hits (spread evenly over the note,
// each hit 85% of its step, the velocity going from the roll's first to its last). Format 1, 960 ticks per quarter
// (Core's own), one MIDI track per Core track that plays notes, named after it, and a first track with the
// tempo (steps, and glides in 1/16 steps) and 4/4. The song's end cuts what still sounds there.
//
// Import: each MIDI track (or each channel of a format 0 file) with notes becomes a Core track (no instrument yet:
// it waits for a plugin) whose notes are cut into patterns of `barsPerPattern` bars; the same bars twice are one
// pattern played by two clips. Times are scaled to 960 per quarter. The tempo can be taken from the file.
//
// No JUCE: tested on its own (tests/midi_tests.cpp).
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

// ---- the file itself
struct MidiNote { int64_t tick = 0, length = 0; int pitch = 60, velocity = 100, channel = 0; };
struct MidiTempo { int64_t tick = 0; double bpm = 120; };
struct MidiTrack { std::string name; std::vector<MidiNote> notes; };
struct MidiFile {
  int ppq = 960;
  std::vector<MidiTrack> tracks;
  std::vector<MidiTempo> tempos;  // from every track, in time order
};
// Only the tracks with notes are kept. Throws DocumentError for a file that is not a MIDI file or is cut short.
// SMPTE time is refused.
MidiFile parseMidi(const std::vector<uint8_t>& bytes);
std::vector<uint8_t> writeMidi(const MidiFile& file);  // format 1; tempos go in the first track

// ---- the song
struct MidiExportOptions {
  bool mutedTracks = false;            // muted tracks too
  std::vector<std::string> onlyTracks; // empty: every track that plays notes
};
MidiFile songToMidi(const Document& doc, const MidiExportOptions& options = {});
std::vector<uint8_t> exportMidi(const Document& doc, const MidiExportOptions& options = {});
void saveMidiFile(const std::vector<uint8_t>& bytes, const std::filesystem::path& file);
std::vector<uint8_t> readMidiFile(const std::filesystem::path& file);  // throws DocumentError

struct MidiImportOptions {
  int64_t atTick = 0;          // where the file's start lands in the song
  int barsPerPattern = 4;      // 1..64
  bool useTempo = false;       // the song takes the file's first tempo (its later changes are not taken)
  std::string namePrefix;      // before each track's name ("" = the MIDI track's own name, or "MIDI n")
};
struct MidiImportResult {
  std::vector<std::string> trackIds;  // the tracks added, in order
  int notes = 0, patterns = 0, clips = 0;
  bool songLengthened = false;
};
// Adds the tracks; the song grows to hold them (at most 4096 bars). Throws DocumentError (no notes in the file, too
// many tracks...) before changing anything.
MidiImportResult importMidi(Document& doc, const MidiFile& file, const MidiImportOptions& options = {});
// The notes of the file (every track, or only `trackIndex`) into one pattern from patternTick on (scaled to 960 per
// quarter); notes past the pattern's end are left out. Returns the notes written.
int importMidiIntoPattern(Pattern& pattern, const MidiFile& file, int64_t patternTick = 0, int trackIndex = -1);

}  // namespace core::doc
