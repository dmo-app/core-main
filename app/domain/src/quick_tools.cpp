#include "quick_tools.hpp"
#include <algorithm>
#include <cstdio>
#include "edit.hpp"
#include "pattern_rack.hpp"

namespace core::doc {

namespace {
constexpr int64_t kBar = 4 * kPpq;
constexpr int64_t kSixteenth = kPpq / 4;
const char* kFrozenKey = "frozen";          // on the source: the audio track it plays from
const char* kFrozenFromKey = "frozenFrom";  // on the audio track: the source
const char* kFrozenPrintKey = "frozenPrint";  // on the source: what the render heard (freezePrint)

bool mixerParam(const std::string& param) { return param == "gainDb" || param == "pan"; }

// What a freeze render of the track hears, as text: its saved form without the mixer (fader, pan, sends, mute, the
// mixer's automation) and without the freeze's own marks, the tracks that key its sidechains, and the tempo and the
// length of the song.
std::string freezePrint(const Document& doc, const std::string& trackId) {
  const Track* t = doc.findTrack(trackId);
  if (!t) return {};
  Document one = doc;
  Track clean = *t;
  clean.mix = Mix{};
  clean.automation.erase(std::remove_if(clean.automation.begin(), clean.automation.end(),
                                        [](const Automation& a) { return mixerParam(a.param); }),
                         clean.automation.end());
  if (clean.extra.is_object()) { clean.extra.erase(kFrozenKey); clean.extra.erase(kFrozenPrintKey); }
  one.tracks = {clean};
  for (const auto& fx : t->effects)
    if (fx.is_object() && fx.contains("sidechain") && fx["sidechain"].is_string())
      if (const Track* key = doc.findTrack(fx["sidechain"].get<std::string>())) one.tracks.push_back(*key);
  const json saved = toJson(one);
  json out = saved["tracks"];
  // The tempo, not the rest of the transport (the loop is not in a render).
  const json transport = saved.value("transport", json::object());
  out.push_back({{"lengthTicks", doc.lengthTicks}, {"bpm", transport.value("bpm", json())}, {"tempoMap", transport.value("tempoMap", json())}});
  return out.dump();
}

uint64_t fnv1a(const std::string& text) {  // the same on every compiler (std::hash is not)
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : text) { h ^= c; h *= 1099511628211ull; }
  return h;
}
std::string printHash(const Document& doc, const std::string& trackId) {
  char buf[17];
  std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)fnv1a(freezePrint(doc, trackId)));
  return buf;
}

void sortNotes(Pattern& p) {
  std::stable_sort(p.notes.begin(), p.notes.end(), [](const Note& a, const Note& b) { return a.tick < b.tick; });
}

int64_t wrap(int64_t v, int64_t len) { return ((v % len) + len) % len; }

std::string extraString(const json& extra, const char* key) {
  return extra.is_object() && extra.contains(key) && extra[key].is_string() ? extra[key].get<std::string>() : std::string();
}
} // namespace

// ---- sections

int64_t nextSectionStart(const Document& doc, int64_t tick) {
  int64_t best = -1;
  for (const auto& s : doc.sections)
    if (s.startTick > tick && s.startTick < doc.lengthTicks && (best < 0 || s.startTick < best)) best = s.startTick;
  return best;
}

int64_t previousSectionStart(const Document& doc, int64_t tick, int64_t slackTicks) {
  int64_t best = -1;
  const int64_t before = tick - std::max<int64_t>(0, slackTicks);
  for (const auto& s : doc.sections)
    if (s.startTick < before && s.startTick > best) best = s.startTick;
  return best;
}

Loop sectionLoop(const Document& doc, const std::string& sectionId) {
  for (const auto& s : doc.sections)
    if (s.id == sectionId) {
      Loop l = doc.loop;
      l.enabled = true;
      l.startTick = s.startTick;
      l.endTick = std::min(s.startTick + s.lengthTicks, doc.lengthTicks);
      if (l.endTick <= l.startTick) throw DocumentError("sections", "the section starts after the song's end");
      return l;
    }
  throw DocumentError("sections", "no section '" + sectionId + "'");
}

const Section* sectionAt(const Document& doc, int64_t tick) {
  const Section* at = nullptr;
  for (const auto& s : doc.sections)
    if (tick >= s.startTick && tick < s.startTick + s.lengthTicks) at = &s;
  return at;
}

// ---- Patterns window

