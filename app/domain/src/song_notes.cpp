#include "song_notes.hpp"
#include <algorithm>
#include <set>
#include "edit.hpp"

namespace core::doc {

namespace {
int64_t floorMod(int64_t a, int64_t m) { const int64_t r = a % m; return r < 0 ? r + m : r; }

Track& notesTrackOf(Document& doc, const std::string& trackId) {
  Track& t = trackById(doc, trackId);
  if (!playsPatterns(t) || t.patterns.empty()) throw DocumentError("notes", "'" + t.name + "' plays no notes");
  return t;
}

Note& noteIn(Pattern& p, const std::string& noteId) {
  for (auto& n : p.notes) if (n.id == noteId) return n;
  throw DocumentError("notes", "no such note");
}
}  // namespace

const Clip* clipAt(const Track& track, int64_t tick) {
  const Clip* found = nullptr;
  for (const auto& c : track.clips)
    if (tick >= c.startTick && tick < c.startTick + c.lengthTicks) found = &c;
  return found;
}

int64_t patternTickIn(const Clip& clip, const Pattern& pattern, int64_t songTick) {
  const int64_t len = std::max<int64_t>(1, pattern.lengthTicks);
  return floorMod(songTick - clip.startTick + clip.offsetTicks, len);
}

std::vector<SongNote2> songNotesIn(const Document& doc, const std::string& trackId, int64_t from, int64_t to) {
  std::vector<SongNote2> out;
  const Track* t = doc.findTrack(trackId);
  if (!t || !playsPatterns(*t) || to <= from) return out;
  for (const auto& c : t->clips) {
    const Pattern* p = t->findPattern(c.patternId);
    if (!p || p->notes.empty() || p->lengthTicks <= 0) continue;
    const int64_t len = p->lengthTicks, clipEnd = c.startTick + c.lengthTicks;
    const int64_t a = std::max(from, c.startTick), b = std::min(to, clipEnd);
    if (a >= b) continue;
    const int64_t origin = c.startTick - c.offsetTicks;  // song tick of pattern tick 0 in the first repeat
    for (int64_t k = (a - origin) / len - 1; origin + k * len < b; ++k) {
      for (const auto& n : p->notes) {
        const int64_t s = origin + k * len + n.tick;
        if (s < a || s >= b) continue;
        out.push_back({c.id, p->id, n.id, s, std::max<int64_t>(1, std::min(n.length, clipEnd - s)), n.pitch, n.velocity});
      }
    }
  }
  std::sort(out.begin(), out.end(), [](const SongNote2& x, const SongNote2& y) {
    return x.start != y.start ? x.start < y.start : x.pitch < y.pitch;
  });
  return out;
}

std::pair<std::string, std::string> addSongNote(Document& doc, const std::string& trackId, int64_t at, int64_t length,
                                                int pitch, double velocity) {
  if (pitch < 0 || pitch > 127) throw DocumentError("notes", "pitch out of range");
  if (length < 1) throw DocumentError("notes", "length must be at least 1 tick");
  if (at < 0) throw DocumentError("notes", "before the song's start");
  Track& t = notesTrackOf(doc, trackId);
  std::string patternId;
  int64_t tick = 0;
  if (const Clip* c = clipAt(t, at)) {
    patternId = c->patternId;
    tick = patternTickIn(*c, patternById(t, patternId), at);
  } else {
    const int64_t bar = 4 * kPpq, start = at / bar * bar;
    patternId = addPattern(t, bar);
    addClip(t, patternId, start, bar);
    tick = at - start;
  }
  Pattern& p = patternById(t, patternId);
  Note n;
  n.id = newNoteId(p);
  n.tick = tick;
  n.length = std::max<int64_t>(1, std::min(length, p.lengthTicks - tick));
  n.pitch = pitch;
  n.velocity = std::clamp(velocity, 0.01, 1.0);
  p.notes.push_back(n);
  return {patternId, n.id};
}

void moveSongNote(Document& doc, const std::string& trackId, const std::string& patternId, const std::string& noteId,
                  int64_t ticks, int semitones) {
  Pattern& p = patternById(notesTrackOf(doc, trackId), patternId);
  Note& n = noteIn(p, noteId);
  n.tick = floorMod(n.tick + ticks, std::max<int64_t>(1, p.lengthTicks));
  n.length = std::max<int64_t>(1, std::min(n.length, p.lengthTicks - n.tick));
  n.pitch = std::clamp(n.pitch + semitones, 0, 127);
}

void resizeSongNote(Document& doc, const std::string& trackId, const std::string& patternId, const std::string& noteId,
                    int64_t length) {
  Pattern& p = patternById(notesTrackOf(doc, trackId), patternId);
  Note& n = noteIn(p, noteId);
  n.length = std::max<int64_t>(1, std::min(length, p.lengthTicks - n.tick));
}

void setSongNoteVelocity(Document& doc, const std::string& trackId, const std::string& patternId,
                         const std::string& noteId, double velocity) {
  noteIn(patternById(notesTrackOf(doc, trackId), patternId), noteId).velocity = std::clamp(velocity, 0.01, 1.0);
}

void removeSongNote(Document& doc, const std::string& trackId, const std::string& patternId, const std::string& noteId) {
  Pattern& p = patternById(notesTrackOf(doc, trackId), patternId);
  const auto it = std::find_if(p.notes.begin(), p.notes.end(), [&](const Note& n) { return n.id == noteId; });
  if (it == p.notes.end()) throw DocumentError("notes", "no such note");
  p.notes.erase(it);
}

std::vector<int> songPitchesIn(const Document& doc, const std::string& trackId, int64_t from, int64_t to) {
  std::set<int> s;
  for (const auto& n : songNotesIn(doc, trackId, from, to)) s.insert(n.pitch);
  return {s.begin(), s.end()};
}

}  // namespace core::doc
