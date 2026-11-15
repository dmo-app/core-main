#include "pattern_sound.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include "core/assets.hpp"
#include "core/graph.hpp"
#include "core/project.hpp"

namespace core::preview {

namespace {

constexpr int64_t kBarTicks = 4 * doc::kPpq;

// The tracks a track's sound goes through or listens to: its output bus and its sends (and theirs), the
// tracks its effects' sidechains take their key from.
void collect(const doc::Document& d, const std::string& id, std::set<std::string>& out) {
  if (id.empty() || out.count(id)) return;
  const doc::Track* t = d.findTrack(id);
  if (!t) return;
  out.insert(id);
  collect(d, t->mix.output, out);
  for (const auto& s : t->mix.sends) collect(d, s.to, out);
  if (t->effects.is_array())
    for (const auto& fx : t->effects)
      if (fx.is_object() && fx.contains("sidechain") && fx["sidechain"].is_string()) {
        const std::string key = fx["sidechain"].get<std::string>();
        if (!out.count(key) && d.findTrack(key)) out.insert(key);  // a key only: it stays silent (no notes)
      }
}

} // namespace

std::unique_ptr<Request> makeRequest(const doc::Document& doc, const std::string& trackId, const std::string& patternId,
                                     const std::filesystem::path& assetRoot) {
  const doc::Track* source = doc.findTrack(trackId);
  if (!source || source->instrument.kind != "builtin") return nullptr;  // audio clips and buses play no notes
  const doc::Pattern* pattern = source->findPattern(patternId);
  if (!pattern || pattern->notes.empty() || pattern->lengthTicks <= 0) return nullptr;
  const int64_t L = pattern->lengthTicks, lead = std::min<int64_t>(L, 2 * kBarTicks);

  std::set<std::string> keep;
  collect(doc, trackId, keep);
  doc::Document d;
  d.name = "preview";
  d.bpm = doc.bpm;  // the song's first tempo (a later tempo change only stretches the picture a little)
  d.lengthTicks = lead + L;
  d.loop = {};
  for (const auto& t : doc.tracks) {
    if (!keep.count(t.id)) continue;
    doc::Track c = t;
    c.automation.clear();  // song-position curves: not what the pattern itself sounds like
    c.mix.solo = false;
    if (t.id == trackId) {
      c.mix.gainDb = 0;  // the window draws the fader as a gain
      c.mix.pan = 0;
      c.mix.mute = false;
      doc::Clip clip;
      clip.id = "preview";
      clip.patternId = patternId;
      clip.startTick = 0;
      clip.lengthTicks = lead + L;
      clip.offsetTicks = (L - lead) % L;  // the clip starts `lead` before the pattern's end
      c.clips = {clip};
    } else {
      // A bus plays what reaches it; a sidechain key is only listened to: no notes of its own.
      c.clips.clear();
      c.audioClips.clear();
      for (auto& p : c.patterns) p.notes.clear();
    }
    d.tracks.push_back(std::move(c));
  }
  auto r = std::make_unique<Request>();
  try {
    r->projection = doc::toProjection(d, assetRoot);
  } catch (const std::exception&) {
    return nullptr;
  }
  r->trackId = trackId;
  r->patternTicks = L;
  r->leadInTicks = lead;
  const std::string text = r->projection.dump();
  r->key = std::to_string(std::hash<std::string>{}(text)) + ":" + std::to_string(text.size());
  return r;
}

Sound render(const Request& request, AssetStore& assets, const std::atomic<bool>* cancel, double sampleRate) {
  Sound out;
  out.sampleRate = sampleRate;
  try {
    const Project project = fromJson(request.projection);
    int index = -1;
    for (size_t i = 0; i < project.tracks.size(); ++i)
      if (project.tracks[i].id == request.trackId) index = (int)i;
    if (index < 0) return out;
    Graph graph(project, sampleRate, &assets, nullptr, true);
    graph.setSolo(index, true);
    graph.preroll();
    const int64_t from = graph.sampleOfTick(request.leadInTicks), to = graph.sampleOfTick(request.leadInTicks + request.patternTicks);
    if (to <= from) return out;
    constexpr int kBlock = 4096;
    std::vector<float> L(kBlock), R(kBlock);
    const int fpp = out.framesPerPeak;
    float runPeak = 0, loudest = 0;
    double sumL = 0, sumR = 0;
    int inRun = 0;
    auto flush = [&] {
      if (inRun == 0) return;
      out.peaks.push_back(runPeak);
      out.rms.push_back((float)std::sqrt(std::max(sumL, sumR) / inRun));
      loudest = std::max(loudest, runPeak);
      runPeak = 0; sumL = sumR = 0; inRun = 0;
    };
    for (int64_t pos = 0; pos < to;) {
      if (cancel && cancel->load()) return Sound{};
      // Blocks end on the pattern's start, so the first kept sample is the pattern's first.
      const int n = (int)std::min<int64_t>(kBlock, (pos < from ? from : to) - pos);
      std::fill(L.begin(), L.begin() + n, 0.0f);
      std::fill(R.begin(), R.begin() + n, 0.0f);
      graph.process(L.data(), R.data(), n, pos, false);
      if (pos >= from)
        for (int i = 0; i < n; ++i) {
          const float l = std::isfinite(L[(size_t)i]) ? L[(size_t)i] : 0.0f, r = std::isfinite(R[(size_t)i]) ? R[(size_t)i] : 0.0f;
          runPeak = std::max({runPeak, std::fabs(l), std::fabs(r)});
          sumL += (double)l * l;
          sumR += (double)r * r;
          if (++inRun == fpp) flush();
        }
      pos += n;
    }
    flush();
    if (loudest < 1e-4f) return Sound{};  // silence (-80 dBFS): nothing to draw, the clip shows its notes
  } catch (const std::exception&) {
    return Sound{};
  }
  return out;
}

} // namespace core::preview
