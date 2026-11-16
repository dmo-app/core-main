// Real-time wrapper around Graph: transport, loop, live notes, meters and timing stats.
// Control thread: publish(), play(), seek()...; audio thread: process(). They only share atomics
// and lock-free single-producer/single-consumer queues, so the audio thread never waits.
#pragma once
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "graph.hpp"
#include "../dsp/analyzer.hpp"
#include "../dsp/loudness.hpp"
#include "../dsp/output_guard.hpp"

namespace core {

template <typename T, int N>
class SpscQueue {  // single producer, single consumer, fixed size
  std::array<T, N> items_{};
  std::atomic<int> head_{0}, tail_{0};
public:
  bool push(const T& v) {
    int h = head_.load(std::memory_order_relaxed), next = (h + 1) % N;
    if (next == tail_.load(std::memory_order_acquire)) return false;
    items_[h] = v; head_.store(next, std::memory_order_release); return true;
  }
  bool pop(T& v) {
    int t = tail_.load(std::memory_order_relaxed);
    if (t == head_.load(std::memory_order_acquire)) return false;
    v = items_[t]; tail_.store((t + 1) % N, std::memory_order_release); return true;
  }
};

struct EngineStats {
  std::atomic<uint64_t> callbacks{0}, frames{0};
  std::atomic<uint64_t> overruns{0};        // render took longer than the buffer lasts
  std::atomic<uint64_t> lateCallbacks{0};   // the device ran ahead of the audio delivered (starvation = audible gap)
  std::atomic<uint64_t> minFrames{UINT64_MAX}, maxFrames{0};  // callback sizes the device really used
  std::atomic<uint64_t> maxRenderNs{0}, lastRenderNs{0}, totalRenderNs{0};
  std::atomic<uint64_t> graphSwaps{0};
  std::atomic<uint64_t> maxSwapDelayNs{0};  // publish() -> graph live in the audio thread
  std::atomic<uint64_t> leakedGraphs{0};    // retire queue full (should stay 0)
  // Saturation: nothing is dropped silently, each case is counted here.
  std::atomic<uint64_t> liveNotesRejected{0};  // live note-on refused: queue full (the command reports an error)
  std::atomic<uint64_t> liveOffOverflows{0};   // live note-off did not fit in the queue: its pitch on its track was released
  std::atomic<uint64_t> liveOffAllTracks{0};   // of those, released on every track (more than 8 tracks for one pitch)
  std::atomic<uint64_t> auditionSteals{0};     // timed note-off slots all taken: the soonest one ended early
  std::atomic<uint64_t> droppedNotes{0};       // notes an instrument's queue could not hold in one block
  std::atomic<uint64_t> denseEventBlocks{0};   // extra event passes in blocks denser than one batch (all events played)
  std::atomic<uint64_t> callbackErrors{0};     // an exception in the audio callback (a plugin): that buffer was silence
  // M0b: the callback's load, render time / the time its buffer lasts, in millionths (1000000 = 100 %).
  std::atomic<uint64_t> totalBudgetNs{0};      // with totalRenderNs: the average load
  std::atomic<uint64_t> maxLoadPpm{0};         // the worst callback since the last reset
  std::atomic<uint64_t> windowMaxLoadPpm{0};   // the worst since the last engine.cpu (it takes and clears it)
  // M10: callbacks per load band of 1 % (the last band: 200 % and more), for p50/p95/p99 over long runs.
  static constexpr int kLoadBands = 201;
  std::atomic<uint64_t> loadBands[kLoadBands] = {};
  std::atomic<uint64_t> nonFiniteBlocks{0};    // output buffers that held NaN or Inf (should stay 0)
  // How much faster the device's clock runs than the computer's, in parts per million (after a minute of
  // playing). Tens of ppm are normal (two crystals); the starvation count follows it without counting it.
  std::atomic<double> deviceDriftPpm{0};
  // M5: calls where a live worker missed the deadline (that block was silence; then serial), and the
  // silent blocks while that worker finished.
  std::atomic<uint64_t> liveStalls{0}, liveStallBlocks{0};
  double loadPercentile(double p) const;       // % of the buffer's time, from loadBands (upper edge of the band)
  void reset();
};

class Engine {
public:
  static constexpr int kMaxTracks = 128;
  // M5: liveThreads workers play the tracks with the audio thread (0 = serial, the default until measured).
  static constexpr int kMaxLiveFrames = 4096;  // the device layer's largest call (device.cpp)
  explicit Engine(double sampleRate, int liveThreads = 0)
      : sampleRate_(sampleRate), loudness_(sampleRate), analyzer_(sampleRate), fxWatch_(sampleRate), guard_(sampleRate),
        pool_(liveThreads > 0 ? std::make_unique<RtWorkerPool>(liveThreads) : nullptr) {}
  ~Engine();

