// A small pool of worker threads for the export (M4a): run(count, job) calls job(0..count-1) spread over
// the workers and the calling thread, and returns when every call has finished. Offline only (it waits
// on a condition variable); real time never uses it.
#pragma once
#include <algorithm>
#include "../dsp/float_mode.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace core {

class WorkerPool {
public:
  // threads: workers besides the caller (0 = the caller does everything, as serial).
  explicit WorkerPool(int threads) {
    for (int i = 0; i < threads; ++i) threads_.emplace_back([this] { loop(); });
  }
  ~WorkerPool() {
    {
      std::lock_guard<std::mutex> g(mutex_);
      stop_ = true;
    }
    wake_.notify_all();
    for (auto& t : threads_) t.join();
  }
  WorkerPool(const WorkerPool&) = delete;
  WorkerPool& operator=(const WorkerPool&) = delete;

  int threads() const { return (int)threads_.size(); }
  // Workers for this machine: one per core but the caller's, at most `cap`.
  static int forThisMachine(int cap = 8) {
    const int cores = (int)std::thread::hardware_concurrency();
    return std::max(0, std::min(cap, cores - 1));
  }

  // Export workers (M4a): CORE_EXPORT_THREADS=<n> when set (0 = serial, to compare or fall back),
  // otherwise forThisMachine().
  static int forExport() {
    std::string v;
#ifdef _MSC_VER
    char* buf = nullptr;
    size_t len = 0;
    if (_dupenv_s(&buf, &len, "CORE_EXPORT_THREADS") == 0 && buf) { v = buf; free(buf); }
#else
    if (const char* e = std::getenv("CORE_EXPORT_THREADS")) v = e;
#endif
    if (!v.empty()) {
      char* end = nullptr;
      const long n = std::strtol(v.c_str(), &end, 10);
      if (end != v.c_str() && n >= 0) return (int)std::min<long>(n, 64);
    }
    return forThisMachine();
  }

  void run(int count, const std::function<void(int)>& job) {
    if (count <= 0) return;
    if (threads_.empty() || count == 1) {
      for (int i = 0; i < count; ++i) job(i);
      return;
    }
    {
      std::lock_guard<std::mutex> g(mutex_);
      job_ = &job;
      count_ = count;
      mode_ = floatMode();  // the workers run in the caller's numeric mode (float_mode.hpp)
      next_.store(0);
      left_ = count;
      ++generation_;
    }
    wake_.notify_all();
    work();
    // Every call done, and every worker out of work() (none can take an index of the next run).
    std::unique_lock<std::mutex> g(mutex_);
    done_.wait(g, [this] { return left_ == 0 && active_ == 0; });
    job_ = nullptr;
  }

private:
  void work() {
    int finished = 0;
    for (int i; (i = next_.fetch_add(1)) < count_;) {
      (*job_)(i);
      ++finished;
    }
    std::lock_guard<std::mutex> g(mutex_);
    left_ -= finished;
    if (left_ == 0) done_.notify_all();
  }
  void loop() {
    uint64_t seen = 0;
    for (;;) {
      {
        std::unique_lock<std::mutex> g(mutex_);
        wake_.wait(g, [&] { return stop_ || generation_ != seen; });
        if (stop_) return;
        seen = generation_;
        if (!job_) continue;  // woke after that run had already finished
        ++active_;
        setFloatMode(mode_);
      }
      work();
      std::lock_guard<std::mutex> g(mutex_);
      if (--active_ == 0 && left_ == 0) done_.notify_all();
    }
  }

  std::vector<std::thread> threads_;
  uint32_t mode_ = 0;
  std::mutex mutex_;
  std::condition_variable wake_, done_;
  const std::function<void(int)>* job_ = nullptr;
  int count_ = 0;
  std::atomic<int> next_{0};
  int left_ = 0;
  int active_ = 0;  // workers inside work()
  uint64_t generation_ = 0;
  bool stop_ = false;
};

} // namespace core
