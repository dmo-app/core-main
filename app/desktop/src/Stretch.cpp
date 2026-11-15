#include "Stretch.h"
#include <algorithm>
#include <cmath>

namespace core::app::dsp {

namespace {
constexpr double kPi = 3.141592653589793;

float at(const std::vector<float>& v, int64_t i) { return i >= 0 && i < (int64_t)v.size() ? v[(size_t)i] : 0.0f; }

// Similarity of two stretches of the mono signal, step samples apart (normalised by the candidate's
// energy, so loud parts do not win only for being loud).
double similarity(const std::vector<float>& mono, int64_t a, int64_t b, int length, int step) {
  double dot = 0, energy = 1e-9;
  for (int i = 0; i < length; i += step) {
    const float x = at(mono, b + i);
    dot += (double)at(mono, a + i) * x;
    energy += (double)x * x;
  }
  return dot / std::sqrt(energy);
}
}  // namespace

Channels stretch(const Channels& in, double ratio) {
  ratio = std::clamp(ratio, 0.25, 4.0);
  if (in.empty() || in[0].empty()) return in;
  constexpr int N = 2048, Hs = N / 2, kSearch = 512;  // 43 ms pieces at 48 kHz, half overlap, +-11 ms search
  const int64_t inLen = (int64_t)in[0].size(), outLen = (int64_t)std::llround(inLen * ratio);
  std::vector<float> mono((size_t)inLen);
  for (const auto& ch : in)
    for (int64_t i = 0; i < inLen; ++i) mono[(size_t)i] += ch[(size_t)i] / (float)in.size();
  std::vector<float> window(N);
  for (int i = 0; i < N; ++i) window[(size_t)i] = (float)(0.5 - 0.5 * std::cos(2 * kPi * i / N));  // periodic Hann: sums to 1 at half overlap
  Channels out(in.size(), std::vector<float>((size_t)(outLen + N), 0.0f));  // the last piece starts before outLen
  const double Ha = Hs / ratio;  // input advance per piece
  int64_t prev = 0;
  for (int64_t k = 0; k * Hs < outLen; ++k) {  // the last piece starts within Hs of the end: every sample gets two halves
    int64_t best = 0;
    if (k > 0) {
      const int64_t natural = prev + Hs, nominal = (int64_t)std::llround(k * Ha);
      const int64_t lo = std::max<int64_t>(0, nominal - kSearch), hi = std::min<int64_t>(std::max<int64_t>(0, inLen - Hs), nominal + kSearch);
      // The nominal place unless another one fits clearly better (in silence every score is 0, and the
      // piece stays where the tempo puts it).
      best = std::clamp<int64_t>(nominal, lo, std::max(lo, hi));
      double score = similarity(mono, natural, best, Hs, 4) + 1e-6;
      for (int64_t p = lo; p <= hi; p += 4) {  // coarse, then fine around the best
        const double s = similarity(mono, natural, p, Hs, 4);
        if (s > score) { score = s; best = p; }
      }
      const int64_t coarse = best;
      score = similarity(mono, natural, coarse, Hs, 1) + 1e-6;
      for (int64_t p = std::max<int64_t>(lo, coarse - 4); p <= std::min<int64_t>(hi, coarse + 4); ++p) {
        const double s = similarity(mono, natural, p, Hs, 1);
        if (s > score) { score = s; best = p; }
      }
    }
    const int64_t o = k * Hs;
    for (size_t c = 0; c < in.size(); ++c) {
      // The first piece has no partner before it: its rising half is not faded in.
      for (int i = 0; i < N; ++i) out[c][(size_t)(o + i)] += at(in[c], best + i) * (k == 0 && i < Hs ? 1.0f : window[(size_t)i]);
    }
    prev = best;
  }
  for (auto& ch : out) ch.resize((size_t)outLen);
  return out;
}

Channels resample(const Channels& in, double ratio) {
  if (in.empty() || in[0].empty() || ratio <= 0) return in;
  const int64_t inLen = (int64_t)in[0].size(), outLen = std::max<int64_t>(1, (int64_t)std::llround(inLen * ratio));
  const double cutoff = std::min(1.0, ratio);  // below the lower Nyquist
  constexpr int kHalf = 16, kRes = 512;
  std::vector<float> kernel((size_t)(2 * kHalf * kRes + 2));
  for (size_t i = 0; i < kernel.size(); ++i) {
    const double x = (double)i / kRes - kHalf;
    const double sinc = x == 0 ? 1.0 : std::sin(kPi * x) / (kPi * x);
    const double w = std::abs(x) >= kHalf ? 0.0 : 0.42 + 0.5 * std::cos(kPi * x / kHalf) + 0.08 * std::cos(2 * kPi * x / kHalf);
    kernel[i] = (float)(sinc * w * cutoff);
  }
  const double half = kHalf / cutoff;
  Channels out(in.size(), std::vector<float>((size_t)outLen));
  for (int64_t o = 0; o < outLen; ++o) {
    const double center = o / ratio;
    const int64_t first = std::max<int64_t>(0, (int64_t)std::ceil(center - half)), last = std::min<int64_t>(inLen - 1, (int64_t)std::floor(center + half));
    for (size_t c = 0; c < in.size(); ++c) {
      double acc = 0;
      for (int64_t k = first; k <= last; ++k) {
        const double pos = ((k - center) * cutoff + kHalf) * kRes;
        const size_t i = (size_t)pos;
        if (i + 1 >= kernel.size()) continue;
        acc += in[c][(size_t)k] * (kernel[i] + (kernel[i + 1] - kernel[i]) * (pos - (double)i));
      }
      out[c][(size_t)o] = (float)acc;
    }
  }
  return out;
}

Channels shiftPitch(const Channels& in, double semitones) {
  if (in.empty() || in[0].empty()) return in;
  const double factor = std::pow(2.0, std::clamp(semitones, -24.0, 24.0) / 12.0);
  Channels out = resample(stretch(in, factor), 1.0 / factor);
  for (auto& ch : out) ch.resize(in[0].size(), 0.0f);  // exactly the same length
  return out;
}

} // namespace core::app::dsp
