#include "render_job.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include "engine.hpp"
#include "graph.hpp"
#include "../io/wav.hpp"
#include "../dsp/finite.hpp"
#include <cstdio>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace core {

namespace fs = std::filesystem;

namespace {
// Publishing: the worker has claimed the end of the job (renaming the .part over the target). It is the
// only way from Running to Succeeded, and cancel() can no longer win once it is taken; it is reported as
// "running" (it lasts a rename), so the app never sees "cancelling" followed by "succeeded".
enum class State { Queued, Running, Cancelling, Publishing, Succeeded, Cancelled, Failed };
const char* name(State s) {
  switch (s) {
    case State::Queued: return "queued";
    case State::Running: return "running";
    case State::Publishing: return "running";
    case State::Cancelling: return "cancelling";
    case State::Succeeded: return "succeeded";
    case State::Cancelled: return "cancelled";
    case State::Failed: return "failed";
  }
  return "?";
}
bool finished(State s) { return s == State::Succeeded || s == State::Cancelled || s == State::Failed; }
fs::path utf8(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
bool sameTarget(const fs::path& a, const fs::path& b) {
  const auto x = fs::absolute(a).lexically_normal();
  const auto y = fs::absolute(b).lexically_normal();
#ifdef _WIN32
  // Match Windows path casing without depending on the process's ANSI codepage/locale.
  return CompareStringOrdinal(x.c_str(), -1, y.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
  return x == y;
#endif
}
constexpr int kBlock = 4096;  // frames per step: cancel is checked this often (~85 ms of audio)
}

void validateRenderOutputs(const std::vector<RenderOutput>& outputs) {
  std::error_code e;
  std::vector<fs::path> targets;
  for (const auto& out : outputs) {
    if (out.path.empty()) throw std::runtime_error("Every file needs a 'path'.");
    const fs::path target = utf8(out.path);
    const fs::path folder = target.has_parent_path() ? target.parent_path() : fs::current_path();
    if (!fs::is_directory(folder, e)) throw std::runtime_error("The folder " + folder.string() + " does not exist.");
    for (const auto& t : targets)
      if (sameTarget(t, target)) throw std::runtime_error("Two files have the same path: " + out.path + ".");
    targets.push_back(target);
  }
  // A file's ".part" (written while it renders) must not be another file of the same export (audit 2026-11-07).
  for (const auto& t : targets)
    for (const auto& u : targets)
      if (sameTarget(fs::path(t.native() + utf8(".part").native()), u))
        throw std::runtime_error("One file is another one's working copy (.part): " + u.string() + ".");
}

void publishRenderOutputs(const std::vector<RenderOutput>& outputs, int& published, const std::function<void()>& beforePlace) {
  const size_t count = outputs.size();
  auto partOf = [](const std::string& path) { return utf8(path + ".part"); };
  published = 0;
  // Reserve backups before moving anything; all paths use the same alias rules.
  std::vector<fs::path> asides(count);
  {
    std::vector<fs::path> taken;
    for (const auto& o : outputs) { taken.push_back(utf8(o.path).lexically_normal()); taken.push_back(partOf(o.path).lexically_normal()); }
    for (size_t f = 0; f < count; ++f) {
      for (int k = 1;; ++k) {
        const fs::path candidate = utf8(outputs[f].path + ".previous" + (k == 1 ? std::string() : "-" + std::to_string(k)));
        std::error_code ignored;
        bool clash = fs::exists(candidate, ignored);
        for (const auto& t : taken) clash = clash || sameTarget(t, candidate);
        if (!clash) { asides[f] = candidate; taken.push_back(candidate.lexically_normal()); break; }
      }
    }
  }
  std::vector<char> aside(count, 0), placed(count, 0);
  auto undo = [&] {
    int left = 0;  // new files that could not be taken back (reported as "published")
    for (size_t f = 0; f < count; ++f) {
      std::error_code ignored;
      if (placed[f]) { fs::remove(utf8(outputs[f].path), ignored); if (ignored) ++left; }
      if (aside[f]) { fs::rename(asides[f], utf8(outputs[f].path), ignored); (void)ignored; }
    }
    published = left;
  };
  try {
    for (size_t f = 0; f < count; ++f) {
      const fs::path target = utf8(outputs[f].path);
      std::error_code e;
      if (fs::is_directory(target, e)) throw std::runtime_error("Cannot replace " + outputs[f].path + ": it is a folder.");
      if (!fs::exists(target, e)) continue;
      fs::rename(target, asides[f], e);
      if (e) { throw std::runtime_error("Cannot replace " + outputs[f].path + ": " + e.message()); }
      aside[f] = 1;
    }
    if (beforePlace) beforePlace();
    for (size_t f = 0; f < count; ++f) {
      std::error_code e;
      fs::rename(partOf(outputs[f].path), utf8(outputs[f].path), e);
      if (e) { throw std::runtime_error("Cannot replace " + outputs[f].path + ": " + e.message()); }
      placed[f] = 1;
    }
  } catch (...) { undo(); throw; }
  for (size_t f = 0; f < count; ++f) {
    std::error_code ignored;
    if (aside[f]) fs::remove(asides[f], ignored);
  }
  published = (int)count;
}

struct RenderJobs::Job {
  uint64_t id;
  RenderJobRequest request;
  int64_t frames = 0;  // per file
  int64_t skip = 0;    // song samples played before the region (not written)
  std::atomic<int> file{0};  // the output being rendered
  std::atomic<State> state{State::Queued};
  std::atomic<double> progress{0};
  std::atomic<bool> cancel{false};
  // Written by the worker before the final state is stored.
  std::string error;
  float peak = 0;
  double renderMs = 0;
  int published = 0;  // files put in place (all of them on success; on a failure only those that could not be undone)
  int threads = 0;  // export workers besides the job's own thread (M4a)
  // Last values reported by takeUpdates().
  State reportedState = State::Queued;
  double reportedProgress = -1;
};

RenderJobs::~RenderJobs() {
  {
    std::lock_guard<std::mutex> g(mutex_);
    if (current_) current_->cancel = true;
  }
  if (worker_.joinable()) worker_.join();
}

bool RenderJobs::busy() const {
  std::lock_guard<std::mutex> g(mutex_);
  return current_ && !finished(current_->state.load());
}

void ExportLevel::add(const float* x, int n) {
  for (int i = 0; i < n; ++i) {
    const float a = std::fabs(x[i]);
    if (!finiteBits(x[i])) broken = true;
    else if (a > peak) peak = a;
  }
}

void ExportLevel::addMono(const float* L, const float* R, int n) {
  for (int i = 0; i < n; ++i) {
    if (!finiteBits(L[i]) || !finiteBits(R[i])) { broken = true; continue; }
    const float a = std::fabs(0.5f * (L[i] + R[i]));
    if (a > peak) peak = a;
  }
}

void checkExportLevel(const ExportLevel& level, const RenderOutput& output) {
  const std::string full = output.path;
  const size_t slash = full.find_last_of("/\\");
  const std::string name = slash == std::string::npos ? full : full.substr(slash + 1);
  if (level.broken)
    throw std::runtime_error("The export of " + name + " has broken samples (NaN or Inf). Nothing was written; an effect or an instrument is unstable.");
  if (!output.withoutMasterGain && level.peak > 1.0f) {
    char db[32];
    std::snprintf(db, sizeof db, "%+.1f", 20 * std::log10((double)level.peak));
    throw std::runtime_error("The export of " + name + " goes over 0 dBFS (peak " + db + " dBFS), so it would clip. Nothing was written: lower the master, or end it with a limiter.");
  }
}

uint64_t exportFileBytes(int64_t frames, int bits, bool mono) {
  return 44 + (uint64_t)std::max<int64_t>(0, frames) * (mono ? 1u : 2u) * (uint64_t)(bits / 8);
}

void checkExportSpace(const std::vector<RenderOutput>& outputs, int64_t frames, int bits, const DiskSpaceQuery& query) {
  constexpr uint64_t kMargin = 16u << 20;  // room left for everything else
  // The bytes each folder gets.
  std::vector<std::pair<fs::path, uint64_t>> folders;
  for (const auto& out : outputs) {
    const fs::path target = utf8(out.path);
    const fs::path folder = target.has_parent_path() ? target.parent_path() : fs::path(".");
    const uint64_t bytes = exportFileBytes(frames, bits, out.mono);
    auto it = std::find_if(folders.begin(), folders.end(), [&](const auto& f) { return f.first == folder; });
    if (it == folders.end()) folders.push_back({folder, bytes});
    else it->second += bytes;
  }
  // The same volume answers the same capacity and free space: those folders share it.
  struct Volume { fs::space_info space; uint64_t bytes = 0; fs::path folder; };
  std::vector<Volume> volumes;
  for (const auto& [folder, bytes] : folders) {
    fs::space_info space{};
    bool known = false;
    if (query) known = query(folder, space);
    else {
      std::error_code e;
      space = fs::space(folder, e);
      known = !e;
    }
    if (!known) continue;  // a folder that cannot be asked: the write itself will say
    auto it = std::find_if(volumes.begin(), volumes.end(), [&](const Volume& v) {
      return v.space.capacity == space.capacity && v.space.available == space.available;
    });
    if (it == volumes.end()) volumes.push_back({space, bytes, folder});
    else it->bytes += bytes;
  }
  for (const auto& v : volumes)
    if (v.space.available < v.bytes + kMargin) {
      const std::u8string name = v.folder.u8string();
      throw std::runtime_error("Not enough disk space: " + std::to_string((v.bytes + (1u << 20) - 1) >> 20) + " MB needed in " +
                               std::string(name.begin(), name.end()) + ", " + std::to_string(v.space.available >> 20) + " MB free.");
    }
}

json RenderJobs::start(RenderJobRequest request) {
  if (request.bits != 24 && request.bits != 32) throw std::runtime_error("bits must be 24 or 32.");
  if (!(request.tailSeconds >= 0 && request.tailSeconds <= 60)) throw std::runtime_error("tailMs must be 0-60000.");
  if (request.outputs.empty()) {
    if (request.path.empty()) throw std::runtime_error("'path' is required.");
    request.outputs.push_back({request.path, -1});
  }
  if (request.path.empty()) request.path = request.outputs.front().path;
  if (request.outputs.size() > 64) throw std::runtime_error("At most 64 files in one export.");
  validateRenderOutputs(request.outputs);
  for (const auto& out : request.outputs)
    if (out.track < -1 || out.track >= (int)request.project.tracks.size()) throw std::runtime_error("No such track for " + out.path + ".");
  // The region: nothing starts after its end (the song is cut there, its tail rings out).
  Project& project = request.project;
  if (request.endTick >= 0) project.lengthTicks = std::min(project.lengthTicks, request.endTick);
  request.endTick = project.lengthTicks;
  if (!(request.startTick >= 0 && request.startTick < request.endTick))
    throw std::runtime_error("The region must start at 0 or later and before its end.");
  // Length and size are known before starting: check them now, not after minutes of work.
  const TempoMap tempo(project.bpm, project.tempoMap, request.sampleRate);
  const int64_t skip = tempo.sampleOfTick(request.startTick);
  const int64_t frames = tempo.sampleOfTick(request.endTick) - skip + (int64_t)std::llround(request.tailSeconds * request.sampleRate);
  if ((uint64_t)frames > WavWriter::maxFrames(request.bits)) throw std::runtime_error("The song is too long for one WAV file (4 GB limit).");
  checkExportSpace(request.outputs, frames, request.bits);

  std::lock_guard<std::mutex> g(mutex_);
  if (current_ && !finished(current_->state.load()))
    throw std::runtime_error("Another export is running (job " + std::to_string(current_->id) + ").");
  if (worker_.joinable()) worker_.join();  // the previous job has finished: its thread ends at once
  auto job = std::make_shared<Job>();
  job->id = nextId_++;
  job->request = std::move(request);
  job->frames = frames;
  job->skip = skip;
  current_ = job;
  jobs_[job->id] = job;
  while (jobs_.size() > 16) jobs_.erase(jobs_.begin());
  job->state = State::Running;
  worker_ = std::thread([this, job] { run(job); });
  return describe(*job);
}

void RenderJobs::run(std::shared_ptr<Job> job) {
  const RenderJobRequest& r = job->request;
  const size_t count = r.outputs.size();
  auto partOf = [](const std::string& path) { return utf8(path + ".part"); };
  const uint64_t t0 = nowNs();
  auto discard = [&] {
    std::error_code ignored;
    for (const auto& out : r.outputs) fs::remove(partOf(out.path), ignored);
  };
  try {
    // Every song sample up to the region's end and its tail is played; the ones before the region are
    // dropped. Progress counts both, over all the files.
    const int64_t total = job->skip + job->frames;
    WorkerPool workers(WorkerPool::forExport());  // M4a: the tracks on several cores, the same file bit for bit
    job->threads = workers.threads();
    for (size_t f = 0; f < count && !job->cancel.load(); ++f) {
      job->file = (int)f;
      Graph graph(r.project, r.sampleRate, r.assets.get(), r.plugins, true);  // the job's own instruments and plugins; audio buffers are read-only
      graph.setSolo(r.outputs[f].track, r.outputs[f].withoutMasterGain);
      // Audio the song needs and cannot be read (moved, deleted, unreadable since the song was loaded): the job
      // fails before any file is written, so an export with holes never replaces the previous one (audit of main
      // fa3b428, F01).
      if (!graph.missingAssets().empty()) {
        std::string list;
        for (const auto& m : graph.missingAssets()) list += (list.empty() ? "" : "; ") + m;
        throw std::runtime_error("Missing audio, nothing exported: " + list);
      }
      graph.useWorkers(&workers);
      graph.preroll();  // effect latency: the song starts on sample 0 of the file
      WavWriter wav(r.outputs[f].path + ".part", (int)r.sampleRate, r.bits, r.outputs[f].mono ? 1 : 2);
      std::vector<float> L(kBlock), R(kBlock);
      ExportLevel level;
      int n = 0;
      for (int64_t pos = 0; pos < total; pos += n) {
        if (job->cancel.load()) break;
        // Blocks end on the region's start, so the first written sample is the region's first.
        const int64_t until = pos < job->skip ? job->skip : total;
        n = (int)std::min<int64_t>(kBlock, until - pos);
        std::fill(L.begin(), L.begin() + n, 0.0f);
        std::fill(R.begin(), R.begin() + n, 0.0f);
        graph.process(L.data(), R.data(), n, pos, false);
        if (pos >= job->skip) {
          level.addFrames(L.data(), R.data(), n, r.outputs[f].mono);
          job->peak = std::max(job->peak, level.peak);
          wav.write(L.data(), R.data(), n);
        }
        job->progress = ((double)f + (double)(pos + n) / (double)std::max<int64_t>(1, total)) / (double)count;
      }
      // A cancelled file's writer closes when it goes out of scope; then the partial files are removed.
      if (!job->cancel.load()) { wav.finish(); checkExportLevel(level, r.outputs[f]); }
    }
    if (testHooks.beforePublish) testHooks.beforePublish(job->id);
    // One transition decides between cancel and publish: Running -> Publishing here, or Running ->
    // Cancelling in cancel(). Whichever happens first wins; the other side sees it.
    State running = State::Running;
    if (job->cancel.load() || !job->state.compare_exchange_strong(running, State::Publishing)) {
      discard();
      job->renderMs = (nowNs() - t0) / 1e6;
      job->state = State::Cancelled;
      return;
    }
    if (testHooks.afterPublishClaimed) testHooks.afterPublishClaimed(job->id);
    publishRenderOutputs(r.outputs, job->published, [&] { if (testHooks.beforePlace) testHooks.beforePlace(job->id); });
    job->renderMs = (nowNs() - t0) / 1e6;
    job->progress = 1;
    job->state = State::Succeeded;  // only now: the final files are closed and in place
  } catch (const std::exception& ex) {
    discard();
    job->error = ex.what();
    job->renderMs = (nowNs() - t0) / 1e6;
    job->state = State::Failed;
  }
}

json RenderJobs::describe(const Job& job) const {
  const State s = job.state.load();
  json out = {{"jobId", job.id}, {"state", name(s)}, {"progress", job.progress.load()}, {"path", job.request.path},
              {"revision", job.request.revision >= 0 ? json(job.request.revision) : json()},
              {"bits", job.request.bits}, {"sampleRate", job.request.sampleRate}, {"tailMs", job.request.tailSeconds * 1000},
              {"seconds", job.frames / job.request.sampleRate}, {"startTick", job.request.startTick},
              {"endTick", job.request.endTick}, {"file", job.file.load()}};
  json files = json::array();
  for (const auto& o : job.request.outputs)
    files.push_back({{"path", o.path}, {"track", o.track >= 0 ? json(job.request.project.tracks[(size_t)o.track].id) : json()}});
  out["files"] = files;
  if (finished(s)) {
    out["renderMs"] = job.renderMs;
    out["threads"] = job.threads + 1;
    if (s == State::Succeeded) out["peak"] = job.peak;
    if (s == State::Failed) {
      out["error"] = job.error;
      if (job.published > 0) out["published"] = job.published;  // new files the failure could not take back
    }
  }
  return out;
}

json RenderJobs::status(uint64_t jobId) const {
  std::lock_guard<std::mutex> g(mutex_);
  auto it = jobs_.find(jobId);
  if (it == jobs_.end()) throw std::runtime_error("No export job " + std::to_string(jobId) + ".");
  return describe(*it->second);
}

json RenderJobs::cancel(uint64_t jobId) {
  std::lock_guard<std::mutex> g(mutex_);
  auto it = jobs_.find(jobId);
  if (it == jobs_.end()) throw std::runtime_error("No export job " + std::to_string(jobId) + ".");
  Job& job = *it->second;
  State running = State::Running;
  if (job.state.compare_exchange_strong(running, State::Cancelling)) {
    job.cancel = true;
    return describe(job);
  }
  json out = describe(job);
  // Too late: the file is being put in place; the job ends succeeded (or failed, if the rename fails).
  if (running == State::Publishing) out["cancelRefused"] = true;
  return out;
}

std::vector<json> RenderJobs::takeUpdates() {
  std::lock_guard<std::mutex> g(mutex_);
  std::vector<json> out;
  for (auto& [id, job] : jobs_) {
    State s = job->state.load();
    if (s == State::Publishing) s = State::Running;  // the same "running" for the app: not a change
    const double p = job->progress.load();
    if (s != job->reportedState || (!finished(s) && p - job->reportedProgress >= 0.01)) {
      job->reportedState = s;
      job->reportedProgress = p;
      out.push_back(describe(*job));
    }
  }
  return out;
}

} // namespace core
