#pragma once
// The filter types of W-Core and the Filter effect, for parity with Surge XT, Phase Plant, Serum and Vital
// (Diogo 2026-11-07: "acho que ainda faltam imensos filtros").
// - 6 dB low and high, 12 dB high, 24 dB band;
// - Ladder: four one-pole stages in a saturated feedback loop, the classic analog (Moog) low-pass;
// - Comb + / Comb -: a short delay tuned to the cutoff, fed back in phase (metallic, plucked) or inverted (hollow,
//   odd harmonics, an octave down);
// - Formant: three resonant bands at the vowel formants; the cutoff moves A-E-I-O-U;
// - Diode: the TB-303's diode ladder (four coupled stages, a high-pass in the loop): thinner bass at high resonance,
//   the squelch of acid;
// - MS-20 Low / MS-20 High: the Korg 35 two-pole with a clipped feedback: it screams and growls instead of singing;
// - Phaser: six all-pass stages with feedback: moving notches (the psychedelic sweep); Shape spreads the stages;
// - Morph: a 24 dB low-pass that Shape turns into a band-pass (0.5) and a high-pass (1), automatable;
// - Dual Notch / Dual Band: two filters an interval apart around the cutoff (Shape = 0 .. 4 octaves apart).
// Drive (0..1) pushes the sound into the filter: a soft clip at its input and, for the resonant types, inside the
// loop (a filter that growls as it resonates). The types with a loop that clips (Diode, MS-20) and any type with
// Drive run at ~192 kHz inside (4x at 48 kHz, 2x where W-Core's voices already run at 96 kHz; all-pass half-bands up
// and down: no latency to report), so the clipping's harmonics do not fold back as noise.
// One VoiceFilter per channel and per filter. Every type is stable for every cutoff, resonance, drive and shape (tests).
#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace core::dsp {

class VoiceFilter {
public:
  // The types after the Synth's first five (0 Low 24, 1 Low 12, 2 Band, 3 High, 4 Notch), numbered on from them.
  enum Type { Low24 = 0, Low12, Band, High24, Notch, Low6, High6, Band24, High12, Ladder, Comb, Formant,
              Diode, Ms20Low, Ms20High, CombMinus, Phaser, Morph, DualNotch, DualBand, kCount };

  void reset() { *this = VoiceFilter{}; }

  // How many times the rate this type runs at inside (1, 2 or 4), at this drive and sample rate: the clipping types
  // run at ~192 kHz (a hard clip of a 5 kHz tone folds back under -60 dB in the audible band).
  static int oversampling(int type, double drive, double sr) {
    if (!(drive > 0 || type == Diode || type == Ms20Low || type == Ms20High)) return 1;
    return sr < 80000 ? 4 : sr < 160000 ? 2 : 1;
  }

