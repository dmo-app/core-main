#pragma once
#include <algorithm>
#include <cmath>

namespace core {

// The song's clock for a slow movement (an LFO, a drift). Diogo 2026-11-06: "after 8 bars, when it starts
// again, it sounds different, and it keeps changing". A movement that only ran on by itself was somewhere else
// on every pass of a loop (a W-Core pad brighter on one pass, duller on the next). With this it is placed by the
// song position at every jump (play, loop round, seek) and runs on per sample in between: every pass is the
// same, and a buffer split anywhere gives the same samples (export = real time).
// at(beat) before each sub-block (beat < 0: stopped, the movement runs free), run(n) once per sub-block.
struct SongClock {
  double beat = -1, perSample = 0, bpm = 120;
  bool smooth = false;  // the last jump came while playing (a loop round, a seek): slide there, never step
  void tempo(double songBpm, double sampleRate) { bpm = std::max(1.0, songBpm); perSample = bpm / 60.0 / sampleRate; }
  // True when the song jumped here: the caller places its movement with phase().
  bool at(double songBeat) {
    if (songBeat < 0) { beat = -1; return false; }
    const bool jumped = beat < 0 || std::fabs(songBeat - beat) > 1e-3;
    if (jumped) smooth = beat >= 0;
    beat = songBeat;
    return jumped;
  }
  void run(int frames) { if (beat >= 0) beat += perSample * frames; }
  // The speed a movement really runs at: hz moved to the nearest whole number of cycles in 4 bars (16 beats),
  // at least one. Diogo chose (2026-11-06) that what moves by itself repeats exactly every 4 bars: a 0.37 Hz
  // width at 150 BPM (2.37 cycles in 4 bars) runs at 2 cycles, so bar 1 and bar 5 sound the same. The sign is
  // kept (a frequency shift down stays down); 0 stays 0.
  static constexpr double kBeats = 16;
  double lock(double hz) const {
    if (hz == 0) return 0;
    const double perCycle = 60.0 / bpm * kBeats;  // seconds in 4 bars
    const double cycles = std::max(1.0, std::round(std::fabs(hz) * perCycle));
    return (hz < 0 ? -cycles : cycles) / perCycle;
  }
  // Where a movement of hz cycles per second (locked to 4 bars) is at this point of the song (0..1).
  double phase(double hz) const { const double p = std::max(0.0, beat) * 60.0 / bpm * lock(hz); return p - std::floor(p); }
};

// One moving phase (0..1) that follows a SongClock. On a jump while playing it keeps sounding where it was and
// slides to the song's place in ~30 ms (a chorus or a flanger never clicks at the loop point); from a stop it
// starts at the song's place at once. Every pass of a loop starts and ends the same way, so passes repeat exactly.
struct SongPhase {
  double phase = 0, offset = 0;
  void place(double to, bool smooth) {
    if (smooth) { double d = phase + offset - to; d -= std::round(d); offset = d; }
    else offset = 0;
    phase = to;
  }
  // This sample's phase, then one step on. slide: per-sample decay of the offset (slideCoef).
  double next(double step, double slide) {
    double v = phase + offset;
    v -= std::floor(v);
    phase += step;
    phase -= std::floor(phase);
    if (offset != 0) { offset *= slide; if (std::fabs(offset) < 1e-12) offset = 0; }
    return v;
  }
  static double slideCoef(double sampleRate) { return std::exp(-1.0 / (0.03 * sampleRate)); }
};

}  // namespace core
