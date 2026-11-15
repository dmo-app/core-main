#include "graph.hpp"
#include "../dsp/smooth.hpp"
#include "../dsp/finite.hpp"
#include <cstdio>
#include <algorithm>
#include <atomic>
#include <bitset>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include "../dsp/sampler.hpp"
#include "../dsp/wave_mod.hpp"
#include "../dsp/sample_path.hpp"

namespace core {

namespace {
// An instrument parameter the instrument reads as a switch or a choice (rounded, or compared to 0.5): it jumps to
// its new value. Gliding there played the values between (filter types 0 -> 19 went through every type, a wavetable
// through every table, unison 1 -> 16 through every voice count) for the ~15 ms of the glide (audit dbbc592).
bool steppedInstrumentParam(std::string_view n) {
  if (n.size() > 3 && n[0] == 'l' && n[1] >= '2' && n[1] <= '9' && n[2] == '_') n.remove_prefix(3);  // W-Core layers 2-4
  if (n.rfind("sec_", 0) == 0) return true;  // a W-Core section on or off
  static constexpr std::string_view kNames[] = {
      // W-Core
      "on", "mode", "octave", "subOct", "subShape", "unison", "polyVoices", "legato", "glideAlways", "osc1Semi",
      "osc1Table", "osc2Table", "osc1Warp", "osc2Warp", "filterMode", "filter2Mode", "filterSerial",
      // Sampler
      "pitched", "retrigger", "reverse", "filterType",
      // Sample Path
      "limiter"};
  for (auto k : kNames) if (n == k) return true;
  return false;
}
}  // namespace

double Automation::valueAt(double tick) const {
  if (points.empty()) return 0;
  if (tick <= points.front().tick) return points.front().value;
  if (tick >= points.back().tick) return points.back().value;
  auto it = std::upper_bound(points.begin(), points.end(), tick, [](double t, const EnvelopePoint& p) { return t < p.tick; });
  const auto& b = *it; const auto& a = *(it - 1);
  double span = (double)(b.tick - a.tick);
  return span <= 0 ? b.value : a.value + (b.value - a.value) * (tick - a.tick) / span;
}

namespace {

std::atomic<uint64_t> g_nativeFailures{0};

// The units that failed (failedUnits): a fixed list the audio thread fills without allocating.
struct FailedSlot { std::atomic<bool> ready{false}; char text[96] = {}; };
FailedSlot g_failedSlots[32];
std::atomic<int> g_failedCount{0};
void noteFailed(const std::string& track, const char* unit) {
  g_nativeFailures.fetch_add(1, std::memory_order_relaxed);
  const int i = g_failedCount.fetch_add(1, std::memory_order_acq_rel);
  if (i < 0 || i >= 32) return;
  std::snprintf(g_failedSlots[i].text, sizeof g_failedSlots[i].text, "%s: %s", track.empty() ? "master" : track.c_str(), unit);
  g_failedSlots[i].ready.store(true, std::memory_order_release);
}

// M8a: a modulator's value (-1..1) at a song tick: its phase comes from the tick alone (seek, loop and
// export all agree). random: a value held for each cycle, from the cycle's number and the modulator.
}  // namespace
double Graph::envelopeValue(const TrackRt::ModRt& m, double age) {
  if (age < m.attack) return age / m.attack;
  return std::pow(0.001, (age - m.attack) / m.decay);
}
double Graph::riseValue(const TrackRt::ModRt& m, double ticks) {
  if (ticks < 0) return 0.0;
  if (ticks < m.cycleTicks) return std::pow(ticks / m.cycleTicks, m.curve);
  if (m.hold) return 1.0;
  constexpr double kDropTicks = kPpq / 64.0;
  return std::max(0.0, 1.0 - (ticks - m.cycleTicks) / kDropTicks);
}
double Graph::modValue(const TrackRt::ModRt& m, double tick, double age, double ageTicks) {
  if (m.shape == kEnvelopeShape) return age >= 0 ? envelopeValue(m, age) : 0.0;
  if (m.shape == kRiseShape) return riseValue(m, ageTicks);
  return lfoValue(m, tick);
}
double Graph::lfoValue(const TrackRt::ModRt& m, double tick) {
  const double pos = tick / m.cycleTicks + m.phase;
  const double cycle = std::floor(pos), x = pos - cycle;
  switch (m.shape) {
    case 0: return std::sin(2 * 3.141592653589793 * x);
    case 1: return 1 - 4 * std::fabs(x - 0.5) ;  // -1 at the cycle start, 1 half way
    case 2: return x < 0.5 ? 1.0 : -1.0;
    case 3: return 1 - 2 * x;
    case 4: return 2 * x - 1;
    case kStepsShape: {
      if (m.stepCount <= 0) return 0.0;
      return m.steps[(size_t)std::min(m.stepCount - 1, (int)(x * m.stepCount))];
    }
    default: {
      uint32_t h = m.seed ^ (uint32_t)(int64_t)cycle * 2654435761u;
      h ^= h >> 16; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
      return h / 2147483647.5 - 1.0;
    }
  }
}
namespace {

uint64_t clockNs() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Audio past +48 dBFS (256) is a unit gone wrong, not music.
constexpr float kRunawayLevel = 256.0f;
// The same guard for the built-in ("core") DSP: a block whose output is non-finite or runaway is bad, and the
// unit that made it is silenced and marked failed (Graph::runTrack / masterEnd). One scan of the block after
// process(); good audio is never touched, so the result stays bit for bit what it was.
bool nativeOutputBad(const float* left, const float* right, int frames) {
  for (int i = 0; i < frames; ++i) {
    const float l = left[i], r = right[i];
    if (!finiteBits(l) || !finiteBits(r) || std::fabs(l) > kRunawayLevel || std::fabs(r) > kRunawayLevel) return true;
  }
  return false;
}

}  // namespace

uint64_t nativeFailures() { return g_nativeFailures.load(std::memory_order_relaxed); }
std::vector<std::string> failedUnits() {
  std::vector<std::string> list;
  const int n = std::min(32, g_failedCount.load(std::memory_order_acquire));
  for (int i = 0; i < n; ++i)
    if (g_failedSlots[i].ready.load(std::memory_order_acquire)) list.emplace_back(g_failedSlots[i].text);
  return list;
}
void clearFailedUnits() {
  for (auto& s : g_failedSlots) s.ready.store(false, std::memory_order_release);
  g_failedCount.store(0, std::memory_order_release);
}


Graph::Graph(const Project& p, double sampleRate, AssetStore* assets, bool offline)
    : sampleRate_(sampleRate), bpm_(p.bpm), masterGain_(std::pow(10.0, p.masterGainDb / 20)),
      tempo_(p.bpm, p.tempoMap, sampleRate), lengthTicks_(p.lengthTicks) {
  songSamples_ = sampleOfTick(lengthTicks_);
  smoothCoef_ = 1.0 - std::exp(-(double)kSubBlock / (0.015 * sampleRate));
  // Built-in effects with their parameters in catalogue order (validated by fromJson). Bypassed ones stay
  // in the chain, so the others keep their places; returns whether any of them plays.
  std::vector<std::pair<Effect*, std::string>> sidechains;  // M7c: compressors and the track that keys each
  auto buildChain = [&](const json& list, std::vector<std::unique_ptr<Effect>>& chain, const std::string& owner) {
    bool any = false;
    for (const auto& fx : list) {
      if (fx.value("type", "") != "builtin" || fx.value("unsupported", false)) continue;
      const std::string name = fx.value("name", "");
      // A tempo-synced effect holds enough for the slowest tempo of the song, and starts at the first.
      auto e = makeEffect(name, sampleRate, tempo_.slowestBpm(), fx.value("version", 1));
      if (!e) continue;
      e->setTempo(p.bpm);
      const auto& specs = effectParams(name);
      const json params = fx.value("params", json::object());
      for (size_t i = 0; i < specs.size() && i < (size_t)kMaxEffectParams; ++i) {
        e->params[i] = e->knob[i] = e->glide[i] = params.value(specs[i].name, specs[i].def);
        const std::string_view knob = specs[i].name;
        if (steppedKnob(knob)) e->stepped.set(i);  // switches jump
      }
      e->knobCount = (int)std::min(specs.size(), (size_t)kMaxEffectParams);
      if (fx.contains("sidechain") && fx["sidechain"].is_string()) sidechains.push_back({e.get(), fx["sidechain"].get<std::string>()});
      e->id = fx.value("id", "");
      e->watchKey = fxWatchKey(owner, e->id);
      e->bypass = fx.value("bypass", false);
      any = any || !e->bypass;
      chain.push_back(std::move(e));
    }
    return any;
  };
  anyMasterEffect_ = buildChain(p.masterEffects, masterEffects_, "master");
  for (const auto& e : p.masterEnvelopes) {
    Automation a;
    if (e.param == "gainDb") { a.slot = -1; a.min = -60; a.max = 12; }
    else if (!resolveEffectParam(e.param, masterEffects_, a)) continue;
    a.points = e.points;
    masterAutomation_.push_back(std::move(a));
  }
  tracks_.reserve(p.tracks.size());
  for (const auto& t : p.tracks) {
    TrackRt rt;
    rt.id = t.id;
    rt.idHash = trackHash(t.id);
    rt.instrument = makeInstrument(t.instrument, sampleRate);
    if (t.instrument == "sampler") {
      // The files at the engine's rate, kept alive by the track (the voices only read them).
      std::vector<SamplerZone> zones;
      for (const auto& z : t.samples) {
        std::string why;
        std::shared_ptr<const AudioData> data = assets ? assets->get(z.path, sampleRate, &why) : nullptr;
        if (!data) { missingAssets_.push_back(assets ? why : z.path + ": no audio store"); continue; }
        // The loop in frames at the engine's rate, inside the file (a loop past its end is cut to it; too short: none).
        const int64_t frames = data->frames();
        int64_t loopStart = 0, loopEnd = 0;
        if (z.loopEnd > 0) {
          loopEnd = std::min<int64_t>(frames - 1, (int64_t)std::llround(z.loopEnd * sampleRate));
          loopStart = std::clamp<int64_t>((int64_t)std::llround(z.loopStart * sampleRate), 0, std::max<int64_t>(0, loopEnd - 1));
          if (loopEnd - loopStart < (int64_t)(kMinLoopSeconds * sampleRate)) loopStart = loopEnd = 0;
        }
        zones.push_back({std::move(data), z.rootKey, z.loKey, z.hiKey, loopStart, loopEnd});
      }
      auto sampler = std::make_unique<Sampler>(sampleRate);
      sampler->setZones(std::move(zones));
      rt.instrument = std::move(sampler);
    }
    if (t.instrument == "wave" && rt.instrument && !t.synth.is_null())
      rt.instrument->setWavePatch(wmod::parsePatch(t.synth, wmod::targets()));  // checked when the track was read
    if (auto* path = t.instrument == "samplepath" ? dynamic_cast<SamplePath*>(rt.instrument.get()) : nullptr) {
      // Core Glitch · Sample Path: its map, and one sound per sampler (the zone's lowest key names it, 0..3).
      path->setPatch(spath::parsePatch(t.synth));  // checked when the track was read
      bool taken[spath::kLayers] = {};
      for (const auto& z : t.samples) {
        const int layer = std::clamp(z.loKey, 0, spath::kLayers - 1);
        if (taken[layer]) continue;
        taken[layer] = true;
        std::string why;
        if (auto data = assets ? assets->get(z.path, sampleRate, &why) : nullptr) path->setLayerSource(layer, std::move(data));
        else missingAssets_.push_back(assets ? why : z.path + ": no audio store");
      }
    }
    if (t.instrument == "wave" && !t.samples.empty() && rt.instrument) {
      // W-Core Grain reads its grains from a sound, at the engine's rate: one per layer at most, the zone's lowest
      // key naming its layer (0 = layer 1, as every older song; 1..3 = layers 2..4).
      bool taken[4] = {};
      for (const auto& z : t.samples) {
        const int layer = std::clamp(z.loKey, 0, 3);
        if (taken[layer]) continue;
        taken[layer] = true;
        std::string why;
        if (auto data = assets ? assets->get(z.path, sampleRate, &why) : nullptr) rt.instrument->setLayerSource(layer, std::move(data));
        else missingAssets_.push_back(assets ? why : z.path + ": no audio store");
      }
    }
    const auto& specs = instrumentParams(t.instrument);
    rt.paramCount = hasNoInstrumentParams(t.instrument) ? 0 : (int)specs.size();
    for (int i = 0; i < rt.paramCount; ++i) {
      rt.baseParams[i] = t.params.at(specs[i].name);
      rt.paramMin[i] = specs[i].min; rt.paramMax[i] = specs[i].max;
      rt.instrument->params[i] = rt.baseParams[i];
      if (steppedInstrumentParam(specs[i].name)) rt.stepped.set((size_t)i);
    }
    rt.gainDb = t.gainDb; rt.pan = t.pan; rt.mute = t.mute; rt.solo = t.solo;
    rt.anyEffect = buildChain(t.effects, rt.effects, t.id);
    if (t.solo) anySolo_ = true;
    // Expand notes (and rolls) into on/off events. Roll hits are spread evenly over the note
    // length using exact tick arithmetic, so they stay locked to the kick at any tempo.
    auto addClip = [&](int64_t start, int64_t end, int64_t offset, int64_t patternTicks, const std::vector<Note>& notes) {
      ClipRt c;
      c.start = start;
      c.end = std::min(end, p.lengthTicks);
      c.offset = offset;
      c.patternTicks = patternTicks;
      for (const auto& n : notes) {
      int count = std::max(1, n.roll.count);
      for (int i = 0; i < count; ++i) {
        int64_t on = n.tick + (int64_t)std::llround((double)n.length * i / count);
        int64_t next = n.tick + (int64_t)std::llround((double)n.length * (i + 1) / count);
        int64_t off = count > 1 ? on + std::max<int64_t>(1, (int64_t)std::llround((next - on) * 0.85)) : n.tick + n.length;
        double vel = count > 1 ? n.roll.velocityFrom + (n.roll.velocityTo - n.roll.velocityFrom) * (double)i / (count - 1) : n.velocity;
        c.events.push_back({on, true, n.pitch, (float)std::min(1.0, vel), on});
        c.events.push_back({off, false, n.pitch, 0, on, (float)std::min(1.0, vel)});
      }
      }
      std::stable_sort(c.events.begin(), c.events.end(), [](const SeqEvent& a, const SeqEvent& b) {
        return a.tick < b.tick || (a.tick == b.tick && !a.on && b.on);  // off before on: repeated notes retrigger
      });
      c.maxEventTick = c.events.empty() ? 0 : c.events.back().tick;
      if (!c.events.empty() && c.start < c.end) rt.clips.push_back(std::move(c));
    };
    if (t.clips.empty()) addClip(0, t.lengthTicks > 0 ? t.lengthTicks : p.lengthTicks, 0, t.patternTicks, t.notes);
    for (const auto& cl : t.clips) addClip(cl.startTick, cl.startTick + cl.lengthTicks, cl.offsetTicks, cl.patternTicks, cl.notes);
    for (const auto& a : t.audioClips) {
      if (a.mute) continue;
      std::string why;
      std::shared_ptr<const AudioData> data = assets ? assets->get(a.path, sampleRate, &why) : nullptr;
      if (!data) { missingAssets_.push_back(assets ? why : a.path + ": no audio store"); continue; }
      AudioClipRt c;
      // Start, offset and end from the exact (fractional) song position, rounded once each: clips cut from
      // one another (split, slice) join sample for sample, with no gap, overlap or one-sample jump.
      const double exactStart = tempo_.sampleAt((double)a.startTick);
      c.start = sampleOfTick(a.startTick);
      c.offset = std::max<int64_t>(0, (int64_t)std::llround(a.offsetSeconds * sampleRate + ((double)c.start - exactStart)));
      const int64_t wanted = (int64_t)std::llround(exactStart + a.durationSeconds * sampleRate) - c.start;
      c.grains = clipGrains(a.pitchSemitones, a.freezeSeconds, sampleRate, c.offset, data->frames());
      if (a.freezeSeconds > 0) c.length = c.grains.freezeSpan > 0 ? wanted : 0;  // a freeze holds a piece of the file: the clip may outlast it
      else c.length = std::min<int64_t>(wanted, data->frames() - c.offset);
      if (c.length <= 0 || c.start >= songSamples_) continue;  // past the file's end, or after the song
      c.length = std::min<int64_t>(c.length, songSamples_ - c.start);  // the song end cuts it (export == real time)
      c.fadeIn = std::min<int64_t>(c.length, (int64_t)std::llround(a.fadeInSeconds * sampleRate));
      c.fadeOut = std::min<int64_t>(c.length, (int64_t)std::llround(a.fadeOutSeconds * sampleRate));
      c.gain = (float)std::pow(10.0, a.gainDb / 20);
      c.data = std::move(data);
      rt.audio.push_back(std::move(c));
    }
    for (const auto& m : t.modulators) {
      TrackRt::ModRt mr{};
      if (m.param == "gainDb") { mr.slot = -1; mr.min = -60; mr.max = 12; }
      else if (m.param == "pan") { mr.slot = -2; mr.min = -1; mr.max = 1; }
      else if (m.param.rfind("fx:", 0) == 0) {  // FCore: a built-in insert's parameter (gone: silent)
        Automation a;
        if (!resolveEffectParam(m.param, rt.effects, a) || a.slot != -3) continue;
        mr.slot = -3; mr.effect = a.effect; mr.effectParam = a.effectParam; mr.min = a.min; mr.max = a.max;
        const auto& fxSpecs = effectParams(rt.effects[(size_t)a.effect]->name());
        mr.octaves = std::string(fxSpecs[(size_t)a.effectParam].unit) == "Hz";
      }
      else {
        mr.slot = -100;
        for (int i = 0; i < rt.paramCount; ++i)
          if (m.param == specs[i].name) { mr.slot = i; mr.min = specs[i].min; mr.max = specs[i].max; mr.octaves = std::string(specs[i].unit) == "Hz"; }
        if (mr.slot == -100) continue;
      }
      const auto& shapes = modulatorShapes();
      mr.shape = (int)(std::find(shapes.begin(), shapes.end(), m.shape) - shapes.begin());
      mr.cycleTicks = m.rateBeats * kPpq;
      if (m.shape == "steps") {
        mr.stepCount = (int)std::min(m.steps.size(), (size_t)kMaxModSteps);
        for (int i = 0; i < mr.stepCount; ++i) mr.steps[(size_t)i] = (float)m.steps[(size_t)i];
        mr.cycleTicks *= std::max(1, mr.stepCount);  // one cycle = the whole sequence
      }
      mr.depth = m.on ? m.depth : 0.0;  // an LFO turned off moves nothing
      mr.phase = m.phase;
      mr.seed = (uint32_t)trackHash(t.id + ":" + m.id);
      mr.attack = m.attackMs * 0.001 * sampleRate;
      mr.decay = std::max(1.0, m.decayMs * 0.001 * sampleRate);
      mr.curve = m.curve;
      mr.hold = m.hold;
      mr.source = m.source;
      rt.anyEnvelope = rt.anyEnvelope || mr.shape == kEnvelopeShape || mr.shape == kRiseShape;
      rt.mods.push_back(mr);
    }
    for (const auto& e : t.envelopes) {
      Automation a;
      if (e.param == "gainDb") { a.slot = -1; a.min = -60; a.max = 12; }
      else if (e.param == "pan") { a.slot = -2; a.min = -1; a.max = 1; }
      else if (e.param.rfind("fx:", 0) == 0) { if (!resolveEffectParam(e.param, rt.effects, a)) continue; }
      else {
        a.slot = -100;
        for (int i = 0; i < rt.paramCount; ++i) if (e.param == specs[i].name) { a.slot = i; a.min = specs[i].min; a.max = specs[i].max; }
        if (a.slot == -100) continue;
      }
      a.points = e.points;
      rt.automation.push_back(std::move(a));
    }
    tracks_.push_back(std::move(rt));
  }
  // Modulators started by another track's notes: that track's note-ons over the whole song (all its clips and
  // repeats, muted or not), in song samples, as its sequencer plays them.
  for (auto& rt : tracks_)
    for (auto& mr : rt.mods) {
      if (mr.source.empty()) continue;
      const auto src = std::find_if(tracks_.begin(), tracks_.end(), [&](const TrackRt& x) { return x.id == mr.source; });
      if (src == tracks_.end()) continue;  // gone: it rests at 0
      for (const ClipRt& clip : src->clips) {
        if (clip.patternTicks <= 0) continue;
        const int64_t origin = clip.start - clip.offset;
        for (int64_t k = 0; origin + k * clip.patternTicks < clip.end; ++k) {
          const int64_t base = origin + k * clip.patternTicks;
          for (const SeqEvent& e : clip.events) {
            if (!e.on) continue;
            const int64_t noteStart = base + e.onTick;
            if (noteStart < clip.start || noteStart >= clip.end) continue;
            mr.sourceOns.push_back(sampleOfTick(base + e.tick));
          }
        }
      }
      std::sort(mr.sourceOns.begin(), mr.sourceOns.end());
      mr.sourceOns.erase(std::unique(mr.sourceOns.begin(), mr.sourceOns.end()), mr.sourceOns.end());
      rt.anyEnvelope = true;
    }
  // Latency compensation: each track's inserts that play (look-ahead, oversampling) delay it; the
  // others wait for the latest one, so everything stays on the beat. The master chain delays the whole mix
  // alike and needs nothing.
  for (auto& rt : tracks_) {
    for (const auto& e : rt.effects)
      if (!e->bypass) rt.latency += e->latency();
  }
  for (const auto& e : masterEffects_)
    if (!e->bypass) masterLatency_ += e->latency();
  // M7a routing: where each track's fader and sends go, in an order where every bus comes after what feeds
  // it (validateRouting has ruled out loops). Without buses the order is the project's, as before.
  {
    std::map<std::string, int> index;
    for (size_t i = 0; i < tracks_.size(); ++i) index[tracks_[i].id] = (int)i;
    for (auto& [effect, from] : sidechains) effect->sidechain = index.at(from);  // validated by fromJson
    for (size_t i = 0; i < tracks_.size(); ++i) {
      TrackRt& rt = tracks_[i];
      const Track& t = p.tracks[i];
      rt.bus = t.instrument == "bus";
      rt.out = t.output.empty() ? -1 : index.at(t.output);
      for (const auto& sd : t.sends) {
        TrackRt::SendRt s;
        s.to = index.at(sd.to);
        s.gain = std::pow(10.0, sd.gainDb / 20);
        s.pre = sd.pre;
        s.id = sd.id;
        rt.sends.push_back(std::move(s));
      }
    }
    // M7c: a compressor keyed by another track reads that track's signal in the same sub-block, so the
    // source plays first (validateRouting has ruled out loops and buses as sources). Without sidechains
    // the tracks keep the project's order, as before.
    std::vector<std::vector<int>> keyedBy(tracks_.size());  // track -> the tracks that key it
    for (size_t i = 0; i < tracks_.size(); ++i)
      for (auto& e : tracks_[i].effects) {
        if (e->sidechain < 0) continue;
        keyedBy[i].push_back(e->sidechain);
        tracks_[(size_t)e->sidechain].keySource = true;
        tracks_[i].keyed = true;
      }
    std::vector<char> placed(tracks_.size(), 0);
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t i = 0; i < tracks_.size(); ++i) {
        if (placed[i] || tracks_[i].bus) continue;
        bool ready = true;
        for (int k : keyedBy[i]) ready = ready && placed[(size_t)k];
        if (ready) { order_.push_back((int)i); placed[i] = 1; progress = true; }
      }
    }
    auto feedsUnplaced = [&](int bus) {  // a bus still waiting for another bus that feeds it
      for (size_t j = 0; j < tracks_.size(); ++j) {
        if (placed[j] || (int)j == bus) continue;
        if (tracks_[j].out == bus) return true;
        for (const auto& s : tracks_[j].sends) if (s.to == bus) return true;
      }
      return false;
    };
    for (bool progress = true; progress;) {
      progress = false;
      for (size_t i = 0; i < tracks_.size(); ++i) {
        if (placed[i] || feedsUnplaced((int)i)) continue;
        bool ready = true;
        for (int k : keyedBy[i]) ready = ready && placed[(size_t)k];
        if (ready) { order_.push_back((int)i); placed[i] = 1; progress = true; }
      }
    }
  }
  // Latency per path: a bus waits for the latest of its inputs; each output and send is delayed to line up
  // where it arrives, and the master waits for the latest of what reaches it. Without buses: every track
  // waits for the latest one, as before.
  {
    std::vector<int> in(tracks_.size(), 0), out(tracks_.size(), 0);
    for (int i : order_) {
      TrackRt& rt = tracks_[(size_t)i];
      // A keyed effect whose key arrives later than the program (its source placed before, so out[k] is known):
      // the program waits there.
      int at = in[(size_t)i];
      for (size_t e = 0; e < rt.effects.size(); ++e) {
        const Effect& fx = *rt.effects[e];
        if (fx.sidechain >= 0 && !fx.bypass) {
          const int late = out[(size_t)fx.sidechain] - at;
          if (late > 0) {
            TrackRt::KeyDelayRt d;
            d.effect = (int)e;
            d.l.assign((size_t)late, 0.0f);
            d.r.assign((size_t)late, 0.0f);
            rt.programDelays.push_back(std::move(d));
            rt.latency += late;
            at += late;
          }
        }
        if (!fx.bypass) at += fx.latency();
      }
      out[(size_t)i] = in[(size_t)i] + rt.latency;
      if (rt.out >= 0) in[(size_t)rt.out] = std::max(in[(size_t)rt.out], out[(size_t)i]);
      else latency_ = std::max(latency_, out[(size_t)i]);
      for (const auto& s : rt.sends) in[(size_t)s.to] = std::max(in[(size_t)s.to], out[(size_t)i]);
    }
    for (int i : order_) {
      TrackRt& rt = tracks_[(size_t)i];
      const int wait = (rt.out >= 0 ? in[(size_t)rt.out] : latency_) - out[(size_t)i];
      if (wait > 0) { rt.delayL.assign((size_t)wait, 0.0f); rt.delayR.assign(rt.delayL.size(), 0.0f); }
      for (auto& s : rt.sends) {
        const int sendWait = in[(size_t)s.to] - out[(size_t)i];
        if (sendWait > 0) { s.delayL.assign((size_t)sendWait, 0.0f); s.delayR.assign(s.delayL.size(), 0.0f); }
      }
    }
    // Sidechains: the program reaches a keyed effect in[i] + the latency before it on its track; the key leaves
    // its source after that source's inserts (out[k]). The key waits for the difference.
    for (size_t i = 0; i < tracks_.size(); ++i) {
      TrackRt& rt = tracks_[i];
      int at = in[i];
      for (size_t e = 0; e < rt.effects.size(); ++e) {
        const Effect& fx = *rt.effects[e];
        for (const auto& pd : rt.programDelays) if (pd.effect == (int)e) at += (int)pd.l.size();
        if (fx.sidechain >= 0) {
          const int wait = at - out[(size_t)fx.sidechain];
          if (wait > 0) {
            TrackRt::KeyDelayRt d;
            d.effect = (int)e;
            d.l.assign((size_t)wait, 0.0f);
            d.r.assign((size_t)wait, 0.0f);
            rt.keyDelays.push_back(std::move(d));
          }
        }
        if (fx.bypass) continue;
        at += fx.latency();
      }
    }
  }
  // Solo: a soloed track is heard through the buses it feeds; a soloed bus brings what feeds it.
  for (size_t i = 0; i < tracks_.size(); ++i) tracks_[i].soloOk = false;
  for (size_t i = 0; i < tracks_.size(); ++i) if (p.tracks[i].solo) markSolo((int)i, [&](int k) { tracks_[(size_t)k].soloOk = true; });
  // M0b: real time measures where its time goes (an export does not: nobody waits for it).
  if (!offline) {
    cpu_ = std::make_shared<CpuMeter>();
    auto& entries = cpu_->entries;
    auto effectName = [](const Effect& e) -> std::string { return e.name(); };
    for (size_t i = 0; i < tracks_.size(); ++i) {
      TrackRt& rt = tracks_[i];
      rt.cpuSlot = (int)entries.size();
      entries.push_back({rt.id, "", p.tracks[i].instrument});
      for (const auto& e : rt.effects) entries.push_back({rt.id, e->id, effectName(*e)});
    }
    masterCpuSlot_ = (int)entries.size();
    for (const auto& e : masterEffects_) entries.push_back({"", e->id, effectName(*e)});
    cpu_->ns = std::make_unique<std::atomic<uint64_t>[]>(std::max<size_t>(1, entries.size()));
    for (size_t i = 0; i < entries.size(); ++i) cpu_->ns[i].store(0, std::memory_order_relaxed);
  }
}

