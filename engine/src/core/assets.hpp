// Audio files used by audio clips, loaded once and shared by the graphs that play them (real time,
// export jobs). Loading and resampling happen on the control thread; the audio thread only reads the
// finished buffers, and the last graph to drop one does so on the control thread (retired graphs).
#pragma once
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include "../io/audio_file.hpp"

namespace core {

class AssetStore {
public:
  explicit AssetStore(size_t maxBytes = (size_t)2 << 30) : maxBytes_(maxBytes) {}
  // The file at this sample rate, loaded and converted once. Null (with *error set) if it cannot be
  // read or would pass the memory limit: the clip stays silent and is reported, the project still loads.
  std::shared_ptr<const AudioData> get(const std::string& path, double sampleRate, std::string* error);
  void prune();         // forgets files no graph uses any more
  size_t bytes() const;

private:
  mutable std::mutex mutex_;
  std::map<std::string, std::shared_ptr<const AudioData>> files_;  // path|rate|size|time
  size_t maxBytes_;
};

} // namespace core
