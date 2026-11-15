// Core Glitch · Sample Path in the document: a "samplepath" track (engine/src/dsp/sample_path.hpp). Its map (slices,
// nodes, links, patterns A..D, FX) is the instrument's "synth"; the sound of sampler i (0..3) is the sample with
// loKey == i. Every change goes through these, inside an edit (one undo step each).
#pragma once
#include <string>
#include "document.hpp"
#include "../../../engine/src/dsp/sample_path_patch.hpp"

namespace core::doc {

constexpr const char* kGlitchInstrument = "samplepath";
constexpr int kGlitchSamplers = spath::kLayers;

bool isGlitch(const Track& track);
// The map, read from the track (an empty map when there is none). Throws DocumentError for a track that is not one.
spath::Patch glitchPatch(const Track& track);
void setGlitchPatch(Document& doc, const std::string& trackId, const spath::Patch& patch);
// The sound of one sampler (null: none). Setting one on a sampler with no slices cuts it in `slices` even parts and
// lays the brief's path on them (six parts: 1-2-3-4-5-6-1, 2 also to 5, 3 twice up a fifth, 4 reversed); `seconds`
// is the sound's length. A sampler that has slices keeps its map (a new sound, the same path). null removes it.
void setGlitchSound(Document& doc, const std::string& trackId, int sampler, const SampleRef* zone, double seconds, int slices = 6);
const SampleRef* glitchSound(const Track& track, int sampler);
// A new Core Glitch track's map: nothing yet (the samplers fill when a sound is set).
json freshGlitchSynth();

}  // namespace core::doc
