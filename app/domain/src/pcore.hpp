// PCore in layers (Diogo, 2026-11-05): a pattern function whose layers open one below the other.
//   1. Style -> part (kick, bass, hats, cymbals, perc, build; lead, arp, pad, fx) + a scale and its root note
//   2. Shape of the part (bass KBBB, KBB_, K_BB...; kick, hats, cymbals, perc, rolls; arps...)
//   3. How many notes: the grid (1/8, 1/16, 1/32, triplets) and more/fewer notes
//   4. Change the result: humanize, quantize, shuffle (the notes' order), reverse, a variation every N bars, a roll
//      at the end
// It writes plain notes into several tracks at once (the selected tracks, a rule for every plugin), one undo step
// for the caller; afterwards each track can be changed alone ("alter"). The style only puts its choices first:
// every shape and scale works in every style. Rules and seeds only: no AI, no GUI, no audio, no engine code.
// Catalogue: pcore/styles.json (beside Core.exe). Contract: docs/contracts/core-pcore.md.
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

struct PCoreScale { std::string id, name, mood; std::vector<int> steps; };
struct PCorePart {
  std::string id, name, group, about;  // group: rhythm | melody
  bool drum = true;
  int pitch = 36;                      // drums: the note when the track has none of its own
  int octave = 48;                     // melody: the C the root is placed above
};
struct PCoreShape {
  std::string id, part, name, about;
  std::string kind = "steps";          // steps | roll | breakdown
  std::string grid = "1/16";           // 1/4 1/8 1/8t 1/16 1/16t 1/32 1/64
  std::string steps;                   // x X o . - (bass: K B b)
  std::string motion = "pedal";        // melody: how the notes move (see pcoreMotions)
  int notes = 3;                       // arps: how many notes of the chord
  int pitch = -1;                      // drums: a sound of its own (46 open hat, 49 crash...); -1: the part's
  std::string rollShape = "steps";     // kind roll: steps | gradual
  int gapBeats = 0;
  int64_t rateFrom = kPpq, rateTo = kPpq / 16;
  // kind breakdown (kick, bass, cymbals): kick and bass that change speed over the whole bars. The kick, the bass
  // and the cymbals draw the same plan, so they stay together. The call's layers can change feel, spacing, length.
  std::string arc = "down";            // down: fast KBBB slows into one kick | up: slow kicks speed up into KBBB
                                       // | updown: slow, fast, slow
  std::string feel = "regular";        // regular: step by step | chaos: at random
  std::string source = "plan";         // plan: write it | notes: thin out the notes already there (down)
  double length = 1.0;                 // 0.1..1: the part of the bars that changes speed; the rest is full KBBB
  int spacing = 4;                     // 1, 2, 4, 8: beats between the kicks at the slowest
};
// Alignment of the kick, the bass and the hats / cymbals (Diogo, 2026-11-07: "uma opção de alinhamento entre os pratos,
// kick e bass, com vários estilos"). One way per style of locking the three together, applied to the notes there:
//   kick: onto the grid it was written on (1/16, triplets, 1/32...)
//   bass: onto its grid; no bass note under a kick (nor in the first `bassClearSteps` 1/16 after it); every note
//         ends `bassTailMs` before the next kick and lasts at most `bassGate` of the space to the next note; the
//         first note after each kick at most `bassFirst` of the others' loudness (KBBB: 80 / 100 / 100)
//   hats: onto their grid, with `swing` and `pushMs` (late +, early -); a closed hat on a kick at most `hatsOnKick`
//         of the others (0: it goes); off the offbeat 1/8 at most 1 - `hatsAccent` of the offbeat hats
//   cymbals: onto their grid with the same swing and push
// Applied twice gives the same notes. Sources: docs/contracts/core-pcore.md (alignment).
struct PCoreAlign {
  std::string id, name, about;
  int bassClearSteps = 0;              // 0 or 1: 1/16 steps after the kick kept free of bass (offbeat bass)
  double bassGate = 0.8;               // 0.3..1
  double bassFirst = 0.8;              // 0.3..1
  double bassTailMs = 8;               // 0..40
  double hatsOnKick = 0.6;             // 0..1
  double hatsAccent = 0.3;             // 0..0.8
  double swing = 0;                    // 0..0.5 (of a half grid step)
  double pushMs = 0;                   // -10..+10
};
struct PCoreStyle {
  std::string id, name, root = "A", about;
  std::string align;                   // the style's way of aligning ("" none)
  std::vector<std::string> match, scales;
  std::map<std::string, std::vector<std::string>> shapes;  // part -> shape ids, the style's favourite first
  double humanize = 0.05, chaos = 0.3;
  double bpm = 0;                      // the style's usual tempo (the Patterns window's templates); 0: not said
};
struct PCoreCatalog {
  std::vector<PCoreScale> scales;
  std::vector<PCorePart> parts;
  std::vector<PCoreShape> shapes;
  std::vector<PCoreStyle> styles;
  std::vector<PCoreAlign> aligns;
  const PCoreAlign* align(const std::string& id) const;
  const PCoreScale* scale(const std::string& id) const;
  const PCorePart* part(const std::string& id) const;
  const PCoreShape* shape(const std::string& id) const;
  const PCoreStyle* style(const std::string& id) const;
};

