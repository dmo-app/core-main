#include "assets.hpp"
#include <filesystem>
#include <stdexcept>

namespace core {

std::shared_ptr<const AudioData> AssetStore::get(const std::string& path, double sampleRate, std::string* error) {
  std::lock_guard<std::mutex> g(mutex_);
  // The file's size and time are part of the key: a file replaced on disk is read again.
  std::error_code e;
  const std::filesystem::path p(std::u8string(path.begin(), path.end()));
  const auto size = std::filesystem::file_size(p, e);
  const auto time = std::filesystem::last_write_time(p, e).time_since_epoch().count();
  const std::string key = path + "|" + std::to_string(sampleRate) + "|" + std::to_string(e ? 0 : size) + "|" + std::to_string(e ? 0 : time);
  if (auto it = files_.find(key); it != files_.end()) return it->second;
  try {
    size_t used = 0;
    for (const auto& [k, data] : files_) used += data->bytes();
    // Frames that fit in what is left, at the source rate (a rough bound before the conversion).
    const int64_t room = (int64_t)((maxBytes_ > used ? maxBytes_ - used : 0) / (2 * sizeof(float)));
    AudioData source = readWav(path, room);
    auto data = std::make_shared<const AudioData>(resample(source, sampleRate));
    if (used + data->bytes() > maxBytes_) throw std::runtime_error(path + ": not enough memory for more audio (limit " + std::to_string(maxBytes_ >> 20) + " MB).");
    files_[key] = data;
    return data;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return nullptr;
  }
}

void AssetStore::prune() {
  std::lock_guard<std::mutex> g(mutex_);
  for (auto it = files_.begin(); it != files_.end();)
    it = it->second.use_count() == 1 ? files_.erase(it) : std::next(it);
}

size_t AssetStore::bytes() const {
  std::lock_guard<std::mutex> g(mutex_);
  size_t used = 0;
  for (const auto& [k, data] : files_) used += data->bytes();
  return used;
}

} // namespace core
