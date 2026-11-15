// Groove tools on a pattern's notes (Fjord UI, phase 3): quantize with strength and swing, humanize, and
// variations. Pure functions on the document, one undo step for the caller. No GUI, no audio, no engine code.
// The same functions serve the piano roll and, later, the AI tools.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

// Moves the notes toward the grid. strength 0..1 (1 = onto the grid); swing 0..1 delays every second grid
// step by swing * grid / 2 (0.33 is about a triplet feel). Notes stay inside the pattern. An empty `ids`
// means every note of the pattern. Returns how many notes moved.
int quantizeNotes(Pattern& pattern, const std::set<std::string>& ids, int64_t grid, double strength, double swing);

// Small random changes, the same for the same seed: timing up to +-timingTicks, velocity up to +-velocity
// (0..1 scale). Notes stay inside the pattern; velocity stays 0.05..1. Returns how many notes changed.
int humanizeNotes(Pattern& pattern, const std::set<std::string>& ids, int64_t timingTicks, double velocity, uint64_t seed);

// Variations of the selected notes (or all of them):
//   "octave"   some notes (about one in four, from the seed) jump an octave up or down
//   "reverse"  the notes play backwards inside the span they cover
//   "thin"     about one note in four goes (never all of them)
//   "accent"   the notes on the beat get louder, the others softer
// Throws DocumentError for another name. Returns how many notes changed (or went).
int varyNotes(Pattern& pattern, const std::set<std::string>& ids, const std::string& kind, uint64_t seed);

// Note tools of the piano roll's Edit menu (Diogo, 2026-11-05). Same rules: an empty `ids` means every note,
// one call is one change, the caller makes it one undo step. Each returns how many notes changed (or were added).
// Every note moves by `semitones` (+12: an octave up). A note that would leave 0..127 stays where it is.
int transposeNotes(Pattern& pattern, const std::set<std::string>& ids, int semitones);
// Adds, for every chosen note, a copy at each interval above it (semitones: {12} an octave, {7} a fifth,
// {3, 7} a minor chord, {4, 7} a major chord). Never a second note at the same tick and pitch; never past 127.
int addChordNotes(Pattern& pattern, const std::set<std::string>& ids, const std::vector<int>& intervals);
// "legato": each note lasts until the next note starts (the last one until the pattern ends); "half" and
// "double": half or twice as long (at least 10 ticks, never past the pattern's end). Throws DocumentError otherwise.
int setNoteLengths(Pattern& pattern, const std::set<std::string>& ids, const std::string& how);
// Velocity times `factor` (1.15: louder, 0.85: softer), kept 0.05..1.
int scaleVelocities(Pattern& pattern, const std::set<std::string>& ids, double factor);

} // namespace core::doc
