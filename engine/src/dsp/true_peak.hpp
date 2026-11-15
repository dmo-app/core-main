// True peak (the level of the wave between samples), for limiter 2 and the loudness meter.
//
// Audit 2026-11-07 (#1): both read it at 4x through short Blackman filters (12-16 taps per phase), whose
// pass band falls from ~14 kHz: a bright master read up to 0.75 dB low, so limiter 2 let the wave pass its
// ceiling (darkpsycore-minimal, ceiling -1.4: -0.1 dBTP in FFmpeg, -0.86 in Core's meter).
//
// Here: 8 points per sample (the sample itself and 7 between it and the next), each a windowed sinc of
// 32 base samples (Kaiser, beta 6.5, about 70 dB), flat to ~21 kHz at 48 kHz. The points are at most 1/16
// of a sample from the wave's highest point, so the reading is within 0.12 dB at 20 kHz and 0.04 dB at
// 10 kHz. It reads the sample kDelay back. Fixed arrays, no allocation: safe on the audio thread.
#pragma once
#include <algorithm>
#include <cmath>

namespace core {

class TruePeakReader {
public:
  static constexpr int kTaps = 48, kPoints = 8, kDelay = kTaps / 2;

  TruePeakReader() {
    static const Coefs c;
    coef_ = &c;
  }
  // Pushes one sample; returns the highest |value| of the wave from sample (now - kDelay) to just before
  // the next one.
  double push(float in) {
    hist_[at_] = in;
    hist_[at_ + kTaps] = in;  // twice, so a window of kTaps is always contiguous
    at_ = (at_ + 1) % kTaps;
    const float* x = hist_ + at_;  // x[0] oldest .. x[kTaps-1] newest
    double most = std::fabs((double)x[kTaps - 1 - kDelay]);
    for (int p = 1; p < kPoints; ++p) {
      const double* h = coef_->h[p];
      double acc = 0;
      for (int j = 0; j < kTaps; ++j) acc += h[j] * x[j];
      most = std::max(most, std::fabs(acc));
    }
    return most;
  }
  // The sample kDelay back (what the reading is centred on).
  float delayed() const { return hist_[(at_ + kTaps - 1 - kDelay) % kTaps]; }
  void reset() { std::fill(std::begin(hist_), std::end(hist_), 0.0f); at_ = 0; }

private:
  struct Coefs {
    double h[kPoints][kTaps];
    Coefs() {
      constexpr double kPi = 3.141592653589793, beta = 7.0;
      auto bessel0 = [](double x) {
        double sum = 1, term = 1;
        for (int k = 1; k < 30; ++k) { term *= (x / (2 * k)) * (x / (2 * k)); sum += term; }
        return sum;
      };
      for (int p = 0; p < kPoints; ++p) {
        double sum = 0;
        for (int j = 0; j < kTaps; ++j) {
          // x[j] is the sample (now - (kTaps-1-j)); the point is at (now - kDelay + p/8).
          const double t = (double)j - (kTaps - 1 - kDelay) - (double)p / kPoints;
          const double sinc = std::fabs(t) < 1e-12 ? 1.0 : std::sin(kPi * t) / (kPi * t);
          const double r = t / (kTaps / 2.0);
          const double w = std::fabs(r) >= 1 ? 0.0 : bessel0(beta * std::sqrt(1 - r * r)) / bessel0(beta);
          h[p][j] = sinc * w;
          sum += h[p][j];
        }
        for (int j = 0; j < kTaps; ++j) h[p][j] /= sum;  // unity at DC
      }
    }
  };
  const Coefs* coef_;
  float hist_[2 * kTaps] = {};
  int at_ = 0;
};

}  // namespace core
