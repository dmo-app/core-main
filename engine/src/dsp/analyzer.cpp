#include "analyzer.hpp"
#include <algorithm>
#include <cmath>

namespace core {

namespace {
constexpr double kPi = 3.141592653589793;

// In place, radix 2 (n a power of two).
void fft(std::vector<std::complex<double>>& a) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double angle = -2 * kPi / (double)len;
    const std::complex<double> step(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      std::complex<double> w(1, 0);
      for (size_t k = 0; k < len / 2; ++k) {
        const std::complex<double> u = a[i + k], v = a[i + k + len / 2] * w;
        a[i + k] = u + v;
        a[i + k + len / 2] = u - v;
        w *= step;
      }
    }
  }
}
}  // namespace

MasterAnalyzer::MasterAnalyzer(double sampleRate) : sampleRate_(sampleRate) {
  for (auto& s : ring_) s.store(0, std::memory_order_relaxed);
  decay_ = std::exp(-1.0 / (0.3 * sampleRate));
  window_.resize(kFft);
  for (int i = 0; i < kFft; ++i) window_[(size_t)i] = 0.5 - 0.5 * std::cos(2 * kPi * i / (kFft - 1));
  work_.resize(kFft);
}

double MasterAnalyzer::bandCentre(int b) { return 20.0 * std::pow(2.0, (b + 0.5) / 6.0); }

void MasterAnalyzer::process(const float* l, const float* r, int n) {
  uint32_t at = written_.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    const double a = l[i], b = r[i];
    ring_[at % kRing].store((float)(0.5 * (a + b)), std::memory_order_relaxed);
    ++at;
    ll_ = ll_ * decay_ + a * a;
    rr_ = rr_ * decay_ + b * b;
    lr_ = lr_ * decay_ + a * b;
  }
  written_.store(at, std::memory_order_release);
  sinceStore_ += n;
  if (sinceStore_ >= 256) {
    sinceStore_ = 0;
    const double energy = ll_ * rr_;
    const double c = energy > 1e-18 ? lr_ / std::sqrt(energy) : 0.0;
    correlation_.store(std::clamp(c, -1.0, 1.0), std::memory_order_relaxed);
  }
}

std::array<float, MasterAnalyzer::kBands> MasterAnalyzer::spectrum() const {
  const uint32_t end = written_.load(std::memory_order_acquire);
  for (int i = 0; i < kFft; ++i) {
    const uint32_t k = end - (uint32_t)kFft + (uint32_t)i;  // wraps like the ring (unsigned)
    work_[(size_t)i] = {(double)ring_[k % kRing].load(std::memory_order_relaxed) * window_[(size_t)i], 0.0};
  }
  fft(work_);
  // A sine of amplitude A peaks at A * N / 4 with the Hann window: scaled so it reads A.
  const double scale = 4.0 / kFft, binHz = sampleRate_ / kFft;
  auto power = [&](int bin) {
    const double m = std::abs(work_[(size_t)std::clamp(bin, 1, kFft / 2 - 1)]) * scale;
    return m * m;
  };
  std::array<float, kBands> out{};
  for (int b = 0; b < kBands; ++b) {
    const double lo = 20.0 * std::pow(2.0, b / 6.0), hi = 20.0 * std::pow(2.0, (b + 1) / 6.0);
    const int first = (int)std::ceil(lo / binHz), last = (int)std::floor(hi / binHz);
    double p = 0;
    if (first <= last) {
      for (int k = first; k <= last && k < kFft / 2; ++k) p = std::max(p, power(k));
    } else {  // narrower than a bin (low bands): between the two bins around its centre
      const double x = bandCentre(b) / binHz;
      const int k = (int)std::floor(x);
      const double f = x - k;
      p = power(k) * (1 - f) + power(k + 1) * f;
    }
    out[(size_t)b] = p > 1e-12 ? (float)std::max(kFloorDb, 10 * std::log10(p)) : (float)kFloorDb;
  }
  return out;
}

}  // namespace core
