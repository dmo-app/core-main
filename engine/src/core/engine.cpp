#include "engine.hpp"
#include "../dsp/finite.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace core {

uint64_t nowNs() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void atomicMax(std::atomic<uint64_t>& a, uint64_t v) {
  uint64_t cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
static void atomicMaxF(std::atomic<float>& a, float v) {
  float cur = a.load(std::memory_order_relaxed);
  while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

void EngineStats::reset() {
  callbacks = 0; frames = 0; overruns = 0; lateCallbacks = 0;
  maxRenderNs = 0; lastRenderNs = 0; minFrames = UINT64_MAX; maxFrames = 0; totalRenderNs = 0; graphSwaps = 0; maxSwapDelayNs = 0; leakedGraphs = 0;
  liveNotesRejected = 0; liveOffOverflows = 0; liveOffAllTracks = 0; auditionSteals = 0; denseEventBlocks = 0; droppedNotes = 0; callbackErrors = 0;
  totalBudgetNs = 0; maxLoadPpm = 0; windowMaxLoadPpm = 0;
  for (auto& b : loadBands) b = 0;
  nonFiniteBlocks = 0;
  deviceDriftPpm = 0;
  liveStalls = 0; liveStallBlocks = 0;
}

double EngineStats::loadPercentile(double p) const {
  uint64_t total = 0;
  for (const auto& b : loadBands) total += b.load(std::memory_order_relaxed);
  if (!total) return 0;
  const double want = p / 100.0 * (double)total;
  uint64_t seen = 0;
  for (int i = 0; i < kLoadBands; ++i) {
    seen += loadBands[i].load(std::memory_order_relaxed);
    if ((double)seen >= want) return i + 1.0;
  }
  return kLoadBands;
}

Engine::~Engine() {
  // A late worker may still be playing a track of a graph: it finishes before any graph goes.
  if (pool_) while (!pool_->idle()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  collectGarbage();
  delete pending_.exchange(nullptr);
  delete current_;
}

uint64_t Engine::publish(std::unique_ptr<Graph> graph) {
  static std::atomic<uint64_t> serials{0};
  const uint64_t serial = ++serials;  // unique in the process, also across Engine instances
  graph->serial = serial;
  graph->publishedNs = nowNs();
  graph->setWatch(&fxWatch_);
  if (pool_ && !liveFallback_.load(std::memory_order_relaxed)) graph->useLiveWorkers(pool_.get(), kMaxLiveFrames);  // M5
  // If the audio thread has not taken the previous one yet, it never will: delete it here.
  delete pending_.exchange(graph.release());
  collectGarbage();
  return serial;
}

void Engine::collectGarbage() {
  Graph* g;
  while (retired_.pop(g)) delete g;
}

// The audio thread raises capBusy_ before it looks at capturing_ (both seq_cst): once capturing_ is false
// here and capBusy_ is seen low, no copy can still be using the buffers, nor start using them.
void Engine::stopCapture() {
  capturing_.store(false);
  while (capBusy_.load()) std::this_thread::yield();
}

void Engine::startCapture(int64_t frames) {
  stopCapture();
  capL_.assign((size_t)frames, 0.0f);
  capR_.assign((size_t)frames, 0.0f);
  capWritten_.store(0);
  swapCount_.store(0);
  capturing_.store(true, std::memory_order_release);
}

std::vector<int64_t> Engine::captureSwaps() const {
  int n = swapCount_.load(std::memory_order_acquire);
  return std::vector<int64_t>(swapLog_, swapLog_ + n);
}

void Engine::stop(bool rewind, bool silenceTails) {
  if (silenceTails) {
    unmuteSerial_.store(UINT64_MAX, std::memory_order_release);  // silent until the caller says which graph
    stopFaded_.store(false, std::memory_order_release);
    silenceRequest_.store(true, std::memory_order_release);
  }
  playing_.store(false);
  stopRequest_.store(true);
  if (rewind) seekTo_.store(0);
}

void Engine::setLoop(bool enabled, int64_t start, int64_t end) {
  start = std::max<int64_t>(0, start);  // the end is measured from the start as kept (sweep: it used the unclamped one)
  loopStart_.store(start);
  loopEnd_.store(std::max<int64_t>(start + 1, end));
  loopEnabled_.store(enabled && end > start);
  loopForPending_.store(pending_.load(std::memory_order_acquire) != nullptr, std::memory_order_release);
}

bool Engine::liveNote(const std::string& trackId, int pitch, double velocity, bool on, int64_t offAfter) {
  if (pitch < 0 || pitch > 127) return false;  // out of MIDI range: never reaches a voice
  LiveNote n{};
  if (trackId.size() >= sizeof(n.track)) return false;  // never truncated into another track's id
  std::memcpy(n.track, trackId.data(), trackId.size());
  n.pitch = pitch; n.velocity = (float)velocity; n.on = on; n.offAfter = offAfter;
  if (liveNotes_.push(n)) return true;
  // Queue full. A note-on is refused (the caller reports it). A note-off must not be lost, or the note
  // would hang: it is kept by pitch and track, and the audio thread releases that pitch on that track.
  if (on) { stats_.liveNotesRejected.fetch_add(1, std::memory_order_relaxed); return false; }
  stats_.liveOffOverflows.fetch_add(1, std::memory_order_relaxed);
  const uint64_t hash = Graph::trackHash(trackId);
  auto& slots = overflowTracks_[pitch];
  bool kept = false;
  for (auto& slot : slots) if (slot.load(std::memory_order_acquire) == hash) { kept = true; break; }
  for (int i = 0; !kept && i < kOverflowTracks; ++i) {
    uint64_t empty = 0;
    kept = slots[i].compare_exchange_strong(empty, hash, std::memory_order_acq_rel);
  }
  if (!kept) {  // every slot of this pitch holds another track: the pitch is released everywhere
    stats_.liveOffAllTracks.fetch_add(1, std::memory_order_relaxed);
    overflowOffs_[pitch >> 6].fetch_or(uint64_t(1) << (pitch & 63), std::memory_order_acq_rel);
  }
  overflowAny_.store(true, std::memory_order_release);
  return true;
}

void Engine::takePeaks(float master[2], float* tracks, int count) {
  master[0] = masterPeak_[0].exchange(0); master[1] = masterPeak_[1].exchange(0);
  for (int i = 0; i < std::min(count, kMaxTracks) * 2; ++i) tracks[i] = trackPeak_[i].exchange(0);
}

void Engine::recordCallback(uint64_t renderNs, uint64_t budgetNs, uint64_t gapNs, uint64_t frames) {
  if (frames) {
    if (frames < stats_.minFrames.load(std::memory_order_relaxed)) stats_.minFrames.store(frames, std::memory_order_relaxed);
    atomicMax(stats_.maxFrames, frames);
  }
  stats_.callbacks.fetch_add(1, std::memory_order_relaxed);
  stats_.lastRenderNs.store(renderNs, std::memory_order_relaxed);
  stats_.totalRenderNs.fetch_add(renderNs, std::memory_order_relaxed);
  atomicMax(stats_.maxRenderNs, renderNs);
  if (renderNs > budgetNs) stats_.overruns.fetch_add(1, std::memory_order_relaxed);
  if (budgetNs) {
    const uint64_t load = (uint64_t)((double)renderNs * 1e6 / (double)budgetNs);
    stats_.totalBudgetNs.fetch_add(budgetNs, std::memory_order_relaxed);
    atomicMax(stats_.maxLoadPpm, load);
    atomicMax(stats_.windowMaxLoadPpm, load);
    stats_.loadBands[std::min<uint64_t>(load / 10000, EngineStats::kLoadBands - 1)].fetch_add(1, std::memory_order_relaxed);
  }
  noteDelivery(nowNs() - renderNs, budgetNs);
  (void)gapNs;
}

// Devices call in bursts (several small callbacks, then a pause), so single gaps say nothing. Instead: has
// the device consumed more time than we delivered (plus its buffer), compared with the lowest lateness of
// the last 10-20 s? Then it played silence: one starvation event until we catch up again. The baseline
// moves because the device's clock is not the computer's: a few tens of ppm add up to more than the
// buffer within half an hour (soak 2026-11-04: 20951 false starvations in one burst after 22 minutes).
void Engine::noteDelivery(uint64_t now, uint64_t budgetNs) {
  if (!firstStartNs_) firstStartNs_ = now;
  const uint64_t elapsed = now - firstStartNs_;
  const int64_t late = (int64_t)elapsed - (int64_t)deliveredNs_;
  if (elapsed - windowNs_ >= 10000000000ull) { minLate_[1] = minLate_[0]; minLate_[0] = late; windowNs_ = elapsed; }
  minLate_[0] = std::min(minLate_[0], late);
  const int64_t base = std::min(minLate_[0], minLate_[1]);
  const bool starving = late - base > (int64_t)deviceBufferNs_ + 2000000;  // 2 ms tolerance for clock jitter
  if (starving && !starving_) stats_.lateCallbacks.fetch_add(1, std::memory_order_relaxed);
  starving_ = starving;
  if (elapsed > 60000000000ull) stats_.deviceDriftPpm.store((double)base / (double)elapsed * 1e6, std::memory_order_relaxed);
  deliveredNs_ += budgetNs;
}

void Engine::process(float* L, float* R, int frames) {
  std::fill(L, L + frames, 0.0f);
  std::fill(R, R + frames, 0.0f);
  stats_.frames.fetch_add(frames, std::memory_order_relaxed);
  const uint64_t startNs = nowNs();
  // M5: a worker that missed the deadline still holds a track. Until it is done nothing touches the graph
  // (no swap, no notes, no render): silence. Then serial from here on.
  if (stalled_) {
    if (!pool_->idle()) { stats_.liveStallBlocks.fetch_add(1, std::memory_order_relaxed); return; }
    stalled_ = false;
    liveFallback_.store(true, std::memory_order_relaxed);
    if (current_) current_->stopLiveWorkers();
  }

  // 1. New project state: keep the sounding notes, retire the old graph.
  int64_t pos = position_.load(std::memory_order_relaxed);
  if (Graph* next = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
    if (!current_) loopForPending_.store(false, std::memory_order_relaxed);  // the first graph moves nothing
    if (current_) {
      const int64_t before = pos;  // the same moment in the old graph's samples
      // Tempo change: stay on the same musical position (and loop region), keep the notes sounding.
      const TempoMap& from = current_->tempo();
      const TempoMap& to = next->tempo();
      const bool loopAlreadyNew = loopForPending_.exchange(false, std::memory_order_acq_rel);  // set for this tempo: stays
      if (from.constant() && to.constant()) {
        const double ratio = to.startSamplesPerTick() / from.startSamplesPerTick();
        if (ratio != 1.0) {
          pos = (int64_t)std::llround(pos * ratio);
          if (!loopAlreadyNew) {
            loopStart_.store((int64_t)std::llround(loopStart_.load() * ratio));
            loopEnd_.store((int64_t)std::llround(loopEnd_.load() * ratio));
          }
        }
      } else {  // a song with tempo changes: through the musical position
        auto move = [&](int64_t s) { return (int64_t)std::llround(to.sampleAt(from.tickAt((double)s))); };
        pos = move(pos);
        if (!loopAlreadyNew) {
          loopStart_.store(move(loopStart_.load()));
          loopEnd_.store(move(loopEnd_.load()));
        }
      }
      if (liveFallback_.load(std::memory_order_relaxed)) next->stopLiveWorkers();
      next->adopt(*current_);
      // Playing on through the edit: held notes the edit changed end or start where they are now.
      if (playing_.load(std::memory_order_relaxed) && wasPlaying_) next->reconcile(*current_, before, pos);
      if (!retired_.push(current_)) stats_.leakedGraphs.fetch_add(1, std::memory_order_relaxed);
    }
    current_ = next;
    if (liveFallback_.load(std::memory_order_relaxed)) next->stopLiveWorkers();
    // Track meters are indexed by the graph's track order: start them again for the new graph, so a
    // peak is never reported under another graph's track.
    for (auto& p : trackPeak_) p.store(0, std::memory_order_relaxed);
    appliedRevision_.store(next->revision, std::memory_order_release);
    appliedSerial_.store(next->serial, std::memory_order_release);
    stats_.graphSwaps.fetch_add(1, std::memory_order_relaxed);
    atomicMax(stats_.maxSwapDelayNs, nowNs() - next->publishedNs);
    capBusy_.store(true);
    if (capturing_.load()) {
      int n = swapCount_.load(std::memory_order_relaxed);
      if (n < 512) { swapLog_[n] = capWritten_.load(std::memory_order_relaxed); swapCount_.store(n + 1, std::memory_order_release); }
    }
    capBusy_.store(false, std::memory_order_release);
  }
  if (!current_) return;
  Graph& g = *current_;

  // 2. Transport requests.
  if (stopRequest_.exchange(false)) g.allNotesOff();
  if (silenceRequest_.exchange(false, std::memory_order_acq_rel)) {
    stopFading_ = true;  // from the gain it has (already silent: at once)
    stopMuted_ = false;
  }
  int64_t seek = seekTo_.exchange(-1);
  bool jumped = false;  // notes held at the new position are picked up once playback renders there
  if (seek >= 0) {
    if (seek != pos) { g.allNotesOff(); jumped = true; }
    pos = seek;
  }

  // 3. Live notes (keyboard / audition), applied at the start of the block.
  LiveNote n;
  if (panic_.exchange(false, std::memory_order_acq_rel)) {
    // Everything queued before the panic is dropped; every note (sequenced, live, timed) is released.
    while (liveNotes_.pop(n)) {}
    for (auto& o : offs_) o.used = false;
    g.allNotesOff();
  }
  while (liveNotes_.pop(n)) {
    g.liveNote(n.track, n.pitch, n.velocity, n.on);
    // A new press (or a release) of the same key on the same track: an older audition's timer no longer ends it.
    for (auto& o : offs_)
      if (o.used && o.pitch == n.pitch && std::strncmp(o.track, n.track, sizeof(o.track)) == 0) o.used = false;
    if (n.on && n.offAfter > 0) {
      // A timed note always gets its note-off. With every slot taken, the one ending soonest is released
      // now and its slot reused (an audition a little shorter, never a hanging note).
      PendingOff* slot = nullptr;
      for (auto& o : offs_) if (!o.used) { slot = &o; break; }
      if (!slot) {
        slot = &offs_[0];
        for (auto& o : offs_) if (o.remaining < slot->remaining) slot = &o;
        g.liveNote(slot->track, slot->pitch, 0, false);
        stats_.auditionSteals.fetch_add(1, std::memory_order_relaxed);
      }
      std::memcpy(slot->track, n.track, sizeof(slot->track)); slot->pitch = n.pitch; slot->remaining = n.offAfter; slot->used = true;
    }
  }
  if (overflowAny_.exchange(false, std::memory_order_acq_rel)) {
    // Note-offs that did not fit in the queue: that pitch on that track is released.
    for (int pitch = 0; pitch < 128; ++pitch)
      for (auto& slot : overflowTracks_[pitch])
        if (const uint64_t hash = slot.exchange(0, std::memory_order_acq_rel)) {
          g.pitchOff(hash, pitch);
          for (auto& o : offs_) if (o.used && o.pitch == pitch && Graph::trackHash(o.track) == hash) o.used = false;
        }
    for (int w = 0; w < 2; ++w)  // the last resort: the pitch on every track
      if (uint64_t bits = overflowOffs_[w].exchange(0, std::memory_order_acq_rel))
        for (int b = 0; b < 64; ++b)
          if (bits >> b & 1) {
            const int pitch = w * 64 + b;
            g.pitchOff(pitch);
            for (auto& o : offs_) if (o.used && o.pitch == pitch) o.used = false;
          }
  }
  // Timed notes (auditions) end on their own sample: the block is cut where one ends, and its note-off comes before
  // the rest is rendered (audit of main fa3b428, F06: the counter went down by the whole block before rendering it,
  // so an audition up to one block long never sounded).
  auto timedOffs = [&](int64_t elapsed) {
    for (auto& o : offs_)
      if (o.used && (o.remaining -= elapsed) <= 0) { g.liveNote(o.track, o.pitch, 0, false); o.used = false; }
  };
  timedOffs(0);

  // 4. Render, splitting the block at the loop end so the loop is sample exact.
  const int tracks = std::min(g.trackCount(), kMaxTracks);
  std::fill(scratchPeaks_, scratchPeaks_ + tracks * 2, 0.0f);
  const bool playing = playing_.load(std::memory_order_relaxed);
  if (playing && (jumped || !wasPlaying_)) g.chase(pos);  // a seek, or play from the middle of held notes
  wasPlaying_ = playing;
  // M5: the workers are waited for at most twice the buffer's time by default (the device has starved by then anyway).
  const uint64_t deadlineNs = startNs + (uint64_t)(liveDeadline_ * 1e9 * frames / sampleRate_);
  int done = 0;
  while (done < frames) {
    int n = frames - done;
    for (const auto& o : offs_) if (o.used) n = (int)std::min<int64_t>(n, std::max<int64_t>(1, o.remaining));
    if (playing) {
      // The loop region, or else the whole song: playback goes round instead of stopping at the end.
      const bool loop = loopEnabled_.load(std::memory_order_relaxed);
      const int64_t start = loop ? loopStart_.load(std::memory_order_relaxed) : 0;
      const int64_t end = loop ? loopEnd_.load(std::memory_order_relaxed) : g.songSamples();
      if (end > start) {
        if (pos >= end) { pos = start; g.allNotesOff(); g.chase(pos); }
        n = (int)std::min<int64_t>(n, end - pos);
      }
    }
    const bool ok = playing ? g.process(L + done, R + done, n, pos, false, nullptr, scratchPeaks_, deadlineNs)
                            : g.process(L + done, R + done, n, -(int64_t)1e12, false, nullptr, scratchPeaks_, deadlineNs);  // stopped: only tails and live notes
    if (playing) pos += n;  // the tracks played it, even when the mix is lost
    if (!ok) {  // a late worker: this block is silence, and the graph waits for it (see the top)
      stalled_ = true;
      stats_.liveStalls.fetch_add(1, std::memory_order_relaxed);
      std::fill(L, L + frames, 0.0f);
      std::fill(R, R + frames, 0.0f);
      position_.store(pos, std::memory_order_relaxed);
      timedOffs(frames - done);
      return;
    }
    done += n;
    timedOffs(n);
  }
  if (uint64_t extra = g.takeExtraEventPasses()) stats_.denseEventBlocks.fetch_add(extra, std::memory_order_relaxed);
  if (uint64_t dropped = g.takeDroppedNotes()) stats_.droppedNotes.fetch_add(dropped, std::memory_order_relaxed);
  atomicMaxF(masterReduction_, g.takeMasterReduction());
  position_.store(pos, std::memory_order_relaxed);
  positionTicks_.store(g.tempo().tickAt((double)pos), std::memory_order_relaxed);

  // 5. Stop: a short fade to silence, then silence until the graph without tails plays.
  if (stopFading_) {
    const float step = (float)(1.0 / (kStopFadeMs * 0.001 * sampleRate_));
    for (int i = 0; i < frames; ++i) {
      L[i] *= stopGain_; R[i] *= stopGain_;
      stopGain_ = std::max(0.0f, stopGain_ - step);
    }
    if (stopGain_ <= 0.0f) { stopFading_ = false; stopMuted_ = true; stopFaded_.store(true, std::memory_order_release); }
  } else if (stopMuted_) {
    if (appliedSerial_.load(std::memory_order_relaxed) >= unmuteSerial_.load(std::memory_order_acquire)) {
      stopMuted_ = false;
      stopGain_ = 1.0f;
    } else {
      std::fill(L, L + frames, 0.0f);
      std::fill(R, R + frames, 0.0f);
    }
  }

  if (playing) {
    capBusy_.store(true);
    if (capturing_.load()) {
      int64_t at = capWritten_.load(std::memory_order_relaxed);
      int64_t n = std::min<int64_t>(frames, (int64_t)capL_.size() - at);
      if (n > 0) {
        std::copy(L, L + n, capL_.begin() + at);
        std::copy(R, R + n, capR_.begin() + at);
        capWritten_.store(at + n, std::memory_order_release);
      }
    }
    capBusy_.store(false, std::memory_order_release);
  }

  float pl = 0, pr = 0, sum = 0;  // sum: NaN or Inf anywhere makes it non-finite
  for (int i = 0; i < frames; ++i) { pl = std::max(pl, std::fabs(L[i])); pr = std::max(pr, std::fabs(R[i])); sum += L[i] + R[i]; }
  if (!finiteBits(sum)) stats_.nonFiniteBlocks.fetch_add(1, std::memory_order_relaxed);
  atomicMaxF(masterPeak_[0], pl); atomicMaxF(masterPeak_[1], pr);
  loudness_.process(L, R, frames);
  analyzer_.process(L, R, frames);
  for (int i = 0; i < tracks * 2; ++i) atomicMaxF(trackPeak_[i], scratchPeaks_[i]);
  guard_.process(L, R, frames);  // last: what the speakers get is never dangerously loud
}

} // namespace core
