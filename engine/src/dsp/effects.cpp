#include "effects.hpp"
#include "devices.hpp"
#include "registry.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include "oversample_simd.hpp"
#include "true_peak.hpp"
#include "voice_filter.hpp"
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#include <immintrin.h>
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

namespace core {

namespace {

constexpr double kPi = 3.141592653589793;

double dbToGain(double db) { return std::pow(10.0, db / 20.0); }

// Decaying feedback (reverb, delay, filter states) would reach denormal numbers in silence, which are
// very slow on x86: anything that small is set to zero.
inline float flush(float x) { return std::fabs(x) < 1e-15f ? 0.0f : x; }
inline double flush(double x) { return std::fabs(x) < 1e-30 ? 0.0 : x; }

// RBJ biquad, transposed direct form II, one per channel.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double z1[2] = {0, 0}, z2[2] = {0, 0};
  enum Kind { LowPass, HighPass, BandPass, Peak, LowShelf, HighShelf, Notch };
  void set(Kind kind, double sampleRate, double freq, double q, double gainDb) {
    freq = std::clamp(freq, 10.0, sampleRate * 0.49);
    const double w = 2 * kPi * freq / sampleRate, cw = std::cos(w), sw = std::sin(w);
    const double alpha = sw / (2 * std::max(0.05, q)), A = std::pow(10.0, gainDb / 40.0);
    double nb0 = 1, nb1 = 0, nb2 = 0, na0 = 1, na1 = 0, na2 = 0;
    switch (kind) {
      case LowPass: nb0 = (1 - cw) / 2; nb1 = 1 - cw; nb2 = nb0; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
      case HighPass: nb0 = (1 + cw) / 2; nb1 = -(1 + cw); nb2 = nb0; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
      case BandPass: nb0 = alpha; nb1 = 0; nb2 = -alpha; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
      case Notch: nb0 = 1; nb1 = -2 * cw; nb2 = 1; na0 = 1 + alpha; na1 = -2 * cw; na2 = 1 - alpha; break;
      case Peak: nb0 = 1 + alpha * A; nb1 = -2 * cw; nb2 = 1 - alpha * A; na0 = 1 + alpha / A; na1 = -2 * cw; na2 = 1 - alpha / A; break;
      case LowShelf: {
        const double s = 2 * std::sqrt(A) * alpha;
        nb0 = A * ((A + 1) - (A - 1) * cw + s); nb1 = 2 * A * ((A - 1) - (A + 1) * cw); nb2 = A * ((A + 1) - (A - 1) * cw - s);
        na0 = (A + 1) + (A - 1) * cw + s; na1 = -2 * ((A - 1) + (A + 1) * cw); na2 = (A + 1) + (A - 1) * cw - s;
        break;
      }
      case HighShelf: {
        const double s = 2 * std::sqrt(A) * alpha;
        nb0 = A * ((A + 1) + (A - 1) * cw + s); nb1 = -2 * A * ((A - 1) + (A + 1) * cw); nb2 = A * ((A + 1) + (A - 1) * cw - s);
        na0 = (A + 1) - (A - 1) * cw + s; na1 = 2 * ((A - 1) - (A + 1) * cw); na2 = (A + 1) - (A - 1) * cw - s;
        break;
      }
    }
    b0 = nb0 / na0; b1 = nb1 / na0; b2 = nb2 / na0; a1 = na1 / na0; a2 = na2 / na0;
  }
  float run(int ch, float x) {
    const double y = b0 * x + z1[ch];
    z1[ch] = flush(b1 * x - a1 * y + z2[ch]);
    z2[ch] = flush(b2 * x - a2 * y);
    return (float)y;
  }
};

// 4x oversampling (saturation 2, limiter 2): one linear-phase low-pass of 65 taps at 4x the rate
// (Blackman-windowed sinc, cut at 0.92 of the base Nyquist). Up: 4 polyphase outputs per input sample,
// 8 base samples late. Down: one output per 4 inputs, 8 base samples late. Fixed arrays, no allocation.
constexpr int kOsTaps = 65, kOsFactor = 4, kOsHistory = 17;  // history: base samples the up-sampler reads
struct OversampleFir {
  double h[kOsTaps];
  OversampleFir() {
    const double fc = 0.92 * 0.5 / kOsFactor;  // of the 4x rate
    double sum = 0;
    for (int k = 0; k < kOsTaps; ++k) {
      const double m = k - (kOsTaps - 1) / 2.0;
      const double sinc = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
      const double w = 0.42 - 0.5 * std::cos(2 * kPi * k / (kOsTaps - 1)) + 0.08 * std::cos(4 * kPi * k / (kOsTaps - 1));
      h[k] = sinc * w;
      sum += h[k];
    }
    for (double& x : h) x /= sum;  // unity gain at DC
  }
};
const OversampleFir& osFir() { static const OversampleFir fir; return fir; }

// The same taps by phase, for the vector kernel (built once, when the first saturation 2 is built).
const UpsampleTaps& upsampleTaps() {
  static const UpsampleTaps taps = [] {
    UpsampleTaps t{};
    const double* h = osFir().h;
    for (int j = 0; j < 16; ++j) for (int p = 0; p < kOsFactor; ++p) t.row[j][p] = h[kOsFactor * j + p];
    t.last = h[kOsTaps - 1];
    return t;
  }();
  return taps;
}

struct Upsampler {
  float x[kOsHistory] = {};  // x[0] newest
  // Pushes one base sample; writes the 4 samples at 4x for base time (now - 8) .. (now - 8 + 3/4).
  void push(float in, double out[kOsFactor]) {
    shift(in);
    const double* h = osFir().h;
    for (int p = 0; p < kOsFactor; ++p) {
      double acc = 0;
      for (int j = 0, k = p; k < kOsTaps; ++j, k += kOsFactor) acc += h[k] * x[j];
      out[p] = acc * kOsFactor;
    }
  }
  // The same with a vector kernel (saturation 2 on AVX): same history, same sums.
  void push(float in, double out[kOsFactor], UpsampleKernel kernel, const UpsampleTaps& taps) {
    shift(in);
    kernel(taps, x, out);
  }
  void shift(float in) {
    std::memmove(x + 1, x, sizeof(float) * (kOsHistory - 1));
    x[0] = in;
  }
};

struct Downsampler {
  // z[0] newest, at 4x. Read 3 places in, so the centre tap lands on a base sample: the output is the
  // base sample 8 before the one whose 4x samples came in last (phase 0 is on the base sample).
  double z[kOsTaps + kOsFactor - 1] = {};
  // Takes 4 samples at 4x (oldest first) and returns one base sample.
  double push(const double in[kOsFactor]) {
    constexpr int size = kOsTaps + kOsFactor - 1;
    std::memmove(z + kOsFactor, z, sizeof(double) * (size - kOsFactor));
    for (int p = 0; p < kOsFactor; ++p) z[kOsFactor - 1 - p] = in[p];
    const double* h = osFir().h;
    double acc = 0;
    for (int k = 0; k < kOsTaps; ++k) acc += h[k] * z[k + kOsFactor - 1];
    return acc;
  }
};

// Parameters changed since the last block? (Coefficients are recomputed only then.)
template <int N> bool changed(const double* now, double* seen) {
  bool any = false;
  for (int i = 0; i < N; ++i) if (now[i] != seen[i]) { seen[i] = now[i]; any = true; }
  return any;
}

// ---- CORE EQ (2026-11-09, replaces the 5-knob EQ): 8 bands, each a Bell, Low/High shelf, Low/High cut (12, 24 or
// 48 dB/oct) or Notch, on Stereo, Left, Right, Mid or Side, and dynamic (it moves its gain by up to Range when
// the band is louder than Threshold). Bands run in order. A Bell or shelf at 0 dB (not dynamic), a Low cut at
// 20 Hz or a High cut at 20 kHz does nothing at all, so the old EQ's five knobs (bands 1-5 of the defaults:
// low cut, low shelf, bell, high shelf, high cut) sound exactly as before.
class Eq final : public Effect {
public:
  static constexpr int kBands = 8, kPerBand = 10, kStages = 4;
  enum Shape { Bell, LowShelf, HighShelf, LowCut, HighCut, Notch };
  enum Chan { Stereo, Left, Right, Mid, Side };
  // Each band: On, Type, Freq, Gain, Q, Slope, Chan, Dyn, Thr, Range. Then output and autoGain.
  static constexpr int kOutput = kBands * kPerBand, kAutoGain = kOutput + 1, kParams = kAutoGain + 1;

