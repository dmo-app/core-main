// RenderGraph: an immutable, pre-computed view of a Project that the audio thread plays.
// Built on the control thread (allocates), played on the audio thread (never allocates).
// The same Graph::process is used for real time and for export, so both are identical.
#pragma once
#include <bitset>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include "assets.hpp"
#include "plugins.hpp"
#include "rt_workers.hpp"
#include "workers.hpp"
#include "project.hpp"
#include "../dsp/analyzer.hpp"
#include "../dsp/effects.hpp"
#include "../dsp/instruments.hpp"
#include "../dsp/clip_grains.hpp"

namespace core {

// Note-ons a plugin instrument could not play because one block held more events than its queue
// (note-offs are never dropped). Grows for the life of the process; engine.stats reports it.
uint64_t pluginNotesDropped();
// Plugins that threw in process() (M3b), for the life of the process. Each is then silent; see
// PluginProcessor::failed and Graph::plugins.
uint64_t pluginFailures();
// Built-in ("core") instruments and effects that produced non-finite or runaway audio and were silenced, for
// the life of the process: the same protection plugins get. See Effect::failed / Instrument::failed.
uint64_t nativeFailures();
// The built-in units that failed and went silent, as "track: unit" ("master: limiter"), since the last
// clearFailedUnits() (the server clears it when a song is loaded: the new graph has fresh units that play
// again). Written on the audio thread without allocating, read by the control thread for the app to show.
std::vector<std::string> failedUnits();
void clearFailedUnits();

// M0b: where the audio thread's time goes, per track (its instrument and audio clips) and per effect,
// built-in or plugin, and on the master chain. The audio thread adds nanoseconds (relaxed atomics); the
// control thread reads them. Shared with the server, so it outlives the graph that wrote it.
struct CpuMeter {
  struct Entry {
    std::string track;   // "" = the master chain
    std::string effect;  // "" = the track's instrument (and its audio clips)
    std::string name;    // the instrument or effect ("elastic", "reverb", a plugin's name)
  };
  std::vector<Entry> entries;
  std::unique_ptr<std::atomic<uint64_t>[]> ns;  // one per entry
  void add(int slot, uint64_t elapsed) { ns[slot].fetch_add(elapsed, std::memory_order_relaxed); }
  uint64_t read(size_t slot) const { return ns[slot].load(std::memory_order_relaxed); }
};

struct SeqEvent {
  int64_t tick;   // relative to the pattern start (may exceed patternTicks for note-offs)
  bool on;
  int pitch;
  float velocity;
  int64_t onTick;  // the tick of this event's note-on (for an off: the note it ends)
  float noteVelocity = 0;  // for an off: its note's velocity (a note picked up mid-way, see Graph::chase)
};

struct Automation {
  int slot;       // instrument param index, or -1 = gainDb, -2 = pan, -3 = an effect's parameter, -4 = a plugin's
  int effect = -1, effectParam = -1;  // slot -3: which effect of the chain, which of its parameters
  PluginProcessor* plugin = nullptr;  // slot -4: the plugin (owned by the track's instrument or effect) and
  int pluginParam = -1;               //   its parameter index; values are 0..1
  float sent = -1;                    // slot -4: the value last given to the plugin (-1: none yet)
  double min, max;
  std::vector<EnvelopePoint> points;
  double smooth = 0;                  // slot -3: the value as heard (glides ~15 ms, no zipper)
  bool smoothInit = false;
  int64_t block = INT64_MIN;
  double valueAt(double tick) const;
};

// Optional log of where events really landed (measurements only, not real time).
struct EventLog {
  struct Entry { int track; int64_t sample; int64_t tick; int pitch; };
  std::vector<Entry> entries;
};

// The effect whose input a plugin window shows (Core FX's spectrum): the engine owns it, every graph it plays
// feeds it, at most one effect at a time (key 0: none). Audio thread writes, control thread reads the spectrum.
struct FxWatch {
  explicit FxWatch(double sampleRate) : analyzer(sampleRate) {}
  std::atomic<uint64_t> key{0};
  MasterAnalyzer analyzer;
};

class Graph {
public:
  // The key of an effect for FxWatch: its track's id ("master" for the master chain) and its own id.
  static uint64_t fxWatchKey(const std::string& owner, const std::string& effectId);
  void setWatch(FxWatch* watch) { watch_ = watch; }  // control thread, before the graph is published
  static constexpr int kSubBlock = 32;
  uint64_t publishedNs = 0;  // set by Engine::publish (measures edit -> audio delay)
  int64_t revision = -1;     // project revision this graph was built from (-1 = none)
  uint64_t serial = 0;       // unique per published graph in this process (set by Engine::publish)
  // false: adopt() takes no sound from the previous graph (no delay or reverb tail, no ringing voice).
  // Stop publishes such a graph, so nothing rings on after it (or comes back at the next Play).
  bool keepTails = true;

