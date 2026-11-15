#include "wavetables.hpp"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <complex>

namespace core {
namespace {

constexpr double kPi = 3.14159265358979323846;
using cd = std::complex<double>;

// In-place radix-2 FFT (inverse when inv), size a power of two.
void fft(std::vector<cd>& a, bool inv) {
  const size_t n = a.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) std::swap(a[i], a[j]);
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const double ang = 2 * kPi / (double)len * (inv ? 1 : -1);
    const cd wl(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      cd w(1);
      for (size_t j = 0; j < len / 2; ++j) {
        const cd u = a[i + j], v = a[i + j + len / 2] * w;
        a[i + j] = u + v; a[i + j + len / 2] = u - v;
        w *= wl;
      }
    }
  }
  if (inv) for (auto& x : a) x /= (double)n;
}

// One cycle of table t (1..8) at morph x (0..1), at phase p (0..1). Drawn at 8x the stored size and band-limited
// by the FFT afterwards, so the shapes with hard edges (pulse, sync, fold, steps) are clean.
double cycle(int t, double x, double p) {
  const double s = std::sin(2 * kPi * p);
  switch (t) {
    case 1: {  // PWM: a square that narrows to a thin pulse
      const double w = 0.5 - 0.46 * x;
      return (p < w ? 1.0 : -1.0) - (2 * w - 1);  // its offset removed
    }
    case 2: {  // Sync: a saw restarted by the cycle, its own speed 1x to 6x (the classic sync sweep)
      const double r = 1 + 5 * x;
      const double q = p * r - std::floor(p * r);
      return (1 - 2 * q) * (1 - p);  // faded with the master cycle, like a hard-sync lead
    }
    case 4: {  // Organ: drawbars pulled out one by one (8', 4', 2 2/3', 2', 1 3/5', 1 1/3', 1')
      static const int h[7] = {1, 2, 3, 4, 5, 6, 8};
      double v = 0;
      for (int k = 0; k < 7; ++k) {
        const double on = std::clamp(x * 7 - k + 1, 0.0, 1.0);
        v += (k == 0 ? 1.0 : on * 0.7) * std::sin(2 * kPi * h[k] * p);
      }
      return v;
    }
    case 5: {  // FM: a sine phase-modulated by its octave, the index from 0 to 7 (bell, then metal)
      return std::sin(2 * kPi * p + 7 * x * std::sin(4 * kPi * p));
    }
    case 6: {  // Fold: a sine driven into a wavefolder (1x to 9x)
      const double d = 1 + 8 * x;
      return std::sin(kPi / 2 * d * s);
    }
    case 8: {  // Digital: a saw in fewer and fewer steps (64 down to 3)
      const double steps = std::round(64 * std::pow(3.0 / 64.0, x));
      const double v = 1 - 2 * p;
      return std::round(v * steps / 2) / (steps / 2);
    }
    default: return s;
  }
}

// Harmonic amplitudes for the additive tables (3 Vowel, 7 Growl): returns 0 for the time-domain tables.
double partial(int t, double x, int n) {
  if (t == 3) {  // Vowel: a saw's harmonics through two formant peaks moving A, E, I, O, U (harmonic numbers of a 110 Hz voice)
    static const double f1[5] = {7.0, 4.5, 2.5, 4.5, 3.0}, f2[5] = {10.0, 16.0, 20.0, 8.0, 7.0};
    const double at = x * 4;
    const int i = std::min(3, (int)at);
    const double m = at - i;
    const double a = f1[i] + (f1[i + 1] - f1[i]) * m, b = f2[i] + (f2[i + 1] - f2[i]) * m;
    const double la = std::log((double)n / a), lb = std::log((double)n / b);
    return (1.0 / n) * (0.15 + std::exp(-la * la / 0.08) * 6 + std::exp(-lb * lb / 0.06) * 4);
  }
  if (t == 7) {  // Growl: a saw with one resonant peak sweeping from the 2nd to the 48th harmonic
    const double c = 2 * std::pow(24.0, x);
    const double l = std::log((double)n / c);
    return (1.0 / n) * (0.3 + 8 * std::exp(-l * l / 0.05));
  }
  return 0;
}

}  // namespace