  // ----- control thread
  uint64_t publish(std::unique_ptr<Graph> graph);  // returns the graph's serial
  void collectGarbage();  // deletes graphs the audio thread has retired
  // Playing from stopped starts a new loudness measurement (the integrated value covers this play).
  void play() {
    if (!playing_.exchange(true)) loudness_.requestReset();
  }
  // The master's loudness (BS.1770): momentary, short-term, integrated since play, true peak.
  LoudnessReading loudness() const { return loudness_.read(); }
  void resetLoudness() { loudness_.requestReset(); }
  // The master's spectrum and stereo correlation (the References page).
  const MasterAnalyzer& analyzer() const { return analyzer_; }
  // One effect's input spectrum, for its window (Core FX): owner is the track's id or "master"; "" stops.
  void watchEffect(const std::string& owner, const std::string& effectId) {
    fxWatch_.key.store(effectId.empty() ? 0 : Graph::fxWatchKey(owner, effectId), std::memory_order_relaxed);
  }
  bool watchingEffect() const { return fxWatch_.key.load(std::memory_order_relaxed) != 0; }
  const MasterAnalyzer& effectAnalyzer() const { return fxWatch_.analyzer; }
  // silenceTails: what still rings (delay and reverb tails) is silenced too. The output fades out over
  // kStopFadeMs and stays silent until unmuteAt()'s graph plays; meanwhile the caller resets the plugins
  // and publishes a graph that keeps no tail (Graph::keepTails = false). Without it the tails ring out.
  void stop(bool rewind, bool silenceTails = false);
  static constexpr double kStopFadeMs = 10;
  // True once the fade of the last stop() has reached silence.
  bool stopFaded() const { return stopFaded_.load(std::memory_order_acquire); }
  // The output plays again once the graph with this serial (or a later one) is playing.
  void unmuteAt(uint64_t serial) { unmuteSerial_.store(serial, std::memory_order_release); }
  void seek(int64_t sample) { seekTo_.store(sample); }
  void setLoop(bool enabled, int64_t start, int64_t end);
  int64_t loopStart() const { return loopStart_.load(); }  // samples (tests, diagnostics)
  int64_t loopEnd() const { return loopEnd_.load(); }
  // Queues a live note for the next block. false = refused (note-on with the queue full, or an id too
  // long); a note-off is never refused: if it does not fit, every note is released instead.
  bool liveNote(const std::string& trackId, int pitch, double velocity, bool on, int64_t offAfterSamples = 0);
  // Releases every note (sequenced, live and timed auditions) at the next block and drops queued live notes.
  void panic() { panic_.store(true, std::memory_order_release); }
  bool playing() const { return playing_.load(); }
  // Revision of the graph the audio thread is playing (-1 until one is taken).
  int64_t appliedRevision() const { return appliedRevision_.load(std::memory_order_acquire); }
  // Serial of that graph (0 = none yet): the meters since the last takePeaks() all come from it.
  uint64_t appliedSerial() const { return appliedSerial_.load(std::memory_order_acquire); }
  // Position in ticks of the tempo the audio thread is playing.
  double positionTicks() const { return positionTicks_.load(std::memory_order_relaxed); }
  int64_t position() const { return position_.load(); }
  bool takeEnded() { return ended_.exchange(false); }
  // Records what the device plays (while playing) into memory, plus where each project update landed.
  // Control thread only; the audio thread just copies into the preallocated buffers. Both calls wait
  // for a copy already under way, so the buffers are never resized or read while the audio thread writes.
  void startCapture(int64_t frames);
  void stopCapture();
  int64_t captured() const { return capWritten_.load(); }
  const std::vector<float>& captureL() const { return capL_; }
  const std::vector<float>& captureR() const { return capR_; }
  std::vector<int64_t> captureSwaps() const;
  // Peaks since the last call (and resets them). master = 2 floats, tracks = 2 per track.
  void takePeaks(float master[2], float* tracks, int trackCount);
  // The master chain's deepest gain reduction (dB) since the last call, for the meter.
  float takeMasterReduction() { return masterReduction_.exchange(0); }
  // Ear guard (dsp/output_guard.hpp), on by default: the last stage of what the device plays.
  void setEarGuard(bool on) { guard_.setEnabled(on); }
  bool earGuard() const { return guard_.enabled(); }
  float takeEarGuardReduction() { return guard_.takeReductionDb(); }
  // Listening level (dB, -60..0): what the device plays is this much quieter; exports are not touched.
  void setListenDb(double db) { guard_.setListenDb(db); }
  double listenDb() const { return guard_.listenDb(); }
  double sampleRate() const { return sampleRate_; }
  EngineStats& stats() { return stats_; }
  int liveThreads() const { return pool_ ? pool_->threads() : 0; }
  // A live worker was late once: the engine plays serial from then on (until a new Engine).
  bool liveFallback() const { return liveFallback_.load(std::memory_order_relaxed); }
  RtWorkerPool* livePool() { return pool_.get(); }  // tests
  // How long the callback waits for the live workers, in buffers' time (2 by default). Tests on slow
  // builds (sanitizers) raise it so that only the late-worker test meets the deadline.
  void setLiveDeadline(double buffers) { liveDeadline_ = buffers; }

