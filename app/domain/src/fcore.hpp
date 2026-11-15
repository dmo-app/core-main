// FCore (2026-11-05): effects by intent. Choose what the sound is for (a build-up, an intro, an impact...) and a
// style; FCore puts Core's own built-in effects on the track and moves them over time, with four knobs that
// are the same for every intent (Amount, Length, Tone, Space) and one sentence that says what it does.
//
// Two ways to play it, the same intents:
//   - "note" (the FCore sampler): on a sampler track. The movement starts at every note and lasts Length
//     (an 8-bar note is an 8-bar build-up). Engine modulators of shape "rise" (engine-protocol-v1, FCore).
//   - "region" (FCore as an effect, on any track): the movement runs over regions of the song, written as
//     automation of the FCore effects, like a rack's rise (fx_racks.hpp). Outside its regions FCore is neutral.
//
// Nothing new plays in the engine apart from the rise modulator: the effects are ordinary built-in inserts
// (tagged "fcore"), so undo, save and export work as always. The intent, style and knobs are kept on the
// track (extra "fcore"), and everything FCore writes is written again from them after every change.
// Contract: docs/contracts/core-fcore.md.
#pragma once
#include <optional>
#include <string>
#include <vector>
#include "document.hpp"
#include "samples.hpp"

namespace core::doc {

// ---- styles (the templates' styles) and intents
struct FCoreStyle {
  std::string id, label;
  double bpmLo = 0, bpmHi = 0;  // samples are "for this style" in this tempo range (a loop: or half / double)
};
const std::vector<FCoreStyle>& fcoreStyles();  // menu order, slowest first
const FCoreStyle* findFCoreStyle(const std::string& id);

struct FCoreIntent {
  std::string id, label;
  bool moves = true;          // false: a fixed sound (atmosphere, clean); Length does nothing
  double defaultBeats = 32;   // Length when it is first chosen
  std::string what;           // what it is for, in plain words (the first layer's buttons)
};
const std::vector<FCoreIntent>& fcoreIntents();
const FCoreIntent* findFCoreIntent(const std::string& id);
// The intents in the order the style shows them: the ones that suit it first. Nothing is ever hidden.
std::vector<std::string> fcoreIntentsFor(const std::string& style);
// Why this intent tends to spoil this style ("" when it suits it or is neutral): shown as a warning.
std::string fcoreIntentWarning(const std::string& style, const std::string& intent);

// ---- what an intent becomes
struct FCoreKnobs {
  double amount = 0.6;       // 0..1: how much of the movement (subtle .. extreme)
  double lengthBeats = 32;   // 1..256 beats (the note's or the region's movement)
  double tone = 0.5;         // 0..1: dark .. bright
  double space = 0.5;        // 0..1: dry .. huge
};
// One parameter that moves: from `from` at the start to `to` at the end of Length, as (t)^curve (in octaves
// for a parameter in Hz). hold: stays at `to` after the end; otherwise back to `from` (the drop). In
// "region" mode the parameter sits at `neutral` outside the regions.
struct FCoreMove {
  std::string role;   // the effect ("filter", "drive", "echo", "space", "shape", "finish") or "sampler"
  std::string param;  // its parameter (sampler: "tune")
  double from = 0, to = 0, neutral = 0;
  double curve = 1;
  bool hold = false;
};
struct FCoreEffect {
  std::string role, name;  // role on the FCore chain, built-in effect name
  json params;             // the values it sits at (a moving one: its `from`)
};
struct FCorePlan {
  std::vector<FCoreEffect> effects;  // in chain order
  std::vector<FCoreMove> moves;
  json sampler = json::object();     // sampler parameters it sets ("note" mode)
  std::string sentence;              // what it does, in plain English, with the real values
  std::vector<std::string> ingredients;  // the parts that sound here, in the sentence's order (all of them, on or off)
};
// The parts of an intent, each one switchable (its mini plugin's switches): "filter", "echo", "space", "shape",
// "drive", "pitch", "attack". Which ones an intent has depends on the style and the mode (a region has no pitch).
struct FCoreIngredient {
  std::string id, label;
  std::string what;  // what it does to the sound, in plain words (no technical names)
};
// Pure: the effects, the moves and the sentence for an intent, a style and the knobs. mode "note" | "region"
// (a region has no sampler: the pitch moves are left out). `off`: the parts switched off (their effects, moves
// and words go; an unknown or absent one is ignored).
FCorePlan planFCore(const std::string& intent, const std::string& style, const FCoreKnobs& knobs, const std::string& mode = "note",
                    const std::vector<std::string>& off = {});
std::vector<FCoreIngredient> fcoreIngredients(const std::string& intent, const std::string& style, const FCoreKnobs& knobs,
                                              const std::string& mode);
std::string fcoreIngredientLabel(const std::string& intent, const std::string& id);  // "" for an unknown part
// Steps (Diogo 2026-11-05: never 4-5 at once on show): an intent starts with its main part only ("filter"; Impact:
// "pitch" in the sampler; Atmosphere: "space"; Clean: none) and "+ Add step" switches the next one on.
std::string fcoreMainPart(const std::string& intent, const std::string& mode);
// A preset uses the steps it needs (Diogo: "without a preset it is simple"): which parts it switches on.
struct FCorePreset {
  std::string id, label;
  std::string what;              // what it sounds like, in plain words
  std::vector<std::string> on;   // the parts it switches on (the others go off; one this intent lacks is ignored)
};
const std::vector<FCorePreset>& fcorePresetsOf(const std::string& intent);
std::vector<std::string> fcorePresetOff(const std::string& intent, const std::string& style, const FCoreKnobs& knobs, const std::string& mode,
                                        const std::string& preset);
std::vector<std::string> fcoreStartOff(const std::string& intent, const std::string& style, const FCoreKnobs& knobs, const std::string& mode);
std::string fcoreIngredientHelp(const std::string& intent, const std::string& style, const std::string& id);

// ---- on a track
struct FCoreRegion { int64_t startTick = 0, endTick = 0; };
struct FCoreInstance {
  std::string intent = "buildup", style = "darkpsycore", mode = "note";
  FCoreKnobs knobs;
  std::vector<FCoreRegion> regions;            // "region" mode, in time order, never overlapping
  std::vector<std::string> off;                // parts switched off (sorted); a new intent starts with its main step only
  std::map<std::string, std::string> effects;  // role -> effect id on the track's chain
  bool bypass = false;
  // "region" mode: automation key -> fingerprint of the lane F-Core wrote. A lane whose points no longer match was
  // changed by hand: from then on it is the user's, and F-Core leaves it as it is (marked "fcoreHand").
  std::map<std::string, std::string> lanes;
  // "note" mode: the sampler's own values before F-Core set its knobs (nullopt = it was at its default), so a bypass
  // or removing F-Core puts the sampler back as it was (audit 2026-11-07, 6).
  std::map<std::string, std::optional<double>> samplerBase;
};
std::optional<FCoreInstance> fcoreOf(const Track& track);
// Puts FCore on the track (or changes the one there): mode "note" needs a sampler track (it moves to sampler
// version 3, pitched, so the note's end ends the sound and the pitch can glide); "region" goes on any track.
// Its effects go at the end of the chain. Throws DocumentError for a full chain, a missing track, an unknown
// intent or style, "note" on a track that is not a sampler.
void applyFCore(Document& doc, const std::string& trackId, const std::string& intent, const std::string& style,
                const FCoreKnobs& knobs, const std::string& mode);
// One plugin on several tracks at once (rule for every Core plugin, Diogo 2026-11-05): the selected tracks get the
// same intent, style and knobs in one step (one Undo), fitted to each track, and each one can then be changed
// alone. Fitted: "note" goes on the sampler tracks and "region" on the others (with `regions`, or the regions
// a track had); a low track (kick, bass, sub: by name) starts with Space and Width off, which blur the low end.
// Every track is checked first: nothing changes when one is refused (DocumentError names it).
struct FCoreApplied { std::string trackId, mode; std::vector<std::string> off; };
std::vector<FCoreApplied> applyFCoreToTracks(Document& doc, const std::vector<std::string>& trackIds, const std::string& intent,
                                             const std::string& style, const FCoreKnobs& knobs, const std::string& mode,
                                             const std::vector<FCoreRegion>& regions = {});
bool fcoreLowTrack(const Track& track);
void setFCoreKnobs(Document& doc, const std::string& trackId, const FCoreKnobs& knobs);
void setFCoreIntent(Document& doc, const std::string& trackId, const std::string& intent);  // Length: the intent's default; main step only
void setFCoreStyle(Document& doc, const std::string& trackId, const std::string& style);
void setFCoreIngredient(Document& doc, const std::string& trackId, const std::string& ingredient, bool on);  // one switch
void setFCorePreset(Document& doc, const std::string& trackId, const std::string& preset);  // its steps on, the others off
void setFCoreBypass(Document& doc, const std::string& trackId, bool bypass);
// "region" mode: a region [startTick, endTick) (at least 1/16 inside the song); one that overlaps another is refused.
void addFCoreRegion(Document& doc, const std::string& trackId, int64_t startTick, int64_t endTick);
int removeFCoreRegions(Document& doc, const std::string& trackId, int64_t startTick, int64_t endTick);  // touching; <= : all
// Takes FCore's effects, modulators and automation away (the sampler stays as it is).
void removeFCore(Document& doc, const std::string& trackId);
// The sentence for the FCore on the track ("" when there is none).
std::string fcoreSentence(const Track& track);
bool isFCoreEffect(const json& fx);
bool isFCoreModulator(const json& modulator);

// ---- samples for a style: the style's first, the others after, never hidden
struct FCoreSampleChoice {
  size_t index = 0;        // into the list given
  bool forStyle = false;   // marked with the style, or a loop at its tempo (or half / double)
  std::string warning;     // why it may give trouble here ("" = none): a loop out of tempo, no known BPM
};
// songBpm > 0: inside a song, a loop must fit that tempo; 0: anywhere in the style's range.
std::vector<FCoreSampleChoice> fcoreSamples(const std::vector<LibrarySample>& samples, const std::string& style, double songBpm = 0);

}  // namespace core::doc