uint64_t Graph::fxWatchKey(const std::string& owner, const std::string& effectId) {
  if (effectId.empty()) return 0;
  uint64_t h = 1469598103934665603ull;  // FNV-1a of "owner\nid"
  for (const char c : owner + "\n" + effectId) { h ^= (unsigned char)c; h *= 1099511628211ull; }
  return h ? h : 1;
}

// The first latency_ samples out are what the effects had not given yet: rendered and dropped here, and
// from then on the caller's position p plays the song at p + latency_ (its sample p is the song's p).
void Graph::preroll() {
  float l[kSubBlock], r[kSubBlock];
  // The master's built-in latency too: an export starts on the song's first sample after a master limiter.
  const int64_t total = latency_ + (anyMasterEffect_ && solo_ < 0 ? masterLatency_ : 0);  // stems skip the master
  for (int64_t pos = 0; pos < total; pos += kSubBlock) {
    const int n = (int)std::min<int64_t>(kSubBlock, total - pos);
    std::fill(l, l + n, 0.0f);
    std::fill(r, r + n, 0.0f);
    process(l, r, n, pos, false);
  }
  shift_ = total;
}

bool Graph::resolveEffectParam(const std::string& param, const std::vector<std::unique_ptr<Effect>>& chain, Automation& a) {
  const size_t colon = param.find(':', 3);
  if (colon == std::string::npos) return false;
  const std::string id = param.substr(3, colon - 3), name = param.substr(colon + 1);
  for (size_t i = 0; i < chain.size(); ++i) {
    if (chain[i]->id != id) continue;
    const auto& specs = effectParams(chain[i]->name());
    for (size_t k = 0; k < specs.size(); ++k)
      if (name == specs[k].name) { a.slot = -3; a.effect = (int)i; a.effectParam = (int)k; a.min = specs[k].min; a.max = specs[k].max; return true; }
  }
  return false;
}

