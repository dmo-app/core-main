#include "pattern_tools.hpp"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <random>
#include "edit.hpp"

namespace core::doc {

namespace {


// Draws from the generator's raw output (fixed by the standard), never from std:: distributions, whose results
// differ between libraries: the same seed gives the same notes on every build.
struct Draw {
  std::mt19937_64 rng;
  explicit Draw(uint64_t seed) : rng(seed) {}
  double unit() { return (double)(rng() >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)
};

long noteNumber(const std::string& id) { return id.size() > 1 && id[0] == 'n' ? idNumber(id.c_str() + 1) : 0; }

struct Ids {  // new note ids n<highest + 1>, n<highest + 2>...
  long next = 1;
  explicit Ids(const Pattern& p) { for (const auto& n : p.notes) next = std::max(next, noteNumber(n.id) + 1); }
  std::string operator()() { return "n" + std::to_string(next++); }
};

double vel(double v) { return std::clamp(std::round(v * 100.0) / 100.0, 0.05, 1.0); }

bool startsIn(const NoteScope& s, const Note& n) {
  return n.tick >= s.start && n.tick < s.end && (s.ids.empty() || s.ids.count(n.id) > 0);
}

void sortNotes(std::vector<Note*>& v) {
  std::sort(v.begin(), v.end(), [](const Note* a, const Note* b) { return a->tick < b->tick || (a->tick == b->tick && a->pitch < b->pitch); });
}

std::string str(const json& j, const char* key) { return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : ""; }

bool validPresetId(const std::string& id) {
  if (id.empty() || id.size() > 80 || id.front() == '/' || id.back() == '/' || id.find("..") != std::string::npos) return false;
  return std::all_of(id.begin(), id.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '/'; });
}

}  // namespace

NoteScope wholePattern(const Pattern& pattern) { return {0, pattern.lengthTicks, {}}; }

std::vector<Note*> notesIn(Pattern& pattern, const NoteScope& scope) {
  std::vector<Note*> v;
  for (auto& n : pattern.notes) if (startsIn(scope, n)) v.push_back(&n);
  sortNotes(v);
  return v;
}

// ---------------------------------------------------------------------------------------------------------------
int normalizeNotes(Pattern& pattern, const NoteScope& scope, const NormalizeOptions& o) {
  if (o.grid <= 0) throw DocumentError("normalize", "the grid must be positive");
  if (o.velocity != "keep" && o.velocity != "equal" && o.velocity != "accent")
    throw DocumentError("normalize", "velocity must be keep, equal or accent");
  auto notes = notesIn(pattern, scope);
  if (notes.empty()) return 0;
  double base = o.velocityValue;
  if (base < 0) {
    double sum = 0;
    for (const Note* n : notes) sum += n->velocity;
    base = sum / (double)notes.size();
  }
  const int64_t end = std::min(scope.end, pattern.lengthTicks);
  std::set<const Note*> changed;
  if (o.align)
    for (Note* n : notes) {
      const int64_t t = std::clamp<int64_t>((int64_t)std::llround((double)n->tick / (double)o.grid) * o.grid, scope.start,
                                            std::max(scope.start, end - 1));
      if (t != n->tick) { n->tick = t; changed.insert(n); }
    }
  if (o.velocity != "keep")
    for (Note* n : notes) {
      double v = base;
      if (o.velocity == "accent" && !o.accents.empty()) {
        const int64_t inBeat = ((n->tick % kPpq) + kPpq) % kPpq;
        const size_t step = (size_t)(inBeat / std::max<int64_t>(1, kPpq / 4));
        v = base * o.accents[step % o.accents.size()];
      }
      v = vel(v);
      if (v != n->velocity) { n->velocity = v; changed.insert(n); }
    }
  if (o.length) {
    const int64_t want = std::max<int64_t>(1, (int64_t)std::llround(std::clamp(o.gate, 0.01, 4.0) * (double)o.grid));
    sortNotes(notes);
    for (Note* n : notes) {
      int64_t len = std::min(want, pattern.lengthTicks - n->tick);
      for (const auto& m : pattern.notes)  // never over the next note of the same pitch
        if (&m != n && m.pitch == n->pitch && m.tick > n->tick) len = std::min(len, m.tick - n->tick);
      len = std::max<int64_t>(1, len);
      if (len != n->length) { n->length = len; changed.insert(n); }
    }
  }
  return (int)changed.size();
}

// ---------------------------------------------------------------------------------------------------------------
DensityResult changeDensity(Pattern& pattern, const NoteScope& scope, const DensityOptions& o) {
  if (o.grid <= 0) throw DocumentError("density", "the grid must be positive");
  static const std::set<std::string> modes = {"percent", "count", "target", "perBeat"}, hows = {"fill", "ghost", "ratchet", "octave"};
  if (!modes.count(o.mode)) throw DocumentError("density", "mode must be percent, count, target or perBeat");
  if (!hows.count(o.how)) throw DocumentError("density", "how must be fill, ghost, ratchet or octave");
  if (!std::isfinite(o.amount)) throw DocumentError("density", "the amount must be a number");
  const int64_t end = std::min(scope.end, pattern.lengthTicks);
  DensityResult r;
  if (end <= scope.start) { r.note = "the scope is empty"; return r; }
  auto notes = notesIn(pattern, scope);
  const int n = (int)notes.size();
  r.before = n;
  long target = n;
  if (o.mode == "percent") {
    if (o.amount != 0) {
      const long delta = std::max<long>(1, std::lround(std::fabs(o.amount) * n / 100.0));
      target = n == 0 ? 0 : (o.amount > 0 ? n + delta : n - delta);
      if (n == 0) r.note = "no notes to take a percentage of: use a count or notes per beat";
    }
  } else if (o.mode == "count") {
    target = n + std::lround(o.amount);
  } else if (o.mode == "target") {
    target = std::lround(o.amount);
  } else {  // perBeat: note starts per quarter note
    target = std::lround(o.amount * (double)(end - scope.start) / (double)kPpq);
  }
  target = std::clamp<long>(target, 0, (long)kMaxPatternNotes);
  r.target = (int)target;
  Draw draw(o.seed);
  Ids newId(pattern);

  if (target < n) {  // remove: off-beat notes first, in a seeded order; the notes on the beat last
    std::vector<std::pair<double, Note*>> order;
    for (Note* m : notes) order.push_back({(m->tick % kPpq == 0 ? 1.0 : 0.0) + draw.unit() * 0.999, m});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::set<std::string> gone;
    for (long i = 0; i < n - target; ++i) gone.insert(order[(size_t)i].second->id);
    auto& all = pattern.notes;
    all.erase(std::remove_if(all.begin(), all.end(), [&](const Note& m) { return gone.count(m.id) > 0; }), all.end());
    r.removed = (int)gone.size();
    r.after = n - r.removed;
    return r;
  }
  long want = target - n;
  if (want <= 0) { r.after = n; return r; }
  want = std::min<long>(want, (long)(kMaxPatternNotes - std::min(kMaxPatternNotes, pattern.notes.size())));

  std::vector<Note> added;
  const int fallbackPitch = o.pitch >= 0 ? std::clamp(o.pitch, 0, 127) : 36;
  auto before = [&](int64_t tick) -> const Note* {  // the scope's note sounding or last started before tick
    const Note* best = nullptr;
    for (const Note* m : notes) if (m->tick <= tick) best = m;
    if (!best && !notes.empty()) best = notes.front();
    return best;
  };

  if (o.how == "fill" || o.how == "ghost") {
    // Empty grid steps: no note of the scope starts in [step, step + grid). Off-beat steps first (the beat is
    // the kick's), then a seeded order: weighted keys u^(1/w).
    std::vector<std::pair<double, int64_t>> steps;
    const int64_t first = ((scope.start + o.grid - 1) / o.grid) * o.grid;
    for (int64_t t = first; t < end; t += o.grid) {
      bool used = false;
      for (const Note* m : notes) if (m->tick >= t && m->tick < t + o.grid) { used = true; break; }
      if (used) continue;
      const double w = t % kPpq == 0 ? 0.25 : (t % (kPpq / 2) == 0 ? 0.8 : 1.0);
      steps.push_back({std::pow(std::max(1e-12, draw.unit()), 1.0 / w), t});
    }
    std::sort(steps.begin(), steps.end(), [](const auto& a, const auto& b) { return a.first > b.first || (a.first == b.first && a.second < b.second); });
    if ((long)steps.size() < want) r.note = "only " + std::to_string(steps.size()) + " empty steps in the scope";
    for (long i = 0; i < want && i < (long)steps.size(); ++i) {
      const int64_t t = steps[(size_t)i].second;
      const Note* near = before(t);
      Note m;
      m.id = newId();
      m.tick = t;
      m.pitch = near ? near->pitch : fallbackPitch;
      const double v = near ? near->velocity : 0.8;
      m.velocity = vel(o.how == "ghost" ? v * 0.4 : v * 0.85);
      const int64_t len = o.how == "ghost" ? o.grid / 2 : (near ? std::min(near->length, o.grid * 9 / 10) : o.grid * 9 / 10);
      m.length = std::max<int64_t>(1, std::min({len, end - t, pattern.lengthTicks - t}));
      added.push_back(m);
    }
  } else if (o.how == "octave") {
    std::vector<std::pair<double, Note*>> order;
    for (Note* m : notes) if (m->pitch + 12 <= 127) order.push_back({draw.unit(), m});
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::set<std::pair<int64_t, int>> taken;
    for (const auto& m : pattern.notes) taken.insert({m.tick, m.pitch});
    for (const auto& [key, m] : order) {
      if ((long)added.size() >= want) break;
      if (taken.count({m->tick, m->pitch + 12})) continue;
      Note c = *m;
      c.id = newId();
      c.pitch = m->pitch + 12;
      c.velocity = vel(m->velocity * 0.85);
      c.hasRoll = false;
      added.push_back(c);
    }
    if ((long)added.size() < want) r.note = "only " + std::to_string(added.size()) + " notes can get an octave copy";
  } else {  // ratchet: split notes into k equal repeats, the longest notes first; k grows (2, 3, 4) until enough
    std::vector<Note*> order;
    for (Note* m : notes) if (m->length >= 4) order.push_back(m);
    std::stable_sort(order.begin(), order.end(), [](const Note* a, const Note* b) { return a->length > b->length; });
    std::map<std::string, int> parts;  // note id -> repeats
    long extra = 0;
    for (int k = 2; k <= 4 && extra < want; ++k)
      for (Note* m : order) {
        if (extra >= want) break;
        if (m->length / k < 2) continue;
        int& p = parts[m->id];
        if (p == 0) p = 1;
        if (p != k - 1) continue;
        p = k;
        ++extra;
      }
    for (Note* m : order) {
      auto it = parts.find(m->id);
      if (it == parts.end() || it->second < 2) continue;
      const int k = it->second;
      const int64_t piece = m->length / k;
      for (int i = 1; i < k; ++i) {
        Note c = *m;
        c.id = newId();
        c.tick = m->tick + piece * i;
        c.length = std::max<int64_t>(1, piece * 9 / 10);
        c.velocity = vel(m->velocity * (1.0 - 0.12 * i));
        c.hasRoll = false;
        added.push_back(c);
      }
      m->length = std::max<int64_t>(1, piece * 9 / 10);
    }
    if (extra < want) r.note = "only " + std::to_string(extra) + " repeats fit in these notes";
  }
  for (auto& m : added) pattern.notes.push_back(std::move(m));
  r.added = (int)added.size();
  r.after = n + r.added;
  return r;
}

// ---------------------------------------------------------------------------------------------------------------
int makeBuildup(Pattern& pattern, const BuildupOptions& o) {
  if (o.length < kPpq) throw DocumentError("buildup", "a build-up is at least one beat long");
  if (o.start < 0 || o.start >= pattern.lengthTicks) throw DocumentError("buildup", "the build-up starts outside the pattern");
  if (o.pitch < 0 || o.pitch > 127) throw DocumentError("buildup", "pitch must be 0-127");
  const int64_t end = std::min(o.start + o.length, pattern.lengthTicks);
  const int64_t playEnd = std::max(o.start, end - std::max<int64_t>(0, o.gapTicks));
  if (o.replace)
    pattern.notes.erase(std::remove_if(pattern.notes.begin(), pattern.notes.end(),
                                       [&](const Note& n) { return n.tick >= o.start && n.tick < end; }),
                        pattern.notes.end());
  Ids newId(pattern);
  int written = 0;
  auto put = [&](int64_t t, int64_t gap) {
    Note n;
    n.id = newId();
    n.tick = t;
    const double x = (double)(t - o.start) / (double)std::max<int64_t>(1, playEnd - o.start);
    n.pitch = std::clamp(o.pitch + (int)std::lround(o.riseFrom + (o.riseTo - o.riseFrom) * x), 0, 127);
    n.velocity = vel(o.velocityFrom + (o.velocityTo - o.velocityFrom) * x);
    n.length = std::max<int64_t>(1, std::min({gap * 3 / 4, playEnd - t, pattern.lengthTicks - t}));
    pattern.notes.push_back(n);
    ++written;
  };
  if (o.shape == "gradual") {
    if (o.rateFrom < 1 || o.rateTo < 1) throw DocumentError("buildup", "the roll's rates are at least one tick");
    // The gap shrinks geometrically with the position, so the roll speeds up evenly to the ear.
    const double span = (double)std::max<int64_t>(1, playEnd - o.start), ratio = (double)o.rateTo / (double)o.rateFrom;
    for (int64_t t = o.start; t < playEnd;) {
      if (pattern.notes.size() >= kMaxPatternNotes) return written;
      const int64_t gap = std::max<int64_t>(1, std::llround((double)o.rateFrom * std::pow(ratio, (double)(t - o.start) / span)));
      put(t, gap);
      t += gap;
    }
    return written;
  }
  if (o.shape != "steps") throw DocumentError("buildup", "shape is steps or gradual");
  // Four parts of the build-up: 1/4, 1/8, 1/16, then 1/32 (roll) or 1/16 again.
  const int64_t quarter = std::max<int64_t>(kPpq / 8, (end - o.start) / 4);
  const int64_t rates[4] = {kPpq, kPpq / 2, kPpq / 4, o.roll ? kPpq / 8 : kPpq / 4};
  for (int part = 0; part < 4; ++part) {
    const int64_t from = o.start + quarter * part, to = part == 3 ? playEnd : std::min(playEnd, o.start + quarter * (part + 1));
    for (int64_t t = from; t < to; t += rates[part]) {
      if (pattern.notes.size() >= kMaxPatternNotes) return written;
      put(t, rates[part]);
    }
  }
  return written;
}

// ---------------------------------------------------------------------------------------------------------------
PatternPreset presetFromJson(const json& j) {
  if (!j.is_object() || str(j, "format") != "core-pattern") throw DocumentError("format", "not a core-pattern");
  if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != 1)
    throw DocumentError("version", "only core-pattern version 1 is known");
  PatternPreset p;
  p.id = str(j, "id");
  if (!validPresetId(p.id)) throw DocumentError("id", "lowercase letters, digits, '-' and '/', 1-80");
  p.name = str(j, "name");
  if (p.name.empty() || p.name.size() > 80) throw DocumentError("name", "1-80 characters");
  p.role = str(j, "role");
  p.description = str(j, "description");
  p.source = str(j, "source");
  p.status = j.contains("status") ? str(j, "status") : "hypothesis";
  if (p.status != "draft" && p.status != "hypothesis" && p.status != "approved") throw DocumentError("status", "draft, hypothesis or approved");
  p.approval = str(j, "approval");
  if (p.status == "approved" && p.approval.empty()) throw DocumentError("approval", "an approved pattern says what Diogo said");
  for (const char* key : {"styles", "tags"}) {
    auto& out = std::string(key) == "styles" ? p.styles : p.tags;
    if (j.contains(key)) {
      if (!j[key].is_array()) throw DocumentError(key, "must be a list of names");
      for (const auto& s : j[key]) if (s.is_string()) out.push_back(s.get<std::string>());
    }
  }
  if (!j.contains("lengthTicks") || !j["lengthTicks"].is_number_integer()) throw DocumentError("lengthTicks", "a whole number of ticks");
  p.lengthTicks = j["lengthTicks"].get<int64_t>();
  if (p.lengthTicks < kPpq / 4 || p.lengthTicks > 64 * 4 * kPpq) throw DocumentError("lengthTicks", "1/16 to 64 bars");
  p.root = j.contains("root") && j["root"].is_number_integer() ? j["root"].get<int>() : -1;
  if (p.root > 127) throw DocumentError("root", "0-127, or -1 for drums");
  if (!j.contains("notes") || !j["notes"].is_array() || j["notes"].size() > kMaxPatternNotes) throw DocumentError("notes", "a list of up to 4096 notes");
  int i = 0;
  for (const auto& nj : j["notes"]) {
    const std::string at = "notes[" + std::to_string(i++) + "]";
    if (!nj.is_object() || !nj.contains("tick") || !nj.contains("pitch") || !nj["tick"].is_number_integer() || !nj["pitch"].is_number_integer())
      throw DocumentError(at, "needs tick and pitch");
    Note n;
    n.id = "n" + std::to_string(i);
    n.tick = nj["tick"].get<int64_t>();
    n.pitch = nj["pitch"].get<int>();
    n.length = nj.contains("length") && nj["length"].is_number_integer() ? nj["length"].get<int64_t>() : 240;
    n.velocity = nj.contains("velocity") && nj["velocity"].is_number() ? nj["velocity"].get<double>() : 0.8;
    if (n.tick < 0 || n.tick >= p.lengthTicks) throw DocumentError(at + ".tick", "inside the pattern");
    if (n.pitch < 0 || n.pitch > 127) throw DocumentError(at + ".pitch", "0-127");
    if (n.length < 1) throw DocumentError(at + ".length", "at least 1 tick");
    if (!(n.velocity > 0 && n.velocity <= 1)) throw DocumentError(at + ".velocity", "above 0, at most 1");
    p.notes.push_back(n);
  }
  return p;
}