  // drive 0..1 (0 = clean, as before); shape 0..1 (Phaser, Morph, Dual: see above; 0.5 is their middle).
  void set(int type, double cutoff, double resonance, double sr, double drive = 0, double shape = 0.5) {
    type_ = std::clamp(type, 0, kCount - 1);
    drive_ = std::clamp(drive, 0.0, 1.0);
    shape_ = std::clamp(shape, 0.0, 1.0);
    os_ = oversampling(type_, drive_, sr);
    const double rs = os_ * sr;  // the rate the filter itself runs at
    // The cutoff's top: 0.45 of the outer rate (as before), at either inner rate.
    const double fc = std::clamp(cutoff, 20.0, sr * 0.45);
    const double res = std::clamp(resonance, 0.0, 0.95);
    dg_ = 1 + 7 * drive_;                       // up to +18 dB into the clip
    dn_ = drive_ > 0 ? std::pow(dg_, -0.6) : 1;  // and back down: a little louder (+3 dB at most on a saw), not by the whole drive
    switch (type_) {
      case Low6: case High6: op_ = 1 - std::exp(-2 * kPi * fc / rs); break;
      case Ladder:
        lg_ = 1 - std::exp(-2 * kPi * fc / rs);
        lk_ = 3.8 * res;  // 4 = self-oscillation; just under it
        break;
      case Diode: case Ms20Low: case Ms20High: {
        // The loops below stay stable (linearised, measured 2026-11-07) up to a coefficient of 0.76: their cutoff stops
        // at 0.225 of the inner rate (21.6 kHz at 48 kHz, which runs them at 96 kHz).
        const double g = 1 - std::exp(-2 * kPi * std::min(fc, 0.225 * rs) / rs);
        if (type_ == Diode) {
          lg_ = g;
          // The loop gain that self-oscillates here (4 at low cutoffs, more as the loop's delay grows), times
          // resonance; the loop's high-pass makes the bottom octaves resonate less (as a 303 does).
          lk_ = res * 0.97 * 4 * (1 + 0.8 * g * g);
          dhp_ = 1 - std::exp(-2 * kPi * 150 / rs);
        } else {
          mg_ = g;
          mk_ = res * 0.97 * 2 / std::sqrt(1 - g);  // 2 = self-oscillation at low cutoffs
        }
        break;
      }
      case Comb: case CombMinus:
        delay_ = std::clamp(rs / fc, 1.0, (double)(kCombSize - 2));
        fb_ = (type_ == Comb ? 0.97 : -0.97) * res;
        damp_ = 1 - std::exp(-2 * kPi * std::min(rs * 0.45, 4 * fc + 2000) / rs);
        break;
      case Formant: {
        // Cutoff 100 Hz .. 8 kHz walks through the five vowels (log scale); resonance narrows the bands.
        const double pos = std::clamp(std::log2(fc / 100.0) / std::log2(80.0), 0.0, 1.0) * 4;
        const int a = std::min(3, (int)pos);
        const double f = pos - a;
        const double q = 0.86 + 0.12 * res;
        for (int k = 0; k < 3; ++k) {
          const double hz = kVowels[a][k] + (kVowels[a + 1][k] - kVowels[a][k]) * f;
          bands_[k].set(std::min(hz, rs * 0.45), q, rs);
        }
        break;
      }
      case Phaser:
        // Six stages from fc / 2^spread up to fc * 2^spread (spread 0 .. 1.5 octaves by Shape), feedback by resonance.
        for (int k = 0; k < kStages; ++k) {
          const double hz = std::clamp(fc * std::pow(2.0, (k - 2.5) / 2.5 * 1.5 * shape_), 20.0, rs * 0.45);
          const double t = std::tan(kPi * hz / rs);
          ap_[k].a = (t - 1) / (t + 1);
        }
        pfb_ = 0.9 * res;
        break;
      case DualNotch: case DualBand: {
        const double oct = 2 * shape_;  // each one up to 2 octaves away from the cutoff
        a_.set(std::clamp(fc * std::pow(2.0, -oct), 20.0, rs * 0.45), res, rs);
        b_.set(std::clamp(fc * std::pow(2.0, oct), 20.0, rs * 0.45), res, rs);
        break;
      }
      default:  // the state-variable types and Morph
        a_.set(fc, type_ == Band24 ? 0.5 : 0.0, rs);
        b_.set(fc, res, rs);
        break;
    }
  }

  double run(double x) {
    if (os_ == 1) return clipTick(x);
    // Up through the half-band(s), the drive's clip and the filter on every sample, and back down.
    double u[2];
    up1_.run(x, u[0], u[1]);
    if (os_ == 2) return down1_.run(clipTick(u[0]), clipTick(u[1]));
    double y[2];
    for (int k = 0; k < 2; ++k) {
      double w0, w1;
      up2_.run(u[k], w0, w1);
      y[k] = down2_.run(clipTick(w0), clipTick(w1));
    }
    return down1_.run(y[0], y[1]);
  }

private:
  static constexpr double kPi = 3.141592653589793;

