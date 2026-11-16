#include "time_range.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include "edit.hpp"

namespace core::doc {
namespace {

Track* trackOf(Document& doc, const std::string& id) {
  for (auto& t : doc.tracks) if (t.id == id) return &t;
  return nullptr;
}
bool isAudio(const Track& t) { return t.instrument.kind == "audio"; }

template <typename List>
std::string freeId(const List& items, const std::string& prefix) {
  long highest = 0;
  for (const auto& x : items)
    if (x.id.rfind(prefix, 0) == 0) {
      const std::string rest = x.id.substr(prefix.size());
      if (!rest.empty() && std::all_of(rest.begin(), rest.end(), [](char c) { return c >= '0' && c <= '9'; }) && rest.size() < 10)
        highest = std::max(highest, std::stol(rest));
    }
  return prefix + std::to_string(highest + 1);
}

// Cuts the track's clips that cross tick (a pattern clip continues its pattern, an audio clip its file), so that
// nothing sounds different. An audio piece shorter than the shortest clip is not cut.
void splitTrackAt(Document& doc, Track& t, int64_t tick) {
  std::vector<std::string> crossing;
  for (const auto& c : t.clips)
    if (c.startTick < tick && c.startTick + c.lengthTicks > tick) crossing.push_back(c.id);
  for (const auto& id : crossing) splitClip(t, id, tick);
  std::vector<std::string> audio;
  for (const auto& a : t.audioClips) {
    const double cut = secondsBetween(doc, (double)a.startTick, (double)tick);
    if (cut >= kMinClipSeconds && a.durationSeconds - cut >= kMinClipSeconds) audio.push_back(a.id);
  }
  const std::string trackId = t.id;
  for (const auto& id : audio) splitAudioClip(doc, trackId, id, tick);
}

double valueAt(const std::vector<AutomationPoint>& pts, int64_t tick) {
  if (pts.empty()) return 0;
  if (tick <= pts.front().tick) return pts.front().value;
  if (tick >= pts.back().tick) return pts.back().value;
  auto it = std::lower_bound(pts.begin(), pts.end(), tick, [](const AutomationPoint& p, int64_t v) { return p.tick < v; });
  if (it->tick == tick) return it->value;
  const auto& b = *it;
  const auto& a = *(it - 1);
  return a.value + (b.value - a.value) * double(tick - a.tick) / double(b.tick - a.tick);
}

void sortPoints(std::vector<AutomationPoint>& pts) {
  std::stable_sort(pts.begin(), pts.end(), [](const AutomationPoint& a, const AutomationPoint& b) { return a.tick < b.tick; });
  // Two points on one tick: the later one written wins.
  for (size_t i = 1; i < pts.size();)
    if (pts[i].tick == pts[i - 1].tick) pts.erase(pts.begin() + (long)(i - 1));
    else ++i;
}

// Removes the points in [from, to); the curve outside keeps its values (points at from - 1 and at to hold them).
void clearPoints(std::vector<AutomationPoint>& pts, int64_t from, int64_t to) {
  if (pts.empty()) return;
  std::vector<AutomationPoint> keep;
  if (from - 1 >= 0 && from - 1 > pts.front().tick && from - 1 < pts.back().tick) keep.push_back({from - 1, valueAt(pts, from - 1), json::object()});
  if (to > pts.front().tick && to < pts.back().tick) keep.push_back({to, valueAt(pts, to), json::object()});
  pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const AutomationPoint& p) { return p.tick >= from && p.tick < to; }), pts.end());
  for (auto& k : keep)
    if (std::none_of(pts.begin(), pts.end(), [&](const AutomationPoint& p) { return p.tick == k.tick; })) pts.push_back(k);
  sortPoints(pts);
}

int clearTrack(Document& doc, Track& t, int64_t from, int64_t to) {
  splitTrackAt(doc, t, from);
  splitTrackAt(doc, t, to);
  int gone = 0;
  const auto before = t.clips.size();
  t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [&](const Clip& c) { return c.startTick >= from && c.startTick < to; }), t.clips.end());
  gone += (int)(before - t.clips.size());
  const auto beforeAudio = t.audioClips.size();
  t.audioClips.erase(std::remove_if(t.audioClips.begin(), t.audioClips.end(), [&](const AudioClip& c) { return c.startTick >= from && c.startTick < to; }),
                     t.audioClips.end());
  gone += (int)(beforeAudio - t.audioClips.size());
  for (auto& a : t.automation) clearPoints(a.points, from, to);
  return gone;
}

}  // namespace

std::vector<std::string> tracksFrom(const Document& doc, const std::string& first, size_t count) {
  std::vector<std::string> out;
  bool on = false;
  for (const auto& t : doc.tracks) {
    if (t.id == first) on = true;
    if (on && out.size() < count) out.push_back(t.id);
  }
  return out;
}