json presetToJson(const PatternPreset& p) {
  json notes = json::array();
  for (const auto& n : p.notes) notes.push_back({{"tick", n.tick}, {"length", n.length}, {"pitch", n.pitch}, {"velocity", n.velocity}});
  json j = {{"format", "core-pattern"}, {"version", 1}, {"id", p.id}, {"name", p.name}, {"role", p.role}, {"styles", p.styles},
            {"tags", p.tags}, {"status", p.status}, {"description", p.description}, {"source", p.source},
            {"lengthTicks", p.lengthTicks}, {"root", p.root}, {"notes", notes}};
  if (!p.approval.empty()) j["approval"] = p.approval;  // an approved pattern reads back (sweep: it was dropped)
  return j;
}

std::vector<PatternPreset> loadPatternLibrary(const std::vector<PatternRoot>& roots, std::vector<std::string>* problems) {
  std::map<std::string, PatternPreset> byId;
  for (const auto& root : roots) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root.folder, ec)) continue;
    std::vector<std::filesystem::path> files;
    for (auto it = std::filesystem::recursive_directory_iterator(root.folder, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
      if (it->is_regular_file() && it->path().extension() == ".json") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
      try {
        std::ifstream in(f, std::ios::binary);
        PatternPreset p = presetFromJson(json::parse(in));
        p.origin = root.kind;
        p.file = f;
        byId.emplace(p.id, std::move(p));  // the first root that has the id wins
      } catch (const std::exception& e) {
        if (problems) problems->push_back(f.filename().string() + ": " + e.what());
      }
    }
  }
  std::vector<PatternPreset> out;
  for (auto& [id, p] : byId) out.push_back(std::move(p));
  return out;
}