  double clipTick(double x) { return tick(drive_ > 0 ? std::tanh(x * dg_) * dn_ : x); }
  double tick(double x) {
    switch (type_) {
      case Low24: return b_.lp(a_.lp(x));
      case Low12: return b_.lp(x);
      case Band: return b_.bp(x);
      case High24: return b_.hp(a_.hp(x));
      case Notch: return x - b_.bp(x);
      case Low6: s1_ += op_ * (x - s1_); return s1_;
      case High6: s1_ += op_ * (x - s1_); return x - s1_;
      case Band24: return b_.bp(a_.bp(x));
      case High12: return b_.hp(x);
      case Ladder: {
        // The loss of level at high resonance is made up (as analog ladders' makeup gain does). Without Drive exactly as
        // before (the makeup also multiplies the loop: it sings from resonance ~0.6, which the templates were tuned on);
        // with Drive the makeup is on the input only, so a pushed ladder growls instead of whistling on its own.
        double in = drive_ > 0 ? std::tanh(x * (1 + 0.5 * lk_) - lk_ * lad_[3]) : std::tanh((x - lk_ * lad_[3]) * (1 + 0.5 * lk_));
        for (double& s : lad_) { s += lg_ * (in - s); in = s; }
        return lad_[3];
      }
      case Comb: case CombMinus: {
        double rd = pos_ - delay_;
        if (rd < 0) rd += kCombSize;
        const int i0 = (int)rd, i1 = (i0 + 1) % kCombSize;
        const double fr = rd - i0;
        const double d = buf_[i0] + (buf_[i1] - buf_[i0]) * fr;
        lp_ += damp_ * (d - lp_);
        const double y = x + fb_ * lp_;
        buf_[pos_] = (float)std::clamp(y, -8.0, 8.0);
        pos_ = (pos_ + 1) % kCombSize;
        return y * (1 - 0.5 * std::fabs(fb_));
      }
      case Formant: return 1.6 * (bands_[0].bp(x) + 0.7 * bands_[1].bp(x) + 0.4 * bands_[2].bp(x));
      case Diode: {
        // A diode ladder: four stages that each clip (the diodes), not one clip at the input as the Ladder's; the
        // feedback passes a high-pass, so the bass thins out as resonance rises: the squelch of acid.
        dhs_ += dhp_ * (lad_[3] - dhs_);
        double in = x - lk_ * (lad_[3] - dhs_);
        for (double& st : lad_) { st += lg_ * (std::tanh(in) - std::tanh(st)); in = st; }
        // Makeup for the level the resonance takes, above the loop's high-pass only (the sub stays where it was).
        return lad_[3] + 0.25 * lk_ * (lad_[3] - dhs_);
      }
      case Ms20Low: {
        // Korg 35 low-pass: two one-poles; the second one's input gets the output back through a high-pass and an
        // asymmetric clip (the diodes of the MS-20), times k (2 = self-oscillation, here just under).
        m_[0] += mg_ * (x - m_[0]);
        const double hp = m_[1] - m_[2];
        const double in2 = m_[0] + mk_ * clip35(hp);
        m_[1] += mg_ * (in2 - m_[1]);
        m_[2] += mg_ * (m_[1] - m_[2]);
        return m_[1] / (1 + 0.35 * mk_);
      }
      case Ms20High: {
        // Korg 35 high-pass: the mirror, two high-poles with the output fed back through a low-pass.
        m_[0] += mg_ * (x - m_[0]);
        const double h1 = x - m_[0];
        const double in2 = h1 + mk_ * clip35(m_[2]);
        m_[1] += mg_ * (in2 - m_[1]);
        const double y = in2 - m_[1];
        m_[2] += mg_ * (y - m_[2]);
        return y / (1 + 0.35 * mk_);
      }
      case Phaser: {
        double s = x + pfb_ * plast_;
        if (drive_ > 0) s = std::tanh(s);
        for (auto& st : ap_) s = st.run(s);
        plast_ = s;
        return 0.5 * (x + s);
      }
      case Morph: {
        // Each stage blends low > band > high by Shape (0 low, 0.5 band, 1 high).
        const double m = shape_ * 2;
        return morph(b_, morph(a_, x, m), m);
      }
      case DualNotch: return b_.notch(a_.notch(x));
      case DualBand: return 0.7071 * (a_.bp(x) + b_.bp(x));
    }
    return x;
  }

