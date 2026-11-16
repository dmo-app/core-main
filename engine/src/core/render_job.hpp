// Export as a background job (plan 3.6): one job at a time, on its own thread, with its own graph and
// DSP instances made from a snapshot of the project. The command loop and the audio thread go on.
// The WAV is written block by block to "<path>.part", then renamed over the target only on success:
// cancelling or failing leaves an existing file untouched.
#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "assets.hpp"
#include "plugins.hpp"
#include "project.hpp"

namespace core {

// One file of a job: the mix (track -1) or one track's stem (taken before the master chain, see
// Graph::setSolo).
struct RenderOutput {
  std::string path;     // UTF-8
  int track = -1;       // index in project.tracks, -1 = the mix
  bool withoutMasterGain = false;  // a stem for a bounce: no master gain (Graph::setSolo)
  bool mono = false;    // one channel, (L + R) / 2: a song or passage exported as an example for the AI
};

// Shared publication for background exports and the synchronous CLI. Call after every
// <path>.part is closed. On failure old targets are restored and surviving new files counted.
void validateRenderOutputs(const std::vector<RenderOutput>& outputs);
// Codex review 2026-11-09 C2: a float export at +15 dBFS was published as a success. Before any file is published,
// each one is checked: NaN or Inf anywhere, or (except a bounce, which goes back into the song before the master)
// a peak over 0 dBFS, fails the export and the files that were there stay as they were.
struct ExportLevel {
  float peak = 0;
  bool broken = false;  // a NaN or an Inf
  void add(const float* x, int n);
};
void checkExportLevel(const ExportLevel& level, const RenderOutput& output);
void publishRenderOutputs(const std::vector<RenderOutput>& outputs, int& published,
                          const std::function<void()>& beforePlace = {});

struct RenderJobRequest {
  Project project;          // a copy: edits made during the export do not change it
  int64_t revision = -1;    // the project revision being exported (-1 = unidentified)
  double sampleRate = 48000;
  int bits = 24;            // 24 (PCM) or 32 (float)
  double tailSeconds = 2;   // after the end of the song, for reverb/release tails
  std::string path;         // UTF-8; the mix's file when `outputs` is empty
  // Several files in one job (the mix and/or stems), rendered one after the other. They are put in place
  // only when all of them are written: a cancel or a failure leaves every existing file untouched.
  std::vector<RenderOutput> outputs;
  // A region of the song: the file starts at startTick and nothing starts at or after endTick (-1 = the
  // song end); notes and effects already sounding there ring out in the tail. The song before the region
  // is played (not written) so held notes, effect tails and LFOs are where they would be.
  int64_t startTick = 0, endTick = -1;
  std::shared_ptr<AssetStore> assets;  // audio clip files (kept alive by the job's graph)
  PluginProvider* plugins = nullptr;   // the job's own plugin instances come from here (offline)
};

class RenderJobs {
public:
  ~RenderJobs();
  // Checks the request (format, folder, disk space, size limit) and starts it. Throws if invalid
  // or if another job is running. Returns the job's status.
  json start(RenderJobRequest request);
  json status(uint64_t jobId) const;
  json cancel(uint64_t jobId);
  bool busy() const;
  // Statuses that changed since the last call (state, or progress by at least 1%), for events.
  std::vector<json> takeUpdates();

  // Tests only: called on the worker thread just before it decides between cancelled and publishing,
  // and just after it has claimed publishing (before the rename). Set them before start() only (the
  // worker reads them without a lock; starting the thread publishes them). beforePlace: the old files
  // are aside, the new ones not yet in place.
  struct Hooks {
    std::function<void(uint64_t jobId)> beforePublish, afterPublishClaimed, beforePlace;
  };
  Hooks testHooks;

private:
  struct Job;
  void run(std::shared_ptr<Job> job);
  json describe(const Job& job) const;

  mutable std::mutex mutex_;
  std::shared_ptr<Job> current_;
  std::map<uint64_t, std::shared_ptr<Job>> jobs_;  // the last few, for status queries
  std::thread worker_;
  uint64_t nextId_ = 1;
};

} // namespace core