  using Effect::Effect;
  const char* name() const override { return "eq"; }

  void process(float* l, float* r, int n) override {
    if (changed<kParams>(params, seen_)) setup();
    for (int b = 0; b < kBands; ++b) {
      Band& bd = bands_[b];
      if (!bd.active) continue;
      if (bd.dyn) { dynamic(bd, l, r, n); continue; }
      run(bd, l, r, 0, n);
    }
    if (out_ != 1.0)
      for (int i = 0; i < n; ++i) { l[i] = (float)(l[i] * out_); r[i] = (float)(r[i] * out_); }
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Eq&>(p);
    for (int b = 0; b < kBands; ++b) {
      for (int s = 0; s < kStages; ++s) for (int c = 0; c < 2; ++c) { bands_[b].f[s].z1[c] = o.bands_[b].f[s].z1[c]; bands_[b].f[s].z2[c] = o.bands_[b].f[s].z2[c]; }
      for (int c = 0; c < 2; ++c) { bands_[b].det.z1[c] = o.bands_[b].det.z1[c]; bands_[b].det.z2[c] = o.bands_[b].det.z2[c]; }
      bands_[b].env = o.bands_[b].env;
      bands_[b].phase = o.bands_[b].phase;
    }
  }

private:
  struct Band {
    Biquad f[kStages], det;
    int stages = 0, shape = Bell, chan = Stereo;
    bool active = false, dyn = false;
    double freq = 1000, gain = 0, q = 1, thr = -24, range = 0, env = 0, applied = 0;
    int phase = 0;  // samples into the current group of 16 (dynamic)
  };

  static Biquad::Kind kindOf(int shape) {
    switch (shape) {
      case LowShelf: return Biquad::LowShelf;
      case HighShelf: return Biquad::HighShelf;
      case LowCut: return Biquad::HighPass;
      case HighCut: return Biquad::LowPass;
      case Notch: return Biquad::Notch;
      default: return Biquad::Peak;
    }
  }
  // The stages of a band at a gain (the cuts' Butterworth Qs for 24 and 48 dB/oct; 12 dB/oct takes the band's Q).
  void design(Band& bd, double gain) {
    if (bd.shape == LowCut || bd.shape == HighCut) {
      static const double q24[2] = {0.5411961, 1.3065630}, q48[4] = {0.5097956, 0.6013449, 0.8999762, 2.5629154};
      for (int s = 0; s < bd.stages; ++s)
        bd.f[s].set(kindOf(bd.shape), sampleRate_, bd.freq, bd.stages == 1 ? bd.q : bd.stages == 2 ? q24[s] : q48[s], 0);
      return;
    }
    bd.f[0].set(kindOf(bd.shape), sampleRate_, bd.freq, bd.q, gain);
  }

  void setup() {
    for (int b = 0; b < kBands; ++b) {
      const double* p = params + b * kPerBand;
      Band& bd = bands_[b];
      bd.shape = std::clamp((int)std::lround(p[1]), 0, (int)Notch);
      bd.freq = std::clamp(p[2], 10.0, 22000.0);
      bd.gain = p[3];
      bd.q = std::clamp(p[4], 0.1, 18.0);
      bd.chan = std::clamp((int)std::lround(p[6]), 0, (int)Side);
      const bool gained = bd.shape == Bell || bd.shape == LowShelf || bd.shape == HighShelf;
      bd.dyn = gained && p[7] > 0.5 && p[9] != 0;
      bd.thr = p[8];
      bd.range = p[9];
      static const int kSlopeStages[3] = {1, 2, 4};
      bd.stages = bd.shape == LowCut || bd.shape == HighCut ? kSlopeStages[std::clamp((int)std::lround(p[5]), 0, 2)] : 1;
      bool does = true;
      if (gained) does = bd.gain != 0 || bd.dyn;
      if (bd.shape == LowCut) does = bd.freq > 20.5;
      if (bd.shape == HighCut) does = bd.freq < 19999.5;
      bd.active = p[0] > 0.5 && does;
      if (!bd.active) continue;
      if (bd.dyn) bd.det.set(Biquad::BandPass, sampleRate_, bd.freq, std::max(0.5, bd.q), 0);
      bd.applied = bd.gain;
      design(bd, bd.gain);
    }
    double trim = 0;
    if (params[kAutoGain] > 0.5) {  // the static curve's average over the spectrum (per octave), taken back
      double sum = 0;
      int count = 0;
      for (double f = 31.25; f <= 16000; f *= std::pow(2.0, 1.0 / 3.0), ++count) sum += 20 * std::log10(std::max(1e-6, magnitude(f)));
      trim = std::clamp(-sum / std::max(1, count), -12.0, 12.0);
    }
    out_ = dbToGain(params[kOutput] + trim);
  }

  // The static response at a frequency (stereo bands, without the dynamic part): for the auto gain.
  double magnitude(double freq) const {
    const double w = 2 * kPi * freq / sampleRate_;
    double m = 1;
    for (const Band& bd : bands_) {
      if (!bd.active || bd.chan == Left || bd.chan == Right || bd.chan == Side) continue;
      for (int s = 0; s < bd.stages; ++s) {
        const Biquad& q = bd.f[s];
        const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
        const double nr = q.b0 + q.b1 * c1 + q.b2 * c2, ni = -(q.b1 * s1 + q.b2 * s2);
        const double dr = 1 + q.a1 * c1 + q.a2 * c2, di = -(q.a1 * s1 + q.a2 * s2);
        m *= std::sqrt((nr * nr + ni * ni) / std::max(1e-18, dr * dr + di * di));
      }
    }
    return m;
  }

  // Runs a band's filters on samples [from, to) of its channel.
  static void run(Band& bd, float* l, float* r, int from, int to) {
    for (int i = from; i < to; ++i) {
      double a = l[i], b = r[i];
      if (bd.chan == Mid || bd.chan == Side) { const double m = (a + b) * 0.5, s = (a - b) * 0.5; a = m; b = s; }
      for (int s = 0; s < bd.stages; ++s) {
        if (bd.chan != Right && bd.chan != Side) a = bd.f[s].run(0, (float)a);
        if (bd.chan == Stereo || bd.chan == Right) b = bd.f[s].run(1, (float)b);
        if (bd.chan == Side) b = bd.f[s].run(1, (float)b);
      }
      if (bd.chan == Mid || bd.chan == Side) { l[i] = (float)(a + b); r[i] = (float)(a - b); }
      else { l[i] = (float)a; r[i] = (float)b; }
    }
  }

