#include "output_guard.hpp"
#include "finite.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace core {

namespace {
double coef(double ms, double sr) { return 1 - std::exp(-1000.0 / (ms * sr)); }  // one-pole step towards a target
// NaN or Inf, read from the bits (finite.hpp): the engine is built with /fp:fast on Windows.
bool broken(float x) { return !finiteBits(x); }
}  // namespace

OutputGuard::OutputGuard(double sampleRate)
    : peakReleaseCoef_(coef(kPeakReleaseMs, sampleRate)),
      ceiling_(std::pow(10.0, kCeilingDb / 20)),
      listenCoef_(coef(kListenSlideMs / 7, sampleRate)) {  // 99.9% of the way in kListenSlideMs
}

void OutputGuard::setListenDb(double db) {
  if (!(db == db)) return;  // NaN: ignored
  listenDb_.store(std::clamp(db, kListenMinDb, kListenMaxDb), std::memory_order_relaxed);
}

void OutputGuard::reset() {
  peakGain_ = 1;
}

void OutputGuard::process(float* l, float* r, int n) {
  if (n <= 0) return;
  // The listening level first: one fixed gain, sliding only after the user changed it.
  const double target = std::pow(10.0, listenDb_.load(std::memory_order_relaxed) / 20);
  if (!started_) { listenGain_ = target; started_ = true; }  // a new output starts at the level, no slide
  if (listenGain_ != target || target != 1) {
    for (int i = 0; i < n; ++i) {
      if (listenGain_ != target) {
        listenGain_ += (target - listenGain_) * listenCoef_;
        if (std::fabs(listenGain_ - target) < 1e-6) listenGain_ = target;
      }
      l[i] = (float)(l[i] * listenGain_);
      r[i] = (float)(r[i] * listenGain_);
    }
  }
  // NaN or Inf is silence, always: even with the peak guard off (offline captures, tests), a broken block never
  // leaves (Codex review 2026-11-09 C1: with the guard off a NaN reached the output).
  bool bad = false;
  for (int i = 0; i < n && !bad; ++i) bad = broken(l[i]) || broken(r[i]);
  if (bad) {
    std::fill(l, l + n, 0.0f);
    std::fill(r, r + n, 0.0f);
    reset();
    return;
  }
  if (!enabled_.load(std::memory_order_relaxed)) return;
  double lowest = 1;
  for (int i = 0; i < n; ++i) {
    double a = l[i], b = r[i];
    const double peak = std::max(std::fabs(a), std::fabs(b));
    const double need = peak * peakGain_ > ceiling_ ? ceiling_ / peak : 1.0;
    if (need < peakGain_) peakGain_ = need;  // instant: the peak never passes
    else if ((peakGain_ += (1.0 - peakGain_) * peakReleaseCoef_) > 0.99999) peakGain_ = 1;  // then bit for bit again
    a *= peakGain_; b *= peakGain_;
    l[i] = (float)std::clamp(a, -ceiling_, ceiling_);
    r[i] = (float)std::clamp(b, -ceiling_, ceiling_);
    lowest = std::min(lowest, peakGain_);
  }
  const float db = lowest < 1 ? (float)(-20 * std::log10(std::max(lowest, 1e-6))) : 0.0f;
  float seen = reduction_.load(std::memory_order_relaxed);
  while (db > seen && !reduction_.compare_exchange_weak(seen, db, std::memory_order_relaxed)) {}
}

}  // namespace core