PatternPreset savePatternPreset(const std::filesystem::path& folder, PatternPreset p, const std::vector<PatternPreset>& existing) {
  if (!validPresetId(p.id)) throw DocumentError("id", "lowercase letters, digits, '-' and '/', 1-80");
  presetFromJson(presetToJson(p));  // the same checks as a file read back
  auto used = [&](const std::string& id) {
    if (std::any_of(existing.begin(), existing.end(), [&](const PatternPreset& e) { return e.id == id; })) return true;
    std::error_code ec;
    return std::filesystem::exists(folder / (id + ".json"), ec);
  };
  const std::string base = p.id;
  for (int k = 2; used(p.id); ++k) {
    const std::string suffix = "-" + std::to_string(k);  // the id stays within 80 (sweep: it could pass and not read back)
    p.id = base.substr(0, 80 - suffix.size()) + suffix;
  }
  p.file = folder / (p.id + ".json");
  std::filesystem::create_directories(p.file.parent_path());
  {
    std::ofstream out(p.file, std::ios::binary);
    out << presetToJson(p).dump(2) << "\n";
    if (!out) throw DocumentError("file", "could not write " + p.file.string());
  }
  p.origin = "user";
  return p;
}

PatternPreset presetFromPattern(const Pattern& pattern, int64_t start, int64_t end, const std::string& name) {
  end = std::min(end, pattern.lengthTicks);
  if (end <= start) throw DocumentError("range", "the range is empty");
  PatternPreset p;
  p.name = name;
  p.status = "draft";
  p.lengthTicks = end - start;
  int lowest = 128, i = 0;
  for (const auto& n : pattern.notes) {
    if (n.tick < start || n.tick >= end) continue;
    Note c = n;
    c.id = "n" + std::to_string(++i);
    c.tick = n.tick - start;
    c.length = std::min(n.length, end - n.tick);
    c.hasRoll = false;
    c.extra = json::object();
    p.notes.push_back(c);
    lowest = std::min(lowest, n.pitch);
  }
  std::sort(p.notes.begin(), p.notes.end(), [](const Note& a, const Note& b) { return a.tick < b.tick || (a.tick == b.tick && a.pitch < b.pitch); });
  p.root = lowest <= 127 ? lowest : -1;
  return p;
}