// Once per aligned sub-block: every knob of a chain glides to its value (a knob moved by an edit does not
// click, audit 2.8). Automation, read after this, still sets the parameters it drives.
static void glideKnobs(std::vector<std::unique_ptr<Effect>>& chain, double coef, int64_t aligned) {
  for (auto& e : chain) {
    if (e->knobCount == 0 || aligned == e->glideBlock) continue;
    for (int p = 0; p < e->knobCount; ++p) {
      if (!e->glideInit || e->stepped.test((size_t)p)) e->glide[p] = e->knob[p];
      else e->glide[p] = glideTo(e->glide[p], e->knob[p], coef);
      e->params[p] = e->glide[p];
    }
    e->glideInit = true;
    e->glideBlock = aligned;
  }
}

// Once per aligned sub-block: the effect parameter glides to the curve (same result for any buffer size).
void Graph::applyEffectAutomation(Automation& a, std::vector<std::unique_ptr<Effect>>& chain, double tick, int64_t aligned) {
  const double v = std::min(a.max, std::max(a.min, a.valueAt(tick)));
  if (aligned != a.block) {
    a.smooth = a.smoothInit ? glideTo(a.smooth, v, smoothCoef_) : v;
    a.smoothInit = true;
    a.block = aligned;
  }
  chain[(size_t)a.effect]->params[a.effectParam] = a.smooth;
}