  // A dynamic band: its level (a band-pass at its frequency, on its channel) moves the gain by up to Range,
  // all of it 12 dB over Threshold. The filter is set again every 16 samples of the song, counted across blocks
  // (Codex review 2026-11-09 C3: the count started again in every block, so the sound depended on the buffer size);
  // each group of 16 is filtered with the gain the level had at the end of the group before.
  void dynamic(Band& bd, float* l, float* r, int n) {
    const double att = std::exp(-1.0 / (0.003 * sampleRate_)), rel = std::exp(-1.0 / (0.12 * sampleRate_));
    constexpr int kStep = 16;
    for (int from = 0; from < n;) {
      const int to = std::min(n, from + (kStep - bd.phase));
      for (int i = from; i < to; ++i) {
        double x;
        switch (bd.chan) {
          case Left: x = l[i]; break;
          case Right: x = r[i]; break;
          case Side: x = (l[i] - r[i]) * 0.5; break;
          default: x = (l[i] + r[i]) * 0.5; break;
        }
        const double y = std::fabs((double)bd.det.run(0, (float)x));
        bd.env = flush(y > bd.env ? att * bd.env + (1 - att) * y : rel * bd.env + (1 - rel) * y);
      }
      run(bd, l, r, from, to);
      bd.phase += to - from;
      from = to;
      if (bd.phase == kStep) {
        bd.phase = 0;
        const double over = std::max(0.0, 20 * std::log10(std::max(1e-9, bd.env)) - bd.thr);
        const double g = bd.gain + bd.range * std::min(1.0, over / 12.0);
        if (std::fabs(g - bd.applied) > 0.01) { bd.applied = g; design(bd, g); }
      }
    }
  }

