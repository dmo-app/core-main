// Measurements for the engine: stability 190-500 BPM, kick/bass alignment, block-size
// independence, real-time timing with live edits. Output: one JSON report.
#include "measure.hpp"
#include "dsp/finite.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include "core/server.hpp"
#include "io/device.hpp"
#include "io/wav.hpp"

namespace core {

json demoProject(double bpm) {
  json kick = {{"id", "kick"}, {"name", "Kick"}, {"instrument", "kick"}, {"patternTicks", 4 * 4 * kPpq},
               {"params", {{"ampDecay", 170}}}, {"notes", json::array()}};
  for (int beat = 0; beat < 15; ++beat) kick["notes"].push_back({{"tick", beat * kPpq}, {"length", kPpq / 4}, {"pitch", 36}, {"velocity", 0.95}});
  // Last beat of every 4 bars: an 8-hit roll rising in velocity.
  kick["notes"].push_back({{"tick", 15 * kPpq}, {"length", kPpq}, {"pitch", 36}, {"velocity", 0.9},
                           {"roll", {{"count", 8}, {"velocityFrom", 0.35}, {"velocityTo", 1.0}}}});
  json bass = {{"id", "bass"}, {"name", "Bass"}, {"instrument", "wave"}, {"instrumentVersion", 2}, {"patternTicks", 4 * kPpq},
               {"params", {{"wave", 3}, {"tone", 400}, {"subLevel", 0.6}, {"unison", 1}, {"uniWidth", 0}, {"env1Attack", 1}, {"env1Decay", 70}, {"env1Sustain", 0.3}, {"env1Release", 12}}}, {"notes", json::array()},
               {"envelopes", {{{"param", "tone"}, {"points", {{{"tick", 0}, {"value", 160}}, {{"tick", 8 * 4 * kPpq}, {"value", 1400}}}}}}}};
  for (int beat = 0; beat < 4; ++beat)
    for (int sixteenth = 1; sixteenth < 4; ++sixteenth)
      bass["notes"].push_back({{"tick", beat * kPpq + sixteenth * kPpq / 4}, {"length", kPpq / 4 - 40}, {"pitch", 33}, {"velocity", 0.8}});
  json hats = {{"id", "hats"}, {"name", "Hats"}, {"instrument", "hats"}, {"patternTicks", kPpq}, {"gainDb", -4},
               {"notes", {{{"tick", kPpq / 4}, {"length", 60}, {"pitch", 42}, {"velocity", 0.5}},
                          {{"tick", kPpq / 2}, {"length", 120}, {"pitch", 46}, {"velocity", 0.8}},
                          {{"tick", 3 * kPpq / 4}, {"length", 60}, {"pitch", 42}, {"velocity", 0.5}}}}};
  return {{"format", kProjectFormat}, {"version", 2}, {"name", "Psy demo"}, {"bpm", bpm},
          {"lengthTicks", 8 * 4 * kPpq}, {"masterGainDb", -8}, {"tracks", {kick, bass, hats}}};
}

namespace {

json sweep(const std::string& outDir) {
  json rows = json::array();
  const double sr = 48000;
  int worstSched = 0; double worstPair = 0, worstKickOnset = 0; bool allFinite = true; float worstPeak = 0; double slowest = 1e9;
  for (int bpm = 190; bpm <= 500; bpm += 10) {
    Project p = fromJson(demoProject(bpm));
    EventLog log;
    RenderResult r = renderProject(p, sr, 512, true, 1.0, &log);
    const double spt = samplesPerTick(bpm, sr);
    // 1. Scheduling: every note-on vs the exact (fractional) grid position.
    double maxSched = 0;
    for (auto& e : log.entries) maxSched = std::max(maxSched, std::fabs(e.sample - e.tick * spt));
    // 2. Kick/bass pairs: distance from each kick to the bass notes of the same beat vs the ideal distance.
    double maxPair = 0;
    for (auto& e : log.entries) {
      if (e.track != 1) continue;
      int64_t beatTick = e.tick / kPpq * kPpq;
      double real = e.sample - (double)std::llround(beatTick * spt);
      double ideal = (e.tick - beatTick) * spt;
      maxPair = std::max(maxPair, std::fabs(real - ideal));
    }
    // 3. Audio: the kick stem must start sounding on the scheduled sample.
    double maxOnset = 0;
    const auto& kickL = r.stemL[0];
    for (auto& e : log.entries) {
      if (e.track != 0) continue;
      int64_t s = e.sample, found = -1;
      for (int64_t i = std::max<int64_t>(0, s - 64); i < std::min<int64_t>((int64_t)kickL.size(), s + 64); ++i) {
        // ignore the previous kick's fade-out: look for the click burst rising above the tail
        if (i >= s && std::fabs(kickL[i]) > 1e-4) { found = i; break; }
      }
      if (found >= 0) maxOnset = std::max(maxOnset, (double)(found - s));
    }
    bool finite = true; double dc = 0;
    for (size_t i = 0; i < r.L.size(); ++i) { if (!finiteBits(r.L[i]) || !finiteBits(r.R[i])) finite = false; dc += r.L[i]; }
    dc /= std::max<size_t>(1, r.L.size());
    RenderResult single = renderProject(p, sr, 128, false, 1.0);  // one pass, no stems, real-time block size
    const double rt = (single.L.size() / sr) * 1000 / std::max(0.001, single.renderMs);
    rows.push_back({{"bpm", bpm}, {"notes", log.entries.size()}, {"maxScheduleErrorSamples", maxSched},
                    {"maxKickBassErrorSamples", maxPair}, {"maxKickOnsetSamples", maxOnset}, {"finite", finite},
                    {"peak", r.peak}, {"dc", dc}, {"renderRealtimeFactor", rt}});
    worstSched = std::max(worstSched, (int)std::ceil(maxSched * 1000)); worstPair = std::max(worstPair, maxPair);
    worstKickOnset = std::max(worstKickOnset, maxOnset); allFinite = allFinite && finite; worstPeak = std::max(worstPeak, r.peak);
    slowest = std::min(slowest, rt);
    if (!outDir.empty() && (bpm == 190 || bpm == 300 || bpm == 500)) {
      writeWav(outDir + "/demo-" + std::to_string(bpm) + ".wav", r.L, r.R, (int)sr, 24);
    }
  }
  return {{"rows", rows}, {"summary", {{"bpmRange", {190, 500}}, {"allFinite", allFinite}, {"maxScheduleErrorSamples", worstSched / 1000.0},
          {"maxKickBassErrorSamples", worstPair}, {"maxKickOnsetSamples", worstKickOnset}, {"maxPeak", worstPeak},
          {"slowestRenderRealtimeFactor", slowest}}}};
}

json blockIndependence() {
  json out = json::array();
  for (double bpm : {190.0, 333.0, 500.0}) {
    Project p = fromJson(demoProject(bpm));
    RenderResult ref = renderProject(p, 48000, 512);
    for (int block : {1, 32, 64, 128, 333, 1024}) {
      RenderResult r = renderProject(p, 48000, block);
      double diff = 0;
      for (size_t i = 0; i < ref.L.size(); ++i) diff = std::max(diff, (double)std::fabs(ref.L[i] - r.L[i]));
      out.push_back({{"bpm", bpm}, {"block", block}, {"maxDifference", diff}});
    }
  }
  return out;
}

struct AudioCheck { float peak = 0; int64_t clipped = 0; bool finite = true; float maxStep = 0; };
AudioCheck checkAudio(const std::vector<float>& L, const std::vector<float>& R, int64_t n) {
  AudioCheck c;
  for (int64_t i = 0; i < n; ++i) {
    float l = L[i], r = R[i];
    if (!finiteBits(l) || !finiteBits(r)) { c.finite = false; continue; }
    c.peak = std::max({c.peak, std::fabs(l), std::fabs(r)});
    if (std::fabs(l) > 1.0f || std::fabs(r) > 1.0f) ++c.clipped;
    if (i > 0) c.maxStep = std::max({c.maxStep, std::fabs(l - L[i - 1]), std::fabs(r - R[i - 1])});
  }
  return c;
}

// Largest sample-to-sample jump right around each project update, relative to the largest jump the
// same music has without edits. Above 1 = a jump bigger than anything in the music itself (a click).
double clickRatio(const std::vector<float>& L, int64_t n, const std::vector<int64_t>& swaps, float baselineStep) {
  float worst = 0;
  for (int64_t at : swaps)
    for (int64_t i = std::max<int64_t>(1, at - 8); i < std::min<int64_t>(n, at + 64); ++i)
      worst = std::max(worst, std::fabs(L[i] - L[i - 1]));
  return baselineStep > 0 ? worst / baselineStep : 0;
}

// One real-time case: plays the 190 BPM demo from the start through the device and records the output.
// edit: "none" | "filter" | "bpm".
json liveCase(const json& opts, const std::string& edit, float baselineStep, std::vector<float>* keepL = nullptr) {
  Server server(makeMiniaudioOutput);
  json r = server.handle({{"cmd", "project.load"}, {"args", {{"project", demoProject(190)}}}});
  if (!r["ok"]) return r;
  json start = server.handle({{"cmd", "engine.start"}, {"args", opts}});
  if (!start["ok"]) return start;
  const json device = start["result"];
  const double sr = device.value("sampleRate", 48000.0);
  const double seconds = opts.value("seconds", 8.0);
  Engine& engine = server.engine();
  engine.startCapture((int64_t)(seconds * sr));
  server.handle({{"cmd", "engine.resetStats"}});
  server.handle({{"cmd", "transport.play"}});
  const auto t0 = std::chrono::steady_clock::now();
  int edits = 0, refused = 0;
  while (engine.captured() < (int64_t)(seconds * sr)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(edit == "bpm" ? 1000 : 100));
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds((int)seconds + 5)) break;  // device stalled
    if (edit == "filter") {
      // A live edit every 100 ms. The engine has no instrument of its own any more (its parameters are gone): the
      // kick's level moves instead, on a track that still has notes, and a refused edit is counted (sweep: the old
      // edit used parameters that did not exist and ignored the refusal).
      const json r = server.handle({{"cmd", "track.set"}, {"args", {{"trackId", "kick"}, {"patch", {{"gainDb", edits % 2 ? -3.0 : 0.0}}}}}});
      if (!r.value("ok", false)) ++refused;
      ++edits;
    } else if (edit == "bpm") {
      server.handle({{"cmd", "tempo.set"}, {"args", {{"bpm", edits % 2 ? 190 : 200}}}});
      ++edits;
    }
  }
  engine.stopCapture();
  json stats = server.handle({{"cmd", "engine.stats"}})["result"];
  server.handle({{"cmd", "engine.stop"}});
  const int64_t n = engine.captured();
  const auto& L = engine.captureL();
  const auto& R = engine.captureR();
  AudioCheck c = checkAudio(L, R, n);
  std::vector<int64_t> swaps = engine.captureSwaps();
  json out = {{"case", edit == "none" ? "live, no edits" : edit == "filter" ? "live, filter only (200<->1200 Hz every 100 ms)" : "live, BPM only (190<->200 every 1 s)"},
              {"sampleRate", sr}, {"requestedBlock", opts.value("blockSize", 128)}, {"deviceBlock", device.value("blockSize", 0)},
              {"callbackFrames", {stats["minCallbackFrames"], stats["maxCallbackFrames"]}},
              {"latencyMs", device.value("latencyMs", 0.0)}, {"exclusive", device.value("exclusive", false)},
              {"backend", device.value("backend", "")}, {"device", device.value("device", "")},
              {"seconds", n / sr}, {"edits", edits}, {"refusedEdits", refused}, {"peak", c.peak}, {"clippedSamples", c.clipped}, {"finite", c.finite},
              {"overruns", stats["overruns"]}, {"starvations", stats["starvations"]},
              {"maxLoadPercent", stats["maxRenderMs"].get<double>() / (std::max(1.0, (double)stats["maxCallbackFrames"].get<uint64_t>()) * 1000.0 / sr) * 100},
              {"maxEditToAudioMs", stats["maxEditToAudioMs"]}};
  if (edit == "none") {
    // The live output must be exactly the offline render of the same project.
    Project p = fromJson(demoProject(190));
    RenderResult ref = renderProject(p, sr, 512, false, 0);
    double diff = 0;
    for (int64_t i = 0; i < std::min<int64_t>(n, (int64_t)ref.L.size()); ++i) diff = std::max(diff, (double)std::fabs(ref.L[i] - L[i]));
    out["maxDifferenceFromOffline"] = diff;
    out["maxStep"] = c.maxStep;
  } else {
    // The first swap is the project load, skip it.
    std::vector<int64_t> editSwaps(swaps.size() > 1 ? swaps.begin() + 1 : swaps.end(), swaps.end());
    out["updatesHeard"] = editSwaps.size();
    out["clickRatio"] = clickRatio(L, n, editSwaps, baselineStep);
  }
  if (keepL) keepL->assign(L.begin(), L.begin() + n);
  return out;
}