PCoreCatalog pcoreCatalogFromJson(const json& j);                    // throws DocumentError
PCoreCatalog loadPCoreCatalog(const std::filesystem::path& file);    // throws DocumentError
const std::vector<std::string>& pcoreMotions();
int64_t pcoreGridTicks(const std::string& grid);                     // 0 for an unknown grid
int pcoreRootPitchClass(const std::string& name);                    // "C".."B", "F#", "Bb"; -1 when unknown

// The style of a song's name ("Hi-tech", "Dark psycore"...) or of an id; nullptr when none matches.
const PCoreStyle* pcoreStyleFor(const PCoreCatalog& c, const std::string& nameOrId);
// The part's shapes: the style's first (in its order), then every other one of that part.
std::vector<const PCoreShape*> pcoreShapesFor(const PCoreCatalog& c, const std::string& part, const std::string& style);
// The scales: the style's first, then the rest.
std::vector<const PCoreScale*> pcoreScalesFor(const PCoreCatalog& c, const std::string& style);
// Whether a shape's notes follow the scale. Drums, rolls, breakdowns and the pedal, octave-last, octaves and laser
// motions play the key note (or fixed steps from it) whatever the scale: for them only the key changes the notes.
bool pcoreShapeUsesScale(const PCoreShape& shape, const PCorePart& part);
// The scale's notes from the key, by name: Hungarian minor from E is "E F# G A# B C D#". "" for an unknown key.
std::string pcoreScaleNoteNames(const PCoreScale& scale, const std::string& root);
// The part a track most likely plays, from its id and name ("kick", "Bass", "Open hats", "Lead"...); "" when unsure.
std::string pcoreGuessPart(const Track& track);

// Layers 1 (style, scale, root), 3 and 4. Each track brings its part and shape (layer 2).
struct PCoreLayers {
  std::string mode = "create";         // create: write the shape; alter: change the notes already in the bars
  std::string style;                   // a catalogue id; "" = none (the scale and root must then be given)
  std::string scale;                   // "" = the style's first
  std::string root;                    // "" = the style's ("F#")
  std::string grid;                    // "" = the shape's own
  double density = 0;                  // -90..+200 %: fewer or more notes (the ones on the beat stay)
  double humanize = -1;                // 0..1; < 0 = the style's
  double quantize = 0;                 // 0..1: how far the notes move to the grid
  bool shuffle = false;                // the notes' pitches (drums: their loudness) in a random order; rhythm stays
  bool reverse = false;                // the bars play backwards
  int variationEvery = 0;              // 0, 2, 4, 8, 16: the last beat of every N bars changes (a small fill)
  int rollBeats = 0;                   // 0..16: the last beats of the bars become a roll, on every track
  uint64_t seed = 1;                   // "Again" moves it on
  // Breakdown shapes only ("" / 0: the shape's own).
  std::string breakFeel;               // regular | chaos
  int breakSpacing = 0;                // 1, 2, 4, 8 beats between the kicks at the slowest
  double breakLength = 0;              // 0.1..1: the part of the bars that changes speed
  // Alignment of kick, bass and hats (PCoreAlign): an id, "style" = the style's own, "" = off. With alignOnly the
  // tracks keep their notes and are only aligned (P-Core's Align button): nothing else changes.
  std::string align;
  bool alignOnly = false;
  // "Again": the written shape also changes in a way of its part, drawn from the seed (accents, ghost notes,
  // octave jumps, an answer, a fill...), so a shape that is always written the same still gives new takes.
  bool vary = false;
};
void checkPCoreLayers(const PCoreLayers& l);  // throws DocumentError

