// Training clips: short excerpts (20-30 s, whole bars) made from a style template, rendered by the engine and
// written with everything Core knows about them, so a model (ACE, through AICORE) can learn from them without
// anyone writing captions by hand. Diogo, 2026-11-05: Core only makes the clips and their facts; AICORE turns
// them into its training template. No AI and no audio analysis by a model here: every fact comes from how the
// clip was built (template, seed, Randomize, the catalogue of the samples it plays).
// Contract: docs/contracts/core-training-clip.md.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include "document.hpp"
#include "samples.hpp"
#include "templates.hpp"

namespace core::doc {

constexpr const char* kTrainingClipFormat = "core-training-clip";
constexpr int kTrainingClipVersion = 1;

// What a clip holds:
//   kbbb    kick and bass only (the KBBB groove and its turns)
//   groove  the drums and the bass: kick, bass, hats, open hats, percussion, accents
//   full    every track of the section (the mix as the template plays it)
const std::vector<std::string>& trainingClipKinds();

struct TrainingClipRequest {
  std::string kind = "kbbb";
  uint64_t seed = 1;                 // the same template, kind, seed and library give the same clip
  double minSeconds = 20, maxSeconds = 30;
  bool randomize = true;             // Randomize with the seed (the groove differs from clip to clip)
  // Its axes (randomizeAxes); empty = A-F and "sound" (K: the built-in kick and bass sounds). "tempo" (J) only when named.
  std::vector<std::string> axes;
  // The catalogue: what the facts say about the samples the clip plays (their role, styles, tags, tail).
  const SampleIndex* library = nullptr;
  // Other samples of the library on the tracks that take one (any that fits the role, by seed). Off: the template's own
  // samples play (the templates are produced with chosen samples, 2026-11-05 #87/#111).
  bool swapSamples = false;
};

struct TrainingClip {
  std::string id;                    // "<template>-<kind>-s<seed>": the file names
  Document doc;                      // the song, the tracks outside the kind muted
  int64_t startTick = 0, endTick = 0;  // the region to render
  int bars = 0;
  double seconds = 0;
  json facts;                        // the .json written next to the .wav (format core-training-clip v1)
};

// Builds one clip: the template's song, the seed's samples and groove, a high-energy section (energy 3 or more;
// the seed picks which and where in it), and a region of whole bars in [minSeconds, maxSeconds] (multiples of 4
// bars when one fits, the closest to the middle). Throws DocumentError (unknown kind, a template with no tracks of
// the kind, no region that fits).
TrainingClip buildTrainingClip(const SongTemplate& tmpl, const TrainingClipRequest& request);

// One English paragraph from the facts ("Dark psycore KBBB loop, 225 BPM, bass root F, 24 bars (25.6 s). ...").
std::string describeTrainingClip(const json& facts);

// After the render: a short fade at both ends of the WAV (PCM 16/24/32 or float 32), in place, so a cut on the
// bar line never clicks. Throws DocumentError (not a WAV Core can write).
void fadeWavEdges(const std::filesystem::path& wav, double fadeMs = 10);

// Checks a rendered clip: not silent, not clipping, loud enough. Fills facts["audio"] (peakDb, loudDb) and returns
// "" when it is good, else why it is not (English). missing: what the engine said did not load (audio):
// a clip with any is never good (a silent instrument would teach the model a wrong sound).
std::string checkTrainingClip(const std::filesystem::path& wav, json& facts, const json& missing);

} // namespace core::doc