  // assets: where audio clips get their (converted) audio; without it audio clips are silent.
  // plugins: where plugin tracks and plugin effects come from (without it they are silent); offline: an
  // export (its own plugin instances) rather than real time (the live ones).
  Graph(const Project& project, double sampleRate, AssetStore* assets = nullptr, PluginProvider* plugins = nullptr, bool offline = false);
  // Plugins that could not be loaded: {key, plugin name, why}. Their tracks / effects are silent.
  const std::vector<json>& missingPlugins() const { return missingPlugins_; }
  const std::vector<std::string>& pluginKeys() const { return pluginKeys_; }  // the plugins this graph plays
  // ref: the song's description of it ({format, id, name, vendor, version, ...}, without its state).
  struct LoadedPlugin { std::string key, name; std::shared_ptr<PluginProcessor> processor; json ref; };
  const std::vector<LoadedPlugin>& plugins() const { return plugins_; }  // the same ones, with their instances
  // Real time only (null for an export): the time each track and effect took, see CpuMeter.
  const std::shared_ptr<CpuMeter>& cpu() const { return cpu_; }
  // Audio files that could not be loaded, with the reason ("path: why"): those clips are silent.
  const std::vector<std::string>& missingAssets() const { return missingAssets_; }
  // Renders `frames` samples starting at song sample `position` (adds into L/R).
  // `looping`: the song wraps at its end (real time); otherwise it plays once (export).
  // `trackPeaks` (optional): 2 floats per track, max |sample| L/R is written into it.
  // Returns false only with live workers (M5) that missed the deadline: L/R got nothing from this call, and
  // the graph must not be used again (process, notes, adopt, delete) until the pool is idle().
  bool process(float* L, float* R, int frames, int64_t position, bool looping, EventLog* log = nullptr, float* trackPeaks = nullptr,
               uint64_t deadlineNs = UINT64_MAX);
  void allNotesOff();
  // After a seek or a loop wrap (and allNotesOff): notes that started before `position` and still sound
  // there start again now, so a pad or a bass held across the jump keeps playing (audit 2.5). Notes
  // that start at `position` or later are played by process() as usual. Audio thread, no allocation.
  void chase(int64_t position);
  // After adopt() while playing, `position` and `previousPosition` being the same moment in each graph's
  // samples: a track whose notes the edit changed releases the adopted notes that are no longer there (a
  // pitch moved, a note removed or shortened) and picks up mid-way the ones the edit put under the play
  // head. Without it an adopted voice kept the old pitch: a lead edited while playing stayed on its high
  // note until stop (2026-11-10). Unchanged tracks are untouched. Audio thread, no allocation.
  void reconcile(const Graph& previous, int64_t previousPosition, int64_t position);
  // For tests: an instrument parameter as the instrument hears it now (after its glide).
  double heardParam(int track, int param) const { return tracks_[(size_t)track].smooth[param]; }
  void pitchOff(int pitch);  // this pitch released on every track (last resort, see Engine::liveNote)
  // This pitch released on the track whose id hashes to trackHash (a live note-off that did not fit in
  // the queue): the same pitch on other tracks plays on. Audio thread, no allocation.
  void pitchOff(uint64_t trackHash, int pitch);
  // FNV-1a of a track id, never 0 (0 marks an empty slot). Stops at the first '\0' (fixed id buffers).
  static uint64_t trackHash(std::string_view id) {
    uint64_t h = 1469598103934665603ull;
    for (char c : id) { if (c == '\0') break; h = (h ^ (unsigned char)c) * 1099511628211ull; }
    return h ? h : 1;
  }
  // Live notes from the UI (keyboard, audition). Real-time safe: the id is only compared, never copied.
  void liveNote(std::string_view trackId, int pitch, double velocity, bool on);
  // Blocks that held more events than one batch (they took extra passes, nothing was dropped).
  // Audio thread only; the engine reads and clears it after each process().
  uint64_t takeExtraEventPasses() { return extraEventPasses_.exchange(0, std::memory_order_relaxed); }
  uint64_t takeDroppedNotes() { return droppedNotes_.exchange(0, std::memory_order_relaxed); }  // instruments' full queues
  // M4a, export only: process() plays the tracks that are not buses on these workers (calls of at least
  // kParallelMinFrames, without looping, log or peaks); the result is the serial one, bit for bit.
  // Null: serial. The pool must outlive the graph's use of it.
  void useWorkers(WorkerPool* workers);
  static constexpr int kParallelMinFrames = 256;
  // M5, real time: process() plays the tracks that are not buses (nor sidechain-keyed) on these workers,
  // in calls of up to maxFrames, with the same sums as serial, bit for bit. Control thread, before the
  // graph is published (it allocates the per-track buffers). Null: serial.
  void useLiveWorkers(RtWorkerPool* pool, int maxFrames);
  void stopLiveWorkers() { live_ = nullptr; }  // audio thread: serial from the next call
  bool liveWorkers() const { return live_ != nullptr; }
  // Moves the sounding state of matching tracks from the previous graph (real time edits).
  void adopt(Graph& previous);
  // Plugin latency (lookahead): every track comes out this many samples after the song position, the
  // tracks with less latency delayed to line up with the latest one.
  int latency() const { return latency_; }
  int masterLatency() const { return masterLatency_; }
  // The master chain's deepest gain reduction since the last call (dB, its compressors and limiters added
  // up); audio thread.
  float takeMasterReduction() { const float r = masterReduction_; masterReduction_ = 0; return r; }  // master effects (limiter 2, saturation 2, plugins)
  // The plugins whose latency the alignment used, with that latency (the server rebuilds when one changes).
  const std::vector<std::pair<std::shared_ptr<PluginProcessor>, int>>& pluginLatencies() const { return latencies_; }
  // Export: renders the latency and drops it, so the song starts on the file's first sample (call once, first).
  void preroll();

