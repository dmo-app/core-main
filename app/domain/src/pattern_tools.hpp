// Pattern tools for creating, not only trimming (Diogo, 2026-11-05): normalize, density up and down (+10%, notes
// per beat...), build-ups with a roll, and a library of saved patterns ("core-pattern" v1) that any track can use,
// stretched or cut to the bars wanted. Rules and seeds only: no AI, no GUI, no audio, no engine code. Each call is
// one change of one pattern; the caller makes it one undo step. Plan: core-improvements
// planos/piano-roll/PLANO-CRIACAO-PADROES-E-ESTILOS.md. Contract of the files: docs/contracts/core-pattern.md.
#pragma once
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

// Which notes a tool works on: those that start in [start, end) and, when `ids` is not empty, are in `ids`.
// An empty scope is an empty scope (never "every note"). New notes start inside it and end by `end`.
struct NoteScope {
  int64_t start = 0, end = 0;
  std::set<std::string> ids;
};
NoteScope wholePattern(const Pattern& pattern);
std::vector<Note*> notesIn(Pattern& pattern, const NoteScope& scope);  // sorted by tick, then pitch

// --- Normalize (MIDI: velocity, length, start; never the audio level) ----------------------------------------
struct NormalizeOptions {
  std::string velocity = "equal";  // "keep" | "equal" (all at velocityValue) | "accent" (accents below)
  double velocityValue = -1;       // < 0: the scope's average
  // "accent": the velocity of the n-th 1/16 of each beat (KBBB study: 0.72/0.52/0.64 on the three bass steps).
  std::vector<double> accents = {1.0, 0.72, 0.52, 0.64};
  bool length = true;              // every note gets gate * grid (but never past the next note of the same pitch)
  double gate = 0.9;
  bool align = false;              // starts onto the grid
  int64_t grid = 240;
};
int normalizeNotes(Pattern& pattern, const NoteScope& scope, const NormalizeOptions& options);  // notes changed

// --- Density: add or remove notes -------------------------------------------------------------------------------
struct DensityOptions {
  // "percent" (+20 = 20% more, -20 = 20% fewer), "count" (+12 / -4 notes), "target" (end with N notes),
  // "perBeat" (N note starts per quarter note in the scope).
  std::string mode = "percent";
  double amount = 20;
  // How notes are added: "fill" (empty grid steps, pitch of the note before), "ghost" (as fill, soft and short),
  // "ratchet" (a note splits into 2, 3, 4 repeats), "octave" (a copy an octave up). Removing keeps the notes on
  // the beat as long as it can.
  std::string how = "fill";
  int64_t grid = 240;
  int pitch = -1;                  // the pitch of new notes when the scope has none (< 0: 36)
  uint64_t seed = 1;
};
struct DensityResult {
  int before = 0, target = 0, after = 0, added = 0, removed = 0;
  std::string note;                // why fewer than asked ("only 6 empty steps"), or ""
};
DensityResult changeDensity(Pattern& pattern, const NoteScope& scope, const DensityOptions& options);

// --- Build-up -> roll -> drop ----------------------------------------------------------------------------------
struct BuildupOptions {
  int64_t start = 0, length = 4 * 4 * kPpq;  // the build-up; the drop comes at start + length
  int pitch = 38;
  // "steps": the note rate per quarter of the build-up is 1/4, 1/8, 1/16, then 1/32 (the roll) in the last quarter
  // when `roll`. "gradual": the gap between notes shrinks smoothly from rateFrom to rateTo ticks (a roll that speeds
  // up the whole way, e.g. 960 = 1/4 down to 120 = 1/32).
  bool roll = true;
  std::string shape = "steps";
  int64_t rateFrom = kPpq, rateTo = kPpq / 8;
  int64_t gapTicks = 0;            // silence before the drop (e.g. one beat)
  double velocityFrom = 0.35, velocityTo = 1.0;
  // The pitch rises over the build-up, from riseFrom to riseTo semitones above `pitch` (a snare roll that climbs).
  double riseFrom = 0, riseTo = 0;
  bool replace = true;             // the notes already in [start, start + length) go first
};
int makeBuildup(Pattern& pattern, const BuildupOptions& options);  // notes written

// --- Pattern library ("core-pattern" v1) --------------------------------------------------------------------------
struct PatternPreset {
  std::string id, name, role, description, status = "hypothesis", source, origin;  // origin: root kind
  std::string approval;            // what Diogo said, for an approved pattern
  std::vector<std::string> styles, tags;
  int64_t lengthTicks = 4 * kPpq * 4;
  int root = -1;                   // the pitch the notes are written around (< 0: drums, never transposed)
  std::vector<Note> notes;         // ids "n1", "n2"...
  std::filesystem::path file;
};
struct PatternRoot { std::string kind; std::filesystem::path folder; };  // "builtin" (read-only) | "user"

PatternPreset presetFromJson(const json& j);  // throws DocumentError
json presetToJson(const PatternPreset& p);
// Every valid preset of the roots (the first root that has an id wins), sorted by id; problems: skipped files.
std::vector<PatternPreset> loadPatternLibrary(const std::vector<PatternRoot>& roots, std::vector<std::string>* problems = nullptr);
// Writes the preset into folder as a new file, never over one: a used id gets -2, -3... Returns it with id/file.
PatternPreset savePatternPreset(const std::filesystem::path& folder, PatternPreset preset, const std::vector<PatternPreset>& existing);
// The preset from the pattern's notes in [start, end) (ticks from start).
PatternPreset presetFromPattern(const Pattern& pattern, int64_t start, int64_t end, const std::string& name);