int doubleRackSlot(Document& doc, const std::string& slotId) {
  const auto slots = rackSlots(doc);
  const PatternSlot* slot = findRackSlot(slots, slotId);
  if (!slot) throw DocumentError("patternSlots", "no pattern '" + slotId + "'");
  if (slot->bars >= kRackMaxBars) throw DocumentError("bars", "the pattern is already " + std::to_string(kRackMaxBars) + " bars long");
  const int bars = slot->bars * 2;
  // All or nothing: a pattern that would pass the notes limit stops the whole Double (it would stop half-filled).
  for (const auto& [trackId, pid] : slot->tracks)
    for (const auto& t : doc.tracks)
      if (t.id == trackId)
        if (const Pattern* p = t.findPattern(pid); p && p->lengthTicks > 0 && p->lengthTicks < (int64_t)bars * kBar) {
          const size_t after = p->notes.size() * (size_t)(((int64_t)bars * kBar + p->lengthTicks - 1) / p->lengthTicks);
          if (after > kMaxPatternNotes)
            throw DocumentError("notes", "'" + t.name + "' would pass " + std::to_string(kMaxPatternNotes) + " notes: nothing doubled");
        }
  setRackBars(doc, slotId, bars);  // the notes repeat into the new half
  return bars;
}

int rotatePatternNotes(Pattern& pattern, int64_t deltaTicks, const std::set<std::string>& noteIds) {
  const int64_t len = pattern.lengthTicks;
  if (len <= 0 || deltaTicks % len == 0) return 0;
  int moved = 0;
  for (auto& n : pattern.notes) {
    if (!noteIds.empty() && !noteIds.count(n.id)) continue;
    n.tick = wrap(n.tick + deltaTicks, len);
    ++moved;
  }
  sortNotes(pattern);
  return moved;
}

int shiftRackRow(Document& doc, const std::string& slotId, const std::string& trackId, int steps) {
  if (steps == 0) return 0;
  const auto slots = rackSlots(doc);
  const PatternSlot* slot = findRackSlot(slots, slotId);
  if (!slot) throw DocumentError("patternSlots", "no pattern '" + slotId + "'");
  const auto it = slot->tracks.find(trackId);
  Track* t = nullptr;
  for (auto& tr : doc.tracks) if (tr.id == trackId) t = &tr;
  if (!t) throw DocumentError("tracks", "no track '" + trackId + "'");
  if (it == slot->tracks.end() || !t->findPattern(it->second)) return 0;  // an empty row
  // Under swing: only this row moves, each swung step re-swung where it lands (pattern_rack.hpp: shiftPatternSteps).
  return shiftPatternSteps(patternById(trackById(doc, trackId), it->second), steps, slot->swing);
}

// ---- nudge

int nudgeClips(Document& doc, const std::vector<ClipRef>& clips, int64_t deltaTicks) {
  if (clips.empty() || deltaTicks == 0) return 0;
  Document next = doc;  // all or nothing
  int64_t lastEnd = 0;
  for (const auto& r : clips) {
    Track& t = trackById(next, r.trackId);
    if (r.audio) {
      const auto c = std::find_if(t.audioClips.begin(), t.audioClips.end(), [&](const AudioClip& a) { return a.id == r.clipId; });
      if (c == t.audioClips.end()) throw DocumentError("clips", "no audio clip '" + r.clipId + "'");
      if (c->startTick + deltaTicks < 0) throw DocumentError("clips", "a clip would start before the song");
      moveAudioClip(next, r.trackId, r.clipId, c->startTick + deltaTicks);
      for (const auto& a : trackById(next, r.trackId).audioClips)
        if (a.id == r.clipId) lastEnd = std::max(lastEnd, audioClipEndTick(next, a));
    } else {
      const auto c = std::find_if(t.clips.begin(), t.clips.end(), [&](const Clip& a) { return a.id == r.clipId; });
      if (c == t.clips.end()) throw DocumentError("clips", "no clip '" + r.clipId + "'");
      if (c->startTick + deltaTicks < 0) throw DocumentError("clips", "a clip would start before the song");
      const int64_t start = c->startTick + deltaTicks, len = c->lengthTicks;
      moveClip(t, r.clipId, start);
      lastEnd = std::max(lastEnd, start + len);
    }
  }
  if (lastEnd > next.lengthTicks) setSongLength(next, (lastEnd + kBar - 1) / kBar * kBar);
  doc = std::move(next);
  return (int)clips.size();
}

int nudgeNotes(Pattern& pattern, const std::set<std::string>& noteIds, int64_t deltaTicks) {
  if (noteIds.empty() || deltaTicks == 0) return 0;
  int moved = 0;
  for (const auto& n : pattern.notes)
    if (noteIds.count(n.id)) {
      if (n.tick + deltaTicks < 0 || n.tick + deltaTicks >= pattern.lengthTicks)
        throw DocumentError("notes", "a note would leave the pattern");
      ++moved;
    }
  for (auto& n : pattern.notes) if (noteIds.count(n.id)) n.tick += deltaTicks;
  sortNotes(pattern);
  return moved;
}

// ---- freeze

