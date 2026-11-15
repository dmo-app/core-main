#include "wav.hpp"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace core {

namespace {
void put32(unsigned char* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> (8 * i)); }
void put16(unsigned char* p, uint16_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }

// Paths are UTF-8 (names with accents work on Windows too).
std::filesystem::path widePath(const std::string& path) {
#ifdef _WIN32
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path.data()), path.size()));
#else
  return std::filesystem::path(path);
#endif
}

std::string utf8(const std::filesystem::path& p) {
  const std::u8string s = p.u8string();
  return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

FILE* openForWriting(const std::string& path) {
#ifdef _WIN32
  return _wfopen(widePath(path).c_str(), L"wb");
#else
  return std::fopen(path.c_str(), "wb");
#endif
}

// The destination folder is created when missing: an export points at where the file should end up,
// not at a folder the caller had to make first.
void ensureParentDir(const std::string& path) {
  const std::filesystem::path dir = widePath(path).parent_path();
  std::error_code ec;
  if (dir.empty() || std::filesystem::is_directory(dir, ec)) return;
  std::filesystem::create_directories(dir, ec);
  // ec.message() is the OS's own language in the system code page; the engine's messages are UTF-8.
  if (ec) throw std::runtime_error("Cannot create the output folder " + utf8(dir) + " (error " + std::to_string(ec.value()) + ").");
}

void header(unsigned char* h, int sampleRate, int bits, uint64_t frames, uint16_t channels) {
  const uint16_t bytes = (uint16_t)(bits / 8);
  const uint32_t dataSize = (uint32_t)(frames * channels * bytes);
  std::memcpy(h, "RIFF", 4); put32(h + 4, 36 + dataSize); std::memcpy(h + 8, "WAVE", 4);
  std::memcpy(h + 12, "fmt ", 4); put32(h + 16, 16); put16(h + 20, bits == 32 ? 3 : 1); put16(h + 22, channels);
  put32(h + 24, (uint32_t)sampleRate); put32(h + 28, (uint32_t)sampleRate * channels * bytes); put16(h + 32, channels * bytes); put16(h + 34, (uint16_t)bits);
  std::memcpy(h + 36, "data", 4); put32(h + 40, dataSize);
}
} // namespace

uint64_t WavWriter::maxFrames(int bits, int channels) {
  return (0xFFFFFFFFull - 44) / ((uint64_t)std::max(1, channels) * (uint64_t)(bits / 8));
}

WavWriter::WavWriter(const std::string& path, int sampleRate, int bits, int channels) : bits_(bits), channels_(channels) {
  if (bits != 24 && bits != 32) throw std::runtime_error("WAV bits must be 24 or 32.");
  if (channels != 1 && channels != 2) throw std::runtime_error("WAV channels must be 1 or 2.");
  ensureParentDir(path);
  file_ = openForWriting(path);
  // The reason is part of the message: "cannot write" alone does not tell a blocked folder from a
  // full disk or a name that is not a file.
  if (!file_) throw std::runtime_error("Cannot write " + path + " (" + std::strerror(errno) + ").");
  unsigned char h[44];
  header(h, sampleRate, bits, 0, (uint16_t)channels_);  // sizes are filled in by finish()
  sampleRate_ = sampleRate;
  if (std::fwrite(h, 1, 44, file_) != 44) { std::fclose(file_); file_ = nullptr; throw std::runtime_error("Cannot write " + path); }
}

WavWriter::~WavWriter() { if (file_) std::fclose(file_); }

void WavWriter::write(const float* L, const float* R, int frames) {
  if (frames_ + (uint64_t)frames > maxFrames(bits_, channels_)) throw std::runtime_error("The WAV file would pass the 4 GB limit of the format.");
  const int bytes = bits_ / 8;
  buffer_.resize((size_t)frames * (size_t)channels_ * bytes);
  unsigned char* out = buffer_.data();
  for (int i = 0; i < frames; ++i) {
    const float mono = 0.5f * (L[i] + R[i]);
    const float both[2] = {channels_ == 1 ? mono : L[i], R[i]};
    for (int c = 0; c < channels_; ++c) {
      float v = both[c];
      if (bits_ == 32) { std::memcpy(out, &v, 4); out += 4; }  // little-endian IEEE float
      else {
        const int32_t x = (int32_t)std::lrint(std::clamp(v, -1.0f, 1.0f) * 8388607.0f);
        out[0] = (unsigned char)x; out[1] = (unsigned char)(x >> 8); out[2] = (unsigned char)(x >> 16);
        out += 3;
      }
    }
  }
  if (std::fwrite(buffer_.data(), 1, buffer_.size(), file_) != buffer_.size()) throw std::runtime_error("Writing the WAV file failed (disk full?).");
  frames_ += (uint64_t)frames;
}

void WavWriter::finish() {
  unsigned char h[44];
  header(h, sampleRate_, bits_, frames_, (uint16_t)channels_);
  const bool ok = std::fseek(file_, 0, SEEK_SET) == 0 && std::fwrite(h, 1, 44, file_) == 44 && std::fflush(file_) == 0;
  const bool closed = std::fclose(file_) == 0;
  file_ = nullptr;
  if (!ok || !closed) throw std::runtime_error("Finishing the WAV file failed (disk full?).");
}

void writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, int sampleRate, int bits,
              int channels) {
  WavWriter w(path, sampleRate, bits, channels);
  const size_t frames = std::min(L.size(), R.size());
  for (size_t at = 0; at < frames; at += 65536) w.write(L.data() + at, R.data() + at, (int)std::min<size_t>(65536, frames - at));
  w.finish();
}

} // namespace core