int trackRoot(const Track& t, const Pattern* pattern) {
  if (t.instrument.name == "kick" || t.instrument.name == "hats" || t.instrument.name == "snare" || t.instrument.name == "drums") return -1;
  if (pattern && !pattern->notes.empty()) {
    std::map<int, int> uses;
    for (const auto& n : pattern->notes) ++uses[n.pitch];
    int best = -1, most = 0;
    for (const auto& [pitch, k] : uses) if (k > most) { best = pitch; most = k; }
    return best;
  }
  if (t.instrument.kind == "builtin" && t.instrument.name == "sampler" && !t.instrument.samples.empty()) return t.instrument.samples.front().rootKey;
  return -1;
}

std::vector<Note> fitPreset(const PatternPreset& preset, int64_t lengthTicks, int toRoot) {
  std::vector<Note> out;
  if (lengthTicks <= 0 || preset.lengthTicks <= 0) return out;
  int shift = preset.root >= 0 && toRoot >= 0 ? toRoot - preset.root : 0;
  if (preset.root < 0 && toRoot >= 0 && !preset.notes.empty() &&
      std::all_of(preset.notes.begin(), preset.notes.end(), [&](const Note& n) { return n.pitch == preset.notes.front().pitch; }))
    shift = toRoot - preset.notes.front().pitch;
  int i = 0;
  for (int64_t at = 0; at < lengthTicks && out.size() < kMaxPatternNotes; at += preset.lengthTicks)
    for (const auto& n : preset.notes) {
      const int64_t t = at + n.tick;
      if (t >= lengthTicks || out.size() >= kMaxPatternNotes) continue;
      const int pitch = n.pitch + shift;
      if (pitch < 0 || pitch > 127) continue;
      Note c = n;
      c.id = "n" + std::to_string(++i);
      c.tick = t;
      c.pitch = pitch;
      c.length = std::max<int64_t>(1, std::min(n.length, lengthTicks - t));
      out.push_back(c);
    }
  return out;
}