  Band bands_[kBands];
  double out_ = 1;
  double seen_[kParams];
  bool init_ = [this] { std::fill(seen_, seen_ + kParams, -1e9); return true; }();
};

// ---- Filter: low-pass, high-pass or band-pass with resonance
class Filter final : public Effect {
public:
  using Effect::Effect;
  const char* name() const override { return "filter"; }
  // Types 0-2 are the first three (a biquad, as before); 3 and up are the voice filters (2026-11-07, parity with
  // Surge XT / Phase Plant): 3 Low 6, 4 High 6, 5 Band 24, 6 Notch, 7 Ladder, 8 Comb, 9 Formant, 10 Low 24, 11 High 24,
  // then 12 Diode, 13 MS-20 Low, 14 MS-20 High, 15 Comb -, 16 Phaser, 17 Morph, 18 Dual Notch, 19 Dual Band.
  // Drive (0 = clean, as before) and Shape (Morph, Phaser, Dual) are the voice filters' (dsp/voice_filter.hpp); a first-
  // three type with Drive runs as its voice-filter twin (Low 12, High 12, Band), clipped inside and oversampled.
  static constexpr int kTypes = 20;
  static constexpr int kMore[kTypes - 3] = {dsp::VoiceFilter::Low6, dsp::VoiceFilter::High6, dsp::VoiceFilter::Band24, dsp::VoiceFilter::Notch,
                                            dsp::VoiceFilter::Ladder, dsp::VoiceFilter::Comb, dsp::VoiceFilter::Formant,
                                            dsp::VoiceFilter::Low24, dsp::VoiceFilter::High24, dsp::VoiceFilter::Diode,
                                            dsp::VoiceFilter::Ms20Low, dsp::VoiceFilter::Ms20High, dsp::VoiceFilter::CombMinus,
                                            dsp::VoiceFilter::Phaser, dsp::VoiceFilter::Morph, dsp::VoiceFilter::DualNotch,
                                            dsp::VoiceFilter::DualBand};
  // The VoiceFilter type of a Filter mode (the instrument pages' curve uses it too).
  static int voiceType(int mode) {
    mode = std::clamp(mode, 0, kTypes - 1);
    if (mode >= 3) return kMore[mode - 3];
    return mode == 1 ? dsp::VoiceFilter::High12 : mode == 2 ? dsp::VoiceFilter::Band : dsp::VoiceFilter::Low12;
  }
  void process(float* l, float* r, int n) override {
    if (changed<6>(params, seen_)) {
      mode_ = std::clamp((int)std::lround(params[0]), 0, kTypes - 1);
      const double drive = std::clamp(params[4], 0.0, 1.0), shape = std::clamp(params[5], 0.0, 1.0);
      voice_ = mode_ >= 3 || drive > 0;
      if (!voice_) {
        f_.set(mode_ == 1 ? Biquad::HighPass : mode_ == 2 ? Biquad::BandPass : Biquad::LowPass, sampleRate_, params[1], params[2], 0);
      } else {
        // Resonance 0.707 (no peak) .. 10 becomes the voice filters' 0 .. 0.95.
        const double res = std::clamp((params[2] - 0.707) / (10 - 0.707), 0.0, 1.0) * 0.95;
        for (auto& v : more_) v.set(voiceType(mode_), params[1], res, sampleRate_, drive, shape);
      }
    }
    // Mix (2026-11-07): 1 = only the filtered sound (as before, exactly); less blends the dry sound back in.
    const double mix = std::clamp(params[3], 0.0, 1.0);
    if (mix >= 1) {
      if (!voice_) { for (int i = 0; i < n; ++i) { l[i] = f_.run(0, l[i]); r[i] = f_.run(1, r[i]); } return; }
      for (int i = 0; i < n; ++i) { l[i] = (float)more_[0].run(l[i]); r[i] = (float)more_[1].run(r[i]); }
      return;
    }
    for (int i = 0; i < n; ++i) {
      const double yl = !voice_ ? f_.run(0, l[i]) : more_[0].run(l[i]), yr = !voice_ ? f_.run(1, r[i]) : more_[1].run(r[i]);
      l[i] = (float)(mix * yl + (1 - mix) * l[i]);
      r[i] = (float)(mix * yr + (1 - mix) * r[i]);
    }
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Filter&>(p);
    std::memcpy(f_.z1, o.f_.z1, sizeof f_.z1); std::memcpy(f_.z2, o.f_.z2, sizeof f_.z2);
    more_[0] = o.more_[0]; more_[1] = o.more_[1];
  }
private:
  Biquad f_;
  dsp::VoiceFilter more_[2];
  int mode_ = 0;
  bool voice_ = false;
  double seen_[6] = {-1e9, -1e9, -1e9, -1e9, -1e9, -1e9};
};

// ---- Compressor: feed-forward, peak detector linked across L/R, soft knee
class Compressor final : public Effect {
public:
  using Effect::Effect;
  const char* name() const override { return "compressor"; }
  void process(float* l, float* r, int n) override {
    const double threshold = params[0], ratio = std::max(1.0, params[1]), knee = params[5];
    const double attack = std::exp(-1.0 / (std::max(0.05, params[2]) * 0.001 * sampleRate_));
    const double release = std::exp(-1.0 / (std::max(1.0, params[3]) * 0.001 * sampleRate_));
    const double makeup = dbToGain(params[4]);
    double deepest = 0;
    for (int i = 0; i < n; ++i) {
      // The level it follows: its own input, or the sidechain's key (M7c).
      const double peak = keyL ? std::max(std::fabs((double)keyL[i]), std::fabs((double)keyR[i]))
                               : std::max(std::fabs((double)l[i]), std::fabs((double)r[i]));
      env_ = flush(peak > env_ ? attack * env_ + (1 - attack) * peak : release * env_ + (1 - release) * peak);
      const double level = 20 * std::log10(std::max(env_, 1e-9)), over = level - threshold;
      double reduction = 0;  // dB, <= 0
      if (2 * over > knee) reduction = (1 / ratio - 1) * over;
      else if (knee > 0 && 2 * over > -knee) reduction = (1 / ratio - 1) * (over + knee / 2) * (over + knee / 2) / (2 * knee);
      deepest = std::min(deepest, reduction);
      const double g = dbToGain(reduction) * makeup;
      l[i] = (float)(l[i] * g);
      r[i] = (float)(r[i] * g);
    }
    reductionDb = (float)-deepest;
  }
  void adopt(Effect& p) override { env_ = static_cast<Compressor&>(p).env_; }
private:
  double env_ = 0;
};

// ---- Limiter: the output never goes above the ceiling
// True peak (one version only: the old sample-peak limiter, which let the wave pass the ceiling between samples,
// was removed on 2026-11-09 and older songs play this one). The level between samples is read at 8 points per sample (true_peak.hpp); the gain
// needed is held over the next 32 samples and smoothed over the same 32, so it is down before the peak
// arrives (look-ahead) and moves without a step. 47 samples late (1 ms at 48 kHz); until the audit of
// 2026-11-07 it read at 4x through a filter that missed up to 0.7 dB on bright masters (39 samples late).
class Limiter final : public Effect {
public:
  explicit Limiter(double sampleRate) : Effect(sampleRate) {}
  const char* name() const override { return "limiter"; }
  int latency() const override { return kLatency; }
  void process(float* l, float* r, int n) override {
    const double ceiling = dbToGain(params[0]);
    const double release = std::exp(-1.0 / (std::max(1.0, params[1]) * 0.001 * sampleRate_));
    double lowest = 1;
    struct Report {  // the deepest reduction of this call, on every return
      Effect& e; double& g;
      ~Report() { e.reductionDb = g < 1 ? (float)(-20 * std::log10(std::max(g, 1e-9))) : 0.0f; }
    } report{*this, lowest};
    // A margin below the ceiling covers what the reading misses between its 8 points (< 0.12 dB at 20 kHz).
    const double limit = ceiling * 0.986;
    for (int i = 0; i < n; ++i) {
      const double peak = std::max(tp_[0].push(l[i]), tp_[1].push(r[i]));  // around the sample kDelay back
      const double need = peak > limit ? limit / peak : 1.0;
      // Hold: the smallest gain needed over the last kWindow samples.
      need_[needAt_] = need;
      needAt_ = (needAt_ + 1) % kWindow;
      double held = 1.0;
      for (int k = 0; k < kWindow; ++k) held = std::min(held, need_[k]);
      // Smooth: the mean of the held gain over kWindow samples. Every sample in that window is the hold of
      // a span that covers the audio sample leaving now, so the mean is never above what it needs.
      sum_ += held - box_[boxAt_];
      box_[boxAt_] = held;
      boxAt_ = (boxAt_ + 1) % kWindow;
      const double boxed = std::min(1.0, sum_ / kWindow);
      gain_ = boxed < gain_ ? boxed : release * gain_ + (1 - release) * boxed;
      // The audio waits for its gain: kDelay (the reading) + kWindow - 1.
      const float dl = delay_[0][delayAt_], dr = delay_[1][delayAt_];
      delay_[0][delayAt_] = tp_[0].delayed();
      delay_[1][delayAt_] = tp_[1].delayed();
      delayAt_ = (delayAt_ + 1) % (kWindow - 1);
      lowest = std::min(lowest, gain_);
      l[i] = (float)(dl * gain_);
      r[i] = (float)(dr * gain_);
    }
  }
  void adopt(Effect& p) override {
    auto& o = static_cast<Limiter&>(p);
    gain_ = o.gain_;
    tp_[0] = o.tp_[0]; tp_[1] = o.tp_[1];
    std::memcpy(need_, o.need_, sizeof need_); needAt_ = o.needAt_;
    std::memcpy(box_, o.box_, sizeof box_); boxAt_ = o.boxAt_; sum_ = o.sum_;
    std::memcpy(delay_, o.delay_, sizeof delay_); delayAt_ = o.delayAt_;
  }
private:
  static constexpr int kWindow = 32, kLatency = TruePeakReader::kDelay + kWindow - 1;
  double gain_ = 1;
  TruePeakReader tp_[2];
  double need_[kWindow] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  double box_[kWindow] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
  double sum_ = kWindow;
  int needAt_ = 0, boxAt_ = 0, delayAt_ = 0;
  float delay_[2][kWindow - 1] = {};
};

// ---- Utility: gain, stereo width, mono below a frequency (bass stays in the middle), polarity (Ø: the wave
// upside down, when a kick and a bass cancel each other)
class Utility final : public Effect {
public:
  using Effect::Effect;
  const char* name() const override { return "utility"; }
  void process(float* l, float* r, int n) override {
    if (changed<1>(params + 2, seen_)) side_.set(Biquad::HighPass, sampleRate_, std::max(20.0, params[2]), 0.707, 0);
    const double gain = dbToGain(params[0]) * (params[3] > 0.5 ? -1.0 : 1.0), width = std::clamp(params[1], 0.0, 2.0);
    const bool monoBass = params[2] > 20.5;
    for (int i = 0; i < n; ++i) {
      const double mid = (l[i] + r[i]) * 0.5;
      double side = (l[i] - r[i]) * 0.5;
      if (monoBass) side = side_.run(0, (float)side);
      side *= width;
      l[i] = (float)((mid + side) * gain);
      r[i] = (float)((mid - side) * gain);
    }
  }
  void adopt(Effect& p) override { auto& o = static_cast<Utility&>(p); side_.z1[0] = o.side_.z1[0]; side_.z2[0] = o.side_.z2[0]; }
private:
  Biquad side_;
  double seen_[1] = {-1e9};
};

// ---- CORE FX: five effects fed by regions of the spectrum, in parallel with the dry sound.
// Each module (Saturate, Reverb, Delay, Modulate, Lo-fi) has up to four regions: a band-pass (centre,
// width in octaves; 8, the top, is the whole spectrum) and how much of that band is sent to it (0..1). Several regions of a module share its
// settings. What comes back ("Return", dB) is added to the dry sound; "Fit" passes it again through the
// module's regions, so new harmonics and echoes stay near where they were sent from. "Amount" scales every
// return: 0 is the dry sound only. "FX only" drops the dry sound (an FX bus). "Solo" (1..5) plays one
// module's return alone. A module turned off stops receiving (its send fades in 5 ms), and what it holds rings
// out (tails): attachments switch a module's On, so a delay or a reverb finishes when the part ends.
// The building blocks are the Core's own: SPACE (reverb), ECHO and SPIRAL (delay), FLUX (modulation), the
// 4x oversampler of the saturation and the true-peak limiter. Latency: the saturation's 16 samples plus the
// limiter's, always (the limiter off is a plain delay of the same length, so turning it on never shifts time).
// params: amount, fxOnly, limiter, output, solo, then per module (sat, rev, del, mod, lofi):
//   On, Type, A, B, Return, Fit, R1Freq, R1Amount, R1Width ... R4Freq, R4Amount, R4Width
class CoreFx final : public Effect {
public:
  static constexpr int kModules = 5, kRegions = 4, kGlobal = 5, kPerModule = 6 + 3 * kRegions;
  enum Module { Sat, Rev, Del, Mod, Lofi };
  static int slot(int module, int k) { return kGlobal + module * kPerModule + k; }  // k: 0 On, 1 Type, 2 A, 3 B, 4 Return, 5 Fit
  static int regionSlot(int module, int region, int k) { return slot(module, 6 + 3 * region + k); }  // k: 0 Freq, 1 Amount, 2 Width
  // After the modules (added at the end, so a saved Core FX keeps its places): Delay into reverb = how much of the
  // delay's echoes goes on into the reverb (the psytrance wash: the echoes smear into the tail); Dry = the dry level.
  static constexpr int kWash = kGlobal + kModules * kPerModule, kDry = kWash + 1;

