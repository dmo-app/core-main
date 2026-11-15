#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace core {
// Writes stereo WAV (or mono: the average of L and R). bits = 32 (float) or 24 (PCM, clipped at ±1). Paths are UTF-8.
void writeWav(const std::string& path, const std::vector<float>& L, const std::vector<float>& R, int sampleRate, int bits = 24,
              int channels = 2);

// The same file written block by block, so a long export never needs the whole song in memory.
// The header is completed by finish(); a writer destroyed before that leaves an unfinished file
// (the caller deletes it).
class WavWriter {
public:
  // channels: 2, or 1 (mono: each sample is (L + R) / 2, for examples given to an AI, Diogo 2026-11-09).
  WavWriter(const std::string& path, int sampleRate, int bits, int channels = 2);
  ~WavWriter();
  WavWriter(const WavWriter&) = delete;
  WavWriter& operator=(const WavWriter&) = delete;
  void write(const float* L, const float* R, int frames);  // throws on a write error (disk full...)
  void finish();                                           // sizes in the header, flush, close
  static uint64_t maxFrames(int bits, int channels = 2);   // the 4 GB limit of the format
private:
  FILE* file_ = nullptr;
  int bits_, channels_ = 2, sampleRate_ = 0;
  uint64_t frames_ = 0;
  std::vector<unsigned char> buffer_;
};
} // namespace core