int applyPreset(Pattern& pattern, const PatternPreset& preset, int64_t start, int64_t lengthTicks, const std::string& mode, int toRoot) {
  if (mode != "replace" && mode != "add") throw DocumentError("mode", "replace or add");
  if (start < 0 || start >= pattern.lengthTicks) throw DocumentError("start", "inside the pattern");
  lengthTicks = std::min(lengthTicks, pattern.lengthTicks - start);
  const int64_t end = start + lengthTicks;
  if (mode == "replace")
    pattern.notes.erase(std::remove_if(pattern.notes.begin(), pattern.notes.end(), [&](const Note& n) { return n.tick >= start && n.tick < end; }),
                        pattern.notes.end());
  std::set<std::pair<int64_t, int>> taken;
  for (const auto& n : pattern.notes) taken.insert({n.tick, n.pitch});
  Ids newId(pattern);
  int written = 0;
  for (auto n : fitPreset(preset, lengthTicks, toRoot)) {
    if (pattern.notes.size() >= kMaxPatternNotes) break;
    n.tick += start;
    if (!taken.insert({n.tick, n.pitch}).second) continue;
    n.id = newId();
    n.length = std::max<int64_t>(1, std::min(n.length, pattern.lengthTicks - n.tick));
    pattern.notes.push_back(n);
    ++written;
  }
  return written;
}