  CoreFx(double sampleRate, double bpm) : Effect(sampleRate), bpm_(bpm) {
    space_ = makeDevice("space", sampleRate, bpm, 1);
    echo_ = makeDevice("echo", sampleRate, bpm, 1);
    spiral_ = makeDevice("spiral", sampleRate, bpm, 1);
    flux_ = makeDevice("flux", sampleRate, bpm, 1);
    limiter_ = std::make_unique<Limiter>(sampleRate);
    limiter_->params[0] = -0.3; limiter_->params[1] = 80;
    // The saturation's up-sampler path is fixed here (AVX or the scalar reference, the same PCM), and its
    // tables are built here, not on the audio thread.
    if (dspPath() == DspPath::Avx) { kernel_ = upsampleKernelAvx(); taps_ = &upsampleTaps(); }
    (void)osFir(); (void)upsampleTaps();
    for (auto& b : buf_) b.assign(kChunk, 0.0f);
    idleAfter_ = (int)(0.5 * sampleRate);
  }
  const char* name() const override { return "corefx"; }
  int latency() const override { return kSatLatency + kLimLatency; }
  void setTempo(double bpm) override { bpm_ = bpm; for (Effect* e : {space_.get(), echo_.get(), spiral_.get(), flux_.get()}) e->setTempo(bpm); }
  void setPosition(double beat) override { for (Effect* e : {space_.get(), echo_.get(), spiral_.get(), flux_.get()}) e->setPosition(beat); }

  void process(float* l, float* r, int n) override {
    for (int at = 0; at < n; at += kChunk) chunk(l + at, r + at, std::min(kChunk, n - at));
  }

  void adopt(Effect& p) override {
    auto& o = static_cast<CoreFx&>(p);
    space_->adopt(*o.space_); echo_->adopt(*o.echo_); spiral_->adopt(*o.spiral_); flux_->adopt(*o.flux_); limiter_->adopt(*o.limiter_);
    std::memcpy(send_, o.send_, sizeof send_); std::memcpy(fit_, o.fit_, sizeof fit_);
    std::memcpy(dry_, o.dry_, sizeof dry_); dryAt_ = o.dryAt_;
    std::memcpy(limDelay_, o.limDelay_, sizeof limDelay_); limAt_ = o.limAt_;
    for (int c = 0; c < 2; ++c) { up_[c] = o.up_[c]; down_[c] = o.down_[c]; }
    std::memcpy(satState_, o.satState_, sizeof satState_);
    std::memcpy(idle_, o.idle_, sizeof idle_);
    std::memcpy(lofi_, o.lofi_, sizeof lofi_);
    std::memcpy(retGain_, o.retGain_, sizeof retGain_);
    std::memcpy(sendGain_, o.sendGain_, sizeof sendGain_);
    rng_ = o.rng_;
  }

  // The magnitude of one region's band-pass at freq (the curve the window draws): 0..1 at the centre.
  static double regionResponse(double freq, double centre, double widthOct) {
    if (widthOct >= kFullWidth) return 1.0;
    const double q = qOf(widthOct), x = freq / std::max(1.0, centre);
    const double d = q * (x - 1.0 / x);
    return 1.0 / std::sqrt(1.0 + d * d);
  }

private:
  static constexpr int kChunk = 256;
  static constexpr double kFullWidth = 7.99;  // a region this wide (the top of Width) is the whole spectrum
  static constexpr int kSatLatency = 2 * (kOsTaps - 1) / 2 / kOsFactor;  // 16, as the saturation's
  static constexpr int kLimLatency = TruePeakReader::kDelay + 32 - 1;  // the limiter v2's
  static constexpr float kSafety = 3.98107f;  // +12 dBFS: the ceiling with the limiter off
  // A band-pass of `width` octaves between its -3 dB points.
  static double qOf(double widthOct) {
    const double w = std::pow(2.0, std::clamp(widthOct, 0.1, 10.0));
    return std::sqrt(w) / (w - 1.0);
  }
  // Region band-pass: Cytomic SVF, unity gain at the centre (k * band), stable when the centre moves.
  struct Band {
    double g = 0, k = 1, a1 = 1, a2 = 0, a3 = 0, w = 0;  // w: the region's weight (its amount)
    bool full = false;  // the whole spectrum: no filter
    double ic1[2] = {0, 0}, ic2[2] = {0, 0};
    void set(double sr, double freq, double q, double weight, bool all) {
      full = all;
      g = std::tan(kPi * std::clamp(freq, 10.0, sr * 0.45) / sr);
      k = 1.0 / std::max(0.01, q);
      a1 = 1.0 / (1.0 + g * (g + k)); a2 = g * a1; a3 = g * a2; w = weight;
    }
    double run(int c, double x) {
      if (full) return x;
      const double v3 = x - ic2[c], v1 = a1 * ic1[c] + a2 * v3, v2 = ic2[c] + a2 * ic1[c] + a3 * v3;
      ic1[c] = flush(2 * v1 - ic1[c]);
      ic2[c] = flush(2 * v2 - ic2[c]);
      return k * v1;
    }
  };

  double p(int i) const { return params[i]; }
  int ip(int i, int lo, int hi) const { return std::clamp((int)std::lround(params[i]), lo, hi); }

