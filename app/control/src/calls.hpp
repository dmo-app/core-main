// Core as programming (Diogo, 2026-11-05): every device of Core is a FUNCTION with a signature (its args are its
// knobs, with ranges), a sample is a VALUE and a track's role (kick, bass, lead, pad...) is its TYPE. A preset is a
// saved call.
//
// This is all of the AI that lives in Core. Core stays focused on making music by hand; the AI (chat, model, guides,
// tastes, sample search) lives in AICORE. What Core gives it:
//   - the functions it can call now, with their signatures, and the song in a few lines (coreState, written to a
//     file by the app: control/bridge.hpp);
//   - checking a proposal of calls against the signatures and the song (validateProposal): unknown functions, knobs,
//     values out of range, missing sections are left out with the reason; types and styles that do not fit give a
//     warning, never a refusal (the style guides, it never blocks);
//   - applying it when Diogo presses Apply (applyProposal): the app commits it as ONE undo step, and nothing outside
//     the calls can change.
// A proposal: {"id": "...", "summary": "...", "calls": [{"fn": "rack:buildup/roll", "on": "Acid lead"},
//   {"fn": "sample", "on": "Kick", "sample": "s:1a2b3c4d5e"}]}
//
// The functions today: Core's built-in effects ("fx:<name>", their real ranges, and "preset": one of their presets),
// the Style FX ("rack:<id>": Core's own devices with their presets), "sample" (a library sample
// on a track), "set" (the knobs or a preset of an effect already on a track or the master: settings and mastering),
// "mix" (a track's volume and pan, the master's volume) and "open" (opens a track's effects, its instrument, or one
// effect's window, after Apply), and Core's own plugins: "wcore" (W-Core · Synth: preset, mood, knobs), "fcore"
// (F-Core · FX: intent, style, mode, knobs, preset) and "rcore"/"dcore" (a shared reverb or echo place by preset). Each
// changes the one already on the track when there is one. P-Core · Patterns and S-Core · Stages join the same list
// through the same signature: nothing per plugin. Core has no VST (2026-11-08).
// Contract: docs/contracts/core-calls.md.
#pragma once
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "document.hpp"
#include "samples.hpp"

namespace core::control {

using json = nlohmann::json;

constexpr int kCallsFormatVersion = 1;
constexpr const char* kAiLabelPrefix = "AI: ";  // the undo label of an applied proposal

// One argument of a function: a knob with its range ("roll" 0..1, "mix" 0..1, "cutoff" 20..20000 Hz).
struct FunctionArg {
  std::string id, name, hint, unit;
  double min = 0, max = 1, def = 0;
};

struct CoreFunction {
  std::string id;           // "fx:reverb", "rack:buildup/roll", "sample"
  std::string family;       // "effect" | "rack" | "sample" (later "fcore", "pcore", "stages")
  std::string name;         // as Core shows it
  std::string role;         // the kind of sound it is made for ("lead", "bass layer", "build-up"); "" = any
  std::string description;
  int version = 1;
  std::vector<std::string> styles;  // the styles it is made for (rack style keys); empty = any
  std::vector<std::string> tags;
  std::vector<FunctionArg> args;
  bool instrument = false;  // replaces the track's instrument (a bass Style FX)
  bool rises = false;       // rises over a section (F-Core): "section" makes the rise
  bool native = false;      // a Style FX: Core's own devices with their presets (no macros)
  bool available = true;
  std::string unavailable;  // why not
  std::vector<std::string> presets;  // an effect's presets by name ("preset" in a call); "set" takes any effect's
  std::map<std::string, std::vector<std::string>> options;  // text choices ("intent", "style", "mode", "mood"): the allowed values
  json extra = json::object();       // written as is into the function's state (preset details, F-Core's presets by intent)