const char* Wavetables::name(int table) {
  static const char* names[kTables + 1] = {"Classic", "PWM", "Sync", "Vowel", "Organ", "FM", "Fold", "Growl", "Digital"};
  return table >= 0 && table <= kTables ? names[table] : "";
}

namespace { std::atomic<bool> gBuilt{false}; }

const Wavetables& Wavetables::get() {
  static const Wavetables tables;
  gBuilt.store(true, std::memory_order_release);
  return tables;
}

bool Wavetables::built() { return gBuilt.load(std::memory_order_acquire); }

Wavetables::Wavetables() {
  data_.assign((size_t)kTables * kFrames * kLevels * (kSize + 1), 0.0f);
  constexpr int kBig = kSize * 8;
  std::vector<cd> big(kBig), small(kSize);
  for (int t = 1; t <= kTables; ++t)
    for (int f = 0; f < kFrames; ++f) {
      const double x = (double)f / (kFrames - 1);
      std::vector<cd> harm(kSize / 2, cd(0));  // harm[n]: harmonic n (sine phase), n >= 1
      if (t == 3 || t == 7) {
        for (int n = 1; n < kSize / 2; ++n) harm[(size_t)n] = cd(0, -0.5 * partial(t, x, n));
      } else {
        for (int i = 0; i < kBig; ++i) big[(size_t)i] = cd(cycle(t, x, (double)i / kBig), 0);
        fft(big, false);
        for (int n = 1; n < kSize / 2; ++n) harm[(size_t)n] = big[(size_t)n] / (double)kBig;
      }
      // Every frame at the same loudness (RMS of a full-scale saw), its peak kept under 1.2 at the fullest level.
      double power = 0;
      for (int n = 1; n < kSize / 2; ++n) power += 2 * std::norm(harm[(size_t)n]);
      double gain = power > 0 ? 0.5 / std::sqrt(power) : 0;
      for (int level = 0; level < kLevels; ++level) {
        const int top = (kSize / 2) >> level;  // harmonics kept
        std::fill(small.begin(), small.end(), cd(0));
        for (int n = 1; n < top && n < kSize / 2; ++n) { small[(size_t)n] = harm[(size_t)n]; small[(size_t)(kSize - n)] = std::conj(harm[(size_t)n]); }
        fft(small, true);
        float* out = data_.data() + (((size_t)(t - 1) * kFrames + (size_t)f) * kLevels + (size_t)level) * (kSize + 1);
        if (level == 0) {
          double peak = 0;
          for (int i = 0; i < kSize; ++i) peak = std::max(peak, std::fabs(small[(size_t)i].real() * kSize * gain));
          if (peak > 1.2) gain *= 1.2 / peak;
        }
        for (int i = 0; i < kSize; ++i) out[i] = (float)(small[(size_t)i].real() * kSize * gain);
        out[kSize] = out[0];
      }
    }
}

double Wavetables::read(int table, double position, double phase, double dt) const {
  table = std::clamp(table, 1, kTables);
  // The level whose harmonics all stay under ~0.45 of the sample rate at this pitch.
  const double allowed = 0.45 / std::max(1e-9, std::fabs(dt));
  int level = 0;
  while (level < kLevels - 1 && (double)((kSize / 2) >> level) > allowed) ++level;
  const double at = std::clamp(position, 0.0, 1.0) * (kFrames - 1);
  const int f0 = std::min(kFrames - 2, (int)at);
  const double fm = at - f0;
  const double idx = (phase - std::floor(phase)) * kSize;
  const int i0 = std::min(kSize - 1, (int)idx);
  const double im = idx - i0;
  const float* a = frame(table, f0, level);
  const float* b = frame(table, f0 + 1, level);
  const double va = a[i0] + (a[i0 + 1] - a[i0]) * im, vb = b[i0] + (b[i0 + 1] - b[i0]) * im;
  return va + (vb - va) * fm;
}

}  // namespace core