  void chunk(float* l, float* r, int n) {
    const double amount = std::clamp(p(0), 0.0, 1.0), out = dbToGain(std::clamp(p(3), -24.0, 6.0));
    const bool fxOnly = p(1) >= 0.5, limit = p(2) >= 0.5;
    const int solo = ip(4, 0, kModules);
    float* in[2] = {l, r};
    // The dry sound, delayed like the saturation's return.
    float dry[2][kChunk];
    for (int i = 0; i < n; ++i) for (int c = 0; c < 2; ++c) dry[c][i] = dryRead(c, in[c][i]);
    double mix[2][kChunk];
    const double dryLevel = std::clamp(p(kDry), 0.0, 1.0);
    for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i) mix[c][i] = (fxOnly || solo) ? 0.0 : dry[c][i] * dryLevel;
    // Delay into reverb: the delay runs first and its echoes (after Fit) go into the reverb's send.
    const double wash = std::clamp(p(kWash), 0.0, 1.0);
    static const int kPlain[kModules] = {Sat, Rev, Del, Mod, Lofi}, kWashed[kModules] = {Sat, Del, Rev, Mod, Lofi};
    const int* order = wash > 0 ? kWashed : kPlain;
    float echoes[2][kChunk];
    bool haveEchoes = false;
    for (int mi = 0; mi < kModules; ++mi) {
      const int m = order[mi];
      const bool on = p(slot(m, 0)) >= 0.5;
      // The return glides to its gain (Amount 0: nothing), so a module turned up or off does not click.
      const double target = (solo && solo != m + 1) ? 0.0 : amount * dbToGain(std::clamp(p(slot(m, 4)), -60.0, 24.0));
      if (!on && sendGain_[m] == 0 && idle_[m] >= idleAfter_) { retGain_[m] = target; continue; }
      setBands(m);
      float* sl = buf_[0].data(); float* sr = buf_[1].data();
      const bool sendFromDry = m != Sat;  // the saturation is 16 samples late itself
      // The send glides in and out (5 ms) when the module is switched (an attachment, a click on On), so what goes
      // in never starts or stops with a step; what is already in rings out.
      const double sendTarget = on ? 1.0 : 0.0, sendGlide = 1.0 - std::exp(-1.0 / (0.005 * sampleRate_));
      for (int i = 0; i < n; ++i) {
        sendGain_[m] += (sendTarget - sendGain_[m]) * sendGlide;
        if (std::fabs(sendTarget - sendGain_[m]) < 1e-6) sendGain_[m] = sendTarget;
        double s[2] = {0, 0};
        if (sendGain_[m] > 0)
          for (int c = 0; c < 2; ++c) {
            const double x = sendFromDry ? dry[c][i] : in[c][i];
            for (int k = 0; k < kRegions; ++k) if (send_[m][k].w > 0) s[c] += send_[m][k].w * send_[m][k].run(c, x);
            s[c] *= sendGain_[m];
          }
        if (m == Rev && haveEchoes) { s[0] += wash * sendGain_[m] * echoes[0][i]; s[1] += wash * sendGain_[m] * echoes[1][i]; }
        sl[i] = (float)s[0]; sr[i] = (float)s[1];
      }
      runModule(m, sl, sr, n);
      const bool fit = p(slot(m, 5)) >= 0.5;
      double energy = 0;
      const double glide = 1.0 - std::exp(-1.0 / (0.02 * sampleRate_));
      for (int i = 0; i < n; ++i) {
        retGain_[m] += (target - retGain_[m]) * glide;
        if (std::fabs(target - retGain_[m]) < 1e-6) retGain_[m] = target;  // lands exactly (Amount 0: the dry sound, bit for bit)
        double y[2] = {sl[i], sr[i]};
        if (fit)
          for (int c = 0; c < 2; ++c) {
            double f = 0; bool any = false;
            for (int k = 0; k < kRegions; ++k) if (fit_[m][k].w > 0) { f += fit_[m][k].run(c, y[c]); any = true; }
            y[c] = any ? f : 0.0;
          }
        energy += y[0] * y[0] + y[1] * y[1];
        if (m == Del && wash > 0) { echoes[0][i] = (float)y[0]; echoes[1][i] = (float)y[1]; }
        mix[0][i] += y[0] * retGain_[m];
        mix[1][i] += y[1] * retGain_[m];
      }
      if (on || sendGain_[m] > 0 || energy > 1e-9 * n) idle_[m] = 0; else idle_[m] += n;
      if (m == Del && wash > 0) haveEchoes = true;
    }
    for (int c = 0; c < 2; ++c) for (int i = 0; i < n; ++i) in[c][i] = (float)(mix[c][i] * out);
    if (limit) limiter_->process(l, r, n);
    else
      for (int i = 0; i < n; ++i) {  // the limiter's latency without the limiter
        for (int c = 0; c < 2; ++c) { const float y = limDelay_[c][limAt_]; limDelay_[c][limAt_] = in[c][i]; in[c][i] = y; }
        limAt_ = (limAt_ + 1) % kLimLatency;
        // Audit 2026-11-09 (C1): with the limiter off and every knob at its top the output reached +46.8 dBFS.
        // A safety clamp at +12 dBFS catches only that runaway: anything under it (every real mix) passes
        // bit for bit, and with the limiter on nothing here runs.
        for (int c = 0; c < 2; ++c) in[c][i] = std::clamp(in[c][i], -kSafety, kSafety);
      }
    if (!limit) limiter_->reductionDb = 0;
    reductionDb = limiter_->reductionDb;
  }

  float dryRead(int c, float x) {
    const float y = dry_[c][dryAt_];
    dry_[c][dryAt_] = x;
    if (c == 1) dryAt_ = (dryAt_ + 1) % kSatLatency;
    return y;
  }

  void setBands(int m) {
    for (int k = 0; k < kRegions; ++k) {
      const double f = std::clamp(p(regionSlot(m, k, 0)), 20.0, 20000.0), a = std::clamp(p(regionSlot(m, k, 1)), 0.0, 1.0);
      const double w = p(regionSlot(m, k, 2)), q = qOf(w);
      send_[m][k].set(sampleRate_, f, q, a, w >= kFullWidth);
      fit_[m][k].set(sampleRate_, f, q, a > 0 ? 1.0 : 0.0, w >= kFullWidth);
    }
  }

  void runModule(int m, float* l, float* r, int n) {
    switch (m) {
      case Sat: saturate(l, r, n); break;
      case Rev: reverb(l, r, n); break;
      case Del: delay(l, r, n); break;
      case Mod: modulate(l, r, n); break;
      default: lofi(l, r, n); break;
    }
  }

  // Saturate: Soft clip, Tube, Tape, Fold, Hard clip, Rectify at 4x the rate; Drive dB, Tone (dark..bright).
  // As the old saturation: more Drive is louder until it clips (Return sets the level that comes back), so a song
  // made with the old saturation sounds the same.
  static double shape(int type, double v) {
    switch (type) {
      case 1: return std::tanh(v + 0.3) - 0.2913126124515909;  // tube: asymmetric, even harmonics (tanh(0.3))
      case 2: return v / (1.0 + std::fabs(v)) * 1.2;           // tape: the gentlest knee
      case 3: return std::sin(0.5 * kPi * std::clamp(v, -6.0, 6.0));  // fold
      case 4: return std::clamp(v, -1.0, 1.0);                 // hard clip
      case 5: return std::fabs(std::tanh(v));                  // rectify: an octave up
      default: return std::tanh(v);                            // soft clip
    }
  }
  void saturate(float* l, float* r, int n) {
    const int type = ip(slot(Sat, 1), 0, 5);
    const double drive = dbToGain(std::clamp(p(slot(Sat, 2)), 0.0, 36.0));
    const double tone = std::clamp(p(slot(Sat, 3)), 0.0, 1.0);
    const double lp = 1.0 - std::exp(-2 * kPi * std::min(1000.0 * std::pow(20.0, tone), sampleRate_ * 0.45) / sampleRate_);
    const double dc = 1.0 - 2 * kPi * 10.0 / sampleRate_;
    float* ch[2] = {l, r};
    for (int c = 0; c < 2; ++c) {
      double* st = satState_[c];  // 0 tone, 1 dc in, 2 dc out
      for (int i = 0; i < n; ++i) {
        const double x = ch[c][i];
        double up[kOsFactor];
        if (kernel_) up_[c].push((float)x, up, kernel_, *taps_);
        else up_[c].push((float)x, up);
        for (double& v : up) v = shape(type, v * drive);
        double y = down_[c].push(up);
        const double hp = y - st[1] + dc * st[2]; st[1] = y; st[2] = flush(hp); y = hp;  // DC (tube, rectify)
        st[0] = flush(st[0] + (y - st[0]) * lp);
        ch[c][i] = (float)st[0];
      }
    }
  }

  // Reverb: SPACE. Hall, Cathedral, Shimmer, and SPACE's own rooms: Plate, Spring, Reverse, Stone church (its
  // Cathedral); Pre-delay ms, Decay s. Only the tail comes back. Types 0-2 sound as before (SPACE's Hall).
  void reverb(float* l, float* r, int n) {
    const int type = ip(slot(Rev, 1), 0, 6);
    static const double size[7] = {0.6, 1.0, 0.8, 0.5, 0.6, 0.6, 1.0}, tone[7] = {7000, 5000, 9000, 9000, 4500, 7000, 5000},
                        shimmer[7] = {0, 0, 0.6, 0, 0, 0, 0}, kind[7] = {0, 0, 0, 1, 2, 4, 3}, motion[7] = {0.3, 0.3, 0.3, 0.1, 0, 0.3, 0.4};
    double* q = space_->params;
    q[0] = size[type]; q[1] = std::clamp(p(slot(Rev, 3)), 0.2, 20.0); q[2] = std::clamp(p(slot(Rev, 2)), 0.0, 250.0);
    q[3] = tone[type]; q[4] = 20; q[5] = motion[type]; q[6] = 1; q[7] = shimmer[type]; q[8] = 1; q[9] = kind[type]; q[10] = 0;
    space_->process(l, r, n);
  }

