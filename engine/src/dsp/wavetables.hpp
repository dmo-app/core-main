// Wavetables for W-Core (Synth): Diogo 2026-11-07, "o Phase Plant e o Surge têm 3 ou mais osciladores com wavetables
// completas". Built-in tables of 16 frames each; an oscillator reads one table at a position (0..1) that morphs
// smoothly from frame to frame. Every frame is stored band-limited per octave (mip levels), so a high note never
// aliases: the level read keeps only the harmonics under Nyquist at that pitch.
#pragma once
#include <array>
#include <vector>

namespace core {

class Wavetables {
public:
  static constexpr int kFrames = 16;    // morph steps per table
  static constexpr int kSize = 1024;    // samples per frame (512 harmonics at the lowest level)
  static constexpr int kLevels = 10;    // level m keeps the first 512 >> m harmonics
  static constexpr int kTables = 8;     // 1..8 (0 = W-Core's classic shapes, played elsewhere)
  // The names shown under the knob, in order from 1.
  static const char* name(int table);
  // Built once (about 3 ms, with allocations), shared by every instrument. W-Core calls it when it is made, off the
  // audio thread; the render only reads. built(): whether that has happened (tests).
  static const Wavetables& get();
  static bool built();
  // One sample of table 1..kTables at a position 0..1 (frame morph), phase 0..1, for a phase step dt (cycles per sample).
  double read(int table, double position, double phase, double dt) const;
private:
  Wavetables();
  std::vector<float> data_;  // [table][frame][level][kSize + 1] (one guard sample for the interpolation)
  const float* frame(int table, int f, int level) const {
    return data_.data() + (((size_t)(table - 1) * kFrames + (size_t)f) * kLevels + (size_t)level) * (kSize + 1);
  }
};

}  // namespace core