// The pitch a track's patterns go around: the most used pitch of `pattern` (ties: the lowest), else the sampler's
// root key, else -1 (keep the preset's pitches). For the built-in kick/hats/drums (fixed rows) always -1.
int trackRoot(const Track& track, const Pattern* pattern);

// The preset's notes over `lengthTicks` (repeated, the last repeat cut), moved to `toRoot` when both roots are set.
// A drum preset (root -1) that plays one pitch goes onto `toRoot` too: a hat pattern on any one-sound track.
// The preset played at `speed` (0.5: ticks and lengths doubled; 2: halved). Throws DocumentError out of 0.25-4.
PatternPreset stretchPreset(const PatternPreset& preset, double speed);
std::vector<Note> fitPreset(const PatternPreset& preset, int64_t lengthTicks, int toRoot = -1);
// Puts the preset at [start, start + lengthTicks) of the pattern: "replace" removes the notes that start there
// first, "add" keeps them (no note twice at the same tick and pitch). Returns the notes written.
int applyPreset(Pattern& pattern, const PatternPreset& preset, int64_t start, int64_t lengthTicks, const std::string& mode = "replace", int toRoot = -1);

// --- PCore: the pattern tool (Diogo, 2026-11-05: "PCore de padrões", one-time use, no device left on the track) ---
// It writes notes once into a range of a pattern (bars 7-12, the bars shown, a new pattern of 32 bars...) and is
// gone: the notes are normal notes. "Again" is the same settings with the next seed, in place of the last take
// (the caller undoes the take first). Nothing runs in real time.
struct PCoreSettings {
  // "create": the range gets the template (any pattern of the library); "alter": the notes already in the range.
  std::string mode = "create";
  std::string templateId = "bass/kbbb-anchor";
  // The template's speed (Diogo, 2026-11-05: psytrance parts in a slambient song): 0.5 = half time (every tick
  // doubled: a 145 BPM pattern keeps its feel at 290), 2 = twice as fast. 0.25-4. The drop pattern follows it.
  double speed = 1;
  int root = -1;                   // the key; -1: the track's (trackRoot), or the template's
  double density = 0;              // -90..+200 %: fewer or more notes
  std::string densityHow = "fill"; // fill | ghost | ratchet | octave
  double gate = 0;                 // 0: keep the lengths; else 0.1-1.0 of a 1/16
  double accent = 0;               // 0-1: how much the accents shape the velocity
  // Which accents: "kbbb" (1 · .72 · .52 · .64, the note on the kick loudest) or "rising" (.62 · .74 · .87 · 1:
  // louder toward the end of every beat, the psytrance bass of FL tutorials, 2026-11-07).
  std::string accentShape = "kbbb";
  // Random pitches (chaos): this share of the notes (0-1) gets a pitch drawn in [pitchLow, pitchHigh] (both -1: off).
  double pitchChance = 0;
  int pitchLow = -1, pitchHigh = -1;
  // The transition, three points (Diogo: "buildup, roll, drop"): the build-up from buildupStart (1/4, 1/8, 1/16,
  // rising), the roll from rollStart ("steps": 1/32; "gradual": speeding up smoothly from rollFrom to rollTo notes
  // per bar, 16 -> 64 by default) and the drop at the end of the range, after gapBeats of silence. A point < 0 is
  // not there: no buildupStart, the roll starts the transition; roll "off", no transition at all. The drop gets
  // dropTemplate (a library id, "": nothing) for dropBars bars after the range (the pattern grows).
  std::string roll = "off";
  int64_t buildupStart = -1, rollStart = -1;
  int rollFrom = 16, rollTo = 64;
  int gapBeats = 0;                // 0-4
  int rollPitch = -1;              // -1: the key
  // The roll's notes climb this many semitones by the drop (0-12; a 909 snare roll). Only on a track that plays
  // pitches (a sampler, a synth): the built-in drums have fixed rows, and there it is left out.
  int rollRise = 0;
  std::string dropTemplate;
  int dropBars = 0;
  uint64_t seed = 1;               // "Again" moves it on
};
void checkPCore(const PCoreSettings& s);  // throws DocumentError on a value out of range
// Writes the range [scope.start, scope.end) of the pattern (it grows to scope.end, up to 64 bars). "create"
// replaces the notes that start there; "alter" works on them (and only on scope.ids when set). Returns the notes
// in the range afterwards. Throws DocumentError (bad settings, a template not in the library, an empty range).
int pcoreApply(Pattern& pattern, const NoteScope& scope, const PCoreSettings& settings,
               const std::vector<PatternPreset>& library, int trackKey);
// The song's tempo glides from what plays at fromTick to bpmTo, arriving at toTick (and stays there): the tempo
// changes in (fromTick, toTick] go. For "roll from bar 7 to 12 up to 250 BPM". Throws DocumentError.
void tempoRampTo(Document& doc, int64_t fromTick, int64_t toTick, double bpmTo);

} // namespace core::doc