  const FunctionArg* findArg(const std::string& id) const;
};

// The functions this Core can call now: the built-in effects, every Style FX (Core's own devices and presets,
// doc::applyNativeRack, no macros), Core's own plugins and "sample". Built every time: a new Style FX file changes
// it at once.
std::vector<CoreFunction> buildFunctions();
// By id, or by the part after the family ("buildup/roll", "reverb") when only one function has it.
const CoreFunction* findFunction(const std::vector<CoreFunction>& functions, const std::string& id);

// "Dark psycore", "hi-tech", "Full-on"... -> the rack style key ("darkpsycore", "hitech", "slambient", "psytrance");
// "" when it says nothing about a style.
std::string styleKey(const std::string& text);
// The song's style key (its template's style), "" when not known.
std::string songStyleKey(const doc::Document& song);

// The type of a track: lead, pad, vox, fx (from its name), else kick, bass, hat, openhat, accent, perc, impact,
// riser (as the sample library says), else from a built-in instrument (kick, bass, hat, drums); "" when nothing says.
std::string trackType(const doc::Track& track);
// Whether a function's role suits a track type (an unknown role or type always suits).
bool roleFits(const std::string& functionRole, const std::string& type);

// The short id of a library sample ("s:1a2b3c4d5e"), and the sample of one, a full sha256 or a path (null if none).
std::string sampleKey(const doc::LibrarySample& sample);
const doc::LibrarySample* findSampleKey(const doc::SampleIndex& index, const std::string& key);

// What the AI reads (core-state.json): {format: "core-state", version, functions: [{id, family, name, role,
// description, version, styles, tags, instrument, rises, available, unavailable, args: [{id, name, hint, unit, min,
// max, default}]}], song: {name, style, bpm, bars, sections: [{name, startBar, endBar}], tracks: [{id, name, type,
// sound, devices: [function ids]}]}, samples: {count, roles: {role: n}} or null}. Never the library itself.
json functionsJson(const std::vector<CoreFunction>& functions);
json songJson(const doc::Document& song, const doc::SampleIndex* samples);
json coreState(const std::vector<CoreFunction>& functions, const doc::Document& song, const doc::SampleIndex* samples);

// ---- a proposal: calls, checked

struct Call {
  const CoreFunction* fn = nullptr;
  std::string trackId, trackName;  // "" = Diogo chooses when applying; "master" for the master (effects only)
  std::map<std::string, double> args;  // checked: known knobs, in range
  std::string sectionName;             // a rise over this section (rising functions only)
  int64_t startTick = 0, endTick = 0;
  std::string sample;                  // "sample": the library sample's sha256
  std::string sampleName;
  std::string preset;                  // an effect preset's name ("fx:..." and "set")
  std::string effectId, effectName;    // "set": the effect already on the chain (its id, and what it is)
  std::string open;                    // "open": "effects", "instrument", "patterns", "mixer" or an effect id
  std::map<std::string, std::string> texts;  // text choices, checked against the function's options
  std::vector<std::string> warnings;   // types that do not fit: kept, shown
};

struct Proposal {
  std::string id, summary;
  std::vector<Call> calls;
  std::vector<std::string> problems;  // why a call or a value was left out (shown, never applied)
  json raw;
  bool needsTrack() const;            // calls without a track (Diogo picks one when applying)
  std::vector<std::string> warnings() const;  // of every call
};

// Checks a proposal against the functions and the song: known functions this computer can run, known knobs in
// range, tracks and sections of the song, samples of the library. Types that do not fit give warnings. Never throws.
Proposal validateProposal(const json& raw, const std::vector<CoreFunction>& functions, const doc::Document& song,
                          const doc::SampleIndex* samples);
// One line per call, for the window: "Build-up: roll on Acid lead (roll 0.7, speed 0.5), rising over Build-up".
std::string describeCall(const Call& call);

// What applying needs from the app.
struct ApplyEnv {
  const doc::SampleIndex* samples = nullptr;
};
// Makes every call of the proposal on `song` (the caller commits it as one undo step). chosenTrack: for the calls
// without a track ("master" for the master). Throws doc::DocumentError when a call is refused: then nothing
// should be committed. Returns one line per call, as applied.
std::vector<std::string> applyProposal(doc::Document& song, const Proposal& p, const std::string& chosenTrack, const ApplyEnv& env);

// What the window opens once the proposal is applied (the "open" calls): {track id ("master" for the master),
// "effects" | "instrument" | an effect id}. Opening changes nothing in the song.
struct OpenRequest { std::string trackId, what; };
std::vector<OpenRequest> opensOf(const Proposal& p, const std::string& chosenTrack);

}  // namespace core::control
