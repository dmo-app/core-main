#include "score.hpp"
#include <algorithm>
#include <cctype>
#include "edit.hpp"

namespace core::doc {

namespace {
constexpr int64_t kBar = 4 * kPpq;

std::string lower(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}
bool has(const std::string& text, const char* word) { return text.find(word) != std::string::npos; }
}  // namespace

std::string scoreStageOf(const std::string& sectionName) {
  const std::string n = lower(sectionName);
  if (has(n, "intro")) return "intro";
  if (has(n, "outro") || has(n, "ending")) return "outro";
  if (has(n, "build") || has(n, "rise") || has(n, "riser") || has(n, "lift")) return "build";
  if (has(n, "break") || has(n, "bridge") || has(n, "calm")) return "break";
  if (has(n, "roll")) return "roll";
  if (has(n, "drop") || has(n, "main") || has(n, "peak") || has(n, "chorus")) return "drop";
  return "";
}

const std::vector<ScoreShape>& scoreShapes() {
  // Section lengths in bars, multiples of 8 (psy phrases are 8 or 16 bars). Dark psycore: the MVP plan of CORE-MANUAL
  // template-rules (core-main#148). Full-on: longer DJ intro and outro (16) so it mixes in and out. Hi-tech: shorter
  // and denser, two peaks. Slambient: long intro and break, a short peak. Hypotheses until heard; any shape works in
  // any style.
  static const std::vector<ScoreShape> shapes = {
      {"darkpsycore-64", "Dark psycore · 64 bars", "darkpsycore",
       "Intro 8, Build-up 8, Drop 16, Break 8, Roll 16, Outro 8: the Core MVP plan.",
       {{"Intro", 8}, {"Build-up", 8}, {"Drop", 16}, {"Break", 8}, {"Roll", 16}, {"Outro", 8}}},
      {"psytrance-96", "Full-on · 96 bars", "psytrance",
       "A 16-bar DJ intro and outro to mix in and out, two drops with a break between.",
       {{"Intro", 16}, {"Build-up", 8}, {"Drop", 16}, {"Break", 16}, {"Build-up 2", 8}, {"Drop 2", 16}, {"Outro", 16}}},
      {"hitech-64", "Hi-tech · 64 bars", "hitech",
       "Short and dense: a quick build, two peaks, a short break.",
       {{"Intro", 8}, {"Build-up", 8}, {"Drop", 16}, {"Break", 8}, {"Roll", 16}, {"Outro", 8}}},
      {"slambient-64", "Slambient · 64 bars", "slambient",
       "A long intro and break for the pads and drones, one short heavy peak.",
       {{"Intro", 16}, {"Build-up", 8}, {"Drop", 16}, {"Break", 16}, {"Outro", 8}}}};
  return shapes;
}

const ScoreShape* findScoreShape(const std::string& id) {
  for (const auto& s : scoreShapes())
    if (id == s.id) return &s;
  return nullptr;
}

void setScoreShape(Document& doc, const std::string& shapeId) {
  const ScoreShape* shape = findScoreShape(shapeId);
  if (!shape) throw DocumentError("score", "no S-Core song shape '" + shapeId + "'");
  Document next = doc;
  while (!next.sections.empty()) removeSection(next, next.sections.front().id);
  int64_t at = 0;
  for (const auto& st : shape->stages) {
    addSection(next, st.name, at, st.bars * kBar);
    at += st.bars * kBar;
  }
  next.lengthTicks = std::max(next.lengthTicks, at);
  doc = std::move(next);
}

const std::vector<std::string>& scoreParts() {
  static const std::vector<std::string> parts = {"kick", "bass", "hats", "openhats", "perc", "lead", "pad", "riser", "impact"};
  return parts;
}

std::string scorePartName(const std::string& part) {
  if (part == "openhats") return "Open hats";
  if (part.empty()) return "Every section";
  std::string n = part;
  n[0] = (char)std::toupper((unsigned char)n[0]);
  return n;
}

std::string scorePartOf(const Track& track) {
  if (!track.extra.is_null() && track.extra.contains("scorePart") && track.extra["scorePart"].is_string())
    return track.extra["scorePart"].get<std::string>();
  const std::string n = lower(track.name + " " + track.id);
  if (has(n, "kick") && !has(n, "bass")) return "kick";
  if (has(n, "open") && (has(n, "hat") || has(n, "hh"))) return "openhats";
  if (has(n, "hat") || has(n, "hh") || has(n, "ride") || has(n, "cymbal")) return "hats";
  if (has(n, "bass") || has(n, "sub")) return "bass";
  if (has(n, "perc") || has(n, "clap") || has(n, "snare") || has(n, "tom") || has(n, "shaker")) return "perc";
  if (has(n, "riser") || has(n, "sweep") || has(n, "uplift") || has(n, "build") || has(n, "noise")) return "riser";
  if (has(n, "impact") || has(n, "crash") || has(n, "hit") || has(n, "boom") || has(n, "stutter")) return "impact";
  if (has(n, "pad") || has(n, "drone") || has(n, "atmo") || has(n, "texture") || has(n, "bed") || has(n, "ambient")) return "pad";
  if (has(n, "lead") || has(n, "arp") || has(n, "synth") || has(n, "acid") || has(n, "melod") || has(n, "pluck") || has(n, "vox"))
    return "lead";
  if (track.instrument.kind == "builtin") {
    if (track.instrument.name == "kick") return "kick";
    if (track.instrument.name == "hats") return "hats";
    if (track.instrument.name == "snare") return "perc";
    if (track.instrument.name == "wave") return "lead";
  }
  return "";
}

std::vector<ScoreSpan> scoreRule(const std::string& part, const std::string& stage, bool beforeDrop) {
  // {from, to}: bars from the section's start; to 0 = its end; negative = counted from its end.
  using V = std::vector<ScoreSpan>;
  const int cut = beforeDrop ? -1 : 0;  // the bar before a drop: no kick, no bass
  if (part.empty()) return {{0, 0}};
  if (part == "kick") {
    if (stage == "intro" || stage == "drop" || stage == "roll" || stage == "outro") return V{{0, 0}};
    if (stage == "build") return V{{0, cut}};
    return {};
  }
  if (part == "bass") {
    if (stage == "intro") return V{{4, 0}};
    if (stage == "build") return V{{0, cut}};
    if (stage == "drop" || stage == "roll") return V{{0, 0}};
    if (stage == "outro") return V{{0, 4}};
    return {};
  }
  if (part == "hats") {
    if (stage == "intro" || stage == "outro") return V{{4, 0}};
    if (stage == "build") return V{{0, 0}};
    if (stage == "drop") return V{{4, 0}};
    if (stage == "roll") return V{{0, 0}};
    return {};
  }
  if (part == "openhats") {
    if (stage == "drop" || stage == "roll") return V{{-8, 0}};
    return {};
  }
  if (part == "perc") {
    if (stage == "build") return V{{-4, 0}};
    if (stage == "drop" || stage == "roll") return V{{-8, 0}};
    return {};
  }
  if (part == "lead") {
    if (stage == "drop" || stage == "roll") return V{{-8, 0}};
    if (stage == "break") return V{{0, 0}};
    return {};
  }
  if (part == "pad") {
    if (stage == "intro" || stage == "break" || stage == "outro") return V{{0, 0}};
    return {};
  }
  if (part == "riser") {
    if (stage == "build") return V{{0, 0}};
    if (stage == "break") return V{{-4, 0}};
    return {};
  }
  if (part == "impact") {
    if (stage == "drop" || stage == "roll") return V{{0, 1}};
    return {};
  }
  return {{0, 0}};
}

bool scorePlays(const Track& track, const Section& section) {
  const int64_t a = section.startTick, b = section.startTick + section.lengthTicks;
  for (const auto& c : track.clips)
    if (c.startTick < b && c.startTick + c.lengthTicks > a) return true;
  return false;
}

namespace {
// The pattern the track played in [a, b) (the first clip there), else the one it plays most, else its first.
std::string patternFor(const Track& t, int64_t a, int64_t b) {
  for (const auto& c : t.clips)
    if (c.startTick < b && c.startTick + c.lengthTicks > a) return c.patternId;
  return t.patterns.empty() ? std::string() : t.patterns.front().id;
}

// Removes every clip in [a, b): clips across an edge are cut there first (the outside part stays).
void clearRange(Track& t, int64_t a, int64_t b) {
  for (size_t i = 0; i < t.clips.size(); ++i) {
    const Clip c = t.clips[i];
    if (c.startTick < a && c.startTick + c.lengthTicks > a) splitClip(t, c.id, a);
  }
  for (size_t i = 0; i < t.clips.size(); ++i) {
    const Clip c = t.clips[i];
    if (c.startTick < b && c.startTick + c.lengthTicks > b) splitClip(t, c.id, b);
  }
  std::vector<std::string> gone;
  for (const auto& c : t.clips)
    if (c.startTick >= a && c.startTick + c.lengthTicks <= b) gone.push_back(c.id);
  for (const auto& id : gone) removeClip(t, id);
}

bool dropAfter(const Document& d, size_t i) {
  if (i + 1 >= d.sections.size()) return false;
  const std::string next = scoreStageOf(d.sections[i + 1].name);
  return next == "drop" || next == "roll";
}

int arrangeOne(Document& d, Track& t, size_t si) {
  const Section& s = d.sections[si];
  const std::string stage = scoreStageOf(s.name);
  if (stage.empty()) return 0;
  const int64_t a = s.startTick, b = s.startTick + s.lengthTicks;
  const std::string pattern = patternFor(t, a, b);
  if (pattern.empty()) return 0;
  clearRange(t, a, b);
  const int bars = (int)(s.lengthTicks / kBar);
  int written = 0;
  for (const auto& span : scoreRule(scorePartOf(t), stage, dropAfter(d, si))) {
    int from = span.from < 0 ? std::max(0, bars + span.from) : std::min(bars, span.from);
    int to = span.to <= 0 ? bars + span.to : std::min(bars, span.to);
    if (to <= from) continue;
    addClip(t, pattern, a + from * kBar, (to - from) * kBar);
    ++written;
  }
  return written;
}

size_t sectionIndex(const Document& d, const std::string& id) {
  for (size_t i = 0; i < d.sections.size(); ++i)
    if (d.sections[i].id == id) return i;
  throw DocumentError("score", "no section '" + id + "'");
}
}  // namespace

int scoreArrange(Document& doc, const std::vector<std::string>& trackIds, const std::vector<std::string>& sectionIds) {
  if (trackIds.empty()) throw DocumentError("score", "no track chosen");
  if (doc.sections.empty()) throw DocumentError("score", "the song has no sections: pick a song shape first");
  Document next = doc;
  std::vector<size_t> which;
  if (sectionIds.empty()) for (size_t i = 0; i < next.sections.size(); ++i) which.push_back(i);
  else for (const auto& id : sectionIds) which.push_back(sectionIndex(next, id));
  int written = 0;
  for (const auto& id : trackIds) {
    Track& t = trackById(next, id);
    if (!playsPatterns(t) || t.patterns.empty()) continue;
    for (size_t i : which) written += arrangeOne(next, t, i);
  }
  doc = std::move(next);
  return written;
}

void scoreSetPlays(Document& doc, const std::string& trackId, const std::string& sectionId, bool on) {
  Document next = doc;
  Track& t = trackById(next, trackId);
  if (!playsPatterns(t)) throw DocumentError("score", t.name + " plays no notes");
  const Section& s = next.sections[sectionIndex(next, sectionId)];
  const int64_t a = s.startTick, b = s.startTick + s.lengthTicks;
  const std::string pattern = patternFor(t, a, b);
  if (pattern.empty()) throw DocumentError("score", t.name + " has no pattern yet: write one with P-Core");
  clearRange(t, a, b);
  if (on) addClip(t, pattern, a, b - a);
  doc = std::move(next);
}

// The rises were F-Core regions on the build sections; F-Core left with Core's own sound (2026-11-16): nothing to add.
int scoreRises(Document&, const std::vector<std::string>&) { return 0; }

}  // namespace core::doc