void Graph::adoptAutomation(std::vector<Automation>& now, const std::vector<std::unique_ptr<Effect>>& chain,
                            const std::vector<Automation>& before, const std::vector<std::unique_ptr<Effect>>& beforeChain) {
  for (auto& a : now) {
    if (a.slot != -3 || chain[(size_t)a.effect]->id.empty()) continue;
    for (const auto& b : before)
      if (b.slot == -3 && b.effectParam == a.effectParam && beforeChain[(size_t)b.effect]->id == chain[(size_t)a.effect]->id) {
        a.smooth = b.smooth; a.smoothInit = b.smoothInit; a.block = b.block;
        break;
      }
  }
}

void Graph::markSolo(int track, const std::function<void(int)>& mark) {
  std::vector<char> down(tracks_.size(), 0), up(tracks_.size(), 0);
  std::function<void(int)> goDown = [&](int k) {
    if (down[(size_t)k]) return;
    down[(size_t)k] = 1; mark(k);
    if (tracks_[(size_t)k].out >= 0) goDown(tracks_[(size_t)k].out);
    for (const auto& s : tracks_[(size_t)k].sends) goDown(s.to);
  };
  std::function<void(int)> goUp = [&](int k) {
    if (up[(size_t)k]) return;
    up[(size_t)k] = 1; mark(k);
    for (size_t j = 0; j < tracks_.size(); ++j) {
      bool feeds = tracks_[j].out == k;
      for (const auto& s : tracks_[j].sends) feeds = feeds || s.to == k;
      if (feeds) goUp((int)j);
    }
  };
  goDown(track);
  goUp(track);
}

void Graph::setSolo(int track, bool withoutMasterGain) {
  solo_ = track;
  stemWithoutMasterGain_ = track >= 0 && withoutMasterGain;
  stemOk_.assign(tracks_.size(), 0);
  if (track >= 0 && track < (int)tracks_.size()) markSolo(track, [&](int k) { stemOk_[(size_t)k] = 1; });
}

void Graph::adopt(Graph& previous) {
  // An effect keeps its sound (delay and reverb tails) across edits: the one with the same id (moved,
  // bypassed, others removed), or without ids the one at the same place, if it is the same kind.
  auto adoptChain = [this](std::vector<std::unique_ptr<Effect>>& now, std::vector<std::unique_ptr<Effect>>& before) {
    for (size_t i = 0; i < now.size(); ++i) {
      Effect* match = nullptr;
      if (!now[i]->id.empty()) {
        for (auto& b : before) if (b && b->id == now[i]->id) { match = b.get(); break; }
      } else if (i < before.size() && before[i] && before[i]->id.empty()) {
        match = before[i].get();
      }
      if (match && std::strcmp(now[i]->name(), match->name()) == 0) {
        if (keepTails) now[i]->adopt(*match);
        if (now[i]->knobCount == match->knobCount) {  // the knobs glide on from where they were heard
          std::copy(match->glide, match->glide + kMaxEffectParams, now[i]->glide);
          now[i]->glideInit = match->glideInit;
          now[i]->glideBlock = match->glideBlock;
        }
      }
    }
  };
  adoptChain(masterEffects_, previous.masterEffects_);
  adoptAutomation(masterAutomation_, masterEffects_, previous.masterAutomation_, previous.masterEffects_);
  for (auto& t : tracks_) {
    for (auto& o : previous.tracks_) {
      if (o.id == t.id && std::strcmp(o.instrument->name(), t.instrument->name()) == 0) {
        if (keepTails) t.instrument->adopt(*o.instrument);
        adoptChain(t.effects, o.effects);
        adoptAutomation(t.automation, t.effects, o.automation, o.effects);
        t.fromL = o.fromL; t.fromR = o.fromR; t.toL = o.toL; t.toR = o.toR; t.rampBlock = o.rampBlock; t.gainInit = o.gainInit;
        t.postFromL = o.postFromL; t.postFromR = o.postFromR; t.postToL = o.postToL; t.postToR = o.postToR;
        std::copy(o.smooth, o.smooth + kMaxParams, t.smooth); t.smoothInit = o.smoothInit; t.paramBlock = o.paramBlock;
        t.lastOnSample = o.lastOnSample; t.envAge = o.envAge; t.envTicks = o.envTicks; t.envBlock = o.envBlock;
        if (keepTails && t.delayL.size() == o.delayL.size()) {  // the same delay: what is on its way still comes out (no gap)
          std::copy(o.delayL.begin(), o.delayL.end(), t.delayL.begin());
          std::copy(o.delayR.begin(), o.delayR.end(), t.delayR.begin());
          t.delayAt = o.delayAt;
        }
        for (auto& d : t.programDelays)  // the program on its way to a keyed effect too
          for (const auto& od : o.programDelays)
            if (keepTails && od.effect == d.effect && od.l.size() == d.l.size()) {
              std::copy(od.l.begin(), od.l.end(), d.l.begin());
              std::copy(od.r.begin(), od.r.end(), d.r.begin());
              d.at = od.at;
            }
        for (auto& d : t.keyDelays)  // a key on its way to its compressor still arrives
          for (const auto& od : o.keyDelays)
            if (keepTails && od.effect == d.effect && od.l.size() == d.l.size()) {
              std::copy(od.l.begin(), od.l.end(), d.l.begin());
              std::copy(od.r.begin(), od.r.end(), d.r.begin());
              d.at = od.at;
            }
        for (auto& s : t.sends)
          for (const auto& os : o.sends)
            if (os.id == s.id && os.to == s.to && o.out == t.out) {
              s.from = os.from; s.now = os.now; s.rampBlock = os.rampBlock; s.init = os.init;
              if (keepTails && s.delayL.size() == os.delayL.size()) {
                std::copy(os.delayL.begin(), os.delayL.end(), s.delayL.begin());
                std::copy(os.delayR.begin(), os.delayR.end(), s.delayR.begin());
                s.delayAt = os.delayAt;
              }
            }
        break;
      }
    }
  }
}

void Graph::liveNote(std::string_view id, int pitch, double velocity, bool on) {
  for (auto& t : tracks_) if (t.id == id) { if (on) t.instrument->noteOn(pitch, velocity); else t.instrument->noteOff(pitch); }
}

void Graph::allNotesOff() { for (auto& t : tracks_) t.instrument->allNotesOff(); }

