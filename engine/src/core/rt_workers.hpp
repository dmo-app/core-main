// M5: worker threads for the real-time callback. Unlike WorkerPool (the export), the audio thread never
// blocks on a lock or a condition variable here and never waits without a limit:
// - each worker has its own semaphore and state (0 idle, 1 asked, 2 running); the audio thread asks a
//   worker by setting 1 and releasing its semaphore, and the worker starts only by moving 1 -> 2;
// - the audio thread takes jobs too, from the same counter, so every job is run once, by whoever takes it;
// - when the audio thread has no job left it takes back every worker that has not started (1 -> 0), and
//   waits only for the ones running (2), at most until the deadline. A worker that is still running then
//   keeps its job; the call returns false and nothing may touch the graph until idle() says so.
// No allocation after construction. The threads start once, with the pool, and sleep on their semaphores.
#pragma once
#include <atomic>
#include "../dsp/float_mode.hpp"
#include <chrono>
#include <cstdint>
#include <memory>
#include <semaphore>
#include <thread>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#define CORE_RT_PAUSE() _mm_pause()
#else
#define CORE_RT_PAUSE() ((void)0)
#endif

namespace core {

// Set by the device layer (Windows: MMCSS "Pro Audio" and time-critical priority); every worker calls it
// once, before its first job. Null: the workers keep the default priority (tests, Linux).
using RtThreadInit = void (*)();
inline std::atomic<RtThreadInit>& rtThreadInit() {
  static std::atomic<RtThreadInit> init{nullptr};
  return init;
}

class RtWorkerPool {
public:
  using Job = void (*)(void* context, int index);

  explicit RtWorkerPool(int threads) {
    workers_.reserve((size_t)threads);
    for (int i = 0; i < threads; ++i) workers_.push_back(std::make_unique<Worker>());
    for (auto& w : workers_) w->thread = std::thread([this, p = w.get()] { loop(*p); });
  }
  ~RtWorkerPool() {
    stop_.store(true);
    for (auto& w : workers_) wake(*w);
    for (auto& w : workers_) w->thread.join();
  }
  RtWorkerPool(const RtWorkerPool&) = delete;
  RtWorkerPool& operator=(const RtWorkerPool&) = delete;

  int threads() const { return (int)workers_.size(); }

  // No worker holds a job (the previous run ended, or its late workers have finished since).
  bool idle() const {
    for (const auto& w : workers_) if (w->state.load(std::memory_order_acquire) != 0) return false;
    return true;
  }

  // Audio thread. job(context, 0..count-1), each once, spread over the workers and this thread. Call only
  // when idle(). true: every job has finished (their writes are visible here). false: deadlineNs (steady
  // clock) passed while a worker was still running one; the jobs it holds finish later, on their own.
  bool run(Job job, void* context, int count, uint64_t deadlineNs) {
    if (count <= 0) return true;
    job_ = job;
    context_ = context;
    count_ = count;
    mode_ = floatMode();  // the workers run in this thread's numeric mode (float_mode.hpp)
    failed_.store(false, std::memory_order_relaxed);
    next_.store(0, std::memory_order_relaxed);
    const int ask = std::min<int>((int)workers_.size(), count - 1);
    for (int i = 0; i < ask; ++i) {
      workers_[(size_t)i]->state.store(1);  // publishes job_, context_, count_
      wake(*workers_[(size_t)i]);
    }
    if (testDelayNs.load(std::memory_order_relaxed) > 0) {
      // Tests only: a worker takes a job before this thread does, so the late worker is late for sure (Codex review
      // 2026-11-09 C8: on a loaded machine the workers woke after this thread had done every job, and the test of a
      // late worker saw none).
      const uint64_t until = now() + 1000000000ull;
      while (next_.load(std::memory_order_acquire) == 0 && now() < until) CORE_RT_PAUSE();
    }
    take();
    bool done = true;
    for (int i = 0; i < ask; ++i) {
      Worker& w = *workers_[(size_t)i];
      int expected = 1;
      if (w.state.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) continue;  // never started
      while (w.state.load(std::memory_order_acquire) != 0) {
        if (now() > deadlineNs) { done = false; break; }
        CORE_RT_PAUSE();
      }
      if (!done) break;
    }
    return done;
  }
  // A job threw on a worker in the last run (the audio thread reports it like its own exception).
  bool failed() const { return failed_.load(std::memory_order_acquire); }

  static uint64_t now() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  // Tests: a worker sleeps this long before each job it takes (a worker the OS let run late).
  std::atomic<int64_t> testDelayNs{0};
  // Tests: semaphore releases not yet taken by their worker, over all workers (at most one each).
  int pendingWakes() const {
    int n = 0;
    for (const auto& w : workers_) n += w->pending.load() ? 1 : 0;
    return n;
  }

private:
  struct Worker {
    std::binary_semaphore wake{0};
    // A release not yet taken. A binary semaphore must never be released twice before an acquire (undefined
    // behaviour): a worker taken back before it started left its release pending, and the next run released it
    // again (audit 2026-11-07: 10000 pending wakes on a semaphore whose maximum is 1).
    std::atomic<bool> pending{false};
    std::atomic<int> state{0};
    std::thread thread;
  };

  // Wakes w, unless a wake is already pending: that one still reaches it, and it reads state after taking it.
  static void wake(Worker& w) {
    if (!w.pending.exchange(true)) w.wake.release();
  }
  void take() {
    for (int i; (i = next_.fetch_add(1, std::memory_order_acq_rel)) < count_;) job_(context_, i);
  }
  void loop(Worker& w) {
    bool initialised = false;
    for (;;) {
      w.wake.acquire();
      w.pending.store(false);  // before state is read: an ask from here on releases again
      if (stop_.load()) return;
      int expected = 1;
      if (!w.state.compare_exchange_strong(expected, 2)) continue;  // taken back (or an old wake)
      if (!initialised) {
        if (RtThreadInit init = rtThreadInit().load()) init();
        initialised = true;
      }
      setFloatMode(mode_);
      try {
        for (int i; (i = next_.fetch_add(1, std::memory_order_acq_rel)) < count_;) {
          if (const int64_t d = testDelayNs.load(std::memory_order_relaxed)) std::this_thread::sleep_for(std::chrono::nanoseconds(d));
          job_(context_, i);
        }
      } catch (...) {
        failed_.store(true, std::memory_order_release);
      }
      w.state.store(0, std::memory_order_release);  // publishes what the jobs wrote
    }
  }

  std::vector<std::unique_ptr<Worker>> workers_;
  Job job_ = nullptr;
  void* context_ = nullptr;
  int count_ = 0;
  uint32_t mode_ = 0;
  std::atomic<int> next_{0};
  std::atomic<bool> failed_{false};
  std::atomic<bool> stop_{false};
};

} // namespace core
