// The Patterns window (Diogo, 2026-11-08: the P-Core channel rack, in a window of its own): one row per note track,
// drums as 1/16 steps and the bass and leads as a small piano roll, over a pattern of 1-8 bars. A pattern of the
// window ("Main Groove", "Variation", "Fill"...) is a slot: one pattern on each track, kept in the song as
// extra.patternSlots [{id, name, bars, swing, tracks: {trackId: patternId}}]. A slot never copies notes: its
// patterns are the tracks' own, so a clip of them on the timeline plays what the window shows.
// Pattern templates: the grooves of the window's left side, the P-Core styles written onto the tracks or the user's
// own (Save as Template, %APPDATA%\Core\PatternTemplates\*.core-patterns.json).
// Rules and data only: no GUI, no audio, no engine code. Contract: docs/contracts/core-pcore.md (Patterns window).
#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include "document.hpp"
#include "pcore.hpp"

namespace core::doc {

constexpr int kRackMaxBars = 8;
constexpr double kRackMaxSwing = 0.75;  // of half a step: 0.75 is close to a triplet feel

struct PatternSlot {
  std::string id, name;
  int bars = 4;                                // 1, 2, 4 or 8
  double swing = 0;                            // 0..kRackMaxSwing: every second 1/16 played later
  std::map<std::string, std::string> tracks;   // trackId -> patternId (a track without one is empty in this slot)
  std::string root, scale;                   // empty: the song's key / the style's scale
};

// The slots of the song. A song without any gets one, "Main Groove", made of the patterns that play at `songTick`
// (it is only saved once something is edited in it). Tracks or patterns that went are left out.
std::vector<PatternSlot> rackSlots(const Document& doc, int64_t songTick = 0);
void setRackSlots(Document& doc, const std::vector<PatternSlot>& slots);
// Saves the slots as rackSlots(doc, songTick) gives them when the song has none yet: the window calls it first in
// every edit, so the default "Main Groove" is the one it showed (the patterns at the playhead).
void ensureRackSlots(Document& doc, int64_t songTick);
// The slot with that id; nullptr when none.
const PatternSlot* findRackSlot(const std::vector<PatternSlot>& slots, const std::string& id);
// The pattern of a track in a slot; nullptr when the track has none there.
const Pattern* rackPattern(const Document& doc, const PatternSlot& slot, const std::string& trackId);
// The same, made when missing (named after the slot, the slot's length) and saved in the song's slots. Throws
// DocumentError for an unknown slot or track.
Pattern& rackPatternFor(Document& doc, const std::string& slotId, const std::string& trackId);

// Rows: the note tracks of the song in order. A drum row (kick, hats, snare, drums, a sampler that plays a drum
// part) is drawn as steps; the others (bass, leads...) as notes.
bool rackStepRow(const Track& track);
// The note a step writes on a drum row: the most used note of the pattern, else the instrument's own sound
// (kick 36, snare 38, hats 42 or 46 on an "open" track, a sampler's root key).
int rackStepPitch(const Track& track, const Pattern* pattern);

// Steps: [start, start + length) in pattern ticks. A step is on when a note starts in it.
bool rackStepOn(const Pattern& pattern, int64_t start, int64_t length);
// On: a note of `pitch` at the step's start (an odd 1/16 later by the swing). Off: every note that starts in it goes.
// Returns whether the step is on now.
bool toggleRackStep(Pattern& pattern, int64_t start, int64_t length, int pitch, double velocity, double swing);
// Every second 1/16 (the "e" and "a" of the beat) plays later by swing x half a step. Moves the notes of the slot's
// patterns from the old swing to the new one, keeping their unswung positions and lengths.
void setRackSwing(Document& doc, const std::string& slotId, double swing);
// The slot's length in bars (1, 2, 4, 8): its patterns are cut (notes after the end go) or grow repeating.
void setRackBars(Document& doc, const std::string& slotId, int bars);

// A new empty slot, named "Pattern N" (or `name`); returns its id.
std::string addRackSlot(Document& doc, const std::string& name = "", int bars = 4);
// A copy of a slot: every pattern copied into a new one on its track; returns the new slot's id.
std::string duplicateRackSlot(Document& doc, const std::string& slotId);
void renameRackSlot(Document& doc, const std::string& slotId, const std::string& name);
// The slot goes from the list (its patterns stay on the tracks, with their clips). The last slot stays.
void removeRackSlot(Document& doc, const std::string& slotId);
// Every note of the slot's patterns on these tracks goes (all tracks when empty). Returns the notes removed.
int clearRackSlot(Document& doc, const std::string& slotId, const std::vector<std::string>& trackIds = {});
// Send to Arrange: clips of the slot's patterns from `atTick` (moved to its bar) for the slot's length, on every
// track that has a pattern in it. What other clips had there is cut away. The song grows when needed.
// Returns the clips placed.
int sendRackSlotToArrange(Document& doc, const std::string& slotId, int64_t atTick);

// What the engine plays while the window plays a slot: the slot's patterns alone, from the song's start, looped over
// the slot's bars (no other clips, no audio, no sections, no tempo changes). The song itself does not change.
Document rackPlayDocument(const Document& doc, const std::string& slotId, int64_t songTick = 0);

// Pattern templates.
struct PatternTemplateRow { std::string part, name; int64_t lengthTicks = 0; std::vector<Note> notes; };
struct PatternTemplate {
  std::string id, name, style, about;
  double bpm = 0;         // 0: not said
  int bars = 4;
  bool builtin = false;   // a P-Core style, written fresh at each load (rows empty)
  std::vector<PatternTemplateRow> rows;
  std::string file;       // the user's: where it is saved
};
constexpr const char* kRackTemplateExtension = ".core-patterns.json";
// The built-in templates: one per P-Core style (its name, its tempo, the bars of a groove).
std::vector<PatternTemplate> rackStyleTemplates(const PCoreCatalog& c);
PatternTemplate rackTemplateFromJson(const json& j);  // throws DocumentError
json rackTemplateToJson(const PatternTemplate& t);
std::vector<PatternTemplate> loadRackTemplates(const std::filesystem::path& folder, std::vector<std::string>* problems = nullptr);
// The slot as a template (its rows: each track's part, name and notes). The song's BPM goes with it.
PatternTemplate rackTemplateFromSlot(const Document& doc, const std::string& slotId, const std::string& name);
// Writes it as <id>.core-patterns.json in the folder (a used id gets -2, -3...). Returns the file.
std::filesystem::path saveRackTemplate(const std::filesystem::path& folder, PatternTemplate t);
// How many tracks of the song a template writes into.
int rackTemplateTracks(const Document& doc, const PatternTemplate& t);
// Load Template: a saved template's rows go into the slot's patterns of the tracks that play the same part (else
// the same name); a style template is written by P-Core (the style's first shape on every track whose part is
// known, the style's alignment). The slot takes the template's bars. Returns the tracks written. Throws
// DocumentError when no track fits.
int applyRackTemplate(Document& doc, const PCoreCatalog* c, const PatternTemplate& t, const std::string& slotId, uint64_t seed = 1);

// One row of P-Core in the window (the row's right-click > Shape): the shape over the slot's bars, the style's
// alignment kept off (the other rows stay as they are). `shape` "" = the style's first for the part.
PCoreResult rackWriteShape(Document& doc, const PCoreCatalog& c, const std::string& slotId, const std::string& trackId,
                           const std::string& part, const std::string& shape, const std::string& style, uint64_t seed, bool vary);

} // namespace core::doc