  // Delay: Ping pong and Stereo (ECHO), Freq shift (SPIRAL); Time (1/16 .. 1 bar), Feedback.
  void delay(float* l, float* r, int n) {
    const int type = ip(slot(Del, 1), 0, 2);
    const double time = ip(slot(Del, 2), 0, 7), fb = std::clamp(p(slot(Del, 3)), 0.0, 0.95);
    if (type < 2) {
      double* q = echo_->params;
      q[0] = time; q[1] = fb; q[2] = 6000; q[3] = 20; q[4] = type == 0 ? 1.0 : 0.0; q[5] = 0; q[6] = 1; q[7] = 1;
      echo_->process(l, r, n);
    } else {
      double* q = spiral_->params;
      q[0] = time; q[1] = 8; q[2] = fb; q[3] = 6000; q[4] = 20; q[5] = 1; q[6] = 1;
      spiral_->process(l, r, n);
    }
  }

  // Modulate: FLUX's Phaser, Flanger, Chorus; Depth, Rate (1/16 .. 4 bars, on the song's tempo).
  void modulate(float* l, float* r, int n) {
    static const double beats[7] = {0.25, 0.5, 1, 2, 4, 8, 16};
    double* q = flux_->params;
    q[0] = ip(slot(Mod, 1), 0, 2);
    q[1] = std::max(0.02, bpm_ / 60.0 / beats[ip(slot(Mod, 3), 0, 6)]);
    q[2] = std::clamp(p(slot(Mod, 2)), 0.0, 1.0); q[3] = 0.5; q[4] = 1;
    flux_->process(l, r, n);
  }

  // Lo-fi: Digital (bits, sample rate), Radio (a narrow band, a little drive), Vinyl (dark, hiss and crackle).
  void lofi(float* l, float* r, int n) {
    const int type = ip(slot(Lofi, 1), 0, 2);
    const double bits = std::clamp(p(slot(Lofi, 2)), 2.0, 16.0), rate = std::clamp(p(slot(Lofi, 3)), 500.0, sampleRate_);
    const double steps = std::pow(2.0, bits - 1), step = rate / sampleRate_;
    const double hpA = 1.0 - std::exp(-2 * kPi * (type == 1 ? 450.0 : 30.0) / sampleRate_);
    const double lpA = 1.0 - std::exp(-2 * kPi * (type == 1 ? 3200.0 : type == 2 ? 7000.0 : 20000.0) / sampleRate_);
    float* ch[2] = {l, r};
    for (int i = 0; i < n; ++i) {
      lofi_[4] += step;
      const bool take = lofi_[4] >= 1.0;
      if (take) lofi_[4] -= 1.0;
      double noise = 0;
      if (type == 2) {  // hiss at -54 dB and a crackle now and then
        rng_ = rng_ * 1664525u + 1013904223u;
        const double u = (rng_ >> 8) * (1.0 / 16777216.0);
        noise = (u - 0.5) * 0.004 + (u > 0.9997 ? (u - 0.99985) * 400.0 : 0.0);
      }
      for (int c = 0; c < 2; ++c) {
        double x = ch[c][i];
        if (take) lofi_[c] = std::round(std::clamp(x, -1.0, 1.0) * steps) / steps;
        x = lofi_[c];
        if (type == 1) x = std::tanh(x * 2.0) * 0.6;
        lofi_[2 + c] = flush(lofi_[2 + c] + (x - lofi_[2 + c]) * lpA);
        x = lofi_[2 + c];
        lofiHp_[c] = flush(lofiHp_[c] + (x - lofiHp_[c]) * hpA);
        ch[c][i] = (float)(x - lofiHp_[c] + noise);
      }
    }
  }

  double bpm_;
  std::unique_ptr<Effect> space_, echo_, spiral_, flux_, limiter_;
  std::vector<float> buf_[2];
  Band send_[kModules][kRegions], fit_[kModules][kRegions];
  float dry_[2][kSatLatency] = {};
  int dryAt_ = 0;
  float limDelay_[2][kLimLatency] = {};
  int limAt_ = 0;
  UpsampleKernel kernel_ = nullptr;  // null: the scalar reference
  const UpsampleTaps* taps_ = nullptr;
  Upsampler up_[2];
  Downsampler down_[2];
  double satState_[2][5] = {};
  double retGain_[kModules] = {};
  double sendGain_[kModules] = {};
  int idle_[kModules] = {};
  int idleAfter_ = 0;
  double lofi_[5] = {};  // held L/R, low-pass L/R, rate phase
  double lofiHp_[2] = {};
  uint32_t rng_ = 22222u;
};


// CORE FX's catalogue: 5 global parameters, then 18 per module (see CoreFx).
// CORE EQ's catalogue: 8 bands of 10 (b1On, b1Type, b1Freq, b1Gain, b1Q, b1Slope, b1Chan, b1Dyn, b1Thr, b1Range ...),
// then output and autoGain. Bands 1-5 start as the old EQ's knobs (all doing nothing): low cut 20 Hz, low shelf
// 100 Hz, bell 1 kHz, high shelf 8 kHz, high cut 20 kHz; 6-8 start off.
std::vector<ParamSpec> coreEqParams() {
  static std::vector<std::string> names;
  constexpr int kB = 8;
  if (names.empty()) {
    names.reserve(kB * 10);
    for (int b = 1; b <= kB; ++b)
      for (const char* k : {"On", "Type", "Freq", "Gain", "Q", "Slope", "Chan", "Dyn", "Thr", "Range"})
        names.push_back("b" + std::to_string(b) + k);
  }
  struct Def { double on, type, freq, q; };
  static const Def defs[kB] = {{1, 3, 20, 0.707}, {1, 1, 100, 0.707}, {1, 0, 1000, 1}, {1, 2, 8000, 0.707},
                               {1, 4, 20000, 0.707}, {0, 0, 250, 1}, {0, 0, 2500, 1}, {0, 0, 5000, 1}};
  std::vector<ParamSpec> out;
  size_t at = 0;
  for (int b = 0; b < kB; ++b) {
    out.push_back({names[at++].c_str(), defs[b].on, 0, 1, ""});
    out.push_back({names[at++].c_str(), defs[b].type, 0, 5, ""});
    out.push_back({names[at++].c_str(), defs[b].freq, 20, 20000, "Hz"});
    out.push_back({names[at++].c_str(), 0, -24, 24, "dB"});
    out.push_back({names[at++].c_str(), defs[b].q, 0.1, 18, ""});
    out.push_back({names[at++].c_str(), 0, 0, 2, ""});
    out.push_back({names[at++].c_str(), 0, 0, 4, ""});
    out.push_back({names[at++].c_str(), 0, 0, 1, ""});
    out.push_back({names[at++].c_str(), -24, -60, 0, "dB"});
    out.push_back({names[at++].c_str(), -6, -24, 24, "dB"});
  }
  out.push_back({"output", 0, -24, 24, "dB"});
  out.push_back({"autoGain", 0, 0, 1, ""});
  return out;
}

