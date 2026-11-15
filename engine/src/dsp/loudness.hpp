// Loudness of the master (ITU-R BS.1770-4 / EBU R128): momentary (400 ms), short-term (3 s) and integrated
// (gated: blocks under -70 LUFS, then under the ungated mean -10 LU, are left out), and the true peak
// (the level between samples, read at 8 points per sample: true_peak.hpp). Stereo, both channels weighted 1.
//
// process() runs on the audio thread: no allocation, no lock. The integrated value comes from a histogram
// of 400 ms block loudness (0.1 LU bins from -70 to +5 LUFS, the way libebur128 keeps it bounded), so a
// song of any length takes the same memory; the bins are atomics the control thread reads.
#pragma once
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include "true_peak.hpp"

namespace core {

// "Not measured" is a finite floor, never -inf or NaN: the engine is built with /fp:fast, where MSVC's
// INFINITY is not infinite and isinf/isfinite may be assumed false/true.
struct LoudnessReading {
  static constexpr double kNone = -1000;  // silence, or the window not filled yet
  static bool measured(double v) { return v > kNone / 2; }
  double momentary = kNone, shortTerm = kNone, integrated = kNone;  // LUFS
  double truePeak = kNone;         // dBTP, the highest since the last reset
  double seconds = 0;              // measured since the last reset
};

class LoudnessMeter {
public:
  explicit LoudnessMeter(double sampleRate);
  // Audio thread. Resets first when a reset was asked for (requestReset, from any thread).
  void process(const float* l, const float* r, int n);
  void requestReset() { resetAsked_.store(true, std::memory_order_release); }
  // Any thread: the values as of the last complete 100 ms block.
  LoudnessReading read() const;

  static constexpr int kBins = 750;  // -70.0 .. +5.0 LUFS in 0.1 LU steps

private:
  struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
    double run(double x) {
      const double y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  void reset();
  void endOfBlock();

  double sampleRate_;
  int blockSamples_;                         // 100 ms
  Biquad shelf_[2], highPass_[2];            // the K-weighting, per channel
  double sum_ = 0;                           // weighted energy of the block so far (both channels)
  int inBlock_ = 0;
  std::array<double, 30> blocks_{};          // mean square of the last 30 blocks of 100 ms
  int blockAt_ = 0, blocksSeen_ = 0;
  TruePeakReader peak_[2];
  double peakSq_ = 0;                        // true peak, squared, since the reset
  std::atomic<bool> resetAsked_{false};
  std::atomic<double> momentary_{0}, shortTerm_{0}, truePeak_{0}, seconds_{0};
  std::atomic<uint32_t> bins_[kBins] = {};
  std::atomic<uint32_t> binsTotal_{0};
};

} // namespace core