// ---------------------------------------------------------------------------------------------------------------
PatternPreset stretchPreset(const PatternPreset& preset, double speed) {
  if (!(speed >= 0.25 && speed <= 4)) throw DocumentError("speed", "0.25 to 4");
  if (speed == 1) return preset;
  PatternPreset out = preset;
  const double k = 1.0 / speed;
  out.lengthTicks = std::clamp<int64_t>(std::llround((double)preset.lengthTicks * k), kPpq / 4, 64LL * 4 * kPpq);
  out.notes.clear();
  for (const auto& n : preset.notes) {
    Note m = n;
    m.tick = std::llround((double)n.tick * k);
    m.length = std::max<int64_t>(1, std::llround((double)n.length * k));
    if (m.tick < out.lengthTicks) out.notes.push_back(m);
  }
  return out;
}

void checkPCore(const PCoreSettings& s) {
  auto in = [](double v, double lo, double hi, const char* what) {
    if (!(v >= lo && v <= hi)) throw DocumentError(std::string("pcore.") + what, "must be " + std::to_string((int)lo) + " to " + std::to_string((int)hi));
  };
  if (s.mode != "create" && s.mode != "alter") throw DocumentError("pcore.mode", "create or alter");
  if (s.mode == "create" && s.templateId.empty()) throw DocumentError("pcore.template", "names a pattern of the library");
  in(s.root, -1, 127, "root");
  in(s.speed, 0.25, 4, "speed");
  in(s.density, -90, 200, "density");
  if (s.densityHow != "fill" && s.densityHow != "ghost" && s.densityHow != "ratchet" && s.densityHow != "octave")
    throw DocumentError("pcore.densityHow", "fill, ghost, ratchet or octave");
  in(s.gate, 0, 1, "gate");
  in(s.accent, 0, 1, "accent");
  if (s.accentShape != "kbbb" && s.accentShape != "rising") throw DocumentError("pcore.accentShape", "kbbb or rising");
  in(s.pitchChance, 0, 1, "pitchChance");
  in(s.pitchLow, -1, 127, "pitchLow");
  in(s.pitchHigh, -1, 127, "pitchHigh");
  if ((s.pitchLow < 0) != (s.pitchHigh < 0) || s.pitchHigh < s.pitchLow) throw DocumentError("pcore.pitchLow", "a range low..high, or both -1");
  if (s.roll != "off" && s.roll != "steps" && s.roll != "gradual") throw DocumentError("pcore.roll", "off, steps or gradual");
  in(s.rollFrom, 1, 128, "rollFrom");
  in(s.rollTo, 1, 128, "rollTo");
  in(s.dropBars, 0, 64, "dropBars");
  in(s.gapBeats, 0, 4, "gapBeats");
  in(s.rollPitch, -1, 127, "rollPitch");
  in(s.rollRise, 0, 12, "rollRise");
}