std::string frozenInto(const Track& track) { return extraString(track.extra, kFrozenKey); }
bool isFrozen(const Track& track) { return !frozenInto(track).empty(); }

std::string cannotFreeze(const Document& doc, const std::string& trackId) {
  const Track* t = doc.findTrack(trackId);
  if (!t) return "There is no such track.";
  if (isFrozen(*t)) return "The track is already frozen.";
  if (t->instrument.kind == "audio") return "An audio track has no plugins to freeze.";
  if (t->instrument.kind == "bus") return "A bus cannot be frozen: freeze the tracks that feed it.";
  bool plugins = t->instrument.kind == "plugin";
  for (const auto& fx : t->effects) plugins = plugins || (fx.is_object() && fx.value("type", std::string()) == "plugin");
  if (!plugins) return "The track has no plugins: there is nothing to freeze.";
  if (t->clips.empty()) return "The track plays nothing in the song.";
  for (const auto& o : doc.tracks)
    for (const auto& fx : o.effects)
      if (fx.is_object() && fx.contains("sidechain") && fx["sidechain"] == trackId)
        return "This track is the sidechain key of " + o.name +
               ": frozen, its plugins are unloaded and that sidechain would go silent. Remove the sidechain first.";
  return cannotBounce(doc, trackId);
}

Printed freezeTrack(Document& doc, const std::string& trackId, const std::filesystem::path& file,
                    const std::filesystem::path& projectDir) {
  if (const std::string why = cannotFreeze(doc, trackId); !why.empty()) throw DocumentError("tracks", why);
  Document next = doc;
  const Track before = *next.findTrack(trackId);
  Printed out = placeBounce(next, trackId, file, projectDir, 0, next.lengthTicks);
  Track& printed = trackById(next, out.trackId);
  const std::string bounce = " (bounce)", frozen = " (frozen)";  // the same length: the name still fits
  if (printed.name.size() >= bounce.size() && printed.name.compare(printed.name.size() - bounce.size(), bounce.size(), bounce) == 0)
    printed.name.replace(printed.name.size() - bounce.size(), bounce.size(), frozen);
  if (!printed.extra.is_object()) printed.extra = json::object();
  printed.extra[kFrozenFromKey] = trackId;
  // The render is pre-fader: the mixer moves to the audio track and stays live (a bounce bakes it, a freeze does not).
  printed.mix = before.mix;
  for (const auto& a : before.automation) if (mixerParam(a.param)) printed.automation.push_back(a);
  Track& source = trackById(next, trackId);  // placeBounce muted it
  source.automation.erase(std::remove_if(source.automation.begin(), source.automation.end(),
                                         [](const Automation& a) { return mixerParam(a.param); }),
                          source.automation.end());
  if (!source.extra.is_object()) source.extra = json::object();
  source.extra[kFrozenKey] = out.trackId;
  source.extra[kFrozenPrintKey] = printHash(next, trackId);
  checkRouting(next);  // the sends and the output moved to the audio track: still no loop
  doc = std::move(next);
  return out;
}

bool freezeStillMatches(const Document& start, const Document& now, const std::string& trackId) {
  if (!start.findTrack(trackId) || !now.findTrack(trackId)) return false;
  return freezePrint(start, trackId) == freezePrint(now, trackId);
}

void unfreezeTrack(Document& doc, const std::string& trackId) {
  Document next = doc;
  Track& source = trackById(next, trackId);
  const std::string audio = frozenInto(source);
  if (audio.empty()) throw DocumentError("tracks", "the track is not frozen");
  source.extra.erase(kFrozenKey);
  source.extra.erase(kFrozenPrintKey);
  source.mix.mute = false;
  // Only the audio this freeze made: the user may have deleted it, and its id may now be another track's freeze.
  if (const Track* a = next.findTrack(audio); a && extraString(a->extra, kFrozenFromKey) == trackId) {
    // The mixer comes back from the audio track, with what was done to it while frozen.
    source.mix = a->mix;
    for (const auto& au : a->automation) if (mixerParam(au.param)) source.automation.push_back(au);
    removeTrack(next, audio);
  }
  doc = std::move(next);
}

std::vector<std::string> unfreezeStale(Document& doc) {
  std::vector<std::string> names;
  for (size_t i = 0; i < doc.tracks.size(); ++i) {
    const Track& t = doc.tracks[i];
    if (!isFrozen(t)) continue;
    const std::string kept = extraString(t.extra, kFrozenPrintKey);
    if (kept.empty() || kept == printHash(doc, t.id)) continue;  // a song frozen before the fingerprint: kept
    names.push_back(t.name);
    unfreezeTrack(doc, t.id);
    i = (size_t)-1;  // the list changed: from the start (each pass unfreezes one, so this ends)
  }
  return names;
}

} // namespace core::doc
