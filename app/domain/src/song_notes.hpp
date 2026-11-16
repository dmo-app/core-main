// The notes of a track as the song plays them (Diogo 2026-11-16: the bottom panel shows several tracks together, on
// the song's timeline, "quase como o piano roll para as pistas que decidir meter"). A note of a looping pattern shows
// once per repeat; editing any of those repeats edits the pattern's note, so every repeat follows (as the clip plays
// it). Ticks here are song ticks unless said otherwise.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

struct SongNote2 {
  std::string clipId, patternId, noteId;
  int64_t start = 0;       // song tick where this repeat sounds
  int64_t length = 0;      // cut at the clip's end
  int pitch = 0;
  double velocity = 0.8;
};

// Every repeat that starts in [from, to), sorted by start then pitch. A track that plays no patterns: none.
std::vector<SongNote2> songNotesIn(const Document& doc, const std::string& trackId, int64_t from, int64_t to);

// The clip that plays at this song tick (the last one placed there, as the engine plays it), or null.
const Clip* clipAt(const Track& track, int64_t tick);
// The pattern tick a song tick falls on inside a clip.
int64_t patternTickIn(const Clip& clip, const Pattern& pattern, int64_t songTick);

// A note at a song tick: in the pattern of the clip there; with no clip, a new one-bar pattern and clip at that bar.
// The length is cut to the pattern's end. Returns {patternId, noteId}. Throws DocumentError (no such track, a track
// without patterns, pitch out of 0..127, length < 1).
std::pair<std::string, std::string> addSongNote(Document& doc, const std::string& trackId, int64_t at, int64_t length,
                                                int pitch, double velocity);
// Moves a pattern's note by `ticks` (wrapping inside the pattern, as a loop plays it) and `semitones` (0..127).
void moveSongNote(Document& doc, const std::string& trackId, const std::string& patternId, const std::string& noteId,
                  int64_t ticks, int semitones);
void resizeSongNote(Document& doc, const std::string& trackId, const std::string& patternId, const std::string& noteId,
                    int64_t length);  // cut to the pattern's end, at least 1
void setSongNoteVelocity(Document& doc, const std::string& trackId, const std::string& patternId,
                         const std::string& noteId, double velocity);  // clamped to 0.01..1
void removeSongNote(Document& doc, const std::string& trackId, const std::string& patternId, const std::string& noteId);

// The pitches a track uses in [from, to), low to high (the rows a folded lane shows).
std::vector<int> songPitchesIn(const Document& doc, const std::string& trackId, int64_t from, int64_t to);

}  // namespace core::doc