template <class F> void Graph::forEachHeld(const TrackRt& t, int64_t position, F&& f) const {
  position += shift_;
  const double posTick = tempo_.tickAt((double)position);
  for (const ClipRt& clip : t.clips) {
    if (posTick < clip.start || posTick >= clip.end + clip.maxEventTick) continue;
    const int64_t origin = clip.start - clip.offset;
    // Repeats whose notes can still sound at posTick: the one it is in, and the ones before it whose
    // note-offs reach past it.
    const int64_t kLast = (int64_t)std::floor((posTick - origin) / clip.patternTicks);
    const int64_t kFirst = std::max<int64_t>(0, (int64_t)std::floor((posTick - origin - clip.maxEventTick) / clip.patternTicks));
    for (int64_t k = kFirst; k <= kLast; ++k) {
      const int64_t base = origin + k * clip.patternTicks;
      for (const SeqEvent& e : clip.events) {
        if (e.on) continue;
        const int64_t noteStart = base + e.onTick;
        if (noteStart < clip.start || noteStart >= clip.end) continue;  // not this clip's note
        const int64_t on = sampleOfTick(noteStart), off = sampleOfTick(base + e.tick);
        if (on >= position || off < position) continue;  // not held
        f(e, position - on, off);
      }
    }
  }
}

void Graph::chase(int64_t position) {
  const int64_t at = position + shift_;
  for (auto& t : tracks_)
    forEachHeld(t, position, [&](const SeqEvent& e, int64_t age, int64_t off) {
      // Started `age` samples ago: the built-in instruments pick it up there (audit E4). One ending right
      // here is not worth starting.
      if (off > at) t.instrument->chaseNote(e.pitch, e.noteVelocity, (double)age);
    });
}

void Graph::reconcile(const Graph& previous, int64_t previousPosition, int64_t position) {
  if (!keepTails) return;  // nothing was adopted
  const int64_t at = position + shift_;
  for (auto& t : tracks_) {
    const TrackRt* o = nullptr;
    for (const auto& p : previous.tracks_)
      if (p.id == t.id && std::strcmp(p.instrument->name(), t.instrument->name()) == 0) { o = &p; break; }
    if (!o) continue;  // a new instrument: it adopted nothing
    std::bitset<128> before, now, chased;
    previous.forEachHeld(*o, previousPosition, [&](const SeqEvent& e, int64_t, int64_t) { before.set((size_t)(e.pitch & 127)); });
    forEachHeld(t, position, [&](const SeqEvent& e, int64_t, int64_t) { now.set((size_t)(e.pitch & 127)); });
    if (before == now) continue;
    // Offs first: a mono lead lets go of the old key before it takes the new one.
    for (int p = 0; p < 128; ++p) if (before[(size_t)p] && !now[(size_t)p]) t.instrument->noteOff(p);
    forEachHeld(t, position, [&](const SeqEvent& e, int64_t age, int64_t off) {
      const size_t p = (size_t)(e.pitch & 127);
      if (before[p] || chased[p] || off <= at) return;
      chased.set(p);
      t.instrument->chaseNote(e.pitch, e.noteVelocity, (double)age);
    });
  }
}

void Graph::pitchOff(int pitch) { for (auto& t : tracks_) t.instrument->noteOff(pitch); }
void Graph::pitchOff(uint64_t hash, int pitch) { for (auto& t : tracks_) if (t.idHash == hash) t.instrument->noteOff(pitch); }

void Graph::renderTrack(int index, TrackRt& t, float* buf, float* bufR, int frames, int64_t songPos, EventLog* log) {
  const bool stereo = t.instrument->stereo();
  // Events whose sample falls in [songPos, songPos + frames), ordered by sample, offs before ons, then
  // clip, pattern repeat and position in the pattern (a note-off of one repeat can land after the first
  // note of the next). They are taken in batches of at most kMaxPending, allocation free: a block
  // denser than that takes several passes over the events, and never drops one.
  struct Pending { int64_t sample; int clip; int64_t loop; int index; };
  auto eventOf = [&t](const Pending& p) -> const SeqEvent& { return t.clips[(size_t)p.clip].events[(size_t)p.index]; };
  auto before = [&](const Pending& a, const Pending& b) {
    if (a.sample != b.sample) return a.sample < b.sample;
    const bool aOn = eventOf(a).on, bOn = eventOf(b).on;
    if (aOn != bOn) return !aOn;
    if (a.clip != b.clip) return a.clip < b.clip;
    return a.loop != b.loop ? a.loop < b.loop : a.index < b.index;
  };
  constexpr int kMaxPending = 256, kMaxClips = 256, kMaxLoops = 8;
  Pending pending[kMaxPending];
  Pending last{};
  bool hasLast = false, more = !t.clips.empty();
  const int64_t end = songPos + frames;
  const double firstTick = tempo_.tickAt((double)songPos) - 2, lastTick = tempo_.tickAt((double)end) + 2;
  // Per clip and repeat, where the next pass starts scanning: every event before it is done.
  int start[kMaxClips][kMaxLoops];
  for (size_t ci = 0; ci < std::min<size_t>(t.clips.size(), kMaxClips); ++ci) std::fill(start[ci], start[ci] + kMaxLoops, -1);
  int cursor = 0;
  while (more) {
    more = false;
    int count = 0;
    for (int ci = 0; ci < (int)t.clips.size(); ++ci) {
      const ClipRt& clip = t.clips[(size_t)ci];
      // Repeats are counted from origin (the clip start minus its phase); a clip's events reach at
      // most maxEventTick past its last repeat.
      if (lastTick < clip.start || firstTick > clip.end + clip.maxEventTick) continue;
      const int64_t origin = clip.start - clip.offset;
      const double localFirst = firstTick - origin, localLast = lastTick - origin;
      const int64_t loopFirst = std::max<int64_t>(0, (int64_t)std::floor((localFirst - clip.maxEventTick) / clip.patternTicks));
      const int64_t loopLast = (int64_t)std::floor(localLast / clip.patternTicks);
      for (int64_t k = loopFirst; k <= loopLast; ++k) {
        const int64_t base = origin + k * clip.patternTicks;
        int* from = ci < kMaxClips && k - loopFirst < kMaxLoops ? &start[ci][k - loopFirst] : nullptr;
        int first = from ? *from : -1;
        if (first < 0) {
          // Sorted ticks also map to nondecreasing samples, including at tempo steps.
          // Find the first event in this window using the same rounded boundary as rendering.
          const auto it = std::lower_bound(clip.events.begin(), clip.events.end(), songPos,
            [&](const SeqEvent& event, int64_t sample) { return sampleOfTick(base + event.tick) < sample; });
          first = (int)(it - clip.events.begin());
          if (from) *from = first;
        }
        bool done = true;  // still inside the finished prefix of this repeat
        for (int i = first; i < (int)clip.events.size(); ++i) {
          const SeqEvent& e = clip.events[(size_t)i];
          const int64_t absTick = base + e.tick;
          const int64_t s = sampleOfTick(absTick);
          if (s >= end) break;  // sorted within a repeat
          const Pending x{s, ci, k, i};
          // A note plays only if it starts inside the clip; its note-off follows it, and the note-off of
          // a note that never started is dropped (it would cut the same pitch in the next clip).
          const int64_t noteStart = base + e.onTick;
          if (noteStart < clip.start || noteStart >= clip.end ||             // not this clip's note
              (hasLast && !before(last, x))) {                                 // applied by an earlier pass
            if (done && from) *from = i + 1;
            continue;
          }
          done = false;
          if (count == kMaxPending) {  // keep the earliest kMaxPending, the rest go next pass
            more = true;
            if (!before(x, pending[count - 1])) continue;
            --count;
          }
          int j = count - 1;  // insertion into the sorted batch
          while (j >= 0 && before(x, pending[j])) { pending[j + 1] = pending[j]; --j; }
          pending[j + 1] = x;
          ++count;
        }
      }
    }
    for (int i = 0; i < count; ++i) {
      const int at = (int)(pending[i].sample - songPos);
      if (at > cursor) { if (!stereo) t.instrument->render(buf + cursor, at - cursor); cursor = at; }
      const SeqEvent& e = eventOf(pending[i]);
      if (stereo) t.instrument->queueNote(at, e.on, e.pitch, e.velocity);
      if (e.on) {
        t.lastOnSample = pending[i].sample;  // M8b: envelope modulators start here
        if (!stereo) t.instrument->noteOn(e.pitch, e.velocity);
        const ClipRt& clip = t.clips[(size_t)pending[i].clip];
        if (log) log->entries.push_back({index, pending[i].sample, clip.start - clip.offset + pending[i].loop * clip.patternTicks + e.tick, e.pitch});
      } else if (!stereo) {
        t.instrument->noteOff(e.pitch);
      }
    }
    if (count) { last = pending[count - 1]; hasLast = true; }
    if (more) extraEventPasses_.fetch_add(1, std::memory_order_relaxed);
  }
  if (stereo) t.instrument->renderStereo(buf, bufR, frames);
  else if (cursor < frames) t.instrument->render(buf + cursor, frames - cursor);
  if (const uint64_t dropped = t.instrument->takeDroppedNotes()) droppedNotes_.fetch_add(dropped, std::memory_order_relaxed);
}

