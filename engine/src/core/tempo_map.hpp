// Tempo map: the song's BPM over its ticks. The song starts at `bpm`; each change sets a new BPM from its
// tick on (a step, as on a sequencer), or, with `ramp`, glides to it: the BPM goes in a straight line (per
// tick) from the previous point to this one and arrives at this tick (M9). Ticks are the musical grid
// (notes, clips, automation); samples are real time. With no change the conversion is exactly
// tick * samplesPerTick: a song without a map renders bit for bit as before the map existed, and steps
// keep that same sum. A ramp is integrated in closed form (log1p/expm1), so ticks and samples convert both
// ways without drift.
#pragma once
#include <cstdint>
#include <vector>

namespace core {

struct TempoChange { int64_t tick = 0; double bpm = 145; bool ramp = false; };

class TempoMap {
public:
  TempoMap() : TempoMap(145, {}, 48000) {}
  // changes: sorted by tick, every tick > 0 (validated by fromJson).
  TempoMap(double bpm, const std::vector<TempoChange>& changes, double sampleRate);
  double sampleAt(double tick) const;         // exact (fractional) sample of a tick
  int64_t sampleOfTick(int64_t tick) const;   // rounded
  double tickAt(double sample) const;
  double bpmAt(double tick) const;
  double slowestBpm() const { return slowest_; }
  bool constant() const { return segs_.size() == 1; }
  double startSamplesPerTick() const { return segs_.front().spt; }

private:
  // A segment from its tick to the next one's: constant (slope 0, as before) or a ramp, with the BPM
  // bpm + slope * (tick - start). k = spt * bpm (samples per tick at 1 BPM), the same in every segment.
  struct Seg { double tick, sample, spt, bpm, slope = 0, k = 0; };
  static double sampleIn(const Seg& s, double tick);
  size_t segAtTick(double tick) const;
  size_t segAtSample(double sample) const;
  std::vector<Seg> segs_;  // the first starts at tick 0, sample 0
  double slowest_ = 145;
};

} // namespace core
