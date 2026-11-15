// What the References page shows of the master (UI04): its spectrum and the stereo correlation.
//
// process() runs on the audio thread (no allocation, no lock): it keeps the last kRing samples of the mid
// signal ((L+R)/2) in a ring of atomics and a running correlation of L and R (about 300 ms). The spectrum is
// computed on demand on the control thread (an FFT of the last kFft samples, Hann window), in 1/6-octave
// bands from 20 Hz to 20 kHz, each the strongest bin in it, in dB where a full-scale sine reads 0 dB.
// Values are finite: kFloorDb for silence (the engine is built with /fp:fast, see loudness.hpp).
#pragma once
#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <vector>

namespace core {

class MasterAnalyzer {
 public:
  static constexpr int kFft = 4096, kRing = 8192, kBands = 60;
  static constexpr double kFloorDb = -120;

  explicit MasterAnalyzer(double sampleRate);
  void process(const float* l, const float* r, int n);  // audio thread

  // Control thread (one caller at a time): kBands values in dB, lowest band first.
  std::array<float, kBands> spectrum() const;
  // -1 (opposite) .. +1 (the same in both channels); 0 for silence.
  double correlation() const { return correlation_.load(std::memory_order_relaxed); }
  // The centre of band b in Hz (bands are 1/6 octave wide, the first starting at 20 Hz).
  static double bandCentre(int b);

 private:
  double sampleRate_;
  std::atomic<float> ring_[kRing];
  std::atomic<uint32_t> written_{0};  // samples written so far (the ring index is this mod kRing)
  double ll_ = 0, rr_ = 0, lr_ = 0, decay_ = 0;  // audio thread
  int sinceStore_ = 0;
  std::atomic<double> correlation_{0};
  std::vector<double> window_;
  mutable std::vector<std::complex<double>> work_;
};

}  // namespace core