  struct Svf {  // the same TPT state-variable filter as the instruments' (bp: unity gain at the cutoff)
    double ic1 = 0, ic2 = 0, g = 0, k = 2, a1 = 0, v1 = 0, v2 = 0;
    void set(double cutoff, double resonance, double sr) {
      g = std::tan(kPi * cutoff / sr);
      k = 2 - 2 * std::clamp(resonance, 0.0, 0.98);
      a1 = 1 / (1 + g * (g + k));
    }
    void tick(double x) { const double v3 = x - ic2; v1 = a1 * ic1 + g * a1 * v3; v2 = ic2 + g * v1; ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2; }
    double lp(double x) { tick(x); return v2; }
    double hp(double x) { tick(x); return x - k * v1 - v2; }
    double bp(double x) { tick(x); return k * v1; }
    double notch(double x) { tick(x); return x - k * v1; }
  };
  static double morph(Svf& f, double x, double m) {
    f.tick(x);
    const double lp = f.v2, bp = f.k * f.v1, hp = x - f.k * f.v1 - f.v2;
    return m <= 1 ? (1 - m) * lp + m * bp : (2 - m) * bp + (m - 1) * hp;
  }
  struct AllPass {  // first order, TPT-free direct form: y = a x + z, z = x - a y
    double a = 0, z = 0;
    double run(double x) { const double y = a * x + z; z = x - a * y; return y; }
  };
  // The MS-20's clip: diodes, soft and a little asymmetric (the DC it makes is taken out by the loop's high-pass).
  static double clip35(double v) { return std::tanh(v + 0.1) - 0.09966799462495582; }

  // 2x up and down: the polyphase all-pass half-band (Laurent de Soras' design: no latency to report). The first
  // stage (8 coefficients, transition 0.04 of the doubled rate) is flat to ~20 kHz at 48 kHz and pushes images and
  // folds down by ~70 dB; the second (2x to 4x, 6 coefficients, transition 0.1) only has to keep that band clean.
  static constexpr double kHb1[8] = {0.04063346092419326, 0.1505051290226746, 0.30075705599187408, 0.46077450496145061,
                                     0.6095243148961883, 0.73850384111885725, 0.84922381039206607, 0.9497427837050002};
  static constexpr double kHb2[6] = {0.039151597734460045, 0.14737711360104661, 0.30264684832849342, 0.48246854276970014,
                                     0.6746159185469639, 0.88300502576937312};
  template <int N, const double* C>
  struct HbUp {
    double x[N] = {}, y[N] = {};
    void run(double in, double& o0, double& o1) {
      double e = in, o = in;
      for (int i = 0; i < N; i += 2) {
        const double t0 = (e - y[i]) * C[i] + x[i]; x[i] = e; y[i] = t0; e = t0;
        const double t1 = (o - y[i + 1]) * C[i + 1] + x[i + 1]; x[i + 1] = o; y[i + 1] = t1; o = t1;
      }
      o0 = e; o1 = o;
    }
  };
  template <int N, const double* C>
  struct HbDown {
    double x[N] = {}, y[N] = {};
    double run(double in0, double in1) {
      double s0 = in1, s1 = in0;
      for (int i = 0; i < N; i += 2) {
        const double t0 = (s0 - y[i]) * C[i] + x[i]; x[i] = s0; y[i] = t0; s0 = t0;
        const double t1 = (s1 - y[i + 1]) * C[i + 1] + x[i + 1]; x[i + 1] = s1; y[i + 1] = t1; s1 = t1;
      }
      return 0.5 * (s0 + s1);
    }
  };

