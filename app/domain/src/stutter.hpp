// STUTTER, note slice (CORE-MANUAL plugins/plans/stutter-voice-stutter-execution-plan.md, INT-02): a note becomes a
// run of ordinary, editable notes (repeats) inside its own length, from a named recipe. Nothing hidden: the result is
// notes in the piano roll / steps, each marked with the recipe that wrote it (extra "stutter"), so reopening plays what
// was heard and Ctrl+Z gives the original back (one undo step for all the selected notes).
//
// Semantics from the plan (section 5): the gap between repeats goes from gapFrom to gapTo ticks, gradually:
// gap(u) = gapFrom * (gapTo / gapFrom)^u, u = how far into the note; onsets are computed exactly and rounded once,
// and an onset at the note's end is never written. Gate = each repeat's length as a share of its gap. count > 0 stops
// after that many repeats, and with finalNote the original note then plays to its end (the "complete-word" exit for
// notes: three fragments, then the whole thing). Audio repeats (VOICE STUTTER) are a separate mode, not this one.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

struct StutterRecipe {
  int64_t gapFrom = kPpq / 4, gapTo = kPpq / 4;  // ticks between repeats (240 = 1/16, 160 = 1/16 triplet, 30 = 1/128)
  double gate = 0.5;                               // 0.05..1
  double velocityFrom = 1, velocityTo = 1;         // a share of the original note's velocity
  int pitchStep = 0;                               // semitones added per repeat (melodic tracks; drums keep the pitch)
  int count = 0;                                   // 0 = repeats fill the note
  bool finalNote = false;                          // after `count` repeats, the note itself plays to its end
};

struct StutterPreset {
  const char* id;
  const char* name;
  const char* style;  // darkpsycore | psytrance | hitech | slambient (a guide, never a block)
  const char* info;
  StutterRecipe recipe;
};
const std::vector<StutterPreset>& stutterPresets();
const StutterPreset* findStutterPreset(const std::string& id);

// The onsets (ticks from the start of a window of `length`) the recipe gives. Empty when the recipe is invalid.
std::vector<int64_t> stutterOnsets(const StutterRecipe& recipe, int64_t length);

// Replaces each note in `ids` by its repeats (pitched = pitchStep applies). Returns the notes written (0 = nothing
// changed, e.g. notes too short for two repeats). Notes not in `ids` are never touched.
int stutterNotes(Pattern& pattern, const std::set<std::string>& ids, const StutterRecipe& recipe, const std::string& presetId,
                 bool pitched = true);

// The same on a track over the song bars [from, to) (to <= from: the whole song), for AICORE's "stutter" call: the
// notes of the patterns its clips play there. A pattern another clip also plays is copied first (makeClipUnique), so
// the change stays where it was asked. One clip's pattern repeats inside a long clip: the repeats come with it.
int stutterTrack(Document& doc, const std::string& trackId, int64_t from, int64_t to, const StutterPreset& preset);

// VOICE STUTTER (plan INT-03/04, the "Basic" recipes of stutter-voice-stutter-recipes-and-research.md section 4): a
// recorded word becomes ordinary, editable audio clips. You mark a fragment of it (the clip editor's selection, in the
// file's seconds); the recipe plays fragment events at its rhythm, then the complete word. Fragment labels A/B/C are
// neutral: A is the marked piece, B the piece of the same length right after it, C the next one (never a claim about
// syllables). Each event clip lasts gate x its gap (at most the fragment), with short fades so the cuts never click.
// The word clip is the original clip itself, moved after the last gap. Vocal Climb and Robotic Fragments move each
// fragment's pitch (the clip's pitchSemitones: grains, length kept; the word stays at its own pitch); Vocal Freeze
// holds the marked piece (freezeSeconds: 40 ms grains drifting through it) for its whole window, then the word.
struct VoiceStutterPreset {
  const char* id;
  const char* name;
  const char* style;
  const char* info;
  std::vector<int> fragments;     // per event: 0 = A, 1 = B, 2 = C
  std::vector<int64_t> gaps;      // ticks from each event to the next (the last one: into the complete word)
  double gate = 0.65;             // event length as a share of its gap (capped at the fragment's length)
  double gainFromDb = 0, gainToDb = 0;  // across the events, added to the clip's own gain
  std::vector<double> gainsDb;    // when set: one per event, instead of the from/to line
  bool scatter = false;           // a seeded, bounded shift of each fragment's start (Broken Voice)
  std::vector<double> pitches;    // when set: semitones per event (the pitch processor)
  bool freeze = false;            // each event holds the fragment for gate x its gap (the freeze processor)
};
const std::vector<VoiceStutterPreset>& voiceStutterPresets();
const VoiceStutterPreset* findVoiceStutterPreset(const std::string& id);

// Applies the recipe to an audio clip: fragmentStart/fragmentSeconds in the file's seconds (inside the clip; a fragment
// outside it is moved in). Returns the ids of the new fragment clips (the word keeps the clip's id). Throws
// DocumentError when the clip, the fragment or the 1024-clip limit does not allow it.
std::vector<std::string> voiceStutter(Document& doc, const std::string& trackId, const std::string& clipId, double fragmentStart,
                                      double fragmentSeconds, const VoiceStutterPreset& preset);

}  // namespace core::doc
