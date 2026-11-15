// What Core's own synths share (W-Core today): a synth shown as LAYERS of knobs, each with a plain-words line,
// the named shapes of the wave knob, and moods (a few knobs plus Core's own effects after the synth).
// Every knob and preset has a plain-words line: the window shows it behind a small "i".
#pragma once
#include <map>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

struct SoundKnob {
  const char* param;  // the engine's parameter
  const char* name;   // as the window shows it
  const char* info;   // what it changes in the sound, in plain words
};
struct SoundLayer {
  const char* id;
  const char* name;
  const char* info;
  std::vector<SoundKnob> knobs;
};

// The named shapes of a wave knob (0..4). Between two names it is a blend of both.
struct WaveShape {
  const char* name;
  double value;
  const char* info;
};
const std::vector<WaveShape>& waveShapes();
// The name at a wave value ("Warm"), or "Warm → Hollow" between two.
std::string waveShapeName(double wave);

// A mood on top of any preset: a few knobs, and effects after the synth on the track.
struct SoundMoodEffect {
  const char* name;  // a built-in effect
  std::map<std::string, double> params;
};
struct SoundMood {
  const char* id;     // "eerie"
  const char* name;   // "Cold Hollow"
  const char* feel;   // the plain word for it ("eerie")
  const char* info;
  std::map<std::string, double> knobs;
  std::vector<SoundMoodEffect> effects;
};
// A mood's knobs as a layer: the previous mood's knobs go back to their values from before it (a knob turned by
// hand since stays), then these are set and remembered in the track's extra[moodKey + "Knobs"].
void setMoodKnobs(Document& doc, const std::string& trackId, const std::string& moodKey, const std::map<std::string, double>& knobs);

}  // namespace core::doc
