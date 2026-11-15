#include "tempo_map.hpp"
#include <algorithm>
#include <cmath>
#include "project.hpp"

namespace core {

TempoMap::TempoMap(double bpm, const std::vector<TempoChange>& changes, double sampleRate) {
  const double k = samplesPerTick(bpm, sampleRate) * bpm;
  segs_.push_back({0.0, 0.0, samplesPerTick(bpm, sampleRate), bpm, 0.0, k});
  slowest_ = bpm;
  for (const auto& c : changes) {
    Seg& last = segs_.back();
    if (c.tick <= last.tick) continue;  // validated before; never two segments on one tick
    // A ramp makes the segment before the point glide to its BPM; a step leaves it constant.
    if (c.ramp && c.bpm != last.bpm) last.slope = (c.bpm - last.bpm) / ((double)c.tick - last.tick);
    const double at = sampleIn(last, (double)c.tick);
    segs_.push_back({(double)c.tick, at, samplesPerTick(c.bpm, sampleRate), c.bpm, 0.0, k});
    slowest_ = std::min(slowest_, c.bpm);  // a straight line: its slowest point is one of its ends
  }
}

// The last segment that starts at or before the point: a binary search (a song may have up to 65
// segments and the scheduler asks per event; audit 2026-11-04 I1: a scan from the end cost 1.9x the
// block time at 65 segments). Allocation free on the audio thread. A NaN lands in the last segment,
// as the scan did.
size_t TempoMap::segAtTick(double tick) const {
  if (segs_.size() == 1) return 0;
  const auto it = std::upper_bound(segs_.begin() + 1, segs_.end(), tick, [](double t, const Seg& s) { return t < s.tick; });
  return (size_t)(it - segs_.begin()) - 1;
}
size_t TempoMap::segAtSample(double sample) const {
  if (segs_.size() == 1) return 0;
  const auto it = std::upper_bound(segs_.begin() + 1, segs_.end(), sample, [](double x, const Seg& s) { return x < s.sample; });
  return (size_t)(it - segs_.begin()) - 1;
}

// A constant segment computes tick * spt and sample / spt exactly as the engine did before the map.
// A ramp: samples = integral of k / bpm(t) dt = (k / slope) * ln(bpm(t) / bpm), written with log1p so a
// gentle ramp keeps its precision. Before its start (a ramp from tick 0, asked for a negative tick in a
// preroll) the segment goes on at its starting tempo.
double TempoMap::sampleIn(const Seg& s, double tick) {
  const double d = tick - s.tick;
  if (s.slope == 0 || d <= 0) return s.sample + d * s.spt;
  return s.sample + (s.k / s.slope) * std::log1p(s.slope * d / s.bpm);
}
double TempoMap::sampleAt(double tick) const { return sampleIn(segs_[segAtTick(tick)], tick); }
int64_t TempoMap::sampleOfTick(int64_t tick) const { return (int64_t)std::llround(sampleAt((double)tick)); }
double TempoMap::tickAt(double sample) const {
  const Seg& s = segs_[segAtSample(sample)];
  const double d = sample - s.sample;
  if (s.slope == 0 || d <= 0) return s.tick + d / s.spt;
  return s.tick + (s.bpm / s.slope) * std::expm1(s.slope * d / s.k);  // the inverse of sampleIn
}
double TempoMap::bpmAt(double tick) const {
  const Seg& s = segs_[segAtTick(tick)];
  return s.slope == 0 || tick <= s.tick ? s.bpm : s.bpm + s.slope * (tick - s.tick);
}

} // namespace core