RangeCopy copyRange(const Document& doc, const TimeRange& range) {
  RangeCopy out;
  if (range.empty()) return out;
  out.lengthTicks = range.length();
  Document scratch = doc;  // the cuts at the edges happen on a copy: the song itself does not change
  const int64_t from = range.fromTick, to = range.toTick;
  for (const auto& id : range.trackIds) {
    Track* t = trackOf(scratch, id);
    if (!t) continue;
    splitTrackAt(scratch, *t, from);
    splitTrackAt(scratch, *t, to);
    RangeCopy::Part part;
    part.trackId = id;
    part.audio = isAudio(*t);
    for (const auto& c : t->clips) {
      if (c.startTick < from || c.startTick >= to) continue;
      Clip x = c;
      x.startTick -= from;
      part.clips.push_back(x);
      if (std::none_of(part.patterns.begin(), part.patterns.end(), [&](const Pattern& p) { return p.id == c.patternId; }))
        if (const Pattern* p = t->findPattern(c.patternId)) part.patterns.push_back(*p);
    }
    for (const auto& c : t->audioClips) {
      if (c.startTick < from || c.startTick >= to) continue;
      AudioClip x = c;
      x.startTick -= from;
      part.audioClips.push_back(x);
    }
    for (const auto& a : t->automation) {
      if (a.points.empty()) continue;
      Automation x = a;
      x.points.clear();
      x.points.push_back({0, valueAt(a.points, from), json::object()});
      for (const auto& p : a.points)
        if (p.tick >= from && p.tick < to) x.points.push_back({p.tick - from, p.value, p.extra});
      x.points.push_back({to - 1 - from, valueAt(a.points, to - 1), json::object()});
      sortPoints(x.points);
      part.automation.push_back(std::move(x));
    }
    out.parts.push_back(std::move(part));
  }
  return out;
}

int clearRange(Document& doc, const TimeRange& range) {
  if (range.empty()) return 0;
  int gone = 0;
  for (const auto& id : range.trackIds)
    if (Track* t = trackOf(doc, id)) gone += clearTrack(doc, *t, range.fromTick, range.toTick);
  return gone;
}

RangePasteResult pasteRange(Document& doc, const RangeCopy& copy, int64_t at, const std::vector<std::string>& targets) {
  RangePasteResult result;
  at = std::max<int64_t>(0, at);
  if (copy.empty()) return result;
  const int64_t len = copy.lengthTicks, end = at + len;
  if (end > doc.lengthTicks) setSongLength(doc, end);  // throws past the longest song
  result.range.fromTick = at;
  result.range.toTick = end;
  for (size_t i = 0; i < copy.parts.size(); ++i) {
    const auto& part = copy.parts[i];
    Track* t = i < targets.size() ? trackOf(doc, targets[i]) : nullptr;
    const bool hasClips = !part.clips.empty() || !part.audioClips.empty();
    if (!t || (hasClips && (part.audio ? !isAudio(*t) : !playsPatterns(*t)))) { ++result.skippedTracks; continue; }
    result.range.trackIds.push_back(t->id);
    clearTrack(doc, *t, at, end);
    const bool home = t->id == part.trackId;
    // Patterns: on its own track the clips play the same patterns (while they are still there); elsewhere a copy.
    std::map<std::string, std::string> patternIds;
    for (const auto& p : part.patterns) {
      if (home && t->findPattern(p.id)) { patternIds[p.id] = p.id; continue; }
      if (t->patterns.size() >= 64) throw DocumentError("patterns", "the track '" + t->name + "' has the maximum of 64 patterns");
      Pattern x = p;
      x.id = freeId(t->patterns, "p");
      t->patterns.push_back(x);
      patternIds[p.id] = x.id;
    }
    const std::string trackId = t->id;
    for (const auto& c : part.clips) {
      Clip x = c;
      x.patternId = patternIds.count(c.patternId) ? patternIds[c.patternId] : c.patternId;
      pasteClip(*trackOf(doc, trackId), x, at + c.startTick);
      ++result.clips;
    }
    for (const auto& c : part.audioClips) {
      pasteAudioClip(doc, trackId, c, at + c.startTick);
      ++result.clips;
    }
    if (home)
      for (const auto& a : part.automation) {
        Track* tt = trackOf(doc, trackId);
        for (auto& lane : tt->automation) {
          if (lane.param != a.param) continue;
          clearPoints(lane.points, at, end);
          for (const auto& p : a.points) lane.points.push_back({at + p.tick, p.value, p.extra});
          sortPoints(lane.points);
        }
      }
  }
  return result;
}

TimeRange duplicateRange(Document& doc, const TimeRange& range) {
  if (range.empty()) return range;
  const RangeCopy copy = copyRange(doc, range);
  std::vector<std::string> targets;
  for (const auto& p : copy.parts) targets.push_back(p.trackId);
  return pasteRange(doc, copy, range.toTick, targets).range;
}

}  // namespace core::doc
