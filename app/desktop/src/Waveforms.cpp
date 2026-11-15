#include "Waveforms.h"
#include <cmath>
#include <set>

namespace core::app {

WaveformCache::WaveformCache(std::function<void()> onReady) : onReady_(std::move(onReady)) { formats_.registerBasicFormats(); }

WaveformCache::~WaveformCache() {
  *alive_ = false;
  pool_.removeAllJobs(true, 10000);
}

std::shared_ptr<const Waveform> WaveformCache::get(const std::string& id, const juce::File& file) {
  // The file's size and date tell a replaced file apart; asked of the disk at most every 2 s per file, not on
  // every repaint of every clip.
  std::string assetId;
  {
    const std::string key = id + "|" + file.getFullPathName().toStdString();
    const juce::uint32 now = juce::Time::getMillisecondCounter();
    std::lock_guard<std::mutex> g(mutex_);
    auto& seen = stats_[key];
    if (seen.version.empty() || now - seen.checkedAt > 2000) {
      seen.version = std::to_string(file.getSize()) + "|" + std::to_string(file.getLastModificationTime().toMilliseconds());
      seen.checkedAt = now;
    }
    assetId = id + "|" + seen.version;
  }
  {
    std::lock_guard<std::mutex> g(mutex_);
    if (auto it = ready_.find(assetId); it != ready_.end()) return it->second;
    if (started_[assetId]) return nullptr;
    started_[assetId] = true;
  }
  std::weak_ptr<std::atomic<bool>> alive = alive_;
  pool_.addJob([this, assetId, file, alive] {
    std::unique_ptr<juce::AudioFormatReader> reader(formats_.createReaderFor(file));
    if (!reader) return;  // unreadable: no waveform (the clip still shows)
    auto wave = std::make_shared<Waveform>();
    wave->sampleRate = reader->sampleRate;
    const int chunk = wave->framesPerPeak * 64;
    juce::AudioBuffer<float> buf((int)reader->numChannels, chunk);
    for (juce::int64 at = 0; at < reader->lengthInSamples; at += chunk) {
      if (auto a = alive.lock(); !a || !*a) return;
      const int n = (int)std::min<juce::int64>(chunk, reader->lengthInSamples - at);
      reader->read(&buf, 0, n, at, true, true);
      for (int s = 0; s < n; s += wave->framesPerPeak) {
        float peak = 0, rms = 0;
        const int len = std::min(wave->framesPerPeak, n - s);
        for (int c = 0; c < buf.getNumChannels(); ++c) {
          peak = std::max(peak, buf.getMagnitude(c, s, len));
          rms = std::max(rms, buf.getRMSLevel(c, s, len));
        }
        wave->peaks.push_back(peak);
        wave->rms.push_back(rms);
      }
    }
    {
      if (auto a = alive.lock(); !a || !*a) return;
      std::lock_guard<std::mutex> g(mutex_);
      ready_[assetId] = wave;
    }
    juce::MessageManager::callAsync([this, alive] { if (auto a = alive.lock(); a && *a && onReady_) onReady_(); });
  });
  return nullptr;
}

PatternWaves::PatternWaves(std::function<void()> onReady) : onReady_(std::move(onReady)) {}

PatternWaves::~PatternWaves() {
  *stop_ = true;  // a render running stops at its next block
  *alive_ = false;
  pool_.removeAllJobs(true, 10000);
}

std::shared_ptr<const Waveform> PatternWaves::get(const doc::Document& doc, unsigned long long revision, const std::string& trackId,
                                                  const std::string& patternId, const std::filesystem::path& assetRoot) {
  const std::string name = trackId + "|" + patternId;
  const auto u8root = assetRoot.u8string();
  const std::string root(u8root.begin(), u8root.end());
  std::lock_guard<std::mutex> g(mutex_);
  Slot& slot = slots_[name];
  if (slot.revision != revision || slot.root != root) {
    slot.revision = revision;
    slot.root = root;
    auto request = preview::makeRequest(doc, trackId, patternId, assetRoot);
    if (!request) {
      slot.key.clear();
      slot.shown.reset();
      slot.next.reset();
    } else if (request->key != slot.key) {
      slot.key = request->key;
      if (auto it = ready_.find(slot.key); it != ready_.end()) {
        slot.shown = it->second;
        slot.next.reset();
      } else if (slot.running) {
        slot.next = std::move(request);  // after the one running: only the latest
      } else {
        start(name, std::move(request));
      }
    }
  }
  return slot.key.empty() ? nullptr : slot.shown;
}

void PatternWaves::start(const std::string& name, std::unique_ptr<preview::Request> request) {
  slots_[name].running = true;
  std::shared_ptr<preview::Request> job(std::move(request));
  std::weak_ptr<std::atomic<bool>> alive = alive_;
  std::shared_ptr<std::atomic<bool>> stop = stop_;
  pool_.addJob([this, name, job, alive, stop] {
    std::shared_ptr<const Waveform> wave;
    {
      if (*stop) return;
      const preview::Sound sound = preview::render(*job, assets_, stop.get());
      if (*stop) return;
      if (!sound.empty()) {
        auto w = std::make_shared<Waveform>();
        w->sampleRate = sound.sampleRate;
        w->framesPerPeak = sound.framesPerPeak;
        w->peaks = sound.peaks;
        w->rms = sound.rms;
        wave = w;
      }
      if (assets_.bytes() > ((size_t)384 << 20)) assets_.prune();
    }
    {
      auto a = alive.lock();
      if (!a || !*a) return;
      std::lock_guard<std::mutex> g(mutex_);
      if (ready_.size() > 600) {  // forget the sounds no pattern shows any more
        std::set<std::string> used;
        for (const auto& [n, s] : slots_) used.insert(s.key);
        for (auto it = ready_.begin(); it != ready_.end();) it = used.count(it->first) ? std::next(it) : ready_.erase(it);
      }
      ready_[job->key] = wave;
      Slot& slot = slots_[name];
      slot.running = false;
      if (slot.key == job->key) slot.shown = wave;
      if (slot.next) {
        auto next = std::move(slot.next);
        if (auto it = ready_.find(next->key); it != ready_.end()) { if (slot.key == next->key) slot.shown = it->second; }
        else start(name, std::move(next));
      }
    }
    juce::MessageManager::callAsync([this, alive] { if (auto a = alive.lock(); a && *a && onReady_) onReady_(); });
  });
}

void drawWaveColumn(juce::Graphics& g, const Waveform& w, float x, float mid, float half, double from, double to, juce::Colour colour,
                    float gain) {
  const float peak = std::min(1.0f, w.peakBetween(from, to) * gain);
  const float body = std::min(peak, rmsBetween(w, from, to) * 1.41f * gain);
  const float hp = peak * half, hb = body * half;
  if (hp < 0.5f) {
    g.setColour(colour.withMultipliedAlpha(0.35f));
    g.fillRect(x, mid - 0.5f, 1.0f, 1.0f);  // silence: only the centre line
    return;
  }
  g.setColour(colour.withMultipliedAlpha(0.42f));
  g.fillRect(x, mid - hp, 1.0f, 2 * hp);
  if (hb >= 0.5f) {
    g.setColour(colour.brighter(0.2f));
    g.fillRect(x, mid - hb, 1.0f, 2 * hb);
  }
}

void drawWaveform(juce::Graphics& g, const Waveform& w, juce::Rectangle<int> area, double from, double to, juce::Colour colour, float pad,
                  float gain) {
  const int width = std::max(1, area.getWidth());
  const float mid = (float)area.getCentreY(), half = area.getHeight() / 2.0f - pad;
  const auto clip = g.getClipBounds();
  const int x0 = std::max(area.getX(), clip.getX()), x1 = std::min(area.getRight(), clip.getRight());
  for (int x = x0; x < x1; ++x) {
    const double a = from + (to - from) * (x - area.getX()) / width, b = from + (to - from) * (x + 1 - area.getX()) / width;
    drawWaveColumn(g, w, (float)x, mid, half, a, b, colour, gain);
  }
}

} // namespace core::app