  static constexpr int kCombSize = 4096;  // 23 Hz at 96 kHz
  static constexpr int kStages = 6;
  // F1-F3 of A E I O U (male voice, Peterson & Barney averages).
  static constexpr double kVowels[5][3] = {{730, 1090, 2440}, {530, 1840, 2480}, {270, 2290, 3010}, {570, 840, 2410}, {300, 870, 2240}};
  int type_ = 0;
  int os_ = 1;
  double drive_ = 0, shape_ = 0.5, dg_ = 1, dn_ = 1;
  Svf a_, b_, bands_[3];
  double op_ = 0, s1_ = 0;
  double lg_ = 0, lk_ = 0, lad_[4] = {};
  double dhp_ = 0, dhs_ = 0;
  double mg_ = 0, mk_ = 0, m_[3] = {};
  AllPass ap_[kStages];
  double pfb_ = 0, plast_ = 0;
  float buf_[kCombSize] = {};
  int pos_ = 0;
  double delay_ = 1, fb_ = 0, damp_ = 1, lp_ = 0;
  HbUp<8, kHb1> up1_;
  HbDown<8, kHb1> down1_;
  HbUp<6, kHb2> up2_;
  HbDown<6, kHb2> down2_;
};

// The filter's curve, for drawing it (the instrument pages): the gain in dB of a VoiceFilter of this type at each of
// the n frequencies hz[], measured on the filter itself (its response to a small impulse, then a DFT), so it is exact
// for every type, Comb, Phaser and Formant included. Cheap enough for 30 redraws a second at ~300 points (8192
// samples x n); the last answer is kept, so an unchanged filter costs nothing.
inline void filterCurveDb(int type, double cutoff, double resonance, double drive, double shape, double sr,
                          const double* hz, double* outDb, int n) {
  struct Last { int type = -1; double c = 0, r = 0, d = 0, s = 0, sr = 0; std::vector<double> hz, db; };
  thread_local Last last;
  if (last.type == type && last.c == cutoff && last.r == resonance && last.d == drive && last.s == shape && last.sr == sr &&
      (int)last.hz.size() == n && std::equal(hz, hz + n, last.hz.begin())) {
    std::copy(last.db.begin(), last.db.end(), outDb);
    return;
  }
  constexpr int kLen = 8192;
  constexpr double kAmp = 1e-3;  // small: the clips stay linear, the curve is the filter's own
  thread_local std::vector<double> ir(kLen);
  VoiceFilter f;
  f.set(type, cutoff, resonance, sr, drive, shape);
  for (int i = 0; i < kLen; ++i) ir[i] = f.run(i == 0 ? kAmp : 0.0) / kAmp;
  // A long ring (Comb, Phaser near the top) is cut with a half-Hann over the last quarter, not abruptly.
  for (int i = kLen * 3 / 4; i < kLen; ++i) ir[i] *= 0.5 + 0.5 * std::cos(3.141592653589793 * (i - kLen * 3 / 4) / (kLen / 4.0));
  for (int k = 0; k < n; ++k) {
    // Goertzel-free direct DFT at one frequency (a rotating phasor).
    const double w = 2 * 3.141592653589793 * std::clamp(hz[k], 0.0, sr / 2) / sr;
    const std::complex<double> step(std::cos(w), -std::sin(w));
    std::complex<double> ph(1, 0), acc(0, 0);
    for (int i = 0; i < kLen; ++i) {
      acc += ir[i] * ph;
      ph *= step;
      if ((i & 1023) == 1023) ph /= std::abs(ph);  // keep the phasor on the unit circle
    }
    outDb[k] = 20 * std::log10(std::max(std::abs(acc), 1e-6));
  }
  last.type = type; last.c = cutoff; last.r = resonance; last.d = drive; last.s = shape; last.sr = sr;
  last.hz.assign(hz, hz + n);
  last.db.assign(outDb, outDb + n);
}

}  // namespace core::dsp
