// Wave Core (W-Core): Core's own lead, pad and drone synth (the engine's "wave" instrument, docs/historico/w-core-wave.md).
// Version 2 (2026-11-08, the modular synth): sources (the waves, noise, grain), the filter, ENV 1 (the volume), and a
// modulation system per layer: ENV 2 and 3, LFO 1..4, Velocity, Note and four Macros, linked to the knobs, the pitch,
// the volume, the pan and the detune (Instrument::synth, engine/src/dsp/wave_mod.hpp). A song of version 1 is converted
// when it is read (migrateLegacyWave) and plays the same.
// The mode (Lead = one note at a time with glide, Pad = up to 8 notes) comes with the preset, or the Lead/Pad switch.
// Delay, reverb, gate and grit are never inside: a mood puts Core's own effects after the synth.
// Every preset works in every style (style guides, never blocks). Presets are measured against real synth samples.
#pragma once
#include <filesystem>
#include <optional>
#include <map>
#include <string>
#include <vector>
#include "document.hpp"
#include "sound_layers.hpp"

namespace core::doc {

constexpr const char* kWaveInstrument = "wave";

// The layers (param = the engine parameter, name, plain-words info).
const std::vector<SoundLayer>& waveLayers();

struct WavePreset {
  const char* id;     // "sunrise-screamer"
  const char* name;   // "Sunrise Screamer"
  const char* style;  // darkpsycore | psytrance | hitech | slambient
  const char* info;
  std::map<std::string, double> values;  // every knob of layer 1 except lfo3Hz
  double driftBars = 1;                  // LFO 3 (the slow drift): one movement every N bars, lfo3Hz follows the tempo
  const char* mood = "dry";              // the mood it was made with (Echo 3/16, Cathedral...)
  const char* category = "lead";         // what it is: bass | lead | acid | pluck | stab | pad | drone (the menu's groups)
  json synth = json();                   // its movements (layer 1's {"lfos", "links"}), null = none
};
// The categories in the order the presets menu shows them, and their names there.
const std::vector<std::pair<std::string, std::string>>& waveCategories();
const std::vector<WavePreset>& wavePresets();
const WavePreset* findWavePreset(const std::string& id);

// Moods: Dry, Echo 3/16, Slap 1/8, Cathedral, Gated, Grit. Their effects go after W-Core on the track, marked with
// kWaveMoodKey, so the next mood replaces them. Leads get a low cut so the bass keeps the bottom.
constexpr const char* kWaveMoodKey = "wcoreMood";
const std::vector<SoundMood>& waveMoods();
const SoundMood* findWaveMood(const std::string& id);
std::string waveMoodOf(const Track& track);  // "" = none
void setWaveMood(Document& doc, const std::vector<std::string>& trackIds, const std::string& moodId);

// The drift speed (LFO 3, free) for one movement every `bars` bars (4/4) at `bpm`.
double waveDriftHz(double bars, double bpm);
// The drift length in bars a track's lfo3Hz is at the song's tempo (for the window).
double waveDriftBars(const Document& doc, const Track& track);
void setWaveDriftBars(Document& doc, const std::string& trackId, double bars);

// Turns a track that plays notes into W-Core (keeps its notes, clips, mix and its own effects), or keeps it if it
// already is; then sets the preset's values and its mood, if any. Several tracks: all or none, one undo step, then
// each track is its own W-Core to adjust alone. Throws DocumentError for an audio track or a bus.
// The sound a new Bass track starts with (a preset of the Bass group).
constexpr const char* kWaveStartBass = "saw-roller";
void setWave(Document& doc, const std::string& trackId, const std::string& presetId = "");
void setWave(Document& doc, const std::vector<std::string>& trackIds, const std::string& presetId = "");

// Makes the track's instrument the built-in `name` (a rack that set the instrument goes;
// curves on the old instrument's own parameters go; volume, pan and effects stay). No-op if it already is.
// Your own sounds (Diogo's plan: personal presets, compare, save): a whole W-Core sound, every knob + movements +
// drift + mood. Saved as one JSON file each ("core-wave-sound" v2; v1 files are converted) in the user's folder (Core: %APPDATA%\Core\Presets\wave).
// Layers (Diogo 2026-11-07: "várias waves com diferentes filters", "para podermos mexer neles todos"): up to four
// complete W-Core voices on one track, played by the same notes and added together. Layer 0 is the classic W-Core
// (its knobs keep their names); layers 1..3 are the same knobs under "l2_", "l3_", "l4_" and a switch "l2_on"...
// (the engine's WaveStack; the switch is 0 = not added, 1 = on, 0.25 = added and off). Lead/Pad and Glide are layer
// 0's for all. Each layer has its own movements (synth.layers[n]). Each layer's Grain can read its own sound (the
// track's sample zone whose lowest key is the layer's number).
constexpr int kWaveLayers = 4;
std::string waveLayerParam(int layer, const std::string& param);  // layer 0: param itself
bool isWaveLayerParam(const std::string& param);                   // "l2_tone", "l3_on"...
bool waveLayerOn(const Track& track, int layer);                   // layer 0 is always on
bool waveLayerExists(const Track& track, int layer);               // added (on or switched off), layer 0 always
// A new layer: the first one not added yet, made a copy of layer 0 (so it starts from the sound you have), switched on.
// Returns its number (1..3); throws DocumentError when all four are in use or the track is not W-Core.
int addWaveLayer(Document& doc, const std::string& trackId);
void setWaveLayerOn(Document& doc, const std::string& trackId, int layer, bool on);  // the light: kept as it is
void removeWaveLayer(Document& doc, const std::string& trackId, int layer);         // off, back to defaults, no sound
// A layer's Grain sound (null = the wave itself; layers 1..3 without one read layer 0's).
void setWaveLayerSound(Document& doc, const std::string& trackId, int layer, const SampleRef* zone);
const SampleRef* waveLayerSound(const Track& track, int layer);
// The section lights (Diogo 2026-11-07, "verde on, vermelho off"): each card of each wave can be left out, its knobs
// kept. Sections: colour, grain, filter, motion, osc2, tables, shape (Filter+), lfo1, lfo2.
bool isWaveSection(const std::string& section);
std::string waveSectionParam(int layer, const std::string& section);  // "sec_grain", "l2_sec_grain"
bool waveSectionOn(const Track& track, int layer, const std::string& section);
void setWaveSectionOn(Document& doc, const std::string& trackId, int layer, const std::string& section, bool on);

// The modulation (version 2). A fresh W-Core (a new track, or one turned into W-Core) has version 1's movements:
// ENV 2 opens Tone by 1.5 octaves (more with a harder note), the velocity sets the volume, LFO 3 drifts Tone and the
// detune a little.
void makeFreshWave(Instrument& instrument);
json defaultWaveSynth();
// The track's movements as a patch ({"layers": [...]}, never null here) and its layer n alone.
json waveSynth(const Track& track);
json waveSynthLayer(const Track& track, int layer);  // {"lfos": [...], "links": [...]}
// Replaces the whole modulation (checked: unknown sources, targets or shapes throw DocumentError).
void setWaveSynth(Document& doc, const std::string& trackId, const json& synth);
void setWaveSynthLayer(Document& doc, const std::string& trackId, int layer, const json& layerSynth);
// One link of a layer: from a source (lfo1..4, env1..3, velocity, note, macro1..4) to a knob ("tone", "wave"...) or
// "pitch", "volume", "pan", "detune", with an amount in the target's units (octaves for tone). Returns its id.
std::string addWaveLink(Document& doc, const std::string& trackId, int layer, const std::string& source,
                        const std::string& target, double amount);
// Changes a link: any of amount, source, target, aux, auxDepth, unipolar, on.
void setWaveLink(Document& doc, const std::string& trackId, int layer, const std::string& linkId, const json& changes);
void removeWaveLink(Document& doc, const std::string& trackId, int layer, const std::string& linkId);
// LFO k (0..3) of a layer: any of shape (sine, triangle, saw, square, random, points), mode (song, note, once),
// sync (true: Rate in beats, false: Hz) and points ([[x, y, curve], ...]).
void setWaveLfo(Document& doc, const std::string& trackId, int layer, int lfo, const json& changes);

struct WaveSound {
  std::string name;
  std::map<std::string, double> values;  // every knob of layer 1 except lfo3Hz
  json synth;                            // layer 1's movements ({"lfos", "links"}); null = none
  double driftBars = 1;
  std::string mood = "dry";
  std::optional<SampleRef> grain;        // layer 1's Grain sound (a library sample), none = the wave itself
  // Layers 2-4's Grain sounds (index 0 = layer 2). Saved as version 2 of the format; a sound without them is
  // still written as version 1, so an older Core reads it (audit 2026-11-07 #12: only layer 1's sound was kept).
  std::optional<SampleRef> layerGrains[3];
  std::filesystem::path file;            // where it was read from ("" = not saved)
};
WaveSound waveSoundOf(const Document& doc, const Track& track);
json waveSoundToJson(const WaveSound& sound);
WaveSound waveSoundFromJson(const json& j);  // throws DocumentError; out-of-range values are clamped
// Puts the sound on every track given (they become W-Core), all or none. A knob it does not list goes to its default.
void setWaveSound(Document& doc, const std::vector<std::string>& trackIds, const WaveSound& sound);
std::vector<WaveSound> loadWaveSounds(const std::filesystem::path& folder, std::vector<std::string>* problems = nullptr);
std::filesystem::path saveWaveSound(const std::filesystem::path& folder, const WaveSound& sound);  // never overwrites

void switchToBuiltinInstrument(Document& doc, const std::string& trackId, const std::string& name, const char* what);

}  // namespace core::doc