// Audio clips of one track into left/right (added). Clips that overlap add up; fades are linear in gain.
void Graph::renderAudio(const TrackRt& t, float* left, float* right, int frames, int64_t songPos) {
  for (const auto& c : t.audio) {
    const int64_t from = std::max(songPos, c.start), to = std::min(songPos + frames, c.start + c.length);
    const float* srcL = c.data->L.data();
    const float* srcR = c.data->R.data();
    if (c.grains.grain > 0) {  // VOICE STUTTER's pitch / freeze (dsp/clip_grains.hpp)
      const int64_t frames = c.data->frames();
      for (int64_t p = from; p < to; ++p) {
        const int64_t inClip = p - c.start;
        float g = c.gain;
        if (inClip < c.fadeIn) g *= (float)inClip / (float)c.fadeIn;
        if (c.length - inClip <= c.fadeOut) g *= (float)(c.length - inClip) / (float)c.fadeOut;
        double l, r;
        clipGrainFrame(c.grains, c.offset, srcL, srcR, frames, inClip, l, r);
        left[p - songPos] += (float)l * g;
        right[p - songPos] += (float)r * g;
      }
      continue;
    }
    for (int64_t p = from; p < to; ++p) {
      const int64_t inClip = p - c.start;
      float g = c.gain;
      if (inClip < c.fadeIn) g *= (float)inClip / (float)c.fadeIn;
      if (c.length - inClip <= c.fadeOut) g *= (float)(c.length - inClip) / (float)c.fadeOut;
      const int64_t src = c.offset + inClip;
      left[p - songPos] += srcL[src] * g;
      right[p - songPos] += srcR[src] * g;
    }
  }
}

// The master gain at a tick (stateless: the export's tracks read it ahead of the master).
double Graph::masterGainAt(double tick) const {
  double masterGain = masterGain_;
  for (const auto& a : masterAutomation_)
    if (a.slot == -1) masterGain = std::pow(10.0, std::min(a.max, std::max(a.min, a.valueAt(tick))) / 20);
  return masterGain;
}

// The master's own state at the start of a sub-block: tempo, knob glide, effect automation.
void Graph::masterStart(const Step& st) {
  if (!tempo_.constant() && st.bpm != effectBpm_) {
    effectBpm_ = st.bpm;
    for (auto& e : masterEffects_) e->setTempo(st.bpm);
  }
  glideKnobs(masterEffects_, smoothCoef_, st.aligned);
  for (auto& a : masterAutomation_) {
    if (a.slot == -1) continue;  // masterGainAt
    applyEffectAutomation(a, masterEffects_, st.tick, st.aligned);
  }
}

void Graph::masterEnd(const Step& st, float* mixL, float* mixR, float* L, float* R) {
  const bool meter = cpu_ && cpuMetering_;
  uint64_t clock = meter ? clockNs() : 0;
  float reduction = 0;
  float dryL[kSubBlock], dryR[kSubBlock];  // a failed master effect lets the mix through, never silences it
  for (size_t e = 0; e < masterEffects_.size(); ++e) {
    if (masterEffects_[e]->bypass || masterEffects_[e]->failed()) continue;
    const bool keep = protectNative_ && st.n <= kSubBlock;
    masterEffects_[e]->setPosition(st.pos >= 0 ? st.beat : -1.0);
    if (keep) { std::copy(mixL, mixL + st.n, dryL); std::copy(mixR, mixR + st.n, dryR); }
    feedWatch(*masterEffects_[e], mixL, mixR, st.n);
    masterEffects_[e]->process(mixL, mixR, st.n);
    if (keep && nativeOutputBad(mixL, mixR, st.n)) {
      if (masterEffects_[e]->markFailed()) noteFailed("", masterEffects_[e]->name());
      std::copy(dryL, dryL + st.n, mixL); std::copy(dryR, dryR + st.n, mixR);  // the mix goes through as it came
    }
    reduction += masterEffects_[e]->reductionDb;
    if (meter) { const uint64_t now = clockNs(); cpu_->add(masterCpuSlot_ + (int)e, now - clock); clock = now; }
  }
  masterReduction_ = std::max(masterReduction_, reduction);
  for (int s = 0; s < st.n; ++s) { L[s] += mixL[s]; R[s] += mixR[s]; }
}

// The next sub-block of a call: where it plays and how long it is.
Graph::Step Graph::step(int64_t position, int done, int frames, bool looping) {
  Step st;
  int64_t pos = position + done;
  if (looping && songSamples_ > 0) {
    pos %= songSamples_;
    if (pos == 0 && done + position > 0) allNotesOff();
  }
  // Sub-blocks are aligned to absolute multiples of kSubBlock, so automation and gain ramps land on
  // the same samples whatever the device buffer size is (export == real time, bit for bit).
  int n = std::min<int>(kSubBlock - (int)(((pos % kSubBlock) + kSubBlock) % kSubBlock), frames - done);
  // Never cross the song end inside one sub-block, so the loop point is sample exact.
  if (looping && songSamples_ > 0) n = (int)std::min<int64_t>(n, songSamples_ - pos);
  st.pos = pos;
  st.n = n;
  st.done = done;
  st.aligned = pos - (((pos % kSubBlock) + kSubBlock) % kSubBlock);
  st.tick = tempo_.tickAt((double)st.aligned);  // automation is read at the aligned sub-block start
  st.bpm = tempo_.constant() ? bpm_ : tempo_.bpmAt(st.tick);
  st.beat = tempo_.tickAt((double)pos) / kPpq;
  st.masterGain = stemWithoutMasterGain_ ? 1.0 : masterGainAt(st.tick);
  return st;
}