int pcoreApply(Pattern& pattern, const NoteScope& scope, const PCoreSettings& s, const std::vector<PatternPreset>& library, int trackKey) {
  checkPCore(s);
  const int64_t maxTicks = 64LL * 4 * kPpq;
  if (scope.start < 0 || scope.end <= scope.start || scope.end > maxTicks) throw DocumentError("pcore", "choose the bars to write (up to 64)");
  const PatternPreset* found = nullptr;
  for (const auto& p : library) if (p.id == s.templateId) found = &p;
  if (s.mode == "create" && !found) throw DocumentError("pcore.template", "'" + s.templateId + "' is not in the pattern library");
  const PatternPreset stretched = found ? stretchPreset(*found, s.speed) : PatternPreset{};
  const PatternPreset* preset = found ? &stretched : nullptr;
  if (pattern.lengthTicks < scope.end) pattern.lengthTicks = scope.end;  // the bars asked exist afterwards
  const int key = s.root >= 0 ? s.root : trackKey;
  const int64_t bar = 4 * kPpq;
  const bool rolling = s.roll != "off";
  // The transition's points, inside the range: build-up <= roll <= drop (the end).
  int64_t rollAt = !rolling ? scope.end : s.rollStart >= 0 ? s.rollStart : s.buildupStart >= 0 ? scope.end : scope.start;
  int64_t buildAt = !rolling || s.buildupStart < 0 ? rollAt : s.buildupStart;
  if (rolling && (buildAt < scope.start || rollAt < buildAt || rollAt > scope.end))
    throw DocumentError("pcore.points", "build-up, roll and drop go in that order, inside the bars chosen");
  const PatternPreset* drop = nullptr;
  if (rolling && !s.dropTemplate.empty()) {
    for (const auto& p : library) if (p.id == s.dropTemplate) drop = &p;
    if (!drop) throw DocumentError("pcore.dropTemplate", "'" + s.dropTemplate + "' is not in the pattern library");
    if (s.dropBars < 1) throw DocumentError("pcore.dropBars", "the drop pattern needs 1-64 bars");
    if (scope.end + (int64_t)s.dropBars * bar > maxTicks) throw DocumentError("pcore.dropBars", "the drop goes past 64 bars");
    pattern.lengthTicks = std::max(pattern.lengthTicks, scope.end + (int64_t)s.dropBars * bar);
  }
  NoteScope body{scope.start, buildAt, s.mode == "alter" ? scope.ids : std::set<std::string>{}};
  if (s.mode == "create") {
    pattern.notes.erase(std::remove_if(pattern.notes.begin(), pattern.notes.end(),
                                       [&](const Note& n) { return n.tick >= scope.start && n.tick < scope.end; }),
                        pattern.notes.end());
    if (body.end > body.start) applyPreset(pattern, *preset, body.start, body.end - body.start, "add", key);
  } else if (s.root >= 0 && trackKey >= 0 && body.end > body.start) {  // alter: a key asked moves the notes there
    for (Note* n : notesIn(pattern, body)) n->pitch = std::clamp(n->pitch + s.root - trackKey, 0, 127);
  }
  const int fallback = key >= 0 ? key : (preset && preset->root >= 0 ? preset->root : 36);
  if (body.end > body.start) {
    if (s.density != 0) {
      DensityOptions d;
      d.mode = "percent";
      d.amount = s.density;
      d.how = s.densityHow;
      d.seed = s.seed;
      d.pitch = fallback;
      changeDensity(pattern, body, d);
    }
    if (s.gate > 0 || s.accent > 0) {
      NormalizeOptions n;
      n.velocity = s.accent > 0 ? "accent" : "keep";
      if (s.accentShape == "rising") n.accents = {0.62, 0.74, 0.87, 1.0};
      for (auto& a : n.accents) a = 1.0 - s.accent * (1.0 - a);
      n.length = s.gate > 0;
      n.gate = std::max(0.1, s.gate);
      n.grid = kPpq / 4;
      normalizeNotes(pattern, body, n);
    }
    if (s.pitchChance > 0 && s.pitchLow >= 0) {  // chaos: its own draws (the density's do not move them)
      Draw draw(s.seed * 0x9E3779B97F4A7C15ULL + 77);
      NoteScope chaos = body;
      if (s.mode == "create") chaos.ids.clear();
      for (Note* n : notesIn(pattern, chaos)) {
        const double u = draw.unit(), v = draw.unit();
        if (u < s.pitchChance) n->pitch = s.pitchLow + std::min(s.pitchHigh - s.pitchLow, (int)(v * (s.pitchHigh - s.pitchLow + 1)));
      }
    }
  }
  if (rolling) {
    const int pitch = s.rollPitch >= 0 ? s.rollPitch : (key >= 0 ? key : (preset && preset->root >= 0 ? preset->root : 38));
    const int64_t gap = (int64_t)s.gapBeats * kPpq, end = scope.end;
    const int64_t playEnd = std::max(rollAt, end - gap);
    // The build-up's velocity runs into the roll's: one rise from buildAt to the drop.
    const double split = (double)(rollAt - buildAt) / (double)std::max<int64_t>(1, playEnd - buildAt);
    const double rise = trackKey >= 0 ? s.rollRise : 0, riseAt = rise * std::min(1.0, split);
    pattern.notes.erase(std::remove_if(pattern.notes.begin(), pattern.notes.end(),
                                       [&](const Note& n) { return n.tick >= buildAt && n.tick < end; }),
                        pattern.notes.end());
    if (rollAt - buildAt >= kPpq) {  // the build-up: 1/4, 1/8, then 1/16 over its last half
      BuildupOptions b;
      b.start = buildAt;
      b.length = rollAt - buildAt;
      b.shape = "steps";
      b.roll = false;
      b.pitch = pitch;
      b.velocityFrom = 0.35;
      b.velocityTo = 0.35 + 0.65 * std::min(1.0, split);
      b.riseTo = riseAt;
      b.replace = false;
      makeBuildup(pattern, b);
    }
    if (end - rollAt >= kPpq) {  // the roll ("steps": a plain 1/32 all the way)
      BuildupOptions b;
      b.start = rollAt;
      b.length = end - rollAt;
      b.gapTicks = gap;
      b.shape = "gradual";
      b.rateFrom = s.roll == "gradual" ? std::max<int64_t>(1, bar / s.rollFrom) : kPpq / 8;
      b.rateTo = s.roll == "gradual" ? std::max<int64_t>(1, bar / s.rollTo) : kPpq / 8;
      b.pitch = pitch;
      b.velocityFrom = 0.35 + 0.65 * std::min(1.0, split);
      b.velocityTo = 1.0;
      b.riseFrom = riseAt;
      b.riseTo = rise;
      b.replace = false;
      makeBuildup(pattern, b);
    }
    if (drop) applyPreset(pattern, stretchPreset(*drop, s.speed), end, (int64_t)s.dropBars * bar, "replace", key);
  }
  return (int)notesIn(pattern, NoteScope{scope.start, scope.end, {}}).size();
}

void tempoRampTo(Document& doc, int64_t fromTick, int64_t toTick, double bpmTo) {
  if (!(bpmTo >= 40 && bpmTo <= 600)) throw DocumentError("tempo", "the final BPM is 40-600");
  if (fromTick < 0 || toTick <= fromTick) throw DocumentError("tempo", "the glide needs a start before its end");
  if (toTick >= doc.lengthTicks) throw DocumentError("tempo", "the glide must end inside the song");
  const double startBpm = bpmAt(doc, fromTick);
  std::vector<TempoPoint> changes;
  for (const auto& t : doc.tempoMap) if (t.tick <= fromTick || t.tick > toTick) changes.push_back(t);
  if (fromTick > 0) {  // the glide starts here: a step to the tempo already playing (a no-op to the ear)
    const bool there = std::any_of(changes.begin(), changes.end(), [&](const TempoPoint& t) { return t.tick == fromTick; });
    if (!there) { TempoPoint p; p.tick = fromTick; p.bpm = startBpm; changes.push_back(p); }
  }
  TempoPoint end;
  end.tick = toTick;
  end.bpm = bpmTo;
  end.ramp = true;
  changes.push_back(end);
  setTempoMap(doc, changes);
}

} // namespace core::doc
