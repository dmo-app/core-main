#include "qualify.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <vector>

namespace core {

json qualifyPlugin(PluginProvider& provider, const json& plugin, double sampleRate, double seconds) {
  json out = {{"plugin", plugin.value("name", std::string("plugin"))}, {"version", plugin.value("version", std::string())},
              {"problems", json::array()}, {"warnings", json::array()}};
  std::string why;
  std::shared_ptr<PluginProcessor> p = provider.get("qualify:" + plugin.value("id", std::string()), plugin, sampleRate, true, &why);
  if (!p) {
    out["problems"].push_back("could not be loaded: " + (why.empty() ? std::string("unknown reason") : why));
    out["verdict"] = "unsafe";
    return out;
  }
  const bool instrument = p->isInstrument();
  out["instrument"] = instrument;
  out["installed"] = p->installedVersion();
  constexpr int kBlock = 32;
  const int64_t phase = std::max<int64_t>(kBlock, (int64_t)(seconds / 4 * sampleRate));  // four phases
  float left[kBlock], right[kBlock];
  MidiNote notes[160];
  uint32_t noise = 22222;
  double peak = 0, signalPeak = 0;
  uint64_t ns = 0;
  std::string problem;
  for (int64_t at = 0; at < 4 * phase && problem.empty(); at += kBlock) {
    const int stage = (int)(at / phase);  // 0 silence, 1 signal, 2 extremes, 3 tail
    const int64_t local = at % phase;
    int count = 0;
    std::fill(left, left + kBlock, 0.0f);
    std::fill(right, right + kBlock, 0.0f);
    if (instrument) {
      const int64_t step = (int64_t)(sampleRate / 8);  // a chord, then sixteenths at ~120 BPM
      const int64_t next = (local + step - 1) / step * step;  // the next sixteenth at or after this block's start
      if (stage == 1 && next < local + kBlock) {
        const int off = (int)(next - local);
        for (int pitch : {48, 55, 60, 64}) notes[count++] = {off, false, pitch, 0.0f};
        for (int pitch : {48, 55, 60, 64}) notes[count++] = {off, true, pitch, 0.8f};
      }
      if (stage == 2 && local == 0) {  // the edges of MIDI and 64 notes at once
        for (int pitch : {0, 127}) notes[count++] = {0, true, pitch, 1.0f};
        for (int k = 0; k < 64; ++k) notes[count++] = {0, true, 30 + k, 1.0f};
      }
      if (stage == 3 && local == 0) {
        for (int pitch = 0; pitch < 128; ++pitch) notes[count++] = {0, false, pitch, 0.0f};
        notes[count++] = {0, false, kAllNotesOff, 0.0f};
      }
    } else if (stage == 1) {  // noise at -6 dBFS with an impulse at the start
      for (int i = 0; i < kBlock; ++i) {
        noise = noise * 1664525u + 1013904223u;
        left[i] = ((float)(noise >> 8) / 8388608.0f - 1.0f) * 0.5f;
        right[i] = -left[i];
      }
      if (local == 0) left[0] = right[0] = 1.0f;
    } else if (stage == 2) {  // full-scale square, then DC
      for (int i = 0; i < kBlock; ++i) {
        const bool dc = local > phase / 2;
        left[i] = right[i] = dc ? 0.5f : (((local + i) / 64) % 2 ? 1.0f : -1.0f);
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    try {
      p->process(left, right, kBlock, notes, count);
    } catch (const std::exception& e) {
      problem = std::string("exception: ") + e.what();
    } catch (...) {
      problem = "exception";
    }
    ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    if (!problem.empty()) break;
    if (p->failed()) { problem = PluginProcessor::failureName(p->failure()); break; }
    for (int i = 0; i < kBlock; ++i) {
      if (!std::isfinite(left[i]) || !std::isfinite(right[i])) { problem = PluginProcessor::failureName(PluginProcessor::Failure::NonFinite); break; }
      const double m = std::max(std::fabs(left[i]), std::fabs(right[i]));
      if (m > 256) { problem = PluginProcessor::failureName(PluginProcessor::Failure::Runaway); break; }
      peak = std::max(peak, m);
      if (stage == 1) signalPeak = std::max(signalPeak, m);
    }
  }
  if (!problem.empty()) out["problems"].push_back(problem);
  const double audioSeconds = 4.0 * (double)phase / sampleRate;
  const double cpu = 100.0 * (double)ns / 1e9 / audioSeconds;
  out["cpuPct"] = cpu;
  out["peakDb"] = peak > 0 ? 20 * std::log10(peak) : -200.0;
  out["seconds"] = audioSeconds;
  if (problem.empty()) {
    if (cpu > 25) out["warnings"].push_back("heavy: " + std::to_string((int)std::lround(cpu)) + " % of one core on its own");
    if (signalPeak < 1e-6) out["warnings"].push_back(instrument ? "made no sound with notes" : "let no sound through");
    if (peak > 4) out["warnings"].push_back("loud: peaks above +12 dBFS");
  }
  out["verdict"] = out["problems"].empty() ? "ok" : "unsafe";
  return out;
}

} // namespace core

namespace core {

namespace {

struct MatrixRun {
  std::vector<float> L, R;
  int latency = 0;             // what the instance reported when it was made
  bool latencyChanged = false;
  bool instrument = false;
  std::string installed, problem;
  uint64_t ns = 0;
};

// What a run plays: an effect's input or an instrument's notes, the same every time.
enum class Input { Impulse, Signal, LeftOnly };

double dbOf(double v) { return v > 0 ? 20 * std::log10(v) : -200.0; }

MatrixRun renderRun(PluginProvider& provider, const json& plugin, const std::string& key, double sr, bool offline, Input input,
                    int64_t frames, uint32_t sizeSeed) {
  MatrixRun run;
  std::string why;
  std::shared_ptr<PluginProcessor> p = provider.get(key, plugin, sr, offline, &why);
  if (!p) { run.problem = "could not be loaded: " + (why.empty() ? std::string("unknown reason") : why); return run; }
  const bool instrument = run.instrument = p->isInstrument();
  run.installed = p->installedVersion();
  run.latency = p->latencySamples();
  run.L.assign((size_t)frames, 0.0f);
  run.R.assign((size_t)frames, 0.0f);
  const int64_t start = (int64_t)(sr / 10);       // 0.1 s of silence first
  const int64_t signalEnd = start + (int64_t)sr;  // a second of signal (Signal, LeftOnly)
  const int64_t step = (int64_t)(sr / 8);         // sixteenths at ~120 BPM
  uint32_t noise = 4242, sizes = sizeSeed;
  MidiNote notes[16];
  for (int64_t at = 0; at < frames && run.problem.empty();) {
    int n = 32;
    if (sizeSeed) { sizes = sizes * 1664525u + 1013904223u; n = 1 + (int)((sizes >> 8) % 512); }
    n = (int)std::min<int64_t>(n, frames - at);
    float* l = run.L.data() + at;
    float* r = run.R.data() + at;
    int count = 0;
    if (instrument) {
      auto chord = [&](int64_t t, bool on) {
        if (t >= at && t < at + n) for (int pitch : {48, 55, 60}) notes[count++] = {(int)(t - at), on, pitch, on ? 0.8f : 0.0f};
      };
      if (input == Input::Impulse) {  // one chord, released half a second later
        chord(start, true);
        chord(start + (int64_t)(sr / 2), false);
      } else {  // a chord on every sixteenth for a second (the offs first: they share the sample)
        for (int64_t t = start + std::max<int64_t>(0, (at - start + step - 1) / step) * step; t < at + n && t <= signalEnd; t += step) {
          if (t > start) chord(t, false);
          if (t < signalEnd) chord(t, true);
        }
      }
    } else {
      for (int i = 0; i < n; ++i) {
        const int64_t s = at + i;
        if (input == Input::Impulse) { if (s == start) l[i] = r[i] = 1.0f; continue; }
        if (s < start || s >= signalEnd) continue;
        noise = noise * 1664525u + 1013904223u;
        const float v = ((float)(noise >> 8) / 8388608.0f - 1.0f) * 0.25f + (((s - start) % step) == 0 ? 0.5f : 0.0f);
        l[i] = v;
        r[i] = input == Input::LeftOnly ? 0.0f : -v;
      }
    }
    const auto t0 = std::chrono::steady_clock::now();
    try {
      p->process(l, r, n, notes, count);
    } catch (const std::exception& e) {
      run.problem = std::string("exception: ") + e.what();
    } catch (...) {
      run.problem = "exception";
    }
    run.ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count();
    if (run.problem.empty() && p->failed()) run.problem = PluginProcessor::failureName(p->failure());
    for (int i = 0; i < n && run.problem.empty(); ++i)
      if (!std::isfinite(l[i]) || !std::isfinite(r[i])) run.problem = PluginProcessor::failureName(PluginProcessor::Failure::NonFinite);
    if (p->latencySamples() != run.latency) run.latencyChanged = true;
    at += n;
  }
  return run;
}

// The largest difference between two runs, each lined up by its latency, relative to a's peak (dB).
double differenceDb(const MatrixRun& a, const MatrixRun& b) {
  const size_t n = std::min(a.L.size() - std::min(a.L.size(), (size_t)a.latency), b.L.size() - std::min(b.L.size(), (size_t)b.latency));
  double peak = 0, diff = 0;
  for (size_t i = 0; i < n; ++i) {
    const size_t ia = i + (size_t)a.latency, ib = i + (size_t)b.latency;
    peak = std::max({peak, (double)std::fabs(a.L[ia]), (double)std::fabs(a.R[ia])});
    diff = std::max({diff, (double)std::fabs(a.L[ia] - b.L[ib]), (double)std::fabs(a.R[ia] - b.R[ib])});
  }
  if (diff == 0) return -200.0;
  return dbOf(diff) - dbOf(std::max(peak, 1e-9));
}

} // namespace

json pluginMatrix(PluginProvider& provider, const json& plugin, double sr, double tailWindow) {
  json out = {{"plugin", plugin.value("name", std::string("plugin"))}, {"version", plugin.value("version", std::string())},
              {"problems", json::array()}};
  const std::string id = plugin.value("id", std::string());
  int runs = 0;
  auto key = [&] { return "matrix:" + id + ":" + std::to_string(runs++); };
  const int64_t start = (int64_t)(sr / 10);
  const int64_t windowFrames = start + (int64_t)(sr / 2) + (int64_t)(tailWindow * sr);
  const int64_t signalFrames = (int64_t)(sr * 1.6);
  // Each run gets a fresh instance (offline: always new; live: a new key), released before the next.
  auto keep = [&] { provider.keepOnly({}); };

  const MatrixRun impulse = renderRun(provider, plugin, key(), sr, true, Input::Impulse, windowFrames, 0);
  keep();
  if (!impulse.problem.empty()) { out["problems"].push_back(impulse.problem); return out; }
  const bool instrument = impulse.instrument;
  out["instrument"] = instrument;
  out["installed"] = impulse.installed;
  const MatrixRun a = renderRun(provider, plugin, key(), sr, true, Input::Signal, signalFrames, 0);
  keep();
  const MatrixRun b = renderRun(provider, plugin, key(), sr, true, Input::Signal, signalFrames, 0);
  keep();
  const MatrixRun c = renderRun(provider, plugin, key(), sr, true, Input::Signal, signalFrames, 777);
  keep();
  const MatrixRun live = renderRun(provider, plugin, key(), sr, false, Input::Signal, signalFrames, 0);
  keep();
  for (const MatrixRun* r : {&a, &b, &c, &live})
    if (!r->problem.empty()) { out["problems"].push_back(r->problem); return out; }

  // Latency: the first sound after the impulse or note, over what was there before it (some plugins hiss).
  double floor = 0;
  for (int64_t i = 0; i < start && i < (int64_t)impulse.L.size(); ++i)
    floor = std::max({floor, (double)std::fabs(impulse.L[(size_t)i]), (double)std::fabs(impulse.R[(size_t)i])});
  const double threshold = std::max(floor * 4, 3.16e-5);  // -90 dBFS
  int64_t first = -1, last = -1;
  for (int64_t i = start; i < (int64_t)impulse.L.size(); ++i) {
    const double m = std::max(std::fabs(impulse.L[(size_t)i]), std::fabs(impulse.R[(size_t)i]));
    if (m > threshold) { if (first < 0) first = i; last = i; }
  }
  out["latency"] = {{"reported", impulse.latency}, {"measured", first < 0 ? json(nullptr) : json(first - start)},
                    {"changed", impulse.latencyChanged || a.latencyChanged || live.latencyChanged}};
  const int64_t inputEnd = instrument ? start + (int64_t)(sr / 2) : start + 1;
  out["tailSeconds"] = last < 0 ? 0.0 : std::max(0.0, (double)(last - inputEnd - impulse.latency) / sr);
  out["tailCapped"] = last >= 0 && last >= (int64_t)impulse.L.size() - (int64_t)(sr / 10);
  out["repeatDb"] = differenceDb(a, b);
  out["callSizesDb"] = differenceDb(a, c);
  out["realtimeDb"] = differenceDb(a, live);
  if (!instrument) {
    const MatrixRun mono = renderRun(provider, plugin, key(), sr, true, Input::LeftOnly, signalFrames, 0);
    keep();
    if (!mono.problem.empty()) out["problems"].push_back(mono.problem);
    double pl = 0, pr = 0;
    for (size_t i = 0; i < mono.L.size(); ++i) { pl = std::max(pl, (double)std::fabs(mono.L[i])); pr = std::max(pr, (double)std::fabs(mono.R[i])); }
    out["monoRightDb"] = dbOf(pr) - dbOf(std::max(pl, 1e-9));
  }
  out["cpuPct"] = 100.0 * (double)a.ns / 1e9 / ((double)signalFrames / sr);
  return out;
}

} // namespace core
