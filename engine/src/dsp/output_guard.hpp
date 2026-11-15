// Ear guard: the last stage of what the speakers or headphones get (Engine::process, after the meters).
// It only catches what is dangerous and never shapes the mix:
// - Peaks: nothing goes over kCeilingDb (-1 dBFS): a peak limiter with instant attack and a short release,
//   then a hard clamp at the ceiling. A mastered song (its own limiter below -1 dBFS) passes bit for bit;
//   what it catches is a soloed track or a song without a master limiter going over full scale.
// - Broken audio (NaN or Inf) is silence, never a burst of noise.
// It does not ride the loudness: a first version held -12 LUFS momentary and Diogo heard the volume move
// (2026-11-06). How loud a song is belongs to its mix (master gain, limiter), not to the output.
// Listening level (Diogo 2026-11-07, "Volume de escuta fixo -10 dB"): one fixed gain on what the device
// plays, set by the user (the server's default is -10 dB, the class's 0 dB). It never moves by itself; a
// change slides there in 30 ms. It applies even with the peak guard off.
// Exports are not touched (render_job); the meters read the song before the guard.
// Audio thread only (process); the reduction is an atomic any thread reads.
#pragma once
#include <atomic>

namespace core {

class OutputGuard {
public:
  static constexpr double kCeilingDb = -1;
  static constexpr double kPeakReleaseMs = 60;

  explicit OutputGuard(double sampleRate);
  void process(float* l, float* r, int n);
  // Off only for an engine that feeds no device (offline captures, tests): the server keeps it on at the output, and
  // NaN/Inf become silence either way.
  void setEnabled(bool on) { enabled_.store(on, std::memory_order_relaxed); }
  bool enabled() const { return enabled_.load(std::memory_order_relaxed); }
  static constexpr double kListenMinDb = -60, kListenMaxDb = 0, kListenSlideMs = 30;
  void setListenDb(double db);  // any thread; clamped to kListenMinDb..kListenMaxDb
  double listenDb() const { return listenDb_.load(std::memory_order_relaxed); }
  // The deepest peak reduction (dB, >= 0) since the last call.
  float takeReductionDb() { return reduction_.exchange(0, std::memory_order_relaxed); }

private:
  void reset();

  double peakGain_ = 1;      // the peak limiter's gain, now
  double peakReleaseCoef_, ceiling_;
  double listenGain_ = 1, listenCoef_;  // the listening gain, now (audio thread), and its slide
  bool started_ = false;
  std::atomic<double> listenDb_{0};
  std::atomic<bool> enabled_{true};
  std::atomic<float> reduction_{0};
};

}  // namespace core