struct PCoreTarget {
  std::string trackId, part, shape;    // shape "" = the style's first for the part (create)
  Pattern* pattern = nullptr;
  int pitch = -1;                      // drums: the track's sound (trackRoot); -1 = the part's / shape's
};
struct PCoreResult { std::string trackId, part, shape; int notes = 0; double perBar = 0; };

// Writes [start, end) of each target's pattern (pattern ticks, whole bars from the pattern's start). The pattern
// grows to `end` (up to 64 bars). "create" replaces the notes that start there; "alter" changes them. When a kick
// and a bass are written together, the bass leaves the kick's ticks free (outside the roll). Throws DocumentError
// (unknown part, shape, scale or style, an empty range) before anything changes.
// clearBass false: the caller clears the bass under the kick itself (pcoreApplyToSong does it in song time).
std::vector<PCoreResult> pcoreWrite(const PCoreCatalog& c, const PCoreLayers& layers, int64_t start, int64_t end,
                                    std::vector<PCoreTarget>& targets, bool clearBass = true);

// What an alignment changed, for the line under P-Core's buttons. `kickFrom`: "track" (a selected kick track),
// "song" (the song's kick track, read only) or "beat" (no kick track: a kick on every beat is assumed).
struct PCoreAlignStats { std::string align, kickFrom; int moved = 0, removed = 0, shortened = 0, softer = 0; };
// The alignment on patterns already in place, over [start, end) of each pattern (pattern ticks). Each track brings
// its part and the kick's ticks in its own pattern's time. A part other than kick, bass, hats or cymbals is left
// alone. bpm turns the milliseconds into ticks. Align the kick first, then read its ticks for the others.
struct PCoreAlignTrack { Pattern* pattern = nullptr; std::string part; std::vector<int64_t> kicks; };
void pcoreAlignNotes(const PCoreAlign& a, double bpm, int64_t start, int64_t end, std::vector<PCoreAlignTrack>& tracks,
                     PCoreAlignStats& stats);
// The alignment a call means: an id, "style" (the style's, else the first), "" none. Throws for an unknown id.
const PCoreAlign* pcoreAlignFor(const PCoreCatalog& c, const PCoreLayers& l);

// The same as a call on the song (the AI's "pcore" function, the UI's Write): `tracks` are {trackId, part, shape};
// each track's pattern is the one its clip plays at songTick (else its first; an empty track gets one). The bars
// [fromBar, toBar) are counted from that pattern's start. Returns one result per track.
struct PCoreTrackChoice { std::string trackId, part, shape; };
// `patterns` names a track's pattern outright (the one open in the piano roll), in place of the one at songTick.
// A choice whose shape is "keep" keeps that track's notes and only changes them ("alter"), in the same call;
// shape "" is the style's first shape for the part.
std::vector<PCoreResult> pcoreApplyToSong(Document& doc, const PCoreCatalog& c, const PCoreLayers& layers,
                                          const std::vector<PCoreTrackChoice>& tracks, int64_t songTick, int fromBar, int toBar,
                                          const std::map<std::string, std::string>& patterns = {},
                                          PCoreAlignStats* aligned = nullptr);
// The pattern a track plays at a song tick: the clip's there, else the track's first; "" when it has none.
std::string patternAt(const Track& track, int64_t songTick);

// JSON forms for the function call (docs/contracts/core-pcore.md).
PCoreLayers pcoreLayersFromJson(const json& j);  // throws DocumentError
json pcoreLayersToJson(const PCoreLayers& l);

// P-Core's "Shuffle" on chosen notes only (the piano roll's Edit > Randomize): the pitches (drums: the
// loudness) of the notes in `ids` swap places at random; the rhythm stays. An empty `ids` is every note. The same
// seed gives the same result. Returns how many notes changed.
int pcoreShuffle(Pattern& pattern, const std::set<std::string>& ids, bool drum, uint64_t seed);

} // namespace core::doc