// One track over one sub-block: instrument (or a bus's inputs), audio clips, inserts, fader, pan and
// sends. What it gives goes to the sink: added at once (real time, buses) or kept per track (the
// export's workers), then added in the same order, so both give the same sums.
template <class Sink>
void Graph::runTrack(int i, const Step& st, float* buf, float* bufR, EventLog* log, Sink& sink) {
  TrackRt& t = tracks_[(size_t)i];
  const int n = st.n;
  const int64_t pos = st.pos, aligned = st.aligned;
  const double tick = st.tick;
  // Tempo-synced effects follow the tempo of the sub-block (only a song with tempo changes ever changes it).
  if (!tempo_.constant() && st.bpm != t.effectBpm) {
    t.effectBpm = st.bpm;
    for (auto& e : t.effects) e->setTempo(st.bpm);
  }
  double gainDb = t.gainDb, pan = t.pan;
  double target[kMaxParams];
  for (int p = 0; p < t.paramCount; ++p) target[p] = t.baseParams[p];
  glideKnobs(t.effects, smoothCoef_, aligned);
  for (auto& a : t.automation) {
    if (a.slot == -3) { applyEffectAutomation(a, t.effects, tick, aligned); continue; }
    double v = std::min(a.max, std::max(a.min, a.valueAt(tick)));
    if (a.slot >= 0) target[a.slot] = v;
    else if (a.slot == -1) gainDb = v;
    else pan = v;
  }
  // M8b: how long ago the last note started (at the aligned sub-block start; none yet, or after a jump
  // back: the envelope rests at 0).
  // Once per aligned sub-block, like the glide: a note-on in the first part of a split sub-block counts
  // from the next one, as it does when the sub-block is played whole (real time == export).
  if (aligned != t.envBlock) {
    t.envAge = t.lastOnSample != INT64_MIN && aligned >= t.lastOnSample ? (double)(aligned - t.lastOnSample) : -1;
    t.envTicks = t.envAge >= 0 ? std::max(0.0, tick - tempo_.tickAt((double)t.lastOnSample)) : -1;
    t.envBlock = aligned;
  }
  // A modulator started by another track's notes counts from the last of them at or before the aligned
  // sub-block start (a note inside a split sub-block counts from the next one, like the track's own notes).
  for (auto& m : t.mods) {
    if (m.source.empty() || aligned == m.srcBlock) continue;
    const auto it = std::upper_bound(m.sourceOns.begin(), m.sourceOns.end(), aligned);
    if (it == m.sourceOns.begin()) { m.srcAge = m.srcTicks = -1; }
    else {
      const int64_t on = *(it - 1);
      m.srcAge = (double)(aligned - on);
      m.srcTicks = std::max(0.0, tick - tempo_.tickAt((double)on));
    }
    m.srcBlock = aligned;
  }
  const double ownAge = t.envAge, ownTicks = t.envTicks;
  for (auto& m : t.mods) {
    const double age = m.source.empty() ? ownAge : m.srcAge, ageTicks = m.source.empty() ? ownTicks : m.srcTicks;  // M8a: LFOs on top of the knob and its automation
    const bool fromNote = m.shape == kEnvelopeShape || m.shape == kRiseShape;
    if (fromNote && m.slot >= 0) continue;  // after the glide, below
    if (m.slot == -3) {
      // FCore: on an insert's parameter, after its knob glide and automation (both above). Worked out once per
      // aligned sub-block; a split sub-block writes the same value again (its base may have been reset).
      double& v = t.effects[(size_t)m.effect]->params[m.effectParam];
      if (aligned != m.fxBlock) {
        const double w = modValue(m, tick, age, ageTicks);
        m.fxValue = std::clamp(m.octaves ? v * std::exp2(m.depth * w) : v + m.depth * w, m.min, m.max);
        m.fxBlock = aligned;
      }
      v = m.fxValue;
      continue;
    }
    const double w = modValue(m, tick, age, ageTicks);
    if (m.slot >= 0) target[m.slot] = std::clamp(m.octaves ? target[m.slot] * std::exp2(m.depth * w) : target[m.slot] + m.depth * w, m.min, m.max);
    else if (m.slot == -1) gainDb = std::clamp(gainDb + m.depth * w, m.min, m.max);
    else pan = std::clamp(pan + m.depth * w, m.min, m.max);
  }
  // Parameters glide to their target once per aligned sub-block (same result for any buffer size).
  if (aligned != t.paramBlock) {
    for (int p = 0; p < t.paramCount; ++p)
      t.smooth[p] = t.smoothInit && !t.stepped.test((size_t)p) ? glideTo(t.smooth[p], target[p], smoothCoef_) : target[p];
    t.smoothInit = true;
    t.paramBlock = aligned;
  }
  for (int p = 0; p < t.paramCount; ++p) t.instrument->params[p] = t.smooth[p];
  // M8b: a note's envelope (or an FCore rise) on an instrument parameter is not glided (a fast decay stays fast).
  if (t.anyEnvelope)
    for (const auto& m : t.mods) {
      if ((m.shape != kEnvelopeShape && m.shape != kRiseShape) || m.slot < 0) continue;
      const double w = m.source.empty() ? modValue(m, tick, ownAge, ownTicks) : modValue(m, tick, m.srcAge, m.srcTicks);
      double& v = t.instrument->params[m.slot];
      v = std::clamp(m.octaves ? v * std::exp2(m.depth * w) : v + m.depth * w, m.min, m.max);
    }
  const bool meter = cpu_ && cpuMetering_;  // setCpuMetering(false): skip every clock read on the audio thread
  uint64_t clock = meter ? clockNs() : 0;
  const bool stereoInstrument = t.bus || t.instrument->stereo();
  if (t.bus) {  // a bus plays what its inputs gave it in this sub-block
    std::copy(t.inL, t.inL + n, buf);
    std::copy(t.inR, t.inR + n, bufR);
    std::fill(t.inL, t.inL + n, 0.0f);
    std::fill(t.inR, t.inR + n, 0.0f);
  } else {
    std::fill(buf, buf + n, 0.0f);
    if (stereoInstrument) std::fill(bufR, bufR + n, 0.0f);
    if (!t.instrument->failed()) {  // a failed native instrument is silent from then on (its track mutes)
      t.instrument->setPosition(pos >= 0 ? st.beat : -1.0, st.bpm);  // slow movements follow the song (SongClock)
      renderTrack(i, t, buf, bufR, n, pos, log);
      if (protectNative_ && nativeOutputBad(buf, stereoInstrument ? bufR : buf, n)) {
        if (t.instrument->markFailed()) noteFailed(t.id, t.instrument->name());
        std::fill(buf, buf + n, 0.0f);
        if (stereoInstrument) std::fill(bufR, bufR + n, 0.0f);
      }
    }
  }
  // Instruments are mono (the same signal both sides), except the stereo ones; audio clips keep their two channels.
  const float* right = stereoInstrument ? bufR : buf;
  if (!t.audio.empty()) {
    std::copy(buf, buf + n, bufR);
    if (pos >= 0) renderAudio(t, buf, bufR, n, pos);
    right = bufR;
    // A broken audio file (NaN or runaway samples) plays as silence for that block. No unit is to blame, so
    // nothing is marked failed, and the first insert or master effect is not taken out for a bad input.
    if (protectNative_ && nativeOutputBad(buf, bufR, n)) { std::fill(buf, buf + n, 0.0f); std::fill(bufR, bufR + n, 0.0f); }
  }
  if (meter) { const uint64_t now = clockNs(); cpu_->add(t.cpuSlot, now - clock); clock = now; }
  if (t.anyEffect) {  // inserts, before the fader: from here the track is stereo
    if (right == buf) std::copy(buf, buf + n, bufR);
    for (size_t e = 0; e < t.effects.size(); ++e) {
      for (auto& d : t.programDelays) {
        if (d.effect != (int)e) continue;
        const size_t len = d.l.size();
        for (int s = 0; s < n; ++s) {
          const float l = d.l[d.at], r = d.r[d.at];
          d.l[d.at] = buf[s]; d.r[d.at] = bufR[s];
          buf[s] = l; bufR[s] = r;
          d.at = d.at + 1 == len ? 0 : d.at + 1;
        }
      }
      if (t.effects[e]->bypass || t.effects[e]->failed()) continue;  // a failed effect is out of the chain
      if (const int k = t.effects[e]->sidechain; k >= 0) {  // M7c: the key of this sub-block
        const TrackRt& src = tracks_[(size_t)k];
        t.effects[e]->keyL = src.keyKept ? src.keyKeptL.data() + st.done : src.keyL;
        t.effects[e]->keyR = src.keyKept ? src.keyKeptR.data() + st.done : src.keyR;
        for (auto& d : t.keyDelays) {
          if (d.effect != (int)e) continue;
          const size_t len = d.l.size();
          for (int s = 0; s < n; ++s) {
            d.outL[s] = d.l[d.at]; d.outR[s] = d.r[d.at];
            d.l[d.at] = t.effects[e]->keyL[s]; d.r[d.at] = t.effects[e]->keyR[s];
            d.at = d.at + 1 == len ? 0 : d.at + 1;
          }
          t.effects[e]->keyL = d.outL;
          t.effects[e]->keyR = d.outR;
        }
      }
      t.effects[e]->setPosition(pos >= 0 ? st.beat : -1.0);
      feedWatch(*t.effects[e], buf, bufR, n);
      t.effects[e]->process(buf, bufR, n);
      if (protectNative_ && nativeOutputBad(buf, bufR, n)) {
        if (t.effects[e]->markFailed()) noteFailed(t.id, t.effects[e]->name());
        std::fill(buf, buf + n, 0.0f); std::fill(bufR, bufR + n, 0.0f);
      }
      if (meter) { const uint64_t now = clockNs(); cpu_->add(t.cpuSlot + 1 + (int)e, now - clock); clock = now; }
    }
    right = bufR;
  }
  if (t.keySource) sink.key(t, st, buf, right);  // M7c: after its inserts, before its fader and mute
  const bool audible = !t.mute && (solo_ >= 0 ? stemOk_[(size_t)i] != 0 : (!anySolo_ || t.soloOk));
  // The master gain only on what reaches the master: a bus's inputs get it once, at the bus's fader.
  const double fader = audible ? std::pow(10.0, gainDb / 20) : 0.0;
  double g = t.out < 0 ? fader * st.masterGain : fader;
  double angle = (pan + 1) * 0.25 * 3.141592653589793;  // constant-power pan
  double gl = g * std::cos(angle) * 1.4142135623730951, gr = g * std::sin(angle) * 1.4142135623730951;
  const double pl = fader * std::cos(angle) * 1.4142135623730951, pr = fader * std::sin(angle) * 1.4142135623730951;
  if (!t.gainInit) { t.toL = gl; t.toR = gr; t.postToL = pl; t.postToR = pr; t.gainInit = true; }
  if (aligned != t.rampBlock) {
    t.fromL = t.toL; t.fromR = t.toR; t.toL = gl; t.toR = gr;
    t.postFromL = t.postToL; t.postFromR = t.postToR; t.postToL = pl; t.postToR = pr;
    t.rampBlock = aligned;
  }
  for (auto& sd : t.sends) {  // sends ramp like the fader (an edit or a mute never clicks)
    const double target = audible ? sd.gain : 0.0;
    if (!sd.init) { sd.now = target; sd.init = true; }
    if (aligned != sd.rampBlock) { sd.from = sd.now; sd.now = target; sd.rampBlock = aligned; }
  }
  // Linear ramp across the aligned sub-block: no zipper noise from automation or fader moves.
  const int offset = (int)(pos - aligned);
  for (int s = 0; s < n; ++s) {
    double f = (offset + s + 1.0) / kSubBlock;
    double l = t.fromL + (t.toL - t.fromL) * f, r = t.fromR + (t.toR - t.fromR) * f;
    float vl = (float)(buf[s] * l), vr = (float)(right[s] * r);
    for (size_t k = 0; k < t.sends.size(); ++k) {
      auto& sd = t.sends[k];
      const double g = sd.from + (sd.now - sd.from) * f;
      // post: fader and pan, without the master gain (the bus's own way to the master adds it)
      const double fl = sd.pre ? 1.0 : t.postFromL + (t.postToL - t.postFromL) * f;
      const double fr = sd.pre ? 1.0 : t.postFromR + (t.postToR - t.postFromR) * f;
      float sl = (float)(buf[s] * fl * g), sr = (float)(right[s] * fr * g);
      if (!sd.delayL.empty()) {
        std::swap(sl, sd.delayL[sd.delayAt]);
        std::swap(sr, sd.delayR[sd.delayAt]);
        if (++sd.delayAt == sd.delayL.size()) sd.delayAt = 0;
      }
      sink.send(t, k, st, s, sl, sr);
    }
    if (!t.delayL.empty()) {  // latency compensation
      std::swap(vl, t.delayL[t.delayAt]);
      std::swap(vr, t.delayR[t.delayAt]);
      if (++t.delayAt == t.delayL.size()) t.delayAt = 0;
    }
    sink.out(i, t, st, s, vl, vr);
  }
}


struct Graph::DirectSink {
  Graph& g;
  float* outL;
  float* outR;
  float* peaks;
  void key(TrackRt& t, const Step& st, const float* l, const float* r) {
    std::copy(l, l + st.n, t.keyL);
    std::copy(r, r + st.n, t.keyR);
  }
  void send(TrackRt& t, size_t k, const Step&, int s, float l, float r) {
    TrackRt& bus = g.tracks_[(size_t)t.sends[k].to];
    bus.inL[s] += l;
    bus.inR[s] += r;
  }
  void out(int i, TrackRt& t, const Step&, int s, float l, float r) {
    float* dl = t.out < 0 ? outL : g.tracks_[(size_t)t.out].inL;
    float* dr = t.out < 0 ? outR : g.tracks_[(size_t)t.out].inR;
    dl[s] += l;
    dr[s] += r;
    if (peaks) {
      peaks[2 * i] = std::max(peaks[2 * i], std::fabs(l));
      peaks[2 * i + 1] = std::max(peaks[2 * i + 1], std::fabs(r));
    }
  }
};

// The export's workers: each track keeps what it gives over the whole call, added later in order.
struct Graph::KeepSink {
  void key(TrackRt& t, const Step& st, const float* l, const float* r) {
    std::copy(l, l + st.n, t.keyKeptL.data() + st.done);
    std::copy(r, r + st.n, t.keyKeptR.data() + st.done);
  }
  void send(TrackRt& t, size_t k, const Step& st, int s, float l, float r) {
    t.sends[k].keptL[(size_t)(st.done + s)] = l;
    t.sends[k].keptR[(size_t)(st.done + s)] = r;
  }
  void out(int, TrackRt& t, const Step& st, int s, float l, float r) {
    t.keptL[(size_t)(st.done + s)] = l;
    t.keptR[(size_t)(st.done + s)] = r;
  }
};