  // Native DSP protection (default on): a built-in instrument or effect whose output is non-finite or
  // runaway is silenced and marked failed, so one bad unit cannot corrupt the whole mix. The check scans the
  // block once after process(); good audio is never touched, so the output stays bit-exact. Turn off only to
  // measure its cost (Phase 5).
  void setNativeProtection(bool on) { protectNative_ = on; }
  bool nativeProtection() const { return protectNative_; }
  // Tests: mark a track's native instrument failed, to exercise the isolation end to end. A real failure is
  // set by the guard in runTrack when a unit's output is non-finite or runaway.
  void failInstrumentForTest(int track) {
    if (track >= 0 && track < (int)tracks_.size() && tracks_[(size_t)track].instrument) tracks_[(size_t)track].instrument->markFailed();
  }

  // Per-track/per-effect CPU metering (M0b) reads the clock twice per track and once per effect on every
  // sub-block: measured at ~4-5 % of the audio thread's DSP time (though <0.05 % of the real-time budget).
  // Default on, so engine.cpu is unchanged; turn it off for a maximum-headroom "performance mode".
  void setCpuMetering(bool on) { cpuMetering_ = on; }
  bool cpuMetering() const { return cpuMetering_; }

  int64_t songSamples() const { return songSamples_; }
  int64_t sampleOfTick(int64_t tick) const { return tempo_.sampleOfTick(tick); }
  double sampleRate() const { return sampleRate_; }
  double bpm() const { return bpm_; }  // at the song's start
  const TempoMap& tempo() const { return tempo_; }
  // -1 = all. Stems: one track, before the master chain (stems add up to the mix only when the master
  // chain is empty or linear).
  // With buses (M7a), a track's stem goes through its buses and sends; a bus's stem is the bus with every
  // track that feeds it. withoutMasterGain: the stem leaves out the master gain too (a bounce: the audio goes back
  // through the master gain when it plays, so it must not have it twice).
  void setSolo(int track, bool withoutMasterGain = false);
  int trackCount() const { return (int)tracks_.size(); }
  const std::string& trackId(int index) const { return tracks_[index].id; }

private:
  FxWatch* watch_ = nullptr;
  void feedWatch(const Effect& e, const float* l, const float* r, int n) const {
    if (watch_ && e.watchKey != 0 && e.watchKey == watch_->key.load(std::memory_order_relaxed)) watch_->analyzer.process(l, r, n);
  }
  // One pattern placed on the song (a track without clips gets one from the song start).
  struct ClipRt {
    int64_t start, end;            // song ticks; only notes that start in [start, end) play
    int64_t offset;                // where in the pattern the clip starts (0 = its beginning)
    int64_t patternTicks, maxEventTick;
    std::vector<SeqEvent> events;  // sorted by tick, offs first; ticks relative to each repeat
  };
  // An audio clip in samples at the graph's rate; the buffer is shared and never changes.
  struct AudioClipRt {
    int64_t start, offset, length, fadeIn, fadeOut;
    float gain;
    std::shared_ptr<const AudioData> data;
    ClipGrains grains;  // pitch or freeze (grain 0: the plain copy)
  };
  struct TrackRt {
    std::string id;
    uint64_t idHash = 0;  // trackHash(id)
    std::unique_ptr<Instrument> instrument;
    std::vector<ClipRt> clips;
    std::vector<AudioClipRt> audio;
    std::vector<Automation> automation;
    std::vector<std::unique_ptr<Effect>> effects;  // the insert chain (bypassed ones too: their tails stay)
    bool anyEffect = false;                        // at least one not bypassed
    double baseParams[kMaxParams];
    double paramMin[kMaxParams], paramMax[kMaxParams];
    int paramCount;
    double gainDb, pan;
    bool mute, solo;
    double fromL = 0, fromR = 0, toL = 0, toR = 0;  // gain ramp over the current aligned sub-block
    double postFromL = 0, postFromR = 0, postToL = 0, postToR = 0;  // the same without the master gain (post sends)
    int64_t rampBlock = INT64_MIN;
    std::bitset<kMaxParams> stepped;  // switches and choices (a filter type, a wavetable, on/off): they jump, never glide
    double smooth[kMaxParams];       // parameters as heard: glide ~15 ms to their target (no zipper/clicks)
    int64_t paramBlock = INT64_MIN;
    bool smoothInit = false;
    bool gainInit = false;
    std::shared_ptr<PluginProcessor> plugin;  // a plugin track's instrument (its parameters can be automated)
    int latency = 0;                          // of its plugins (instrument, inserts that play)
    std::vector<float> delayL, delayR;        // latency_ - latency samples: lines it up with the latest track
    size_t delayAt = 0;
    int cpuSlot = -1;  // CpuMeter: its instrument; its effects follow, in chain order
    // M7a routing. A bus gathers its inputs in inL/inR during the sub-block (tracks before the buses
    // they feed: see order_), then plays them like an instrument.
    bool bus = false;
    float inL[kSubBlock] = {}, inR[kSubBlock] = {};
    int out = -1;            // index of the bus its fader feeds, -1 = the master
    struct SendRt {
      int to;                // the bus
      double gain;           // linear
      bool pre;
      double from = 0, now = 0;  // gain ramp over the aligned sub-block (as the fader's)
      int64_t rampBlock = INT64_MIN;
      bool init = false;
      std::vector<float> delayL, delayR;  // lines it up at the bus with its other inputs
      size_t delayAt = 0;
      std::string id;
      std::vector<float> keptL, keptR;    // M4a: what it gave over the call (export workers)
    };
    std::vector<SendRt> sends;
    bool soloOk = false;     // audible while some track is soloed (soloed, fed by one, or feeding a soloed bus)
    // M8a: LFOs added to parameters (after automation), from the song position.
    struct ModRt {
      int slot;              // instrument param index, -1 = gainDb, -2 = pan, -3 = an insert effect's parameter
      int shape;             // index in modulatorShapes()
      double cycleTicks, depth, phase, min, max;
      bool octaves;          // a parameter in Hz: depth in octaves
      uint32_t seed;         // random: per modulator
      double attack = 0, decay = 1;  // envelope (M8b): in samples
      double curve = 1;              // rise (FCore): (t)^curve over cycleTicks
      bool hold = false;             // rise: stays at 1 after its end (else back to 0: the drop)
      std::array<float, kMaxModSteps> steps{};  // steps: the values (no allocation on the audio thread)
      int stepCount = 0;
      int effect = -1, effectParam = -1;  // slot -3: which effect of the chain and which of its parameters
      int64_t fxBlock = INT64_MIN;   // slot -3: the aligned sub-block its value was worked out for
      double fxValue = 0;            // and that value (a split sub-block writes the same one again)
      // Started by another track's notes (designed ducking): their note-on song samples, sorted, worked out
      // when the graph is built (the same in real time and export, whatever order the tracks play in).
      std::string source;
      std::vector<int64_t> sourceOns;
      int64_t srcBlock = INT64_MIN;  // the aligned sub-block srcAge / srcTicks are for
      double srcAge = -1, srcTicks = -1;
    };
    std::vector<ModRt> mods;
    double effectBpm = 0;                // the BPM its tempo-synced effects were last given
    int64_t lastOnSample = INT64_MIN;    // M8b: the song sample of its last note-on (envelope modulators)
    bool anyEnvelope = false;
    double envAge = -1;                  // samples since that note-on at the current aligned sub-block (-1: none)
    double envTicks = -1;                // the same in ticks (rise modulators: their length is in beats)
    int64_t envBlock = INT64_MIN;
    std::vector<float> keptL, keptR;     // M4a: what it gave over the call (export workers)
    // M7c: a track that keys a sidechain keeps its signal after its inserts (before the fader) for the
    // sub-block, or over the whole call when an export worker played it (keyKept).
    bool keySource = false;
    bool keyKept = false;
    bool keyed = false;                  // one of its effects takes a sidechain (it plays after its sources)
    float keyL[kSubBlock] = {}, keyR[kSubBlock] = {};
    std::vector<float> keyKeptL, keyKeptR;
    // A keyed effect whose input is later than its key (latency before it on this track, or before its bus): the key
    // waits that long, so the detector hears the key with the program it ducks (audit 2026-11-07 #9).
    struct KeyDelayRt {
      int effect = -1;
      std::vector<float> l, r;  // the wait, in samples
      size_t at = 0;
      float outL[kSubBlock] = {}, outR[kSubBlock] = {};
    };
    std::vector<KeyDelayRt> keyDelays;
    // The other way round: a key later than the program (latency on the key's track, a look-ahead limiter...): the
    // program waits before that effect, so the detector and the audio it ducks arrive together (audit of main
    // fa3b428, F04). The track's latency counts it.
    std::vector<KeyDelayRt> programDelays;
  };
  // One sub-block of a process() call.
  struct Step {
    int64_t pos = 0, aligned = 0;  // song sample, and the aligned sub-block start it belongs to
    int n = 0, done = 0;           // its length, and where it starts in the caller's buffers
    double tick = 0, bpm = 0, masterGain = 1;
    double beat = 0;  // the song position at pos, in beats (effects on the grid: GLITCH, PULSE)
  };
  struct DirectSink;
  struct KeepSink;
  Step step(int64_t position, int done, int frames, bool looping);
  double masterGainAt(double tick) const;
  void masterStart(const Step& st);
  void masterEnd(const Step& st, float* mixL, float* mixR, float* L, float* R);
  template <class Sink> void runTrack(int i, const Step& st, float* buf, float* bufR, EventLog* log, Sink& sink);
  void processParallel(float* L, float* R, int frames, int64_t position);
  bool processLive(float* L, float* R, int frames, int64_t position, float* trackPeaks, uint64_t deadlineNs);
  static void liveJob(void* graph, int k);
  static double lfoValue(const TrackRt::ModRt& m, double tick);  // M8a, -1..1
  static constexpr int kEnvelopeShape = 6;                         // M8b: "envelope" in modulatorShapes()
  static constexpr int kRiseShape = 7;                             // FCore: "rise" in modulatorShapes()
  static constexpr int kStepsShape = 8;                            // "steps" in modulatorShapes()
  // FCore: 0..1, `ticks` after the note-on (-1: none): (t)^curve over the length, then held at 1 or back to 0
  // over 1/64 of a beat (the drop, without a click).
  static double riseValue(const TrackRt::ModRt& m, double ticks);
  // 0..1 or -1..1: what a modulator gives at this sub-block (an LFO from the tick, the others from the note).
  static double modValue(const TrackRt::ModRt& m, double tick, double age, double ageTicks);
  // M8b: 0..1, `age` samples after the note-on (linear rise over attack, then -60 dB over decay).
  static double envelopeValue(const TrackRt::ModRt& m, double age);
  // Calls mark for the track, every bus it reaches and every track that reaches it (solo, stems).
  void markSolo(int track, const std::function<void(int)>& mark);
  void renderTrack(int index, TrackRt& t, float* buf, float* bufR, int frames, int64_t songPos, EventLog* log);
  static void renderAudio(const TrackRt& t, float* left, float* right, int frames, int64_t songPos);