std::vector<ParamSpec> coreFxParams() {
  static std::vector<std::string> names;  // ParamSpec keeps the name's pointer: built once, kept
  struct Mod { const char* id; double type; ParamSpec a, b; bool on; double freq, amount, width; };
  const Mod mods[CoreFx::kModules] = {
      {"sat", 5, {"Drive", 6, 0, 36, "dB"}, {"Tone", 0.5, 0, 1, ""}, true, 150, 0.4, 2},
      {"rev", 6, {"PreDelay", 20, 0, 250, "ms"}, {"Decay", 2, 0.2, 20, "s"}, true, 6000, 0.3, 2.5},
      {"del", 2, {"Time", 2, 0, 7, ""}, {"Feedback", 0.35, 0, 0.95, ""}, true, 2500, 0.3, 2},
      {"mod", 2, {"Depth", 0.5, 0, 1, ""}, {"Rate", 2, 0, 6, ""}, false, 900, 0.3, 2},
      {"lofi", 2, {"Bits", 12, 2, 16, ""}, {"Rate", 22000, 1000, 48000, "Hz"}, false, 9000, 0.25, 1.5}};
  if (names.empty()) {
    names.reserve(CoreFx::kModules * CoreFx::kPerModule);
    for (const auto& m : mods) {
      for (const char* k : {"On", "Type"}) names.push_back(std::string(m.id) + k);
      names.push_back(std::string(m.id) + m.a.name);
      names.push_back(std::string(m.id) + m.b.name);
      for (const char* k : {"Return", "Fit"}) names.push_back(std::string(m.id) + k);
      for (int r = 1; r <= CoreFx::kRegions; ++r)
        for (const char* k : {"Freq", "Amount", "Width"}) names.push_back(std::string(m.id) + "R" + std::to_string(r) + k);
    }
  }
  std::vector<ParamSpec> out = {{"amount", 0.5, 0, 1, ""}, {"fxOnly", 0, 0, 1, ""}, {"limiter", 1, 0, 1, ""},
                                {"output", 0, -24, 6, "dB"}, {"solo", 0, 0, 5, ""}};
  size_t at = 0;
  for (const auto& m : mods) {
    out.push_back({names[at++].c_str(), m.on ? 1.0 : 0.0, 0, 1, ""});
    out.push_back({names[at++].c_str(), 0, 0, m.type, ""});
    out.push_back({names[at++].c_str(), m.a.def, m.a.min, m.a.max, m.a.unit});
    out.push_back({names[at++].c_str(), m.b.def, m.b.min, m.b.max, m.b.unit});
    out.push_back({names[at++].c_str(), 0, -24, 24, "dB"});
    out.push_back({names[at++].c_str(), 1, 0, 1, ""});
    static const double spread[CoreFx::kRegions] = {1, 0.5, 2, 4};
    for (int r = 0; r < CoreFx::kRegions; ++r) {
      out.push_back({names[at++].c_str(), std::clamp(m.freq * spread[r], 20.0, 20000.0), 20, 20000, "Hz"});
      out.push_back({names[at++].c_str(), r == 0 ? m.amount : 0.0, 0, 1, ""});
      out.push_back({names[at++].c_str(), m.width, 0.3, 8, "oct"});
    }
  }
  out.push_back({"delToRev", 0, 0, 1, ""});
  out.push_back({"dry", 1, 0, 1, ""});
  return out;
}

}  // namespace

#ifndef CORE_DSP_AVX
UpsampleKernel upsampleKernelAvx() { return nullptr; }
#endif

bool cpuRunsAvx() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
  int info[4] = {};
  __cpuid(info, 1);
  const bool avx = (info[2] & (1 << 28)) != 0, osxsave = (info[2] & (1 << 27)) != 0;
  if (!avx || !osxsave) return false;
  return (_xgetbv(0) & 6) == 6;  // the OS saves XMM and YMM
#elif (defined(__GNUC__) || defined(__clang__)) && (defined(__x86_64__) || defined(__i386__))
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (!__get_cpuid(1, &a, &b, &c, &d)) return false;
  if (!(c & (1u << 28)) || !(c & (1u << 27))) return false;
  unsigned lo = 0, hi = 0;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return (lo & 6) == 6;
#else
  return false;
#endif
}

DspPath resolveDspPath(bool built, bool supported, const char* env) {
  if (env && std::strcmp(env, "scalar") == 0) return DspPath::Scalar;
  return built && supported ? DspPath::Avx : DspPath::Scalar;
}

namespace {
std::atomic<int> gForcedDspPath{-1};
// CORE_DSP_SIMD read once at start-up ("" when unset), without MSVC's getenv deprecation warning.
std::string simdEnv() {
#ifdef _MSC_VER
  char* value = nullptr;
  size_t size = 0;
  std::string out;
  if (_dupenv_s(&value, &size, "CORE_DSP_SIMD") == 0 && value) out = value;
  std::free(value);
  return out;
#else
  const char* value = std::getenv("CORE_DSP_SIMD");
  return value ? value : "";
#endif
}
}

DspPath dspPath() {
  const int forced = gForcedDspPath.load(std::memory_order_relaxed);
  if (forced >= 0) return (DspPath)forced;
  static const DspPath resolved = resolveDspPath(upsampleKernelAvx() != nullptr, cpuRunsAvx(), simdEnv().c_str());
  return resolved;
}

const char* dspPathName(DspPath path) { return path == DspPath::Avx ? "avx" : "scalar"; }

void forceDspPathForTests(DspPath path) {
  const bool avx = path == DspPath::Avx && upsampleKernelAvx() != nullptr && cpuRunsAvx();
  gForcedDspPath.store((int)(avx ? DspPath::Avx : DspPath::Scalar), std::memory_order_relaxed);
}

// Every built-in effect describes itself here once: id, DSP version, parameter catalogue (in the order
// Effect::params uses) and how to build it. Single source of truth for validation, the graph and the app.
void registerNativeEffects(NativeRegistry& reg) {
  reg.add({"eq", NativeKind::Effect, 2, false, coreEqParams(), nullptr,
           [](double sr, double, int) { return std::make_unique<Eq>(sr); }});
  reg.add({"filter", NativeKind::Effect, 1, false,
           {{"mode", 0, 0, 19, ""}, {"cutoff", 2000, 20, 20000, "Hz"}, {"resonance", 0.707, 0.3, 10, ""}, {"mix", 1, 0, 1, ""},
            {"drive", 0, 0, 1, ""}, {"shape", 0.5, 0, 1, ""}},
           nullptr, [](double sr, double, int) { return std::make_unique<Filter>(sr); }});
  reg.add({"compressor", NativeKind::Effect, 1, false,
           {{"threshold", -18, -60, 0, "dB"}, {"ratio", 4, 1, 20, ":1"}, {"attack", 10, 0.1, 100, "ms"},
            {"release", 100, 10, 1000, "ms"}, {"makeup", 0, 0, 24, "dB"}, {"knee", 6, 0, 12, "dB"}},
           nullptr, [](double sr, double, int) { return std::make_unique<Compressor>(sr); }});



  reg.add({"limiter", NativeKind::Effect, 2, false,
           {{"ceiling", -0.3, -12, 0, "dB"}, {"release", 80, 10, 500, "ms"}},
           nullptr, [](double sr, double, int) { return std::make_unique<Limiter>(sr); }});
  reg.add({"utility", NativeKind::Effect, 1, false,
           {{"gain", 0, -24, 24, "dB"}, {"width", 1, 0, 2, ""}, {"monoBelow", 20, 20, 300, "Hz"}, {"polarity", 0, 0, 1, ""}},
           nullptr, [](double sr, double, int) { return std::make_unique<Utility>(sr); }});
  registerNativeDevices(reg);  // Core Devices (devices.cpp), after the classic effects
  reg.add({"corefx", NativeKind::Effect, 1, false, coreFxParams(), nullptr,
           [](double sr, double bpm, int) { return std::make_unique<CoreFx>(sr, bpm); }});
}

std::unique_ptr<Effect> makeEffect(const std::string& name, double sampleRate, double bpm, int version) {
  const NativePluginDesc* d = nativePlugins().find(NativeKind::Effect, name);
  if (d && d->makeEffect) return d->makeEffect(sampleRate, bpm, version);
  return nullptr;
}

} // namespace core
