// Core project model (format "core-project", version 2).
// Plain data shared by the UI and the audio engine (AICORE never talks to the engine: it proposes calls
// to the app through the file bridge, app/control/bridge.hpp).
// Time is in ticks (PPQ = 960) so tempo changes never move notes off the grid.
#pragma once
#include "tempo_map.hpp"
#include "../dsp/param_spec.hpp"  // core::ParamSpec: defined in the DSP layer, with the plugins it describes
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include "../../third_party/json.hpp"

namespace core {

using json = nlohmann::json;

constexpr int kPpq = 960;
constexpr const char* kProjectFormat = "core-project";
constexpr int kProjectVersion = 2;
constexpr double kMinBpm = 40.0;
constexpr double kMaxBpm = 600.0;
constexpr int kMaxTrackIdLength = 40;  // ids are [A-Za-z0-9_-]{1,40}

// A roll repeats one note `count` times inside its length (ratchet / snare roll),
// with the velocity moving from velocityFrom to velocityTo.
struct Roll {
  int count = 0;  // 0 or 1 = no roll
  double velocityFrom = 1.0;
  double velocityTo = 1.0;
};

struct Note {
  int64_t tick = 0;     // start, relative to the start of the pattern
  int64_t length = 240; // ticks
  int pitch = 36;
  double velocity = 0.8;
  Roll roll;
};

struct EnvelopePoint {
  int64_t tick = 0;     // absolute song tick
  double value = 0.0;
};

// Automation of one parameter, linear between points (before the first and after the last: their values).
struct Envelope {
  // An instrument parameter ("cutoff"), the mixer ("gainDb", "pan"), or an insert effect's parameter by
  // the effect's id: "fx:<id>:<param>" (an effect that is no longer there: the envelope is kept, silent).
  std::string param;
  std::vector<EnvelopePoint> points;
};

// A pattern placed on the song: it starts at startTick, repeats every patternTicks, and stops
// starting notes at startTick + lengthTicks (a note already playing ends normally).
struct Clip {
  int64_t startTick = 0;
  int64_t lengthTicks = 4 * 4 * kPpq;
  int64_t patternTicks = 4 * kPpq;
  int64_t offsetTicks = 0;  // the clip starts this far into its pattern (0 = at its beginning)
  std::vector<Note> notes;
};

// A piece of an audio file placed on the song (instrument "audio"). The start follows the musical grid
// (ticks); where it starts in the file and how long it plays are in seconds, so tempo changes move the
// clip but never stretch or re-pitch it.
struct AudioClip {
  int64_t startTick = 0;
  std::string path;            // UTF-8, absolute (the app resolves the project's assets)
  double offsetSeconds = 0;    // where in the file the clip starts
  double durationSeconds = 1;
  double gainDb = 0;
  double fadeInSeconds = 0, fadeOutSeconds = 0;
  bool mute = false;
  // VOICE STUTTER's processors (0 = off: the file as it is). pitchSemitones moves the pitch and keeps the length
  // (overlapping 30 ms grains read at the new rate); freezeSeconds > 0 holds that much of the file from offsetSeconds
  // with 40 ms grains drifting slowly through it, for the clip's whole duration (a vocal freeze).
  double pitchSemitones = 0, freezeSeconds = 0;
};

// A file played by the sampler on the keys loKey..hiKey; rootKey is the note it was recorded at.
struct SampleZone {
  std::string path;            // UTF-8, absolute (the app resolves it)
  int rootKey = 60, loKey = 0, hiKey = 127;
  // A sustain loop in the file's seconds: while the note is held the voice plays [loopStart, loopEnd) again and
  // again (a short crossfade at the seam), then the release ends it. loopEnd 0 = no loop (the default).
  double loopStart = 0, loopEnd = 0;
};
constexpr double kMinLoopSeconds = 0.002;

// M7a: a copy of a track's signal into a bus. Post-fader (after gain and pan) unless pre: then right
// after the inserts, before the fader. A muted track sends nothing.
struct Send {
  std::string id;      // unique in the track ("s1", ...)
  std::string to;      // a bus track's id
  double gainDb = 0;   // -60..12
  bool pre = false;
};
constexpr int kMaxSends = 8;

// M8a: a tempo-synced LFO on one parameter. Its phase comes from the song position (ticks), so it is the
// same after a seek or a loop, live and in an export. Added to the parameter's value (after automation).
struct Modulator {
  std::string id;              // unique in the track
  // A built-in instrument parameter, "gainDb", "pan", or (FCore) an insert effect's parameter by the effect's
  // id, "fx:<id>:<param>" (built-in effects only; an effect that is not there: kept, silent).
  std::string param;
  std::string shape = "sine";  // sine | triangle | square | saw (down) | ramp (up) | random (held per cycle)
                               // | envelope (M8b: from each note of the track, 0 -> 1 -> 0)
                               // | rise (FCore: from each note, 0 -> 1 over rateBeats, then 0 again or held)
                               // | steps (a step sequence: one held value per step, looping on the song's grid)
  double rateBeats = 1;        // beats per cycle (0.25 = a sixteenth), 1/64..64; rise: its length, 1/64..256;
                               // steps: the length of ONE step (the cycle is rateBeats x the number of steps)
  bool on = true;              // off: it moves nothing (its depth is kept for when it is on again)
  double depth = 0;            // in the parameter's units; for a parameter in Hz, in octaves (-8..8)
  double phase = 0;            // 0..1, where the cycle starts at tick 0
  double attackMs = 0;         // envelope: rise to 1 after each note-on, 0..2000 ms
  double decayMs = 150;        // envelope: then down to -60 dB (0.001) in this time, 1..10000 ms
  double curve = 1;            // rise: (t)^curve, 0.25..4 (1 straight, 2 slow then fast, 0.5 fast then slow)
  bool hold = false;           // rise: at its end it stays at 1 (an intro opening) instead of dropping to 0
  // envelope / rise: counted from the notes of this other track instead of the track's own (designed ducking:
  // the bass's volume dips at every kick, with this envelope's shape). "" = its own notes. A muted track counts.
  std::string source;
  std::vector<double> steps;   // steps: 1..kMaxModSteps values, each -1..1 (times depth)
};
constexpr int kMaxModulators = 16;
constexpr int kMaxModSteps = 16;
const std::vector<std::string>& modulatorShapes();

struct Track {
  std::string id;
  std::string name;
  std::string instrument;               // "kick" | "wave" (W-Core) | "hats" | "drums" (kick 36 + hats 42/44/46, ride 51, shaker 70) | "sampler" | "plugin" (VST3, for A/B checks) | "audio" | "bus"
  std::map<std::string, double> params; // instrument parameters (unknown keys rejected)
  // Version of the built-in instrument's sound. An update that changes the sound gets a new number and
  // keeps the old one playable, so old songs never change silently. (Same rule as plugin.version.)
  int instrumentVersion = 1;
  double gainDb = 0.0;
  double pan = 0.0;
  bool mute = false;
  bool solo = false;
  int64_t patternTicks = 4 * kPpq;      // the pattern loops every patternTicks
  int64_t lengthTicks = 0;              // track plays this long (0 = whole song); v1 "bars"
  std::vector<Note> notes;
  std::vector<Envelope> envelopes;
  // When not empty, the track plays these clips instead of notes/patternTicks/lengthTicks (one
  // pattern looping from the song start). The native app always sends clips.
  std::vector<Clip> clips;
  std::vector<AudioClip> audioClips;    // instrument == "audio"; overlapping clips add up
  std::vector<SampleZone> samples;      // instrument == "sampler": the first zone that holds the key plays
  // instrument == "plugin": which plugin plays this track and its saved state. Kept as data, so a
  // project opens even when the plugin is not installed ("missing instrument"): notes stay, sound is silent.
  // {format: "vst3" | "clap" | "core", id: "<class id>", name, vendor, version: "<x.y.z used when saved>",
  //  state: "<base64 chunk>", bypass}. Opening with another installed version is reported, never silent.
  json plugin;
  // Insert effects, in order (pre-fader): {type: "builtin", name, version, params: {...}, bypass} or
  // {type: "plugin", plugin: {...same as above}, bypass}. Built-in ones play (M4); plugins are kept as data.
  json effects = json::array();
  // M7a routing. Where the track's signal goes after its fader: "" = the master, or a bus track's id. An
  // instrument "bus" plays what its inputs (outputs and sends of other tracks) give it, through its own
  // inserts and fader, to its output. Buses may feed buses; the routing never loops.
  std::string output;
  std::vector<Send> sends;
  std::vector<Modulator> modulators;  // M8a
  // instrument == "wave" (W-Core, version 2): its modulation, {"layers": [{"lfos": [...], "links": [...]}]}
  // (dsp/wave_mod.hpp). null = none.
  json synth;
  // What loading had to change and could not carry over (a version-1 W-Core automation on a knob that is a link now).
  std::vector<std::string> loadNotes;
};

struct Project {
  std::string name;
  double bpm = 145.0;                   // at the song's start
  std::vector<TempoChange> tempoMap;    // later changes: {tick > 0, bpm}, sorted (empty = one tempo)
  int64_t lengthTicks = 16 * 4 * kPpq;  // song length (export length)
  double masterGainDb = 0.0;
  json masterEffects = json::array();  // the master chain, after the master gain (same shape as Track::effects)
  std::vector<Envelope> masterEnvelopes;  // "gainDb" (the master gain) and "fx:<id>:<param>" of the master chain
  std::vector<Track> tracks;
  // Effect ids with ':' renamed on load ("old -> new"): reported, never dropped in silence.
  std::vector<std::string> renamedEffects;
  // What an older version's conversion could not carry over ("track: what"): reported, never dropped in silence.
  std::vector<std::string> conversionNotes;

