#include "groove.hpp"
#include "edit.hpp"
#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace core::doc {

namespace {

bool chosen(const std::set<std::string>& ids, const Note& n) { return ids.empty() || ids.count(n.id) > 0; }

int64_t keepInside(const Pattern& p, const Note& n, int64_t tick) {
  return std::clamp<int64_t>(tick, 0, std::max<int64_t>(0, p.lengthTicks - n.length));
}

} // namespace

int quantizeNotes(Pattern& pattern, const std::set<std::string>& ids, int64_t grid, double strength, double swing) {
  if (grid <= 0) throw DocumentError("quantize", "the grid must be positive");
  strength = std::clamp(strength, 0.0, 1.0);
  swing = std::clamp(swing, 0.0, 1.0);
  int moved = 0;
  for (auto& n : pattern.notes) {
    if (!chosen(ids, n)) continue;
    const int64_t step = (int64_t)std::llround((double)n.tick / (double)grid);
    int64_t target = step * grid;
    if (step % 2 != 0) target += (int64_t)std::llround(swing * (double)grid / 2.0);
    const int64_t tick = keepInside(pattern, n, n.tick + (int64_t)std::llround(strength * (double)(target - n.tick)));
    if (tick != n.tick) { n.tick = tick; ++moved; }
  }
  return moved;
}

int humanizeNotes(Pattern& pattern, const std::set<std::string>& ids, int64_t timingTicks, double velocity, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::uniform_real_distribution<double> unit(-1.0, 1.0);
  int changed = 0;
  for (auto& n : pattern.notes) {
    if (!chosen(ids, n)) continue;
    const double dt = unit(rng), dv = unit(rng);  // drawn for every note, so one note's result never depends on the others'
    const int64_t tick = keepInside(pattern, n, n.tick + (int64_t)std::llround(dt * (double)std::max<int64_t>(0, timingTicks)));
    const double v = std::clamp(std::round((n.velocity + dv * std::max(0.0, velocity)) * 100.0) / 100.0, 0.05, 1.0);
    if (tick != n.tick || v != n.velocity) ++changed;
    n.tick = tick;
    n.velocity = v;
  }
  return changed;
}

int varyNotes(Pattern& pattern, const std::set<std::string>& ids, const std::string& kind, uint64_t seed) {
  std::mt19937_64 rng(seed);
  std::vector<Note*> notes;
  for (auto& n : pattern.notes) if (chosen(ids, n)) notes.push_back(&n);
  std::sort(notes.begin(), notes.end(), [](const Note* a, const Note* b) { return a->tick < b->tick || (a->tick == b->tick && a->pitch < b->pitch); });
  if (notes.empty()) {
    if (kind != "octave" && kind != "reverse" && kind != "thin" && kind != "accent") throw DocumentError("variation", "unknown variation '" + kind + "'");
    return 0;
  }
  int changed = 0;
  if (kind == "octave") {
    for (Note* n : notes) {
      if (rng() % 4 != 0) continue;
      const int up = n->pitch + 12, down = n->pitch - 12;
      const int pitch = (rng() % 2 == 0 && up <= 127) || down < 0 ? up : down;
      if (pitch >= 0 && pitch <= 127) { n->pitch = pitch; ++changed; }
    }
  } else if (kind == "reverse") {
    // Each note ends where the mirror of its start is: the span keeps its place, the order turns round.
    const int64_t from = notes.front()->tick;
    int64_t to = 0;
    for (const Note* n : notes) to = std::max(to, n->tick + n->length);
    for (Note* n : notes) {
      const int64_t mirrored = keepInside(pattern, *n, from + to - (n->tick + n->length));
      if (mirrored != n->tick) { n->tick = mirrored; ++changed; }
    }
  } else if (kind == "thin") {
    std::set<std::string> gone;
    for (const Note* n : notes) if (rng() % 4 == 0) gone.insert(n->id);
    if (gone.size() == notes.size()) gone.erase(notes.front()->id);  // never all of them
    auto& all = pattern.notes;
    const size_t before = all.size();
    all.erase(std::remove_if(all.begin(), all.end(), [&](const Note& n) { return gone.count(n.id) > 0; }), all.end());
    changed = (int)(before - all.size());
  } else if (kind == "accent") {
    for (Note* n : notes) {
      const bool onBeat = n->tick % kPpq == 0;
      const double v = std::clamp(std::round((onBeat ? n->velocity + 0.15 : n->velocity - 0.1) * 100.0) / 100.0, 0.05, 1.0);
      if (v != n->velocity) { n->velocity = v; ++changed; }
    }
  } else {
    throw DocumentError("variation", "unknown variation '" + kind + "'");
  }
  return changed;
}

int transposeNotes(Pattern& pattern, const std::set<std::string>& ids, int semitones) {
  int changed = 0;
  for (auto& n : pattern.notes) {
    if (!chosen(ids, n) || semitones == 0) continue;
    const int pitch = n.pitch + semitones;
    if (pitch < 0 || pitch > 127) continue;
    n.pitch = pitch;
    ++changed;
  }
  return changed;
}

int addChordNotes(Pattern& pattern, const std::set<std::string>& ids, const std::vector<int>& intervals) {
  std::vector<Note> added;
  auto taken = [&](int64_t tick, int pitch) {
    for (const auto& n : pattern.notes) if (n.tick == tick && n.pitch == pitch) return true;
    for (const auto& n : added) if (n.tick == tick && n.pitch == pitch) return true;
    return false;
  };
  for (const auto& n : pattern.notes) {
    if (!chosen(ids, n)) continue;
    for (int step : intervals) {
      const int pitch = n.pitch + step;
      if (step == 0 || pitch < 0 || pitch > 127 || taken(n.tick, pitch)) continue;
      Note c = n;
      c.pitch = pitch;
      c.id.clear();
      added.push_back(c);
    }
  }
  for (auto& c : added) {
    c.id = newNoteId(pattern);
    pattern.notes.push_back(c);
  }
  return (int)added.size();
}

int setNoteLengths(Pattern& pattern, const std::set<std::string>& ids, const std::string& how) {
  if (how != "legato" && how != "half" && how != "double") throw DocumentError("length", "unknown length tool '" + how + "'");
  std::vector<int64_t> starts;
  for (const auto& n : pattern.notes) starts.push_back(n.tick);
  std::sort(starts.begin(), starts.end());
  int changed = 0;
  for (auto& n : pattern.notes) {
    if (!chosen(ids, n)) continue;
    const int64_t room = std::max<int64_t>(1, pattern.lengthTicks - n.tick);
    int64_t length = n.length;
    if (how == "legato") {
      const auto next = std::upper_bound(starts.begin(), starts.end(), n.tick);
      length = (next != starts.end() ? *next : pattern.lengthTicks) - n.tick;
    } else if (how == "half") {
      length = n.length / 2;
    } else {
      length = n.length * 2;
    }
    length = std::clamp<int64_t>(length, std::min<int64_t>(10, room), room);
    if (length != n.length) { n.length = length; ++changed; }
  }
  return changed;
}

int scaleVelocities(Pattern& pattern, const std::set<std::string>& ids, double factor) {
  int changed = 0;
  for (auto& n : pattern.notes) {
    if (!chosen(ids, n)) continue;
    const double v = std::clamp(std::round(n.velocity * std::max(0.0, factor) * 100.0) / 100.0, 0.05, 1.0);
    if (v != n.velocity) { n.velocity = v; ++changed; }
  }
  return changed;
}

} // namespace core::doc
