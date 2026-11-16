// Generated test signals: no audio fixtures in the repo. Deterministic on every compiler (own PRNG, no std::
// distributions, whose output differs between standard libraries).
#pragma once
#include <cmath>
#include <cstdint>
#include <vector>

namespace coretest {

inline std::vector<float> sine(double hz, double sampleRate, int64_t frames, float amplitude = 0.5f) {
  std::vector<float> out((size_t)frames);
  const double pi = 3.14159265358979323846;
  for (int64_t i = 0; i < frames; ++i) out[(size_t)i] = amplitude * (float)std::sin(2 * pi * hz * (double)i / sampleRate);
  return out;
}

inline std::vector<float> impulse(int64_t frames, int64_t at = 0, float amplitude = 1.0f) {
  std::vector<float> out((size_t)frames, 0.0f);
  if (at >= 0 && at < frames) out[(size_t)at] = amplitude;
  return out;
}

// xorshift32: the same numbers everywhere for the same seed.
inline std::vector<float> noise(int64_t frames, uint32_t seed = 12345, float amplitude = 0.25f) {
  std::vector<float> out((size_t)frames);
  uint32_t x = seed ? seed : 1;
  for (int64_t i = 0; i < frames; ++i) {
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    out[(size_t)i] = amplitude * ((float)(x >> 8) / 8388608.0f - 1.0f);
  }
  return out;
}

inline float peakOf(const std::vector<float>& v) { float p = 0; for (float s : v) p = std::fmax(p, std::fabs(s)); return p; }
inline bool allFinite(const std::vector<float>& v) { for (float s : v) if (!std::isfinite(s)) return false; return true; }
inline int64_t firstAbove(const std::vector<float>& v, float threshold) {
  for (size_t i = 0; i < v.size(); ++i) if (std::fabs(v[i]) > threshold) return (int64_t)i;
  return -1;
}

}  // namespace coretest