void Graph::useLiveWorkers(RtWorkerPool* pool, int maxFrames) {
  live_ = nullptr;
  workers_ = nullptr;
  parallel_.clear();
  if (!pool || pool->threads() == 0 || maxFrames <= 0) return;
  for (const int i : order_) if (!tracks_[(size_t)i].bus && !tracks_[(size_t)i].keyed) parallel_.push_back(i);
  // Everything the audio thread will need, allocated here: a call is at most maxFrames, so at most
  // maxFrames / kSubBlock + 1 sub-blocks.
  for (const int i : parallel_) {
    TrackRt& t = tracks_[(size_t)i];
    t.keptL.assign((size_t)maxFrames, 0.0f); t.keptR.assign((size_t)maxFrames, 0.0f);
    for (auto& sd : t.sends) { sd.keptL.assign((size_t)maxFrames, 0.0f); sd.keptR.assign((size_t)maxFrames, 0.0f); }
    if (t.keySource) { t.keyKeptL.assign((size_t)maxFrames, 0.0f); t.keyKeptR.assign((size_t)maxFrames, 0.0f); }
  }
  steps_.reserve((size_t)(maxFrames / kSubBlock + 2));
  live_ = pool;
  liveMaxFrames_ = maxFrames;
}

// A worker's job: one track over every sub-block of the call (as in the export).
void Graph::liveJob(void* graph, int k) {
  Graph& g = *static_cast<Graph*>(graph);
  float buf[kSubBlock], bufR[kSubBlock];
  KeepSink keep;
  for (const Step& st : g.steps_) g.runTrack(g.parallel_[(size_t)k], st, buf, bufR, nullptr, keep);
}

// M5: processParallel for the callback. The same split of the work and the same order of the sums, so the
// same output bit for bit; no allocation, and the wait for the workers ends at the deadline.
bool Graph::processLive(float* L, float* R, int frames, int64_t position, float* trackPeaks, uint64_t deadlineNs) {
  steps_.clear();  // reserved in useLiveWorkers: no allocation
  for (int done = 0; done < frames;) {
    steps_.push_back(step(position, done, frames, false));
    done += steps_.back().n;
  }
  for (const int i : parallel_) tracks_[(size_t)i].keyKept = tracks_[(size_t)i].keySource;
  const bool finished = live_->run(&Graph::liveJob, this, (int)parallel_.size(), deadlineNs);
  if (!finished) return false;  // a worker still holds a track: nothing else may run on this graph now
  for (const int i : parallel_) tracks_[(size_t)i].keyKept = false;
  if (live_->failed()) throw std::runtime_error("A track failed on a worker thread.");
  float buf[kSubBlock], bufR[kSubBlock], mixL[kSubBlock], mixR[kSubBlock];
  for (const Step& st : steps_) {
    masterStart(st);
    const bool master = anyMasterEffect_ && solo_ < 0;
    if (master) { std::fill(mixL, mixL + st.n, 0.0f); std::fill(mixR, mixR + st.n, 0.0f); }
    float* outL = master ? mixL : L + st.done;
    float* outR = master ? mixR : R + st.done;
    for (const int i : order_) {
      TrackRt& t = tracks_[(size_t)i];
      if (t.bus || t.keyed) {
        // Keys from tracks the workers played are read from their kept buffers (keyKept).
        for (const int j : parallel_) tracks_[(size_t)j].keyKept = tracks_[(size_t)j].keySource;
        DirectSink sink{*this, outL, outR, trackPeaks};
        runTrack(i, st, buf, bufR, nullptr, sink);
        for (const int j : parallel_) tracks_[(size_t)j].keyKept = false;
        continue;
      }
      for (int s = 0; s < st.n; ++s) {
        const size_t at = (size_t)(st.done + s);
        for (auto& sd : t.sends) {
          tracks_[(size_t)sd.to].inL[s] += sd.keptL[at];
          tracks_[(size_t)sd.to].inR[s] += sd.keptR[at];
        }
        float* dl = t.out < 0 ? outL : tracks_[(size_t)t.out].inL;
        float* dr = t.out < 0 ? outR : tracks_[(size_t)t.out].inR;
        const float l = t.keptL[at], r = t.keptR[at];
        dl[s] += l;
        dr[s] += r;
        if (trackPeaks) {
          trackPeaks[2 * i] = std::max(trackPeaks[2 * i], std::fabs(l));
          trackPeaks[2 * i + 1] = std::max(trackPeaks[2 * i + 1], std::fabs(r));
        }
      }
    }
    if (master) masterEnd(st, mixL, mixR, L + st.done, R + st.done);
  }
  return true;
}

void Graph::useWorkers(WorkerPool* workers) {
  workers_ = workers;
  parallel_.clear();
  if (!workers) return;
  // A track keyed by a sidechain plays with the buses, after the workers (its key is ready then).
  for (const int i : order_) if (!tracks_[(size_t)i].bus && !tracks_[(size_t)i].keyed) parallel_.push_back(i);
}

bool Graph::process(float* L, float* R, int frames, int64_t position, bool looping, EventLog* log, float* trackPeaks, uint64_t deadlineNs) {
  position += shift_;  // after preroll (export)
  if (workers_ && workers_->threads() > 0 && !looping && !log && !trackPeaks && !cpu_ && frames >= kParallelMinFrames && parallel_.size() >= 2) {
    processParallel(L, R, frames, position);
    return true;
  }
  if (live_ && !looping && !log && frames <= liveMaxFrames_ && parallel_.size() >= 2)
    return processLive(L, R, frames, position, trackPeaks, deadlineNs);
  float buf[kSubBlock], bufR[kSubBlock], mixL[kSubBlock], mixR[kSubBlock];
  int done = 0;
  while (done < frames) {
    const Step st = step(position, done, frames, looping);
    masterStart(st);
    // With a master chain the tracks mix into a sub-block buffer first; without one they add straight
    // into the output (as before: the same sums, bit for bit).
    const bool master = anyMasterEffect_ && solo_ < 0;  // stems are taken before the master chain
    if (master) { std::fill(mixL, mixL + st.n, 0.0f); std::fill(mixR, mixR + st.n, 0.0f); }
    DirectSink sink{*this, master ? mixL : L + done, master ? mixR : R + done, trackPeaks};
    for (const int i : order_) runTrack(i, st, buf, bufR, log, sink);
    if (master) masterEnd(st, mixL, mixR, L + done, R + done);
    done += st.n;
  }
  return true;
}

// M4a: the export with the tracks on several threads. Every track that is not a bus plays the whole call
// on its own (the same sub-blocks as the serial loop), keeping what it gives; then, sub-block by
// sub-block and in the serial order, those are added into the buses and the mix, the buses play and the
// master chain runs. The same operations in the same order: the file is the serial one, bit for bit.
void Graph::processParallel(float* L, float* R, int frames, int64_t position) {
  steps_.clear();
  for (int done = 0; done < frames;) {
    steps_.push_back(step(position, done, frames, false));
    done += steps_.back().n;
  }
  for (const int i : parallel_) {  // offline: allocation is allowed
    TrackRt& t = tracks_[(size_t)i];
    if ((int)t.keptL.size() < frames) { t.keptL.resize((size_t)frames); t.keptR.resize((size_t)frames); }
    for (auto& sd : t.sends)
      if ((int)sd.keptL.size() < frames) { sd.keptL.resize((size_t)frames); sd.keptR.resize((size_t)frames); }
    if (t.keySource && (int)t.keyKeptL.size() < frames) { t.keyKeptL.resize((size_t)frames); t.keyKeptR.resize((size_t)frames); }
    t.keyKept = t.keySource;  // its key is read from the call's buffer until the end of this call
  }
  workers_->run((int)parallel_.size(), [&](int k) {
    float buf[kSubBlock], bufR[kSubBlock];
    KeepSink keep;
    for (const Step& st : steps_) runTrack(parallel_[(size_t)k], st, buf, bufR, nullptr, keep);
  });
  float buf[kSubBlock], bufR[kSubBlock], mixL[kSubBlock], mixR[kSubBlock];
  for (const Step& st : steps_) {
    masterStart(st);
    const bool master = anyMasterEffect_ && solo_ < 0;
    if (master) { std::fill(mixL, mixL + st.n, 0.0f); std::fill(mixR, mixR + st.n, 0.0f); }
    float* outL = master ? mixL : L + st.done;
    float* outR = master ? mixR : R + st.done;
    DirectSink sink{*this, outL, outR, nullptr};
    for (const int i : order_) {
      TrackRt& t = tracks_[(size_t)i];
      if (t.bus || t.keyed) { runTrack(i, st, buf, bufR, nullptr, sink); continue; }
      // Per sample, as runTrack adds them: its sends, then its output.
      for (int s = 0; s < st.n; ++s) {
        const size_t at = (size_t)(st.done + s);
        for (auto& sd : t.sends) {
          tracks_[(size_t)sd.to].inL[s] += sd.keptL[at];
          tracks_[(size_t)sd.to].inR[s] += sd.keptR[at];
        }
        float* dl = t.out < 0 ? outL : tracks_[(size_t)t.out].inL;
        float* dr = t.out < 0 ? outR : tracks_[(size_t)t.out].inR;
        dl[s] += t.keptL[at];
        dr[s] += t.keptR[at];
      }
    }
    if (master) masterEnd(st, mixL, mixR, L + st.done, R + st.done);
  }
  for (const int i : parallel_) tracks_[(size_t)i].keyKept = false;
}

} // namespace core