json fourCases(const json& opts, const std::string& outDir) {
  json cases = json::array();
  // 1. Offline WAV at 190 BPM, no edits: synthesis, mix and render.
  Project p = fromJson(demoProject(190));
  RenderResult r = renderProject(p, 48000, 512, false, 1.0);
  AudioCheck c = checkAudio(r.L, r.R, (int64_t)r.L.size());
  if (!outDir.empty()) writeWav(outDir + "/case1-offline-190.wav", r.L, r.R, 48000, 24);
  cases.push_back({{"case", "offline WAV, 190 BPM, no edits"}, {"sampleRate", 48000}, {"peak", c.peak},
                   {"clippedSamples", c.clipped}, {"finite", c.finite}, {"maxStep", c.maxStep}});
  // 2-4. Live.
  json live = liveCase(opts, "none", 0);
  const float baseline = live.value("maxStep", 0.0f);
  cases.push_back(live);
  cases.push_back(liveCase(opts, "filter", baseline));
  cases.push_back(liveCase(opts, "bpm", baseline));
  return cases;
}

} // namespace

json runMeasure(const std::string& outDir, const json& realtime) {
  json report;
  report["sweep"] = sweep(outDir);
  report["blockIndependence"] = blockIndependence();
  const uint64_t t0 = nowNs();
  for (int i = 0; i < 100; ++i) Graph g(fromJson(demoProject(500)), 48000);
  report["graphRebuildMs"] = (nowNs() - t0) / 1e6 / 100;
  if (!realtime.is_null()) report["cases"] = fourCases(realtime, outDir);
  return report;
}

} // namespace core
