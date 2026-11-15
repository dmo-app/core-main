// Waveform overviews of the project's audio files, computed in the background and kept in memory
// (a cache: rebuilt from the files whenever needed, never saved).
#pragma once
#include <cmath>
#include <functional>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include "core/assets.hpp"
#include "pattern_sound.hpp"

namespace core::app {

struct Waveform {
  double sampleRate = 48000;
  int framesPerPeak = 512;
  std::vector<float> peaks;  // max |sample| of both channels for each run of framesPerPeak frames
  std::vector<float> rms;    // the louder channel's RMS over the same runs (the waveform's body)
  float peakAt(double seconds) const {
    const int64_t i = (int64_t)(seconds * sampleRate / framesPerPeak);
    return i >= 0 && i < (int64_t)peaks.size() ? peaks[(size_t)i] : 0.0f;
  }
  // The loudest peak from one time to another (at least the one at 'from').
  float peakBetween(double from, double to) const {
    const int64_t n = (int64_t)peaks.size();
    const int64_t a = std::max<int64_t>(0, (int64_t)(from * sampleRate / framesPerPeak));
    const int64_t b = std::min<int64_t>(n - 1, (int64_t)(to * sampleRate / framesPerPeak));
    float peak = 0;
    for (int64_t i = a; i <= b; ++i) peak = std::max(peak, peaks[(size_t)i]);
    return peak;
  }
};

// The mean RMS from one time to another (the body a waveform draws under its peaks).
inline float rmsBetween(const Waveform& w, double from, double to) {
  const int64_t n = (int64_t)w.rms.size();
  if (n == 0) return 0.0f;
  const int64_t a = std::max<int64_t>(0, (int64_t)(from * w.sampleRate / w.framesPerPeak));
  const int64_t b = std::min<int64_t>(n - 1, (int64_t)(to * w.sampleRate / w.framesPerPeak));
  double sum = 0;
  for (int64_t i = a; i <= b; ++i) sum += (double)w.rms[(size_t)i] * w.rms[(size_t)i];
  return b >= a ? (float)std::sqrt(sum / (double)(b - a + 1)) : 0.0f;
}

class WaveformCache {
public:
  // onReady runs on the message thread when an overview has been computed.
  explicit WaveformCache(std::function<void()> onReady);
  ~WaveformCache();
  // The overview of this file, or null while it is being computed (or if it cannot be read: tried
  // once per version of the file; a replaced file is read again).
  std::shared_ptr<const Waveform> get(const std::string& assetId, const juce::File& file);

private:
  std::function<void()> onReady_;
  juce::AudioFormatManager formats_;
  juce::ThreadPool pool_{1};
  std::mutex mutex_;
  std::map<std::string, std::shared_ptr<const Waveform>> ready_;
  std::map<std::string, bool> started_;
  struct Stat {
    std::string version;  // size|modified
    juce::uint32 checkedAt = 0;
  };
  std::map<std::string, Stat> stats_;
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

// The sound of each pattern of the instrument tracks (preview::render: the engine's own instruments and effects,
// offline), so a clip shows a real waveform instead of only its notes. Rendered in the background when the
// pattern's sound changes (its notes, the instrument, its knobs, the effects); the fader is drawn as a gain, so
// moving it renders nothing. While a new sound is rendered the previous picture stays (no flicker while a knob
// is turned); the renders of one pattern never queue up: only the latest wanted one runs next.
class PatternWaves {
public:
  explicit PatternWaves(std::function<void()> onReady);
  ~PatternWaves();
  // UI thread. Null: no sound to show (a plugin, no notes, silence) or the first render is not done yet; the clip
  // then draws its notes. `revision` is the History's: the document is looked at again only when it changed.
  std::shared_ptr<const Waveform> get(const doc::Document& doc, unsigned long long revision, const std::string& trackId,
                                      const std::string& patternId, const std::filesystem::path& assetRoot);

private:
  struct Slot {
    unsigned long long revision = ~0ull;
    std::string root, key;                  // the sound wanted now ("" = none)
    std::shared_ptr<const Waveform> shown;  // the last one rendered for this pattern
    std::unique_ptr<preview::Request> next; // wanted while another render of this pattern runs
    bool running = false;
  };
  void start(const std::string& slot, std::unique_ptr<preview::Request> request);  // mutex held
  std::function<void()> onReady_;
  core::AssetStore assets_{(size_t)512 << 20};
  juce::ThreadPool pool_{2};
  std::mutex mutex_;
  std::map<std::string, Slot> slots_;                                // trackId|patternId
  std::map<std::string, std::shared_ptr<const Waveform>> ready_;    // key -> sound (null: silent)
  std::shared_ptr<std::atomic<bool>> stop_ = std::make_shared<std::atomic<bool>>(false);
  std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

// One pixel column of a waveform in two layers: the peaks from 'from' to 'to' as a dark outer layer and
// the RMS body as a lighter inner layer (as on the reference images), mirrored around 'mid'.
void drawWaveColumn(juce::Graphics& g, const Waveform& w, float x, float mid, float half, double from, double to, juce::Colour colour,
                    float gain = 1.0f);
// A whole waveform from 'from' to 'to' seconds across an area, one column per pixel.
void drawWaveform(juce::Graphics& g, const Waveform& w, juce::Rectangle<int> area, double from, double to, juce::Colour colour,
                  float pad = 2.0f, float gain = 1.0f);

} // namespace core::app