  // ----- audio thread
  void process(float* L, float* R, int frames);
  // Called by the device layer around process() to record timing.
  void recordCallback(uint64_t renderNs, uint64_t budgetNs, uint64_t gapNs, uint64_t frames = 0);
  // The starvation check of recordCallback, at a given time (now: when the callback started; tests).
  void noteDelivery(uint64_t nowNs, uint64_t budgetNs);
  // Device buffer length (all periods): how far the device may run ahead of us before it starves.
  void setDeviceBuffer(uint64_t ns) { deviceBufferNs_ = ns; firstStartNs_ = 0; deliveredNs_ = 0; starving_ = false; minLate_[0] = minLate_[1] = 0; windowNs_ = 0; }

private:
  // Track ids are copied into fixed buffers (no allocation in the audio thread); project validation
  // keeps them at 40 characters at most.
  struct LiveNote { char track[48]; int pitch; float velocity; bool on; int64_t offAfter; };
  struct PendingOff { char track[48]; int pitch; int64_t remaining; bool used; };
  static_assert(sizeof(LiveNote::track) > kMaxTrackIdLength && sizeof(PendingOff::track) == sizeof(LiveNote::track));

  double sampleRate_;
  Graph* current_ = nullptr;                // owned by the audio thread
  std::atomic<Graph*> pending_{nullptr};    // control -> audio
  SpscQueue<Graph*, 256> retired_;          // audio -> control (drained on every publish and state tick)
  SpscQueue<LiveNote, 256> liveNotes_;
  PendingOff offs_[32] = {};
  std::atomic<bool> playing_{false}, ended_{false}, stopRequest_{false}, rewindRequest_{false};
  std::atomic<bool> panic_{false};
  bool wasPlaying_ = false;  // audio thread: playback started in this block (notes held there are picked up)
  // Stop's fade and silence (see stop()). The gain and its state belong to the audio thread.
  std::atomic<bool> silenceRequest_{false}, stopFaded_{false};
  std::atomic<uint64_t> unmuteSerial_{0};
  float stopGain_ = 1.0f;
  bool stopFading_ = false, stopMuted_ = false;
  // Live note-offs that did not fit in the queue. By pitch, the tracks they were for (Graph::trackHash of
  // the id, 0 = empty): only that pitch on that track is released, so the same pitch on another track
  // plays on (engine tests 2026-11-04: B dropped 22 dB when A's queue filled). Written by the one control
  // thread, taken (exchanged to 0) by the audio thread. With every slot of a pitch taken by other tracks,
  // the pitch goes in overflowOffs_ and is released on every track (counted in liveOffAllTracks).
  static constexpr int kOverflowTracks = 8;
  std::atomic<uint64_t> overflowTracks_[128][kOverflowTracks] = {};
  std::atomic<bool> overflowAny_{false};
  std::atomic<uint64_t> overflowOffs_[2] = {0, 0};
  std::atomic<int64_t> seekTo_{-1}, position_{0}, appliedRevision_{-1};
  std::atomic<uint64_t> appliedSerial_{0};
  std::atomic<double> positionTicks_{0};
  std::atomic<bool> loopEnabled_{false};
  std::atomic<int64_t> loopStart_{0}, loopEnd_{0};
  // The loop was set (in samples of the new tempo) after a new graph was published and before the audio thread took
  // it: that graph's tempo change must not move it again (sweep: the loop was converted twice).
  std::atomic<bool> loopForPending_{false};
  std::atomic<float> masterPeak_[2] = {};
  std::atomic<float> masterReduction_{0};
  LoudnessMeter loudness_;  // measured by the audio thread on the master output
  MasterAnalyzer analyzer_;  // the same
  FxWatch fxWatch_;
  OutputGuard guard_;  // after the meters and the capture: they read the song as it is
  std::atomic<float> trackPeak_[kMaxTracks * 2] = {};
  float scratchPeaks_[kMaxTracks * 2] = {};
  uint64_t firstStartNs_ = 0, deliveredNs_ = 0;
  bool starving_ = false;
  uint64_t deviceBufferNs_ = 0;
  // The lowest lateness (time consumed - time delivered) in this window of 10 s and the one before: the
  // baseline a starvation is measured from, so a slow clock drift is followed and never counted.
  int64_t minLate_[2] = {0, 0};
  uint64_t windowNs_ = 0;
  std::vector<float> capL_, capR_;
  std::atomic<bool> capturing_{false};
  std::atomic<bool> capBusy_{false};  // audio thread: inside the capture copy (see stopCapture)
  std::atomic<int64_t> capWritten_{0};
  int64_t swapLog_[512] = {};
  std::atomic<int> swapCount_{0};
  EngineStats stats_;
  std::unique_ptr<RtWorkerPool> pool_;  // M5
  double liveDeadline_ = 2.0;
  bool stalled_ = false;                // audio thread: a worker still holds a track of current_
  std::atomic<bool> liveFallback_{false};
};

uint64_t nowNs();

} // namespace core