  std::vector<TrackRt> tracks_;
  std::vector<int> order_;      // processing order: tracks before the buses they feed (no buses: as listed)
  std::vector<char> stemOk_;    // setSolo: the tracks the stem plays
  bool stemWithoutMasterGain_ = false;
  std::vector<std::unique_ptr<Effect>> masterEffects_;
  float masterReduction_ = 0;
  bool anyMasterEffect_ = false;
  bool protectNative_ = true;  // see setNativeProtection
  bool cpuMetering_ = true;    // see setCpuMetering
  std::vector<Automation> masterAutomation_;  // master gain (slot -1) and master effects (slot -3)
  // An effect parameter's automation for "fx:<id>:<param>" in this chain; false if there is no such effect.
  static bool resolveEffectParam(const std::string& param, const std::vector<std::unique_ptr<Effect>>& chain, Automation& a);
  void applyPluginAutomation(Automation& a, double tick, int64_t aligned);
  void applyEffectAutomation(Automation& a, std::vector<std::unique_ptr<Effect>>& chain, double tick, int64_t aligned);
  // The glide state of effect automation, from the previous graph (the same effect id and parameter).
  // Calls f(event) for each note-off event of t whose note is held at `position` (song samples): started
  // before it, its off not yet played (at `position` or later).
  template <class F> void forEachHeld(const TrackRt& t, int64_t position, F&& f) const;
  static void adoptAutomation(std::vector<Automation>& now, const std::vector<std::unique_ptr<Effect>>& chain,
                              const std::vector<Automation>& before, const std::vector<std::unique_ptr<Effect>>& beforeChain);
  double sampleRate_, bpm_, masterGain_, smoothCoef_ = 1;
  TempoMap tempo_;        // ticks <-> samples (one BPM, or the song's tempo changes)
  double effectBpm_ = 0;  // the BPM the tempo-synced effects (delay) were last given
  int64_t lengthTicks_, songSamples_;
  int solo_ = -1;
  bool anySolo_ = false;
  std::atomic<uint64_t> extraEventPasses_{0};  // atomic: export workers count too
  std::atomic<uint64_t> droppedNotes_{0};
  WorkerPool* workers_ = nullptr;
  std::vector<int> parallel_;  // the tracks the workers play (not buses), in order_
  std::vector<Step> steps_;
  RtWorkerPool* live_ = nullptr;  // M5
  int liveMaxFrames_ = 0;
  std::vector<std::string> missingAssets_;
  std::vector<json> missingPlugins_;
  std::vector<std::string> pluginKeys_;
  std::vector<LoadedPlugin> plugins_;
  std::shared_ptr<CpuMeter> cpu_;
  int masterCpuSlot_ = -1;  // CpuMeter: the master chain's first effect
  int latency_ = 0;
  int masterLatency_ = 0;  // master effects, built-in and plugins (export preroll only; real time plays it late)
  int64_t shift_ = 0;  // preroll: how far ahead of the caller's position the song plays
  std::vector<std::pair<std::shared_ptr<PluginProcessor>, int>> latencies_;
};

} // namespace core
