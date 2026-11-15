#include "sampler.hpp"
#include <algorithm>
#include <cmath>

namespace core {

namespace {
constexpr double kStealMs = 3.0;     // a voice cut by the next note fades out this fast (no click)
constexpr double kMinAttackMs = 0.1; // even with attack 0: a drum hit keeps its own front edge
constexpr double kSoftAttackMs = 1.0; // a pitched note (a bass) or a late start begins mid-wave: 1 ms, or it clicks
constexpr double kEndFadeMs = 2.0;    // the last 2 ms of a file fade out, so a file cut mid-wave does not click
constexpr double kDecayOff = 10000;  // decay at its maximum: the sample plays to its end
constexpr double kSilent = 0.001;
constexpr double kResidualMs = 1.0;  // a voice taken while still sounding dies away in about this time    // -60 dB: the decay reaches this at `decay` ms

// Version 4 (audit 2026-11-07 #15): a sample played above its rate reads more than one frame per output sample, and
// what was under the file's Nyquist lands above the output's and folds back down (a 16 kHz hat partial played an
// octave up came back at 16 kHz, as loud as it was). The read is then a windowed sinc whose cutoff falls with the
// step: kCut / step of the input's Nyquist, kZeros zero crossings each side (Kaiser, beta 8: about -80 dB from
// 0.497 of the output rate on, flat to 0.39). Played at its rate or below, the read stays Hermite.
constexpr int kZeros = 12, kTableRes = 512;
constexpr double kCut = 0.88;
constexpr double kMaxBandStep = 8;  // above three octaves up the kernel stops widening (a wider one costs too much)
struct SincTable {
  float k[kZeros * kTableRes + 2];
  SincTable() {
    auto i0 = [](double x) { double sum = 1, term = 1; for (int n = 1; n < 40; ++n) { term *= (x / (2 * n)) * (x / (2 * n)); sum += term; } return sum; };
    const double beta = 8, norm = i0(beta);
    for (int n = 0; n <= kZeros * kTableRes + 1; ++n) {
      const double u = (double)n / kTableRes, r = u / kZeros;
      const double w = r < 1 ? i0(beta * std::sqrt(1 - r * r)) / norm : 0;
      const double sinc = n == 0 ? 1 : std::sin(3.141592653589793 * u) / (3.141592653589793 * u);
      k[n] = (float)(sinc * w);
    }
  }
  // The kernel at u zero-crossing units from its centre (0 beyond kZeros).
  double at(double u) const {
    u = std::fabs(u) * kTableRes;
    const int i = (int)u;
    if (i >= kZeros * kTableRes) return 0;
    return k[i] + (k[i + 1] - k[i]) * (u - (double)i);
  }
};
const SincTable gSinc;  // built when the program starts, never on the audio thread
}  // namespace

void Sampler::queueNote(int offset, bool on, int pitch, double velocity) {
  if (queued_ >= kQueue) { ++droppedNotes_; return; }  // more than a block can hold: counted
  queue_[queued_++] = Queued{offset, on, pitch, (float)velocity};
}

void Sampler::chaseNote(int pitch, double velocity, double age) {
  if (queued_ >= kQueue) { ++droppedNotes_; return; }
  queue_[queued_++] = Queued{0, true, pitch, (float)velocity, std::max(0.0, age)};
}

void Sampler::allNotesOff() {
  for (auto& v : voices_) if (v.active && !v.released) v.released = true;
  queued_ = 0;
}

double Sampler::minAttackMs() const {
  return params[pitched] >= 0.5 || params[start] > 0 ? kSoftAttackMs : kMinAttackMs;
}

void Sampler::trigger(int pitch, double velocity, double age) {
  const SamplerZone* zone = nullptr;
  for (const auto& z : zones_) if (pitch >= z.loKey && pitch <= z.hiKey && z.data && z.data->frames() > 0) { zone = &z; break; }
  if (!zone) return;
  const bool keyed = params[pitched] >= 0.5;
  // retrigger off: while this zone still sounds (not released, not fading) the note is ignored, so a riser
  // plays once through a roll. Only the same zone (and, pitched, the same key): another zone of a
  // multi-sample still plays. Audit 2026-11-04 E2: any sounding voice blocked every note.
  if (params[retrigger] < 0.5)
    for (const auto& v : voices_)
      if (v.active && !v.released && v.fadeStep == 0 && v.data == zone->data.get() && (!keyed || v.pitch == pitch)) return;
  // Monophonic: the sounding note fades out (a bass line legato, a hat choking the open one). A voice
  // that has not played a sample yet (notes on the same sample) is never heard: it goes at once, so
  // stacked notes do not add up (audit E3: 24 notes at one instant peaked at +4.9 dBFS).
  for (auto& v : voices_) {
    if (!v.active) continue;
    if (v.played == 0) v.active = false;
    else if (v.fadeStep == 0) v.fadeStep = 1.0 / (kStealMs * 0.001 * sampleRate_);
  }
  Voice* voice = nullptr;
  for (auto& v : voices_) if (!v.active) { voice = &v; break; }
  if (!voice) {
    // All 16 still fading (notes closer than 3 ms apart): take the quietest, and let the level it had
    // die away over 1 ms instead of dropping to zero in one sample (audit E1: a click).
    voice = &voices_[0];
    for (auto& v : voices_) if (v.fade < voice->fade) voice = &v;
    residualL_ += voice->lastL;
    residualR_ += voice->lastR;
  }
  const double semis = params[tune] + (keyed ? pitch - zone->rootKey : 0);
  const bool backwards = params[reverse] >= 0.5;
  *voice = Voice{};
  voice->dir = backwards ? -1 : 1;
  voice->data = zone->data.get();
  voice->active = true;
  voice->pitch = pitch;
  voice->step = voice->dir * std::pow(2.0, semis / 12.0);
  voice->keySemis = keyed ? pitch - zone->rootKey : 0;
  voice->pos = std::max(0.0, params[start]) * 0.001 * sampleRate_;
  if (backwards) voice->pos = std::max(0.0, (double)zone->data->frames() - 2 - voice->pos);  // Start counts from the end
  voice->velocity = std::clamp(velocity, 0.0, 1.0);
  if (!backwards && zone->loopEnd > zone->loopStart && zone->loopEnd < zone->data->frames()) {
    voice->loopStart = zone->loopStart;
    voice->loopEnd = zone->loopEnd;
    // Up to 10 ms, a quarter of the loop, and no more than there is before its start (equal-gain blend).
    voice->crossfade = (double)std::min<int64_t>({(int64_t)(0.010 * sampleRate_), (zone->loopEnd - zone->loopStart) / 4, zone->loopStart});
  }
  if (age > 0) {  // caught mid-way: where the sample and the envelope would be now, faded in
    const double attackSamples = std::max(minAttackMs(), params[attack]) * 0.001 * sampleRate_;
    voice->pos += age * voice->step;
    voice->age = age;
    if (age < attackSamples) voice->env = age / attackSamples;
    else if (params[decay] < kDecayOff) {
      const double sus = std::clamp(params[sustain], 0.0, 1.0);
      voice->env = sus + (1 - sus) * std::pow(kSilent, (age - attackSamples) / std::max(1.0, params[decay] * 0.001 * sampleRate_));
    }
    else voice->env = 1;
    voice->fadeIn = 0;
    voice->fenv = std::pow(kSilent, age / std::max(1.0, std::clamp(params[filterDecay], 5.0, 4000.0) * 0.001 * sampleRate_));
    if (voice->pos < 0) voice->active = false;  // reversed and already past its start
    if (voice->loopEnd > 0 && voice->pos >= (double)voice->loopEnd) {  // caught inside the loop: where it would be
      const double length = (double)(voice->loopEnd - voice->loopStart);
      voice->pos = voice->loopStart + std::fmod(voice->pos - voice->loopStart, length);
    }
  }
}

void Sampler::releaseKey(int pitch) {
  // One-shots play on after the note ends, unless they loop (a held pad, a drone): those release too.
  const bool keyed = params[pitched] >= 0.5;
  for (auto& v : voices_) if (v.active && v.pitch == pitch && (keyed || v.loopEnd > 0)) v.released = true;
}

void Sampler::renderVoices(float* left, float* right, int from, int to) {
  const double sr = sampleRate_;
  const double attackSamples = std::max(minAttackMs(), params[attack]) * 0.001 * sr;
  const bool decays = params[decay] < kDecayOff;
  const double decaySamples = std::max(1.0, params[decay] * 0.001 * sr);
  const double decayCoef = std::pow(kSilent, 1.0 / decaySamples);
  const double releaseCoef = std::pow(kSilent, 1.0 / std::max(1.0, params[release] * 0.001 * sr));
  const double gain = std::pow(10.0, params[level] / 20);
  const double endFade = kEndFadeMs * 0.001 * sr;  // output samples
  // The pitch follows tune while the note plays (read once per call, like every parameter).
  for (auto& v : voices_)
    if (v.active) v.step = v.dir * std::pow(2.0, (params[tune] + v.keySemis) / 12.0);
  const double sus = std::clamp(params[sustain], 0.0, 1.0);
  const int fType = (int)std::lround(std::clamp(params[filterType], 0.0, 3.0));
  const double fCut = std::clamp(params[cutoff], 20.0, 20000.0), fRes = std::clamp(params[resonance], 0.0, 0.95);
  const double fEnvOct = std::clamp(params[filterEnv], -5.0, 5.0);
  const double fDecCoef = std::pow(kSilent, 1.0 / std::max(1.0, std::clamp(params[filterDecay], 5.0, 4000.0) * 0.001 * sr));
  const double velSens = std::clamp(params[velocitySens], 0.0, 1.0);
  if (residualL_ != 0 || residualR_ != 0) {  // a taken voice's last level, dying away
    const double k = std::exp(-1.0 / (kResidualMs * 0.001 * sr));
    for (int s = from; s < to; ++s) {
      residualL_ *= k; residualR_ *= k;
      left[s] += (float)residualL_;
      right[s] += (float)residualR_;
    }
    if (std::fabs(residualL_) < 1e-7 && std::fabs(residualR_) < 1e-7) residualL_ = residualR_ = 0;
  }
  for (auto& v : voices_) {
    if (!v.active) continue;
    const AudioData& d = *v.data;
    const int64_t frames = d.frames();
    const float* L = d.L.data();
    const float* R = d.R.data();
    const bool loops = v.loopEnd > 0;
    const double loopLength = (double)(v.loopEnd - v.loopStart);
    // The sample at i + f: 4-point Hermite (the edges repeat the first and last frames).
    auto read = [&](const float* X, int64_t i, double f) -> double {
      const double x0 = X[i], x1 = X[i + 1];
      const double xm = i > 0 ? X[i - 1] : x0, x2 = i + 2 < frames ? X[i + 2] : x1;
      return x0 + 0.5 * f * (x1 - xm + f * (2 * xm - 5 * x0 + 4 * x1 - x2 + f * (3 * (x0 - x1) + x2 - xm)));
    };
    // Version 4, played faster than its rate: band-limited (see kCut). The weights are normalised by their sum, so
    // a constant stays exactly constant; outside the file the first and last frames repeat, as for Hermite.
    const double speed = std::fabs(v.step);
    const bool band = speed > 1.0 + 1e-9;
    const double cut = kCut / std::min(speed, kMaxBandStep), reach = kZeros / cut;
    auto readBoth = [&](int64_t i, double f, double& l, double& r) {
      if (!band) { l = read(L, i, f); r = read(R, i, f); return; }
      const double p = (double)i + f;
      const int64_t lo = (int64_t)std::ceil(p - reach), hi = (int64_t)std::floor(p + reach);
      double sumL = 0, sumR = 0, sw = 0;
      for (int64_t k = lo; k <= hi; ++k) {
        const double w = gSinc.at((p - (double)k) * cut);
        const int64_t j = std::clamp<int64_t>(k, 0, frames - 1);
        sumL += L[j] * w; sumR += R[j] * w; sw += w;
      }
      l = sumL / sw; r = sumR / sw;
    };
    for (int s = from; s < to; ++s) {
      if (loops && v.pos >= (double)v.loopEnd) {  // the seam (the crossfade made it smooth)
        v.pos -= loopLength;
        // A step longer than the loop (a short loop played high) crosses it more than once per sample (audit
        // 2026-11-07 #14: note 127 of a 96-frame loop at +24 ran past the end and went silent).
        if (v.pos >= (double)v.loopEnd) v.pos = (double)v.loopStart + std::fmod(v.pos - (double)v.loopStart, loopLength);
      }
      if (v.pos < 0) { v.active = false; break; }  // reversed: its start reached
      const int64_t i = (int64_t)v.pos;
      if (i + 1 >= frames) { v.active = false; break; }
      const double f = v.pos - (double)i;
      // Envelope: linear attack, then (if set) an exponential decay to -60 dB, and the release.
      // Released during the attack, the release starts from the level reached: the attack no longer climbs on
      // (audit of main fa3b428, F02: attack 1 s, note-off at 20 ms rose 34 dB after the key was let go).
      if (!v.released && v.age < attackSamples) v.env = (v.age + 1) / attackSamples;
      else if (!v.released && v.age < attackSamples + 1) v.env = 1;
      // Released, the level only falls: Sustain no longer pulls it back up (audit 2026-11-07 #4: with Sustain 1 the
      // decay toward it and the release met halfway, and a looping note stayed at -33 dBFS for good). Without Sustain
      // the decay goes on under the release, as before.
      else if (decays && !(v.released && sus > 0)) v.env = sus > 0 ? sus + (v.env - sus) * decayCoef : v.env * decayCoef;
      if (v.released) v.env *= releaseCoef;
      if (v.fadeStep > 0) { v.fade -= v.fadeStep; if (v.fade <= 0) { v.active = false; break; } }
      if (v.env < 1e-5 && (v.age >= attackSamples || v.released)) { v.active = false; break; }
      double g = gain * (1 - velSens + velSens * v.velocity) * v.env * v.fade;
      if (!loops) {  // the end of the file: fade over its last 2 ms of output (a bass sample cut mid-wave)
        const double rest = v.dir < 0 ? v.pos / -v.step : ((double)(frames - 1) - v.pos) / v.step;
        if (rest < endFade) g *= std::max(0.0, rest / endFade);
      }
      if (v.fadeIn < 1) { v.fadeIn = std::min(1.0, v.fadeIn + 1.0 / (kStealMs * 0.001 * sr)); g *= v.fadeIn; }
      double sl, sr2;
      readBoth(i, f, sl, sr2);
      if (loops && v.crossfade > 0 && v.pos >= (double)v.loopEnd - v.crossfade) {
        // The end of the loop fades into the audio just before its start, so the jump back lands on the same sound.
        const double a = ((double)v.loopEnd - v.pos) / v.crossfade;  // 1 -> 0 toward the seam
        const double back = v.pos - loopLength;
        const int64_t j = (int64_t)back;
        const double fj = back - (double)j;
        double bl, br;
        readBoth(j, fj, bl, br);
        sl = sl * a + bl * (1 - a);
        sr2 = sr2 * a + br * (1 - a);
      }
      if (fType > 0) {  // the filter: 12 dB state-variable (TPT), its cutoff moved by the envelope
        if (v.played % 16 == 0) {
          const double fc = std::clamp(fCut * std::pow(2.0, fEnvOct * v.fenv), 20.0, sr * 0.45);
          v.g = std::tan(3.141592653589793 * fc / sr);
          v.k = 2 - 2 * fRes;
          v.a1 = 1 / (1 + v.g * (v.g + v.k));
        }
        v.fenv *= fDecCoef;
        auto tick = [&](double x, double& ic1, double& ic2) {
          const double v3 = x - ic2, v1 = v.a1 * ic1 + v.g * v.a1 * v3, v2 = ic2 + v.g * v1;
          ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
          return fType == 1 ? v2 : fType == 2 ? v.k * v1 : x - v.k * v1 - v2;
        };
        sl = tick(sl, v.ic1L, v.ic2L);
        sr2 = tick(sr2, v.ic1R, v.ic2R);
      }
      v.lastL = sl * g;
      v.lastR = sr2 * g;
      left[s] += (float)v.lastL;
      right[s] += (float)v.lastR;
      v.pos += v.step;
      v.age += 1;
      v.played += 1;
    }
  }
}

void Sampler::renderStereo(float* left, float* right, int frames) {
  int cursor = 0;
  for (int q = 0; q < queued_; ++q) {
    const int at = std::clamp(queue_[q].offset, 0, frames);
    if (at > cursor) { renderVoices(left, right, cursor, at); cursor = at; }
    if (queue_[q].on) trigger(queue_[q].pitch, queue_[q].velocity, queue_[q].age);
    else releaseKey(queue_[q].pitch);
  }
  queued_ = 0;
  if (cursor < frames) renderVoices(left, right, cursor, frames);
}

void Sampler::adopt(const Instrument& previous) {
  const auto* p = dynamic_cast<const Sampler*>(&previous);
  if (!p) return;
  for (int i = 0; i < kVoices; ++i) {
    const Voice& v = p->voices_[i];
    const bool kept = v.active && std::any_of(zones_.begin(), zones_.end(), [&](const SamplerZone& z) { return z.data.get() == v.data; });
    voices_[i] = kept ? v : Voice{};
  }
  residualL_ = p->residualL_;
  residualR_ = p->residualR_;
}

} // namespace core
