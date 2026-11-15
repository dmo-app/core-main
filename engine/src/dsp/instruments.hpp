// Built-in instruments: kick, W-Core (bass, leads, pads), hats, snare and the sampler.
// Real-time safe: no allocation, no locks, no exceptions inside render/noteOn/noteOff.
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace core {

struct AudioData;
namespace wmod { struct Patch; }

constexpr int kMaxParams = 464;  // W-Core: 106 per layer, 4 layers (+3 on switches) = 427, + 9 section lights per layer = 463

class Instrument {
public:
  explicit Instrument(double sampleRate) : sampleRate_(sampleRate) {}
  virtual ~Instrument() = default;
  // Parameters in the order of instrumentParams(name).
  double params[kMaxParams] = {};
  virtual void noteOn(int pitch, double velocity) = 0;
  virtual void noteOff(int pitch) = 0;
  virtual void allNotesOff() = 0;
  // Adds mono samples to out[0..frames).
  virtual void render(float* out, int frames) = 0;
  // Takes over the sounding state of the previous instance of the same track,
  // so editing the project while playing never cuts a note.
  virtual void adopt(const Instrument& previous) = 0;
  virtual const char* name() const = 0;
  // A stereo instrument (W-Core, the sampler) renders once per block with its notes at their samples, instead of
  // render() between notes: queueNote, then renderStereo (which adds to left/right).
  virtual bool stereo() const { return false; }
  virtual void queueNote(int /*offset*/, bool /*on*/, int /*pitch*/, double /*velocity*/) {}
  virtual void renderStereo(float* /*left*/, float* /*right*/, int /*frames*/) {}
  // The sound a granular layer reads from (W-Core Grain): the track's sample, already at the engine
  // rate. Set on the control thread before the instrument goes live; null means "use your own wave".
  virtual void setSource(std::shared_ptr<const AudioData> /*source*/) {}
  // W-Core layers: the sound one layer's Grain reads (0 = the first layer, the same as setSource).
  virtual void setLayerSource(int layer, std::shared_ptr<const AudioData> source) { if (layer == 0) setSource(std::move(source)); }
  // W-Core's modulation (the track's "synth": LFO shapes and links, wave_mod.hpp). Control thread, before it goes live.
  virtual void setWavePatch(const wmod::Patch& /*patch*/) {}
  // The song position (beats) and tempo at the first sample of the next render: the graph calls it before
  // every sub-block (beat < 0 while stopped). An instrument with a slow movement (W-Core Drift) follows it
  // (see SongClock), so every pass of a loop sounds the same.
  virtual void setPosition(double /*beat*/, double /*bpm*/) {}
  // A note caught already sounding (a seek or a loop jump into it) that started `age` samples ago. The
  // built-in instruments pick it up where it would be (envelope, sample position) with a 3 ms fade-in;
  // by default the note starts now.
  virtual void chaseNote(int pitch, double velocity, double /*age*/) {
    if (stereo()) queueNote(0, true, pitch, velocity);
    else noteOn(pitch, velocity);
  }
  // Notes this instrument had to drop since the last call (a queue full in one block). The graph adds
  // them to the engine's counters, so a drop is never silent.
  uint64_t takeDroppedNotes() { const uint64_t n = droppedNotes_; droppedNotes_ = 0; return n; }
  // A native instrument that produced non-finite or runaway audio is marked failed and silenced from then on
  // (its track goes silent, the rest of the song plays on). Written on the
  // audio thread, read by the control thread for reporting, so it is atomic. True the first time only.
  bool markFailed() { int none = 0; return failure_.compare_exchange_strong(none, 1, std::memory_order_acq_rel); }
  bool failed() const { return failure_.load(std::memory_order_acquire) != 0; }
protected:
  double sampleRate_;
  uint64_t droppedNotes_ = 0;
  std::atomic<int> failure_{0};
};

std::unique_ptr<Instrument> makeInstrument(const std::string& name, double sampleRate);

} // namespace core
