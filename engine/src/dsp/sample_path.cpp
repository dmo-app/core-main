// Core Glitch · Sample Path: the instrument (see sample_path.hpp).
#include "sample_path.hpp"
#include "devices.hpp"
#include "effects.hpp"
#include "registry.hpp"
#include "../io/audio_file.hpp"
#include <algorithm>
#include <cmath>

namespace core {

namespace {
double dbGain(double db) { return std::pow(10.0, db / 20.0); }

// A built-in effect with every knob at its default (params[] starts at 0, which is not a default).
std::unique_ptr<Effect> effectWithDefaults(const std::string& name, double sampleRate, double bpm, int version) {
  const bool block = !coreFxBlockParams(name).empty();
  auto fx = block ? makeDevice(name, sampleRate, bpm, version) : makeEffect(name, sampleRate, bpm, version);
  if (!fx) return nullptr;
  const auto& specs = block ? coreFxBlockParams(name) : nativeEffectParams(name);
  for (size_t i = 0; i < specs.size() && i < (size_t)kMaxEffectParams; ++i) fx->params[i] = specs[i].def;
  return fx;
}
}  // namespace

SamplePath::SamplePath(double sampleRate) : Instrument(sampleRate) {
  for (int l = 0; l < spath::kLayers; ++l) { bufL_[l].assign(kBlock, 0.0f); bufR_[l].assign(kBlock, 0.0f); }
  mixL_.assign(kBlock, 0.0f);
  mixR_.assign(kBlock, 0.0f);
  reverb_ = effectWithDefaults("space", sampleRate, songBpm_, 1);
  // The safety limiter is the instrument's own, with no look-ahead: the shared limiter looks 55 samples ahead, and an
  // instrument has no way to tell the graph its latency (audit of main fa3b428, F05: the track came 55 samples late).
  limRelease_ = std::exp(-1.0 / (0.060 * sampleRate));
  params[amp] = -8; params[pitch] = 0; params[size] = 0.45; params[damp] = 0.58; params[mix] = 0.12; params[limiter] = 1;
  for (int l = 0; l < spath::kLayers; ++l) buildFx(l);
}

SamplePath::~SamplePath() = default;

void SamplePath::setPatch(spath::Patch patch) {
  patch_ = std::move(patch);
  for (int l = 0; l < spath::kLayers; ++l) buildFx(l);
}

void SamplePath::setLayerSource(int layer, std::shared_ptr<const AudioData> source) {
  if (layer >= 0 && layer < spath::kLayers) layers_[layer].sound = std::move(source);
}

void SamplePath::buildFx(int layer) {
  LayerRt& L = layers_[layer];
  for (int s = 0; s < spath::kFxSlots; ++s) {
    const spath::FxType type = patch_.layers[layer].fx[s].type;
    if (L.fx[s] && L.fxType[s] == type) continue;
    L.fx[s].reset();
    L.fxType[s] = type;
    if (type == spath::FxType::Delay) L.fx[s] = effectWithDefaults("echo", sampleRate_, songBpm_, 1);
    else if (type == spath::FxType::Filter) L.fx[s] = effectWithDefaults("filter", sampleRate_, songBpm_, 1);
    else if (type == spath::FxType::Stutter) L.fx[s] = effectWithDefaults("glitch", sampleRate_, songBpm_, 1);
  }
}

double SamplePath::stepSamples(const spath::Pattern& p, const spath::Node& n) const {
  const double bpm = patch_.sync ? songBpm_ : patch_.bpm;
  return std::max(16.0, spath::stepBeats(p, n) * 60.0 / std::clamp(bpm, 20.0, 999.0) * sampleRate_);
}

void SamplePath::setPosition(double beat, double bpm) {
  songBeat_ = beat;
  if (bpm > 0) songBpm_ = bpm;
  for (auto& L : layers_)
    for (auto& fx : L.fx) if (fx) fx->setTempo(songBpm_);
  if (reverb_) reverb_->setTempo(songBpm_);
}

void SamplePath::queueNote(int offset, bool on, int pitch, double velocity) {
  if (queued_ == kQueue) { ++droppedNotes_; return; }
  // Kept in offset order (the graph sends them in order; a stray one is put in its place).
  int i = queued_++;
  while (i > 0 && queue_[i - 1].offset > offset) { queue_[i] = queue_[i - 1]; --i; }
  queue_[i] = {offset, on, pitch, (float)velocity};
}

void SamplePath::applyNote(const Queued& q, int at) {
  if (q.on) {
    transpose_ = q.pitch - 60;
    velocity_ = std::clamp((double)q.velocity, 0.0, 1.0);
    if (held_++ == 0) startWalks(at);
  } else if (held_ > 0 && --held_ == 0) {
    stopWalks();
  }
}

void SamplePath::startWalks(int /*at*/) {
  for (int l = 0; l < spath::kLayers; ++l) {
    LayerRt& L = layers_[l];
    const spath::Pattern& p = pattern(l);
    L.walk.reset(p);
    L.elapsed = 0;
    L.steps = 0;
    L.walking = L.walk.node >= 0;
    if (!L.walking) continue;
    step(l, 0);
    L.nextAt = stepSamples(p, p.nodes[(size_t)L.walk.node]);
  }
}

void SamplePath::stopWalks() {
  for (auto& L : layers_) L.walking = false;
  for (auto& v : voices_)
    if (v.active && v.age < v.length - (int64_t)v.release) v.length = v.age + (int64_t)v.release;  // into the release
}

void SamplePath::allNotesOff() {
  held_ = 0;
  queued_ = 0;
  stopWalks();
}

void SamplePath::step(int layer, int64_t into) {
  LayerRt& L = layers_[layer];
  ++L.steps;
  const spath::Layer& layerData = patch_.layers[layer];
  const spath::Pattern& p = pattern(layer);
  if (L.walk.node < 0 || L.walk.node >= (int)p.nodes.size()) return;
  if (layerData.mute || !L.sound || L.sound->frames() < 4) return;
  const spath::Node& n = p.nodes[(size_t)L.walk.node];
  if (n.slice < 0 || n.slice >= (int)layerData.slices.size()) return;
  const AudioData& d = *L.sound;
  const int64_t frames = d.frames();
  const spath::Slice& s = layerData.slices[(size_t)n.slice];
  double a = std::clamp((s.start + n.startOffset) * sampleRate_, 0.0, (double)(frames - 2));
  double b = std::clamp((s.end + n.endOffset) * sampleRate_, 0.0, (double)(frames - 2));
  if (b - a < 8) return;  // nothing left of the part
  const double semis = n.pitch + n.fine / 100.0 + transpose_ + params[pitch];
  const double rate = std::pow(2.0, std::clamp(semis, -48.0, 48.0) / 12.0);
  const double stepLen = stepSamples(p, n);
  const double fade = std::max(0.0, p.fadeMs) * 0.001 * sampleRate_;
  double length = std::min((b - a) / rate, stepLen * std::clamp(n.gate, 1.0, 100.0) / 100.0);
  double release = std::max(fade, std::max(0.0, n.releaseMs) * 0.001 * sampleRate_);
  // The release reads on past the part, never past the sound's ends (a fade reaches 0 before them).
  const double room = (n.reverse ? b : (double)(frames - 2) - a) / rate;
  if (length + release > room) { release = std::max(0.0, room - length); if (release < 1) { length = std::max(1.0, room - 1); release = 1; } }
  const double attack = std::max({1.0, fade, std::max(0.0, n.attackMs) * 0.001 * sampleRate_});
  if ((double)into >= length + release) return;
  // A free voice, or the quietest-to-be (the oldest of this layer, else the oldest); a stolen one fades out.
  int pick = -1;
  for (int i = 0; i < kVoices; ++i) if (!voices_[i].active) { pick = i; break; }
  if (pick < 0) {
    int64_t oldest = -1;
    for (int i = 0; i < kVoices; ++i) if (voices_[i].age > oldest) { oldest = voices_[i].age; pick = i; }
  }
  Voice& v = voices_[pick];
  v.data = &d;
  v.layer = layer;
  v.active = true;
  v.step = n.reverse ? -rate : rate;
  v.pos = (n.reverse ? b : a) + v.step * (double)into;
  v.age = 0;
  v.length = (int64_t)std::llround(length + release) - into;
  v.attack = into > 0 ? std::max(attack, 0.003 * sampleRate_) : attack;  // a chased step comes in with a 3 ms fade
  v.release = std::max(1.0, release);
  const double g = dbGain(std::clamp(n.level, -60.0, 12.0)) * velocity_;
  const double pan = std::clamp(n.pan, -100.0, 100.0) / 100.0;
  const double angle = (pan + 1) * 0.25 * 3.141592653589793;
  v.gainL = g * std::cos(angle) * 1.41421356;
  v.gainR = g * std::sin(angle) * 1.41421356;
  v.fade = 1;
  v.fadeStep = 0;
}

void SamplePath::renderVoices(int from, int to) {
  for (auto& v : voices_) {
    if (!v.active) continue;
    const AudioData& d = *v.data;
    const int64_t frames = d.frames();
    const float* L = d.L.data();
    const float* R = d.channels > 1 && d.R.size() == d.L.size() ? d.R.data() : L;
    float* outL = bufL_[v.layer].data();
    float* outR = bufR_[v.layer].data();
    auto read = [&](const float* X, int64_t i, double f) -> double {  // 4-point Hermite, as the sampler
      const double x0 = X[i], x1 = X[i + 1];
      const double xm = i > 0 ? X[i - 1] : x0, x2 = i + 2 < frames ? X[i + 2] : x1;
      return x0 + 0.5 * f * (x1 - xm + f * (2 * xm - 5 * x0 + 4 * x1 - x2 + f * (3 * (x0 - x1) + x2 - xm)));
    };
    const double sustainEnd = (double)v.length - v.release;
    for (int s = from; s < to; ++s) {
      if (v.age >= v.length || v.pos < 0 || v.pos >= (double)(frames - 1) || v.fade <= 0) { v.active = false; break; }
      double env = 1;
      if ((double)v.age < v.attack) env = (double)v.age / v.attack;
      if ((double)v.age >= sustainEnd) env = std::min(env, std::max(0.0, ((double)v.length - (double)v.age) / v.release));
      env *= v.fade;
      v.fade -= v.fadeStep;
      const int64_t i = (int64_t)v.pos;
      const double f = v.pos - (double)i;
      outL[s] += (float)(read(L, i, f) * env * v.gainL);
      outR[s] += (float)(read(R, i, f) * env * v.gainR);
      v.pos += v.step;
      ++v.age;
    }
  }
}

void SamplePath::runFx(int layer, int n) {
  LayerRt& L = layers_[layer];
  float* l = bufL_[layer].data();
  float* r = bufR_[layer].data();
  for (int s = 0; s < spath::kFxSlots; ++s) {
    const spath::Fx& fx = patch_.layers[layer].fx[s];
    const double a = std::clamp(fx.a, 0.0, 1.0), b = std::clamp(fx.b, 0.0, 1.0), mix = std::clamp(fx.mix, 0.0, 1.0);
    if (fx.type == spath::FxType::Crush) {
      if (mix <= 0) continue;
      const int hold = 1 + (int)std::lround(a * a * 15);
      const double levels = std::pow(2.0, 16.0 - b * 13.0);  // 16 bits down to 3
      for (int i = 0; i < n; ++i) {
        if (L.holdLeft[s]-- <= 0) {
          L.holdLeft[s] = hold - 1;
          L.holdL[s] = (float)(std::round(l[i] * levels) / levels);
          L.holdR[s] = (float)(std::round(r[i] * levels) / levels);
        }
        l[i] = (float)(l[i] * (1 - mix) + L.holdL[s] * mix);
        r[i] = (float)(r[i] * (1 - mix) + L.holdR[s] * mix);
      }
      continue;
    }
    Effect* e = L.fx[s].get();
    if (!e || L.fxType[s] != fx.type || e->failed()) continue;
    if (fx.type == spath::FxType::Delay) {
      e->params[0] = std::round(a * 7);        // time: 1/16 .. 1 bar
      e->params[1] = b * 0.9;                  // feedback
      e->params[6] = mix;
    } else if (fx.type == spath::FxType::Filter) {
      e->params[0] = 0;                        // low-pass
      e->params[1] = 20.0 * std::pow(1000.0, a);
      e->params[2] = 0.707 + b * 7.3;          // resonance
      e->params[3] = mix;
    } else if (fx.type == spath::FxType::Stutter) {
      e->params[0] = std::round(a * 3);        // rate: 1/8 .. 1/64
      e->params[2] = b;                        // chance
      e->params[3] = mix;
      e->setPosition(songBeat_);
    }
    e->process(l, r, n);
  }
}

void SamplePath::renderStereo(float* left, float* right, int frames) {
  int qi = 0;
  for (int chunk = 0; chunk < frames; chunk += kBlock) {
    const int end = std::min(frames, chunk + kBlock), n = end - chunk;
    for (int l = 0; l < spath::kLayers; ++l) {
      std::fill(bufL_[l].begin(), bufL_[l].begin() + n, 0.0f);
      std::fill(bufR_[l].begin(), bufR_[l].begin() + n, 0.0f);
    }
    int cur = chunk;
    while (cur < end) {
      while (qi < queued_ && queue_[qi].offset <= cur) applyNote(queue_[qi++], cur);
      // Up to the next note or the next step, whichever comes first.
      int stop = end;
      if (qi < queued_) stop = std::min(stop, std::max(cur + 1, queue_[qi].offset));
      for (const auto& L : layers_)
        if (L.walking) {
          const int64_t due = (int64_t)std::ceil(L.nextAt) - L.elapsed;  // samples until the next step
          stop = (int)std::min<int64_t>(stop, cur + std::max<int64_t>(0, due));
        }
      if (stop > cur) {
        renderVoices(cur - chunk, stop - chunk);
        for (auto& L : layers_) if (L.walking) L.elapsed += stop - cur;
        cur = stop;
      }
      // Steps due now (a step lasts at least 16 samples, so this ends).
      for (int l = 0; l < spath::kLayers; ++l) {
        LayerRt& L = layers_[l];
        while (L.walking && (double)L.elapsed >= std::ceil(L.nextAt)) {
          const spath::Pattern& p = pattern(l);
          if (L.walk.node >= (int)p.nodes.size()) L.walk.reset(p);
          L.walk.advance(p);
          if (L.walk.node < 0) { L.walking = false; break; }
          step(l, 0);
          L.nextAt += stepSamples(p, p.nodes[(size_t)L.walk.node]);
        }
      }
    }
    // Each sampler's slots, then the mix, the reverb, the level and the limiter.
    std::fill(mixL_.begin(), mixL_.begin() + n, 0.0f);
    std::fill(mixR_.begin(), mixR_.begin() + n, 0.0f);
    for (int l = 0; l < spath::kLayers; ++l) {
      runFx(l, n);
      if (patch_.layers[l].mute) continue;
      for (int i = 0; i < n; ++i) { mixL_[i] += bufL_[l][i]; mixR_[i] += bufR_[l][i]; }
    }
    if (reverb_ && !reverb_->failed() && params[mix] > 0) {
      reverb_->params[0] = std::clamp(params[size], 0.0, 1.0);
      reverb_->params[1] = 0.4 + 5.6 * std::clamp(params[size], 0.0, 1.0);    // decay, s
      reverb_->params[3] = 18000.0 - 15000.0 * std::clamp(params[damp], 0.0, 1.0);  // tone
      reverb_->params[6] = std::clamp(params[mix], 0.0, 1.0);
      reverb_->setPosition(songBeat_);
      reverb_->process(mixL_.data(), mixR_.data(), n);
    }
    const float g = (float)dbGain(std::clamp(params[amp], -60.0, 12.0));
    for (int i = 0; i < n; ++i) { mixL_[i] *= g; mixR_[i] *= g; }
    if (params[limiter] >= 0.5) {
      // Peaks over -0.3 dBFS are held down at once (gain set on the sample), then the gain comes back over ~60 ms.
      constexpr double kCeiling = 0.96605;  // -0.3 dB
      for (int i = 0; i < n; ++i) {
        const double peak = std::max(std::fabs((double)mixL_[i]), std::fabs((double)mixR_[i]));
        const double need = peak > kCeiling ? kCeiling / peak : 1.0;
        limGain_ = need < limGain_ ? need : 1.0 - (1.0 - limGain_) * limRelease_;
        if (limGain_ > need) limGain_ = need;
        mixL_[i] = (float)(mixL_[i] * limGain_);
        mixR_[i] = (float)(mixR_[i] * limGain_);
      }
    } else {
      limGain_ = 1.0;
    }
    for (int i = 0; i < n; ++i) { left[chunk + i] += mixL_[i]; right[chunk + i] += mixR_[i]; }
    if (songBeat_ >= 0) songBeat_ += n * songBpm_ / 60.0 / sampleRate_;
  }
  while (qi < queued_) applyNote(queue_[qi++], frames);
  queued_ = 0;
}

void SamplePath::chaseNote(int pitch, double velocity, double age) {
  // The walk as it would be `age` samples after its note-on: the same steps a play from there gives.
  transpose_ = pitch - 60;
  velocity_ = std::clamp(velocity, 0.0, 1.0);
  if (held_++ > 0) return;
  const int64_t target = std::max<int64_t>(0, (int64_t)std::llround(age));
  for (int l = 0; l < spath::kLayers; ++l) {
    LayerRt& L = layers_[l];
    const spath::Pattern& p = pattern(l);
    L.walk.reset(p);
    L.steps = 1;
    L.walking = L.walk.node >= 0;
    if (!L.walking) continue;
    double stepStart = 0;
    L.nextAt = stepSamples(p, p.nodes[(size_t)L.walk.node]);
    int guard = 0;
    while ((double)target >= std::ceil(L.nextAt) && guard++ < 1000000) {
      L.walk.advance(p);
      if (L.walk.node < 0) { L.walking = false; break; }
      ++L.steps;
      stepStart = std::ceil(L.nextAt);
      L.nextAt += stepSamples(p, p.nodes[(size_t)L.walk.node]);
    }
    L.elapsed = target;
    if (!L.walking) continue;
    --L.steps;  // step() counts it again
    step(l, target - (int64_t)stepStart);
  }
}

void SamplePath::adopt(const Instrument& previous) {
  const auto* o = dynamic_cast<const SamplePath*>(&previous);
  if (!o) return;
  held_ = o->held_;
  transpose_ = o->transpose_;
  velocity_ = o->velocity_;
  songBpm_ = o->songBpm_;
  songBeat_ = o->songBeat_;
  for (int l = 0; l < spath::kLayers; ++l) {
    LayerRt& L = layers_[l];
    const LayerRt& P = o->layers_[l];
    L.walk = P.walk;
    L.walking = P.walking;
    L.elapsed = P.elapsed;
    L.nextAt = P.nextAt;
    L.steps = P.steps;
    if (L.walk.node >= (int)pattern(l).nodes.size()) L.walk.reset(pattern(l));
    for (int s = 0; s < spath::kFxSlots; ++s) {
      if (L.fx[s] && P.fx[s] && L.fxType[s] == P.fxType[s]) L.fx[s]->adopt(const_cast<Effect&>(*P.fx[s]));
      L.holdL[s] = P.holdL[s]; L.holdR[s] = P.holdR[s]; L.holdLeft[s] = P.holdLeft[s];
    }
  }
  // Voices go on when they read the same sound (the files are shared by the asset store).
  for (int i = 0; i < kVoices; ++i) {
    voices_[i] = o->voices_[i];
    Voice& v = voices_[i];
    if (v.active && (!layers_[v.layer].sound || layers_[v.layer].sound.get() != v.data)) v.active = false;
  }
  if (reverb_ && o->reverb_) reverb_->adopt(const_cast<Effect&>(*o->reverb_));
  limGain_ = o->limGain_;
}

} // namespace core