  const Track* findTrack(const std::string& id) const;
  Track* findTrack(const std::string& id);
};

// Parameter catalogue per instrument: default, min, max. Shared by validation,
// the command list for the AI tools and the DSP. core::ParamSpec itself is defined in dsp/param_spec.hpp.
const std::vector<ParamSpec>& instrumentParams(const std::string& instrument);
int currentInstrumentVersion(const std::string& instrument);  // built-in DSP versions
// Built-in effects: their parameters (in the order Effect::params uses) and DSP versions.
const std::vector<ParamSpec>& effectParams(const std::string& effect);
int currentEffectVersion(const std::string& effect);
std::vector<std::string> effectNames();
// One effect chain validated: known built-in names, parameters filled and clamped, plugins checked.
json effectsFromJson(const json& value);
bool isMixParam(const std::string& param); // gainDb, pan
std::vector<std::string> instrumentNames();
// Plugin, audio and bus tracks have no built-in instrument parameters.
bool hasNoInstrumentParams(const std::string& instrument);
// M7a: every output and send goes to a bus of this project, never to itself, and nothing loops. Throws.
void validateRouting(const Project& project);

json toJson(const Project& project);
// Validates, fills defaults and migrates older versions (v1 = the JS step patterns).
// Throws std::runtime_error with a readable message.
Project fromJson(const json& value);
// One track in the v1 shape of the JS editor (kind synth|drums, pattern in steps) as v2 JSON.
json trackFromV1(const json& v1Track);
// One v2 track JSON validated on its own (for track.add / track.set).
Track trackFromJson(const json& value);

double samplesPerTick(double bpm, double sampleRate);

} // namespace core
