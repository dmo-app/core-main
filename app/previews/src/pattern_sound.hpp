// The sound of one pattern of a track, for the waveform drawn inside its clips on the arrangement.
// The engine's own instruments and effects play the pattern offline (the sampler with its files, the built-in
// synths, the track's effects and the buses it feeds), so the picture is the real shape and level of the sound,
// not a guess. The fader is left out (the window draws it as a gain, so moving it never renders again), and so
// are the song-position automation curves and other tracks (a sidechain hears silence). A plugin instrument
// cannot play here: its pattern has no sound and the clip keeps its notes.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "document.hpp"

namespace core { class AssetStore; }

namespace core::preview {

using json = nlohmann::json;

// What one render needs: the engine's project with only the tracks that play a part (the pattern's track, the
// buses it goes to, the tracks its sidechains listen to) and the pattern placed once, after a short lead-in
// (the end of the pattern before it, so a tail from the previous repeat is in the picture too).
struct Request {
  json projection;              // core-project v2
  std::string trackId;
  int64_t patternTicks = 0;     // the pattern's length
  int64_t leadInTicks = 0;      // played first and not kept
  std::string key;              // same key = same sound (the projection's hash and size)
};

// Null when the pattern has no sound to show: no such track or pattern, no notes, an audio track or a plugin.
std::unique_ptr<Request> makeRequest(const doc::Document& doc, const std::string& trackId, const std::string& patternId,
                                     const std::filesystem::path& assetRoot = {});

// Peaks and RMS of the pattern's sound in runs of framesPerPeak frames (louder channel), from the pattern's
// start to its end. Empty when it is silent (a plugin, a muted send...) or the engine refused the project.
struct Sound {
  double sampleRate = 48000;
  int framesPerPeak = 128;  // 2.7 ms at 48 kHz: the detail of a zoomed-in hat
  std::vector<float> peaks, rms;
  double seconds() const { return sampleRate > 0 ? (double)peaks.size() * framesPerPeak / sampleRate : 0; }  // never inf (deep hunt U-5)
  bool empty() const { return peaks.empty(); }
};

// cancel (optional): checked every block; a cancelled render returns an empty sound.
Sound render(const Request& request, AssetStore& assets, const std::atomic<bool>* cancel = nullptr, double sampleRate = 48000);

} // namespace core::preview
