#include "loudness.hpp"
#include <algorithm>
#include <cmath>

namespace core {

namespace {
constexpr double kPi = 3.141592653589793;
double toLufs(double meanSquare) { return meanSquare > 1e-30 ? -0.691 + 10 * std::log10(meanSquare) : LoudnessReading::kNone; }
double fromLufs(double lufs) { return std::pow(10.0, (lufs + 0.691) / 10); }
}  // namespace

LoudnessMeter::LoudnessMeter(double sampleRate) : sampleRate_(sampleRate), blockSamples_(std::max(1, (int)std::lround(sampleRate * 0.1))) {
  // K-weighting at any sample rate (the BS.1770 filters as analogue prototypes, through the bilinear
  // transform; at 48 kHz these are the standard's coefficients).
  {
    const double f0 = 1681.974450955533, gain = 3.999843853973347, q = 0.7071752369554196;
    const double k = std::tan(kPi * f0 / sampleRate), vh = std::pow(10.0, gain / 20), vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1 + k / q + k * k;
    Biquad s;
    s.b0 = (vh + vb * k / q + k * k) / a0;
    s.b1 = 2 * (k * k - vh) / a0;
    s.b2 = (vh - vb * k / q + k * k) / a0;
    s.a1 = 2 * (k * k - 1) / a0;
    s.a2 = (1 - k / q + k * k) / a0;
    shelf_[0] = shelf_[1] = s;
  }
  {
    const double f0 = 38.13547087602444, q = 0.5003270373238773;
    const double k = std::tan(kPi * f0 / sampleRate), a0 = 1 + k / q + k * k;
    Biquad h;
    h.b0 = 1; h.b1 = -2; h.b2 = 1;
    h.a1 = 2 * (k * k - 1) / a0;
    h.a2 = (1 - k / q + k * k) / a0;
    highPass_[0] = highPass_[1] = h;
  }
  reset();
}

void LoudnessMeter::reset() {
  for (int c = 0; c < 2; ++c) { shelf_[c].z1 = shelf_[c].z2 = highPass_[c].z1 = highPass_[c].z2 = 0; }
  sum_ = 0;
  inBlock_ = 0;
  blocks_.fill(0);
  blockAt_ = blocksSeen_ = 0;
  peak_[0].reset();
  peak_[1].reset();
  peakSq_ = 0;
  momentary_.store(0, std::memory_order_relaxed);
  shortTerm_.store(0, std::memory_order_relaxed);
  truePeak_.store(0, std::memory_order_relaxed);
  seconds_.store(0, std::memory_order_relaxed);
  for (auto& b : bins_) b.store(0, std::memory_order_relaxed);
  binsTotal_.store(0, std::memory_order_release);
}

void LoudnessMeter::process(const float* l, const float* r, int n) {
  if (resetAsked_.exchange(false, std::memory_order_acq_rel)) reset();
  for (int i = 0; i < n; ++i) {
    const float in[2] = {l[i], r[i]};
    for (int c = 0; c < 2; ++c) {
      const double y = highPass_[c].run(shelf_[c].run(in[c]));
      sum_ += y * y;
      const double p = std::max(std::fabs((double)in[c]), peak_[c].push(in[c]));
      peakSq_ = std::max(peakSq_, p * p);
    }
    if (++inBlock_ == blockSamples_) endOfBlock();
  }
}

void LoudnessMeter::endOfBlock() {
  blocks_[(size_t)blockAt_] = sum_ / blockSamples_;  // both channels, weight 1 each
  blockAt_ = (blockAt_ + 1) % (int)blocks_.size();
  ++blocksSeen_;
  sum_ = 0;
  inBlock_ = 0;
  auto mean = [&](int count) {
    double s = 0;
    for (int k = 1; k <= count; ++k) s += blocks_[(size_t)((blockAt_ - k + (int)blocks_.size()) % (int)blocks_.size())];
    return s / count;
  };
  if (blocksSeen_ >= 4) {
    const double m = mean(4);
    momentary_.store(m, std::memory_order_relaxed);
    // A gating block (400 ms, every 100 ms): over the absolute gate it counts for the integrated value.
    const double lufs = toLufs(m);
    if (lufs >= -70.0) {
      const int bin = std::clamp((int)std::floor((lufs + 70.0) * 10), 0, kBins - 1);
      bins_[bin].fetch_add(1, std::memory_order_relaxed);
      binsTotal_.fetch_add(1, std::memory_order_release);
    }
  }
  if (blocksSeen_ >= 30) shortTerm_.store(mean(30), std::memory_order_relaxed);
  truePeak_.store(peakSq_, std::memory_order_relaxed);
  seconds_.store((double)blocksSeen_ * blockSamples_ / sampleRate_, std::memory_order_relaxed);
}

LoudnessReading LoudnessMeter::read() const {
  LoudnessReading out;
  out.momentary = toLufs(momentary_.load(std::memory_order_relaxed));
  out.shortTerm = toLufs(shortTerm_.load(std::memory_order_relaxed));
  const double peak = truePeak_.load(std::memory_order_relaxed);
  out.truePeak = peak > 1e-30 ? 10 * std::log10(peak) : LoudnessReading::kNone;
  out.seconds = seconds_.load(std::memory_order_relaxed);
  if (binsTotal_.load(std::memory_order_acquire) == 0) return out;
  // Bins at their centre: the ungated mean sets the relative gate (-10 LU), then the mean above it.
  uint32_t counts[kBins];
  double energy = 0, blocks = 0;
  for (int b = 0; b < kBins; ++b) {
    counts[b] = bins_[b].load(std::memory_order_relaxed);
    energy += counts[b] * fromLufs(-70.0 + (b + 0.5) / 10);
    blocks += counts[b];
  }
  if (blocks == 0) return out;
  const double gate = toLufs(energy / blocks) - 10;
  energy = blocks = 0;
  for (int b = 0; b < kBins; ++b) {
    const double centre = -70.0 + (b + 0.5) / 10;
    if (centre < gate) continue;
    energy += counts[b] * fromLufs(centre);
    blocks += counts[b];
  }
  if (blocks > 0) out.integrated = toLufs(energy / blocks);
  return out;
}

} // namespace core
