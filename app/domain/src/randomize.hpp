// Randomize (style randomizer, phase 1 + axis J): re-rolls the groove and the arrangement of a song made from a
// style template, inside what is normal for its style, from a seed. Plan: core-improvements randomizer
// study (Astra, 2026-11-03), axes A-F. No GUI, no audio, no engine code: a pure function on the document,
// one Undo step for the caller. Contract: docs/contracts/core-randomize.md.
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

// The axes, by name (an empty list = A-F; J only when named):
//   bass   A: the bass grammar per section (kbbb, kbb, kb-b, triplet, offbeat: the ones of the style) and its turns
//   gate   B: the bass note length: a fraction of the gap to the next note, never longer than the style's ms
//   hats   C: the closed hats per section: grid (straight 16ths, 16ths with holes, offbeat 8ths) and 1/32 rolls
//   kick   D: a ghost kick on the last 16th, per section
//   roles  E: percussion, accents and open hats in or out per section (never in the breaks), the lead's entry
//   fills  F: fills every 8 or 16 bars and how they roll, bars without kick before a lift, the riser's length
//   tempo  J: a tempo per section in steps (slower into a break, back or faster into a drop); replaces the tempo map
const std::vector<std::string>& randomizeAxes();

struct RandomizeOptions {
  uint64_t seed = 1;                 // the same seed on the same song gives the same result
  int strength = 2;                  // 1 subtle, 2 medium, 3 bold: how often a section leaves the template's choice
  std::vector<std::string> axes;     // names from randomizeAxes(); empty = all
};

// The style card of a song: its menus. Known styles: "Dark psycore", "Hi-tech", "Slambient" (from the
// template, songStyle); throws DocumentError for a song that has no known style.
// Returns one line per change, in English ("Groove: bass kbb, gate 40 ms"...). Tracks it does not know
// (by id) and sections without the parts stay as they are. Section energy comes from the template
// (sections[].energy); a song without it is read from what plays in each section.
std::vector<std::string> randomizeStyle(Document& doc, const RandomizeOptions& options);

// Changes the song's tempo (its start BPM and its tempo map) and keeps the bass gates in ms (audit
// 48c9c38 E8/D4, Diogo 2026-11-04: the domain rewrites the notes). In every stretch whose BPM changes, the
// clips of the bass track (id "bass" or the built-in bass) that start there get a copy of their pattern
// with the note lengths scaled by new/old BPM, as Randomize J does; a longer note stays inside 80 % of
// the gap to the next one. Drums, hats and the rest keep their ticks (their notes are hits, not gates).
// The UI, the AI tools and Randomize J use it; one Undo step for the caller. Returns what it did.
// Throws DocumentError for a BPM outside 40-600 or an invalid map (nothing changes then).
std::vector<std::string> setTempo(Document& doc, double bpm, std::vector<TempoPoint> changes);

} // namespace core::doc
