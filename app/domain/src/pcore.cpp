#include "pcore.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <random>
#include <set>
#include "edit.hpp"
#include "groove.hpp"
#include "pattern_tools.hpp"
#include "samples.hpp"

namespace core::doc {
namespace {

constexpr int64_t kBar = 4 * kPpq;
constexpr int64_t kMaxBars = 64;
constexpr size_t kMaxNotes = 4096;  // the document's limit per pattern

// Raw generator output only (never std:: distributions): the same seed gives the same notes on every build.
struct Draw {
  std::mt19937_64 rng;
  explicit Draw(uint64_t seed) : rng(seed * 0x9E3779B97F4A7C15ULL + 0x632BE59BD9B4E019ULL) {}
  double unit() { return (double)(rng() >> 11) * (1.0 / 9007199254740992.0); }
  int below(int n) { return n <= 1 ? 0 : (int)(unit() * n); }
};

std::string lower(std::string s) {
  for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
  return s;
}

double vel(double v) { return std::clamp(std::round(v * 100.0) / 100.0, 0.05, 1.0); }

std::string str(const json& j, const char* key, const std::string& fallback = "") {
  return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}

long noteNumber(const std::string& id) { return id.size() > 1 && id[0] == 'n' ? idNumber(id.c_str() + 1) : 0; }

// A note being made: ticks from the start of the range.
struct Hit { int64_t tick = 0, length = 0; int pitch = 60; double velocity = 0.8; };

// The scale degree d (any integer) as a pitch above `root`.
int degreePitch(const std::vector<int>& scale, int root, int d) {
  const int n = (int)scale.size();
  const int oct = (int)std::floor((double)d / n);
  const int i = ((d % n) + n) % n;
  return root + scale[(size_t)i] + 12 * oct;
}

int clampPitch(int p) {
  while (p > 127) p -= 12;
  while (p < 0) p += 12;
  return p;
}

}  // namespace

// ------------------------------------------------------------------------------------------------- catalogue

const PCoreScale* PCoreCatalog::scale(const std::string& id) const {
  for (const auto& s : scales) if (s.id == id) return &s;
  return nullptr;
}
const PCorePart* PCoreCatalog::part(const std::string& id) const {
  for (const auto& p : parts) if (p.id == id) return &p;
  return nullptr;
}
const PCoreShape* PCoreCatalog::shape(const std::string& id) const {
  for (const auto& s : shapes) if (s.id == id) return &s;
  return nullptr;
}
const PCoreStyle* PCoreCatalog::style(const std::string& id) const {
  for (const auto& s : styles) if (s.id == id) return &s;
  return nullptr;
}
const PCoreAlign* PCoreCatalog::align(const std::string& id) const {
  for (const auto& a : aligns) if (a.id == id) return &a;
  return nullptr;
}

const std::vector<std::string>& pcoreMotions() {
  static const std::vector<std::string> m = {"pedal", "octave-last", "answer", "walk", "near", "pedal-fifth", "wide",
                                             "up", "down", "updown", "random", "octaves", "chord", "chord-move", "drone", "laser", "beat-steps"};
  return m;
}

int64_t pcoreGridTicks(const std::string& g) {
  static const std::map<std::string, int64_t> grids = {{"1/4", kPpq}, {"1/8", kPpq / 2}, {"1/8t", kPpq / 3}, {"1/16", kPpq / 4},
                                                       {"1/16t", kPpq / 6}, {"1/32", kPpq / 8}, {"1/64", kPpq / 16}};
  const auto it = grids.find(g);
  return it == grids.end() ? 0 : it->second;
}

int pcoreRootPitchClass(const std::string& name) {
  static const std::map<std::string, int> names = {{"C", 0}, {"C#", 1}, {"Db", 1}, {"D", 2}, {"D#", 3}, {"Eb", 3}, {"E", 4}, {"F", 5},
                                                    {"F#", 6}, {"Gb", 6}, {"G", 7}, {"G#", 8}, {"Ab", 8}, {"A", 9}, {"A#", 10}, {"Bb", 10}, {"B", 11}};
  const auto it = names.find(name);
  return it == names.end() ? -1 : it->second;
}

PCoreCatalog pcoreCatalogFromJson(const json& j) {
  if (!j.is_object() || str(j, "format") != "core-pcore") throw DocumentError("pcore", "not a core-pcore catalogue");
  if (!j.contains("version") || !j["version"].is_number_integer() || j["version"].get<int>() != 1)
    throw DocumentError("pcore", "only version 1 is known");
  PCoreCatalog c;
  const auto list = [&](const char* key) -> const json& {
    if (!j.contains(key) || !j[key].is_array()) throw DocumentError(std::string("pcore.") + key, "a list is needed");
    return j[key];
  };
  for (const auto& s : list("scales")) {
    PCoreScale x{str(s, "id"), str(s, "name"), str(s, "mood"), {}};
    if (s.contains("steps") && s["steps"].is_array()) for (const auto& v : s["steps"]) if (v.is_number_integer()) x.steps.push_back(v.get<int>());
    if (x.id.empty() || x.steps.empty() || x.steps.front() != 0 || !std::is_sorted(x.steps.begin(), x.steps.end()) || x.steps.back() > 11)
      throw DocumentError("pcore.scales." + x.id, "a scale has an id and steps 0..11 rising from 0");
    c.scales.push_back(x);
  }
  for (const auto& p : list("parts")) {
    PCorePart x;
    x.id = str(p, "id"); x.name = str(p, "name", x.id); x.group = str(p, "group", "rhythm"); x.about = str(p, "about");
    x.drum = p.value("drum", true);
    x.pitch = p.value("pitch", 36);
    x.octave = p.value("octave", 48);
    if (x.id.empty() || x.pitch < 0 || x.pitch > 127 || x.octave < 0 || x.octave > 115) throw DocumentError("pcore.parts." + x.id, "bad part");
    c.parts.push_back(x);
  }
  for (const auto& s : list("shapes")) {
    PCoreShape x;
    x.id = str(s, "id"); x.part = str(s, "part"); x.name = str(s, "name", x.id); x.about = str(s, "about");
    x.kind = str(s, "kind", "steps"); x.grid = str(s, "grid", "1/16"); x.steps = str(s, "steps"); x.motion = str(s, "motion", "pedal");
    x.notes = s.value("notes", 3);
    x.pitch = s.value("pitch", -1);
    if (s.contains("roll") && s["roll"].is_object()) {
      const auto& r = s["roll"];
      x.rollShape = str(r, "shape", "steps");
      x.gapBeats = r.value("gapBeats", 0);
      x.rateFrom = r.value("rateFrom", (int64_t)kPpq);
      x.rateTo = r.value("rateTo", (int64_t)kPpq / 16);
    }
    if (s.contains("breakdown") && s["breakdown"].is_object()) {
      const auto& b = s["breakdown"];
      x.arc = str(b, "arc", "down");
      x.feel = str(b, "feel", "regular");
      x.source = str(b, "source", "plan");
      x.length = b.value("length", 1.0);
      x.spacing = b.value("spacing", 4);
    }
    const std::string where = "pcore.shapes." + x.id;
    if (x.id.empty() || !c.part(x.part)) throw DocumentError(where, "a shape has an id and a known part");
    if (x.kind == "steps") {
      if (x.steps.empty() || x.steps.size() > 512) throw DocumentError(where, "steps: 1-512 characters");
      if (x.steps.find_first_not_of("xXo.-KBb") != std::string::npos) throw DocumentError(where, "steps use x X o . - K B b");
      if (pcoreGridTicks(x.grid) == 0) throw DocumentError(where, "unknown grid " + x.grid);
    } else if (x.kind == "roll") {
      if (x.rollShape != "steps" && x.rollShape != "gradual") throw DocumentError(where, "a roll is steps or gradual");
      if (x.gapBeats < 0 || x.gapBeats > 4 || x.rateFrom < 1 || x.rateTo < 1) throw DocumentError(where, "bad roll");
    } else if (x.kind == "breakdown") {
      if (x.part != "kick" && x.part != "bass" && x.part != "cymbals") throw DocumentError(where, "a breakdown is for the kick, the bass or the cymbals");
      if (x.feel != "regular" && x.feel != "chaos") throw DocumentError(where, "a breakdown is regular or chaos");
      if (x.arc != "down" && x.arc != "up" && x.arc != "updown") throw DocumentError(where, "arc is down, up or updown");
      if (x.source != "plan" && !(x.source == "notes" && x.arc == "down")) throw DocumentError(where, "source is plan, or notes for a down arc");
      if (!(x.length >= 0.1 && x.length <= 1.0)) throw DocumentError(where, "length 0.1..1");
      if (x.spacing != 1 && x.spacing != 2 && x.spacing != 4 && x.spacing != 8) throw DocumentError(where, "spacing 1, 2, 4 or 8 beats");
    } else {
      throw DocumentError(where, "kind is steps, roll or breakdown");
    }
    if (std::find(pcoreMotions().begin(), pcoreMotions().end(), x.motion) == pcoreMotions().end()) throw DocumentError(where, "unknown motion " + x.motion);
    if (x.notes < 1 || x.notes > 8 || x.pitch > 127) throw DocumentError(where, "notes 1-8, pitch up to 127");
    if (c.shape(x.id)) throw DocumentError(where, "the id is used twice");
    c.shapes.push_back(x);
  }
  if (j.contains("aligns") && j["aligns"].is_array())  // optional: older catalogues have none
    for (const auto& a : j["aligns"]) {
      PCoreAlign x;
      x.id = str(a, "id"); x.name = str(a, "name", x.id); x.about = str(a, "about");
      x.bassClearSteps = a.value("bassClearSteps", 0);
      x.bassGate = a.value("bassGate", 0.8);
      x.bassFirst = a.value("bassFirst", 0.8);
      x.bassTailMs = a.value("bassTailMs", 8.0);
      x.hatsOnKick = a.value("hatsOnKick", 0.6);
      x.hatsAccent = a.value("hatsAccent", 0.3);
      x.swing = a.value("swing", 0.0);
      x.pushMs = a.value("pushMs", 0.0);
      const std::string where = "pcore.aligns." + x.id;
      const auto in = [](double v, double lo, double hi) { return std::isfinite(v) && v >= lo && v <= hi; };
      if (x.id.empty() || x.id == "style" || c.align(x.id)) throw DocumentError(where, "an alignment has an id of its own");
      if ((x.bassClearSteps != 0 && x.bassClearSteps != 1) || !in(x.bassGate, 0.3, 1) || !in(x.bassFirst, 0.3, 1) ||
          !in(x.bassTailMs, 0, 40) || !in(x.hatsOnKick, 0, 1) || !in(x.hatsAccent, 0, 0.8) || !in(x.swing, 0, 0.5) || !in(x.pushMs, -10, 10))
        throw DocumentError(where, "bassClearSteps 0/1, bassGate 0.3..1, bassFirst 0.3..1, bassTailMs 0..40, hatsOnKick 0..1, "
                                   "hatsAccent 0..0.8, swing 0..0.5, pushMs -10..10");
      c.aligns.push_back(x);
    }
  for (const auto& s : list("styles")) {
    PCoreStyle x;
    x.id = str(s, "id"); x.name = str(s, "name", x.id); x.root = str(s, "root", "A"); x.about = str(s, "about");
    x.humanize = std::clamp(s.value("humanize", 0.05), 0.0, 1.0);
    x.chaos = std::clamp(s.value("chaos", 0.3), 0.0, 1.0);
    if (s.contains("bpm") && s["bpm"].is_number()) x.bpm = std::clamp(s["bpm"].get<double>(), 0.0, 600.0);
    const std::string where = "pcore.styles." + x.id;
    if (x.id.empty() || pcoreRootPitchClass(x.root) < 0) throw DocumentError(where, "a style has an id and a root note (C..B)");
    x.align = str(s, "align");
    if (!x.align.empty() && !c.align(x.align)) throw DocumentError(where, "unknown alignment " + x.align);
    if (s.contains("match") && s["match"].is_array()) for (const auto& m : s["match"]) if (m.is_string()) x.match.push_back(lower(m.get<std::string>()));
    if (s.contains("scales") && s["scales"].is_array())
      for (const auto& m : s["scales"]) {
        if (!m.is_string() || !c.scale(m.get<std::string>())) throw DocumentError(where, "unknown scale");
        x.scales.push_back(m.get<std::string>());
      }
    if (s.contains("shapes") && s["shapes"].is_object())
      for (const auto& [part, ids] : s["shapes"].items()) {
        if (!c.part(part) || !ids.is_array()) throw DocumentError(where, "unknown part " + part);
        for (const auto& id : ids) {
          const PCoreShape* sh = id.is_string() ? c.shape(id.get<std::string>()) : nullptr;
          if (!sh || sh->part != part) throw DocumentError(where, "unknown shape for " + part);
          x.shapes[part].push_back(sh->id);
        }
      }
    c.styles.push_back(x);
  }
  if (c.scales.empty() || c.parts.empty() || c.shapes.empty()) throw DocumentError("pcore", "the catalogue is empty");
  return c;
}

PCoreCatalog loadPCoreCatalog(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw DocumentError("pcore", "cannot read " + file.string());
  json j;
  try { j = json::parse(in); } catch (const std::exception& e) { throw DocumentError("pcore", std::string("not JSON: ") + e.what()); }
  return pcoreCatalogFromJson(j);
}

const PCoreStyle* pcoreStyleFor(const PCoreCatalog& c, const std::string& nameOrId) {
  const std::string n = lower(nameOrId);
  if (n.empty()) return nullptr;
  for (const auto& s : c.styles) if (s.id == n) return &s;
  for (const auto& s : c.styles) for (const auto& m : s.match) if (m == n) return &s;
  return nullptr;
}

std::vector<const PCoreShape*> pcoreShapesFor(const PCoreCatalog& c, const std::string& part, const std::string& style) {
  std::vector<const PCoreShape*> out;
  if (const PCoreStyle* st = pcoreStyleFor(c, style); st && st->shapes.count(part))
    for (const auto& id : st->shapes.at(part)) out.push_back(c.shape(id));
  for (const auto& s : c.shapes)
    if (s.part == part && std::find(out.begin(), out.end(), &s) == out.end()) out.push_back(&s);
  return out;
}

std::vector<const PCoreScale*> pcoreScalesFor(const PCoreCatalog& c, const std::string& style) {
  std::vector<const PCoreScale*> out;
  if (const PCoreStyle* st = pcoreStyleFor(c, style)) for (const auto& id : st->scales) out.push_back(c.scale(id));
  for (const auto& s : c.scales) if (std::find(out.begin(), out.end(), &s) == out.end()) out.push_back(&s);
  return out;
}

bool pcoreShapeUsesScale(const PCoreShape& s, const PCorePart& part) {
  if (part.drum || s.kind != "steps") return false;
  static const std::set<std::string> keyOnly = {"pedal", "octave-last", "octaves", "laser"};
  return keyOnly.count(s.motion) == 0;
}

std::string pcoreScaleNoteNames(const PCoreScale& scale, const std::string& root) {
  static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const int r = pcoreRootPitchClass(root);
  if (r < 0) return {};
  std::string out;
  for (int st : scale.steps) out += (out.empty() ? "" : " ") + std::string(names[(r + st) % 12]);
  return out;
}

std::string pcoreGuessPart(const Track& t) {
  const std::string text = lower(t.id + " " + t.name + " " + t.instrument.name);
  const auto has = [&](const char* w) { return text.find(w) != std::string::npos; };
  // Short names only as whole words ("Sub", "OH", "BD 2"), never inside another word.
  std::set<std::string> words;
  {
    std::string w;
    for (char ch : text + " ") {
      if (std::isalnum((unsigned char)ch)) w += ch;
      else { if (!w.empty()) words.insert(w); w.clear(); }
    }
  }
  const auto word = [&](const char* w) { return words.count(w) > 0; };
  if (has("kick") || word("bd")) return "kick";
  if (has("bass") || word("sub")) return "bass";
  if (has("hat") || word("hh") || word("oh") || word("ch")) return "hats";
  if (has("ride") || has("crash") || has("cymbal")) return "cymbals";
  if (has("build") || has("roll") || has("riser") || has("fill")) return "build";
  if (has("clap") || has("snare") || has("perc") || word("rim") || has("rimshot") || word("tom") || word("toms") || has("shaker") || has("tamb") || has("conga") ||
      has("bongo") || word("sd") || word("top") || word("tops"))
    return "perc";
  if (has("arp")) return "arp";
  if (has("pad") || has("atmo") || has("drone")) return "pad";
  if (has("zap") || has("laser") || has("fx") || has("stab")) return "fx";
  if (has("lead") || has("pluck") || has("acid") || has("synth")) return "lead";
  const std::string role = trackSampleRole(t);
  if (role == "kick" || role == "bass") return role;
  if (role == "hat" || role == "openhat") return "hats";
  if (role == "ride" || role == "crash") return "cymbals";
  if (role == "snare" || role == "clap" || role == "perc" || role == "accent") return "perc";
  return "";
}

void checkPCoreLayers(const PCoreLayers& l) {
  if (l.mode != "create" && l.mode != "alter") throw DocumentError("pcore.mode", "create or alter");
  if (!l.grid.empty() && pcoreGridTicks(l.grid) == 0) throw DocumentError("pcore.grid", "1/4, 1/8, 1/8t, 1/16, 1/16t, 1/32 or 1/64");
  if (!l.root.empty() && pcoreRootPitchClass(l.root) < 0) throw DocumentError("pcore.root", "a note name C..B (F#, Bb...)");
  if (!std::isfinite(l.density) || l.density < -90 || l.density > 200) throw DocumentError("pcore.density", "-90..200 %");
  if (!std::isfinite(l.humanize) || l.humanize > 1) throw DocumentError("pcore.humanize", "0..1 (negative: the style's)");
  if (!std::isfinite(l.quantize) || l.quantize < 0 || l.quantize > 1) throw DocumentError("pcore.quantize", "0..1");
  static const std::set<int> every = {0, 1, 2, 4, 8, 16};
  if (!every.count(l.variationEvery)) throw DocumentError("pcore.variationEvery", "0, 1, 2, 4, 8 or 16 bars");
  if (l.rollBeats < 0 || l.rollBeats > 16) throw DocumentError("pcore.rollBeats", "0..16 beats");
  if (!l.breakFeel.empty() && l.breakFeel != "regular" && l.breakFeel != "chaos") throw DocumentError("pcore.breakFeel", "regular or chaos");
  if (l.breakSpacing != 0 && l.breakSpacing != 1 && l.breakSpacing != 2 && l.breakSpacing != 4 && l.breakSpacing != 8)
    throw DocumentError("pcore.breakSpacing", "1, 2, 4 or 8 beats (0: the shape's)");
  if (!std::isfinite(l.breakLength) || (l.breakLength != 0 && (l.breakLength < 0.1 || l.breakLength > 1)))
    throw DocumentError("pcore.breakLength", "0.1..1 (0: the shape's)");
}

// ------------------------------------------------------------------------------------------------- the layers

namespace {

struct Tonal { std::vector<int> scale; int root = 57; double chaos = 0.3; };

// Layer 2 at layer 3's grid: the shape's steps over [0, len), the pitches by its motion.
std::vector<Hit> shapeHits(const PCoreShape& s, const PCorePart& part, const Tonal& tone, int drumPitch, int64_t grid,
                           int64_t len, Draw& draw) {
  std::vector<Hit> out;
  const int steps = (int)s.steps.size();
  const bool bass = part.id == "bass";
  const int root = part.drum ? drumPitch : clampPitch(part.octave + tone.root % 12);
  const int n = (int)tone.scale.size();
  // Chord tones as scale degrees: 1, 3, 5, octave, 10th...
  std::vector<int> chord = {0, 2, 4, n, n + 2, n + 4, 2 * n, 2 * n + 2};
  chord.resize((size_t)std::clamp(s.notes, 1, 8));
  int idx = 0, nearDeg = 0, walkDeg = 0, stepDeg = 0;
  int64_t stepBeat = -1;
  for (int64_t rep = 0, at = 0; at < len; ++rep, at += grid * steps) {
    int inRep = 0;
    const int notesInRep = (int)std::count_if(s.steps.begin(), s.steps.end(), [](char ch) { return ch == 'x' || ch == 'X' || ch == 'o' || ch == 'B' || ch == 'b'; });
    for (int i = 0; i < steps; ++i) {
      const char ch = s.steps[(size_t)i];
      const int64_t t = at + grid * i;
      if (t >= len) break;
      if (ch == '-') {  // the note (or chord) before goes on
        if (!out.empty()) {
          const int64_t last = out.back().tick;
          for (auto it = out.rbegin(); it != out.rend() && it->tick == last; ++it) it->length = t + grid - it->tick;
        }
        continue;
      }
      if (ch != 'x' && ch != 'X' && ch != 'o' && ch != 'B' && ch != 'b') continue;
      Hit h;
      h.tick = t;
      h.velocity = ch == 'X' ? 1.0 : ch == 'o' ? 0.5 : ch == 'b' ? 0.58 : ch == 'B' ? 0.8 : 0.8;
      if (bass && ch == 'B') {  // the KBBB accents of the study: the note right after the kick strongest
        const char before = i > 0 ? s.steps[(size_t)i - 1] : s.steps.back();
        if (before != 'K') h.velocity = 0.7;
      }
      h.length = part.drum ? std::max<int64_t>(30, grid / 2) : std::max<int64_t>(30, grid * 85 / 100);
      int deg = 0, semis = 0;
      bool chordHit = false;
      const std::string& m = s.motion;
      const int64_t bar = t / kBar;
      if (m == "pedal") deg = 0;
      else if (m == "octave-last") { if (i == steps - 1 || (i + 1 < steps && s.steps[(size_t)i + 1] == 'K')) deg = n; }
      else if (m == "answer") {
        static const int answers[4] = {3, 4, 2, 5};
        if (i >= steps * 3 / 4) deg = answers[rep % 4];
      } else if (m == "walk") {
        if (t % kBar == 0 || idx == 0) { static const int walk[4] = {0, 0, 3, 4}; walkDeg = walk[bar % 4]; }
        deg = walkDeg;
      } else if (m == "near") {
        const double r = draw.unit();
        const int jump = 1 + (draw.unit() < tone.chaos * 0.5 ? 1 : 0);
        if (r < 0.35) nearDeg -= jump; else if (r < 0.7) nearDeg += jump;
        nearDeg = std::clamp(nearDeg, -3, n + 2);
        deg = nearDeg;
      } else if (m == "beat-steps") {
        // A moving roll (hi-tech, psycore): every beat's notes share one scale step, over a two-bar cycle that
        // keeps coming home to the key. Wilder styles (chaos) swap some beats for a near step at random.
        static const int cycle[8] = {0, 0, 1, 0, 0, 2, 1, -1};
        const int64_t beat = t / kPpq;
        if (beat != stepBeat) {
          stepBeat = beat;
          stepDeg = cycle[beat % 8];
          if (beat % 4 != 0 && draw.unit() < tone.chaos * 0.3) stepDeg = draw.below(4) - 1;
        }
        deg = stepDeg;
      } else if (m == "pedal-fifth") deg = inRep % 3 == 2 ? 4 : 0;
      else if (m == "wide") {
        const int span = std::max(2, (int)std::lround(n * (0.8 + 2.2 * tone.chaos)));
        deg = draw.below(span * 2 + 1) - span / 2;
      } else if (m == "up" || m == "down" || m == "updown") {
        const bool arp = part.id == "arp";
        const int count = arp ? (int)chord.size() : n + 1;
        int k = idx;
        if (m == "updown") { const int cyc = std::max(1, 2 * count - 2); k = idx % cyc; if (k >= count) k = cyc - k; }
        else { k = idx % count; if (m == "down") k = count - 1 - k; }
        deg = arp ? chord[(size_t)k] : k;
      } else if (m == "random") deg = chord[(size_t)draw.below((int)chord.size())];
      else if (m == "octaves") { static const int o[3] = {0, 7, 12}; semis = o[idx % 3]; }
      else if (m == "chord" || m == "chord-move") {
        static const int moves[4] = {0, 5, 3, 4};
        deg = m == "chord" ? 0 : moves[(t / (2 * kBar)) % 4];
        chordHit = true;
      } else if (m == "drone") chordHit = true;
      else if (m == "laser") {
        const int fall = 24 * inRep / std::max(1, notesInRep);
        semis = 24 - fall + (rep % 2 == 0 ? 0 : draw.below(5));
      }
      if (part.drum) {
        h.pitch = drumPitch;
        out.push_back(h);
      } else if (chordHit) {
        const std::vector<int> stack = s.motion == "drone" ? std::vector<int>{0, 4} : std::vector<int>{0, 2, 4};
        for (int d : stack) { Hit c = h; c.pitch = clampPitch(degreePitch(tone.scale, root, deg + d)); out.push_back(c); }
      } else {
        h.pitch = clampPitch(semis != 0 ? root + semis : degreePitch(tone.scale, root, deg));
        out.push_back(h);
      }
      ++idx;
      ++inRep;
      if (out.size() >= kMaxNotes) return out;
    }
  }
  // A held note ('-') never runs into the next one, nor past the range.
  for (size_t i = 0; i < out.size(); ++i) out[i].length = std::min(out[i].length, len - out[i].tick);
  return out;
}

// The breakdown's plan, shared by the kick, the bass and the cymbals (same settings, same seed: the same plan).
// The bars split into the part that changes speed (`length`) and full-speed KBBB:
//   down:   KBBB, then the fall, then one last kick and silence
//   up:     the rise (slow kicks speeding up), then KBBB to the end
//   updown: the rise, KBBB, the fall, one last kick
// Regular goes in stages (KBBB, KBB_, K_B_, a kick per beat, then every 2, 4, 8 beats up to `spacing`); chaos
// picks cells at random, sparser towards the slow end (a kick every `spacing` beats on average), with short 1/32
// bursts near the fast end.
struct BreakHit { int64_t tick = 0, step = 0; char what = 'K'; double velocity = 1.0; };
struct BreakSettings { std::string arc, feel; double length = 1; int spacing = 4; };

std::vector<BreakHit> breakdownPlan(const BreakSettings& s, int64_t len, uint64_t seed) {
  std::vector<BreakHit> out;
  constexpr int64_t s16 = kPpq / 4, s32 = kPpq / 8;
  const int beats = (int)(len / kPpq);
  if (beats < 1) return out;
  Draw draw(seed * 31ULL + 17ULL);
  const bool loose = s.feel == "chaos";
  const int trans = std::clamp((int)std::lround(beats * s.length), 1, beats);
  int rise = 0, fallAll = 0;
  if (s.arc == "up") rise = trans;
  else if (s.arc == "updown") { rise = trans / 2; fallAll = trans - rise; }
  else fallAll = trans;
  const int hold = beats - trans;
  const int last = fallAll <= 1 ? fallAll : std::max(1, (int)std::lround(fallAll / 9.0));
  const int fall = fallAll - last;
  // One beat of a cell: 4 characters at 1/16 or 8 at 1/32 (K kick, B bass, . nothing).
  const auto cell = [&](int beat, const std::string& c) {
    const int64_t step = c.size() == 8 ? s32 : s16;
    for (size_t i = 0; i < c.size(); ++i) {
      if (c[i] == '.') continue;
      BreakHit h;
      h.tick = (int64_t)beat * kPpq + (int64_t)i * step;
      h.step = step;
      h.what = c[i];
      if (h.what == 'K') h.velocity = loose ? 0.85 + 0.15 * draw.unit() : 1.0;
      else h.velocity = loose ? 0.6 + 0.3 * draw.unit() : (i > 0 && c[i - 1] == 'K' ? 0.85 : 0.72);
      out.push_back(h);
    }
  };
  // n beats from `from`, going down (fast to slow) or up (slow to fast).
  const auto section = [&](int from, int n, bool up) {
    if (n <= 0) return;
    if (!loose) {
      // Each stage lasts whole repeats of its steps (a kick every 4 beats needs 4 beats), so every slower stage is
      // heard. When the beats are few, KBB_ goes first, then the in-between slow ones, then K... and K_B_, the
      // slowest (the chosen spacing) last.
      struct Stage { std::string steps; double weight; int beats; };
      std::vector<Stage> stages = {{"KBBB", 2, 1}, {"KBB.", 1.5, 1}, {"K.B.", 1.5, 1}, {"K...", 1, 1}};
      for (int gap : {2, 4, 8})
        if (gap <= s.spacing) stages.push_back({"K" + std::string((size_t)gap * 4 - 1, '.'), 1, gap});
      std::vector<int> use;
      for (int k = 0; k < (int)stages.size(); ++k) use.push_back(k);
      std::vector<int> order = {1};
      for (int k = 4; k + 1 < (int)stages.size(); ++k) order.push_back(k);
      order.push_back(3);
      order.push_back(2);
      if (stages.size() > 4) order.push_back((int)stages.size() - 1);
      const auto need = [&] { int k = 0; for (int u : use) k += stages[(size_t)u].beats; return k; };
      for (int drop : order)
        if (need() > n && use.size() > 1) use.erase(std::find(use.begin(), use.end(), drop));
      double weight = 0;
      for (int u : use) weight += stages[(size_t)u].weight;
      const int extra = std::max(0, n - need());
      std::vector<int> length(use.size());
      int others = 0;
      for (size_t u = 1; u < use.size(); ++u) {
        const Stage& st = stages[(size_t)use[u]];
        const int want = st.beats + (int)std::floor(extra * st.weight / weight);
        length[u] = std::max(st.beats, want / st.beats * st.beats);
        others += length[u];
      }
      length[0] = n - others;  // KBBB takes what is left
      if (up) { std::reverse(use.begin(), use.end()); std::reverse(length.begin(), length.end()); }
      for (size_t u = 0, at = 0; u < use.size(); at += (size_t)length[u], ++u) {
        const std::string& steps = stages[(size_t)use[u]].steps;
        for (int b = 0; b < length[u]; ++b) {
          std::string one;
          for (size_t i = 0; i < 4; ++i) one += steps[((size_t)b * 4 + i) % steps.size()];
          cell(from + (int)at + b, one);
        }
      }
      return;
    }
    static const std::vector<std::string> pools[5] = {
        {"K...", "..K."},
        {"K...", "..K.", ".K..", "K..K", "...K"},
        {"K.B.", "K..B", "KB..", ".KB.", "K.K."},
        {"KBB.", "K.BB", "KB.B", "K.KB", "KBBK"},
        {"KBBB", "KBBK", "KKBB", "KBKB"}};
    for (int b = 0; b < n; ++b) {
      const double p = up ? 1.0 - (double)(b + 1) / n : (double)b / n;
      const int level = std::clamp((int)std::lround((1.0 - p) * 4.0 + (draw.unit() - 0.5) * 1.6), 0, 4);
      if (up && b == 0) { cell(from + b, "K..."); continue; }               // the rise starts on a kick
      if (up && b == n - 1) { cell(from + b, "KBBB"); continue; }           // and arrives at full KBBB
      if (!up && b == 0) { cell(from + b, "KBBB"); continue; }              // the fall starts on the beat
      if (!up && b == n - 1) { cell(from + b, s.spacing > 1 ? "...." : "K..."); continue; }  // a breath before the last kick
      if (level >= 3 && draw.unit() < 0.2) { cell(from + b, "KBBBBBBB"); continue; }  // a 1/32 burst
      if (level <= 1 && s.spacing > 1 && draw.unit() >= 1.0 / (level == 0 ? s.spacing : std::max(1, s.spacing / 2))) continue;  // a gap
      const auto& pool = pools[(size_t)level];
      cell(from + b, pool[(size_t)draw.below((int)pool.size())]);
    }
  };
  section(0, rise, true);
  for (int b = 0; b < hold; ++b) cell(rise + b, "KBBB");
  section(rise + hold, fall, false);
  if (last > 0) out.push_back({(int64_t)(rise + hold + fall) * kPpq, s16, 'K', 1.0});  // the one last kick, then silence
  return out;
}

BreakSettings breakSettings(const PCoreShape& s, const PCoreLayers& l) {
  return {s.arc, l.breakFeel.empty() ? s.feel : l.breakFeel, l.breakLength > 0 ? l.breakLength : s.length,
          l.breakSpacing > 0 ? l.breakSpacing : s.spacing};
}

std::vector<Hit> breakdownHits(const PCoreShape& s, const PCoreLayers& l, const PCorePart& part, const Tonal& tone,
                               int drumPitch, int64_t len, const std::vector<Note>& there) {
  const bool kick = part.id == "kick", cymbals = part.id == "cymbals";
  const int root = part.drum ? drumPitch : clampPitch(part.octave + tone.root % 12);
  std::vector<Hit> out;
  const auto plan = breakdownPlan(breakSettings(s, l), len, l.seed);
  if (s.source == "notes" && !there.empty()) {
    // Down from what is there: a note stays while the plan still plays something on its 1/16 (its 1/32 in a
    // burst); the last kick is the plan's.
    std::set<int64_t> open;
    for (const auto& b : plan) open.insert(b.tick / (kPpq / 8));
    const int64_t end = plan.empty() ? len : plan.back().tick;
    for (const auto& n : there) {
      const int64_t slot = (n.tick + kPpq / 16) / (kPpq / 8);
      if (n.tick >= end || !(open.count(slot) || open.count(slot - 1))) continue;
      out.push_back({n.tick, std::min(n.length, end - n.tick), n.pitch, n.velocity});
    }
    if (kick && !plan.empty()) out.push_back({end, std::min<int64_t>(kPpq, len - end), drumPitch, 1.0});
    if (cymbals && !plan.empty()) out.push_back({end, len - end, drumPitch, 1.0});
    return out;
  }
  int64_t lastBeat = -1;
  for (size_t i = 0; i < plan.size(); ++i) {
    const auto& b = plan[i];
    const bool final = i + 1 == plan.size() && s.arc != "up";
    if (cymbals) {
      // A crash with the first kick of every beat (quarter crashes while it is fast, then they follow the kicks
      // as they spread out); the last kick's crash rings to the end.
      if (b.what != 'K' || b.tick / kPpq == lastBeat) continue;
      lastBeat = b.tick / kPpq;
      out.push_back({b.tick, final ? len - b.tick : kPpq / 2, root, final ? 1.0 : 0.75 + 0.15 * (b.velocity - 0.85) / 0.15});
      continue;
    }
    if ((b.what == 'K') != kick) continue;
    Hit h;
    h.tick = b.tick;
    h.pitch = root;
    h.velocity = b.velocity;
    // The last kick rings for a beat; the others are short.
    h.length = kick ? (final ? kPpq : std::max<int64_t>(30, b.step / 2)) : std::max<int64_t>(30, b.step * 85 / 100);
    h.length = std::min(h.length, len - h.tick);
    out.push_back(h);
  }
  return out;
}

std::vector<Hit> rollHits(const PCoreShape& s, int pitch, int64_t len) {
  Pattern p;
  p.lengthTicks = len;
  BuildupOptions o;
  o.start = 0;
  o.length = len;
  o.pitch = pitch;
  o.roll = true;
  o.shape = s.rollShape;
  o.gapTicks = (int64_t)s.gapBeats * kPpq;
  o.rateFrom = s.rateFrom;
  o.rateTo = s.rateTo;
  makeBuildup(p, o);
  std::vector<Hit> out;
  for (const auto& n : p.notes) out.push_back({n.tick, n.length, n.pitch, n.velocity});
  return out;
}

Pattern asPattern(const std::vector<Hit>& hits, int64_t len) {
  Pattern p;
  p.lengthTicks = len;
  int i = 0;
  for (const auto& h : hits) {
    Note n;
    n.id = "n" + std::to_string(++i);
    n.tick = h.tick; n.length = std::max<int64_t>(1, h.length); n.pitch = h.pitch; n.velocity = vel(h.velocity);
    p.notes.push_back(n);
  }
  return p;
}

void sortNotes(Pattern& p) {
  std::sort(p.notes.begin(), p.notes.end(), [](const Note& a, const Note& b) { return a.tick < b.tick || (a.tick == b.tick && a.pitch < b.pitch); });
}

// The last beat of every N bars: the notes there double (a ratchet), melodic ones climb the scale.
void variation(Pattern& p, int every, const Tonal& tone, bool drum, int64_t grid, Draw& draw) {
  if (every <= 0) return;
  const int64_t block = every * kBar;
  std::vector<Note> extra;
  for (auto& n : p.notes) {
    const int64_t inBlock = n.tick % block;
    if (inBlock < block - kPpq) continue;
    if (!drum) {
      const int k = (int)((inBlock - (block - kPpq)) / std::max<int64_t>(1, grid)) + 1;
      // move up k scale steps from the note's own pitch
      int pc = n.pitch;
      for (int s = 0; s < k; ++s) {
        int best = pc + 12;
        for (int o = -12; o <= 12; o += 12)
          for (int st : tone.scale) { const int cand = (tone.root % 12) + st + 12 * ((pc / 12)) + o; if (cand > pc && cand < best) best = cand; }
        pc = best;
      }
      n.pitch = clampPitch(pc);
    }
    if (draw.unit() < 0.6 && n.length >= 60 && p.notes.size() + extra.size() < kMaxNotes) {
      Note r = n;
      r.length = n.length / 2;
      n.length = std::max<int64_t>(1, n.length / 2);
      r.tick = n.tick + std::max<int64_t>(1, grid / 2);
      r.velocity = vel(n.velocity * 0.7);
      if (r.tick < p.lengthTicks) extra.push_back(r);
    }
  }
  int id = (int)p.notes.size();
  for (auto& e : extra) { e.id = "v" + std::to_string(++id); p.notes.push_back(e); }
}

// The last `beats` beats become a roll: 1/16 then 1/32 in the last half, getting louder.
void roll(Pattern& p, int beats, int pitch) {
  if (beats <= 0) return;
  const int64_t from = std::max<int64_t>(0, p.lengthTicks - (int64_t)beats * kPpq), end = p.lengthTicks;
  p.notes.erase(std::remove_if(p.notes.begin(), p.notes.end(), [&](const Note& n) { return n.tick >= from; }), p.notes.end());
  for (auto& n : p.notes) n.length = std::min(n.length, std::max<int64_t>(1, from - n.tick));
  const int64_t half = from + (end - from) / 2;
  int i = 0;
  for (int64_t t = from; t < end && p.notes.size() < kMaxNotes;) {
    const int64_t step = t < half ? kPpq / 4 : kPpq / 8;
    Note n;
    n.id = "r" + std::to_string(++i);
    n.tick = t;
    n.length = std::max<int64_t>(1, std::min(step * 3 / 4, end - t));
    n.pitch = pitch;
    n.velocity = vel(0.45 + 0.55 * (double)(t - from) / (double)std::max<int64_t>(1, end - from));
    p.notes.push_back(n);
    t += step;
  }
}

void shuffleNotes(Pattern& p, bool drum, Draw& draw) {
  sortNotes(p);
  const size_t n = p.notes.size();
  for (size_t i = n; i > 1; --i) {
    const size_t j = (size_t)draw.below((int)i);
    if (drum) std::swap(p.notes[i - 1].velocity, p.notes[j].velocity);
    else std::swap(p.notes[i - 1].pitch, p.notes[j].pitch);
  }
}

void reverseNotes(Pattern& p) {
  for (auto& n : p.notes) n.tick = std::max<int64_t>(0, p.lengthTicks - n.tick - n.length);
  sortNotes(p);
}

// "Again" (Diogo, 2026-11-07: "deveria deixar de repetir possibilidades"): most shapes are written the same for any
// seed, so another take changes the written notes in a way of its part, drawn from the take's seed. One bar's plan,
// repeated every bar (or every 2 / 4 bars), keeps the groove a groove. One or two changes from:
//   all:     accents (a new loud / soft plan), ghost notes in empty steps, thinner (off-beat notes go)
//   kick:    a ghost kick before the next bar, a fill on the last beat every 4 or 8 bars (no ghosts, no thinning)
//   bass:    octave jumps, an answer at the end of every 2 or 4 bars, shorter or longer notes
//   melody:  the line moves 1-3 scale steps, octave jumps, each bar backwards
// The same seed gives the same take.
int scaleShift(int pitch, const Tonal& tone, int steps) {
  const int n = (int)tone.scale.size();
  if (n == 0 || steps == 0) return pitch;
  const int rel = pitch - tone.root % 12;
  const int oct = (int)std::floor((double)rel / 12.0);
  const int pc = ((rel % 12) + 12) % 12;
  int deg = 0;
  for (int i = 0; i < n; ++i) if (tone.scale[(size_t)i] <= pc) deg = i;
  return clampPitch(degreePitch(tone.scale, tone.root % 12, oct * n + deg + steps));
}

void varyTake(Pattern& p, const PCorePart& part, const Tonal& tone, int64_t grid, int64_t len, uint64_t seed) {
  if (p.notes.empty() || grid <= 0) return;
  Draw draw(seed ^ 0x7A6E5D4C3B2A1908ULL);
  const bool kick = part.id == "kick", bass = part.id == "bass", melody = !part.drum && !bass;
  const int64_t cycle = kBar * (int64_t)(1 << draw.below(3));  // the plan repeats every 1, 2 or 4 bars
  const int slots = (int)std::max<int64_t>(1, cycle / grid);
  auto slot = [&](const Note& n) { return (int)(((n.tick % cycle) / grid) % slots); };
  std::vector<int> kinds;
  if (kick) kinds = {10, 11, 0};
  else if (bass) kinds = {0, 3, 20, 21, 22};
  else if (melody) kinds = {0, 3, 30, 31, 32, 2};
  else kinds = {0, 1, 2, 3};
  const int howMany = 1 + (draw.unit() < 0.5 ? 1 : 0);
  std::set<int> chosen;
  while ((int)chosen.size() < std::min<int>(howMany, (int)kinds.size())) chosen.insert(kinds[(size_t)draw.below((int)kinds.size())]);
  for (int k : chosen) {
    if (k == 0) {  // accents: a loud / soft plan over the cycle
      std::vector<double> plan((size_t)slots);
      for (auto& v : plan) v = draw.unit() < 0.35 ? 1.0 : 0.62 + 0.18 * draw.unit();
      for (auto& n : p.notes) if (n.tick % kPpq != 0 || !kick) n.velocity = vel(std::max(0.3, n.velocity * plan[(size_t)slot(n)] / 0.9));
    } else if (k == 1) {  // ghost notes in empty steps
      std::set<int> used;
      for (const auto& n : p.notes) used.insert(slot(n));
      std::vector<int> empty;
      for (int i = 0; i < slots; ++i) if (!used.count(i)) empty.push_back(i);
      if (empty.empty()) continue;
      const int add = 1 + draw.below(std::max(1, std::min<int>(3, (int)empty.size())));
      std::set<int> pick;
      while ((int)pick.size() < std::min<int>(add, (int)empty.size())) pick.insert(empty[(size_t)draw.below((int)empty.size())]);
      const Note model = p.notes.front();
      int id = (int)p.notes.size();
      for (int64_t at = 0; at < len; at += cycle)
        for (int i : pick) {
          const int64_t t = at + (int64_t)i * grid;
          if (t >= len || p.notes.size() >= kMaxNotes) continue;
          Note g = model;
          g.id = "g" + std::to_string(++id);
          g.tick = t;
          g.length = std::max<int64_t>(30, std::min(model.length, grid * 3 / 4));
          g.velocity = vel(0.32 + 0.15 * draw.unit());
          p.notes.push_back(g);
        }
    } else if (k == 2) {  // thinner: one or two off-beat steps of the cycle go
      std::vector<int> off;
      std::set<int> seen;
      for (const auto& n : p.notes) if (n.tick % kPpq != 0 && seen.insert(slot(n)).second) off.push_back(slot(n));
      if (off.size() < 3) continue;
      std::set<int> drop;
      const int take = 1 + draw.below(2);
      while ((int)drop.size() < take) drop.insert(off[(size_t)draw.below((int)off.size())]);
      p.notes.erase(std::remove_if(p.notes.begin(), p.notes.end(), [&](const Note& n) { return n.tick % kPpq != 0 && drop.count(slot(n)); }), p.notes.end());
    } else if (k == 3) {  // a fill on the last beat every 4 or 8 bars (the notes there double)
      Draw d2(seed * 31 + 7);
      variation(p, draw.unit() < 0.5 ? 4 : 8, tone, part.drum, grid, d2);
    } else if (k == 10) {  // a ghost kick on the last 1/16 before the next bar, every 1, 2 or 4 bars
      const int64_t every = kBar * (int64_t)(1 << draw.below(3));
      const Note model = p.notes.front();
      int id = (int)p.notes.size();
      for (int64_t at = every; at <= len; at += every) {
        Note g = model;
        g.id = "g" + std::to_string(++id);
        g.tick = at - kPpq / 4;
        g.length = std::min<int64_t>(model.length, kPpq / 4);
        g.velocity = vel(0.45 + 0.2 * draw.unit());
        if (g.tick >= 0 && g.tick < len && p.notes.size() < kMaxNotes) p.notes.push_back(g);
      }
    } else if (k == 11) {  // the kick doubles on the last beat every 4 or 8 bars
      const int64_t block = kBar * (draw.unit() < 0.5 ? 4 : 8);
      std::vector<Note> extra;
      for (const auto& n : p.notes)
        if (n.tick % block == block - kPpq) {
          Note g = n;
          g.tick = n.tick + kPpq / 2 + (draw.unit() < 0.5 ? kPpq / 4 : 0);
          g.velocity = vel(n.velocity * 0.8);
          if (g.tick < len) extra.push_back(g);
        }
      int id = (int)p.notes.size();
      for (auto& g : extra) { g.id = "g" + std::to_string(++id); if (p.notes.size() < kMaxNotes) p.notes.push_back(g); }
    } else if (k == 20 || k == 31) {  // octave jumps on some steps of the cycle
      std::set<int> up;
      const int count = 1 + draw.below(3);
      for (int i = 0; i < count; ++i) up.insert(draw.below(slots));
      const int dir = melody && draw.unit() < 0.4 ? -12 : 12;
      for (auto& n : p.notes) if (up.count(slot(n))) n.pitch = clampPitch(n.pitch + dir);
    } else if (k == 21) {  // an answer: the last beat of every 2 or 4 bars moves to another scale note
      const int64_t block = kBar * (draw.unit() < 0.5 ? 2 : 4);
      const int steps = 2 + draw.below(3) * (draw.unit() < 0.3 ? -1 : 1);
      for (auto& n : p.notes) if (n.tick % block >= block - kPpq) n.pitch = scaleShift(n.pitch, tone, steps);
    } else if (k == 22) {  // shorter or longer notes
      const double f = draw.unit() < 0.5 ? 0.55 : 1.35;
      for (auto& n : p.notes) n.length = std::max<int64_t>(30, std::min<int64_t>((int64_t)std::lround(n.length * f), grid));
    } else if (k == 30) {  // the line moves 1-3 scale steps up or down
      const int steps = (1 + draw.below(3)) * (draw.unit() < 0.5 ? -1 : 1);
      for (auto& n : p.notes) n.pitch = scaleShift(n.pitch, tone, steps);
    } else if (k == 32) {  // each bar's pitches backwards (the rhythm stays)
      sortNotes(p);
      std::map<int64_t, std::vector<Note*>> bars;
      for (auto& n : p.notes) bars[n.tick / kBar].push_back(&n);
      for (auto& [b, v] : bars)
        for (size_t i = 0, j = v.size(); i + 1 < j; ++i, --j) std::swap(v[i]->pitch, v[j - 1]->pitch);
    }
  }
  sortNotes(p);
}

// Layers 3 (density) and 4, on the notes of one range (ticks from its start).
void transform(Pattern& p, const PCoreLayers& l, const PCoreStyle* style, const Tonal& tone, bool drum, int64_t grid,
               int rollPitch, uint64_t seed) {
  Draw draw(seed);
  if (l.density != 0 && !p.notes.empty()) {
    DensityOptions d;
    d.mode = "percent";
    d.amount = l.density;
    d.how = drum ? "fill" : "fill";
    d.grid = grid;
    d.seed = seed;
    const Pattern before = p;
    changeDensity(p, wholePattern(p), d);
    // A shape that already fills every step of its own grid (hats on 1/16, an arp): more notes go between the steps
    // (never off a grid the person chose).
    if (l.density > 0 && l.grid.empty() && p.notes.size() <= before.notes.size() && grid / 2 >= kPpq / 16) {
      p = before;
      d.grid = grid / 2;
      changeDensity(p, wholePattern(p), d);
    }
    // Fewer notes never means none: the notes on the beat (else the first one) stay.
    if (p.notes.empty()) {
      for (const auto& n : before.notes) if (n.tick % kPpq == 0) p.notes.push_back(n);
      if (p.notes.empty()) p.notes.push_back(before.notes.front());
    }
  }
  variation(p, l.variationEvery, tone, drum, grid, draw);
  if (l.shuffle) shuffleNotes(p, drum, draw);
  if (l.reverse) reverseNotes(p);
  roll(p, l.rollBeats, rollPitch);
  const double hum = l.humanize >= 0 ? l.humanize : style ? style->humanize : 0.0;
  if (hum > 0 && !p.notes.empty()) humanizeNotes(p, {}, (int64_t)std::lround(hum * 24), hum * 0.15, seed ^ 0xA5A5A5A5ULL);
  if (l.quantize > 0 && !p.notes.empty()) quantizeNotes(p, {}, grid, l.quantize, 0);
  // Quantize, shuffle, reverse or a fill can land two notes of one sound on one tick: the louder stays.
  {
    std::map<std::pair<int64_t, int>, size_t> at;
    std::vector<Note> one;
    for (const auto& n : p.notes) {
      const auto [it, isNew] = at.try_emplace({n.tick, n.pitch}, one.size());
      if (isNew) one.push_back(n);
      else if (n.velocity > one[it->second].velocity) one[it->second] = n;
    }
    p.notes = std::move(one);
  }
  sortNotes(p);
  if (p.notes.size() > kMaxNotes) p.notes.resize(kMaxNotes);
}

}  // namespace

std::vector<PCoreResult> pcoreWrite(const PCoreCatalog& c, const PCoreLayers& l, int64_t start, int64_t end,
                                    std::vector<PCoreTarget>& targets, bool clearBass) {
  checkPCoreLayers(l);
  if (start < 0 || end <= start) throw DocumentError("pcore", "the bars are empty");
  if (end > kMaxBars * kBar) throw DocumentError("pcore", "64 bars at most");
  if (targets.empty()) throw DocumentError("pcore", "no track to write into");
  const PCoreStyle* style = l.style.empty() ? nullptr : c.style(l.style);
  if (!l.style.empty() && !style) throw DocumentError("pcore.style", "unknown style " + l.style);
  const std::string scaleId = !l.scale.empty() ? l.scale : style && !style->scales.empty() ? style->scales.front() : "minor";
  const PCoreScale* scale = c.scale(scaleId);
  if (!scale) throw DocumentError("pcore.scale", "unknown scale " + scaleId);
  const std::string rootName = !l.root.empty() ? l.root : style ? style->root : "A";
  Tonal tone{scale->steps, pcoreRootPitchClass(rootName), style ? style->chaos : 0.3};
  const int64_t len = end - start;

  // Check everything first: nothing changes when one target is wrong.
  struct Plan { PCoreTarget* t; const PCorePart* part; const PCoreShape* shape; };
  std::vector<Plan> plans;
  for (auto& t : targets) {
    if (!t.pattern) throw DocumentError("pcore", "a track without a pattern");
    const PCorePart* part = c.part(t.part);
    if (!part) throw DocumentError("pcore.part", "unknown part " + t.part + " (" + t.trackId + ")");
    const PCoreShape* shape = nullptr;
    if (l.mode == "create") {
      if (t.shape.empty()) { const auto list = pcoreShapesFor(c, t.part, l.style); if (!list.empty()) shape = list.front(); }
      else shape = c.shape(t.shape);
      if (!shape || shape->part != t.part) throw DocumentError("pcore.shape", "unknown shape " + t.shape + " for " + t.part);
    }
    plans.push_back({&t, part, shape});
  }

  std::vector<PCoreResult> results;
  std::vector<Pattern> made;
  for (size_t k = 0; k < plans.size(); ++k) {
    const auto& [t, part, shape] = plans[k];
    const int drumPitch = t->pitch >= 0 ? t->pitch : shape && shape->pitch >= 0 ? shape->pitch : part->pitch;
    const int64_t grid = !l.grid.empty() ? pcoreGridTicks(l.grid) : shape && shape->kind != "roll" ? pcoreGridTicks(shape->grid) : kPpq / 4;
    const uint64_t seed = l.seed * 1000003ULL + k * 7919ULL;
    Pattern p;
    p.lengthTicks = len;
    if (l.mode == "create") {
      Draw draw(seed);
      // A breakdown draws from the call's seed, not the track's: the kick, the bass and the cymbals share one plan.
      if (shape->kind == "breakdown") {
        std::vector<Note> there;
        for (const auto& n : t->pattern->notes)
          if (n.tick >= start && n.tick < end) { Note m = n; m.tick -= start; there.push_back(m); }
        // "Again" on a breakdown: another spacing and length from the call's seed (the same for the kick, the bass
        // and the cymbals), unless the window chose them.
        PCoreLayers lb = l;
        if (l.vary) {
          Draw shared(l.seed * 0x51ED27ULL + 3);
          static const int spacings[] = {1, 2, 4, 8};
          static const double lengths[] = {0.5, 0.625, 0.75, 0.875, 1.0};
          if (lb.breakSpacing == 0) lb.breakSpacing = spacings[shared.below(4)];
          if (lb.breakLength == 0) lb.breakLength = lengths[shared.below(5)];
        }
        p = asPattern(breakdownHits(*shape, lb, *part, tone, drumPitch, len, there), len);
      } else {
        p = asPattern(shape->kind == "roll" ? rollHits(*shape, drumPitch, len) : shapeHits(*shape, *part, tone, drumPitch, grid, len, draw), len);
        if (l.vary) varyTake(p, *part, tone, grid, len, seed);
      }
    } else {
      int i = 0;
      for (const auto& n : t->pattern->notes)
        if (n.tick >= start && n.tick < end) { Note m = n; m.tick -= start; m.length = std::min(m.length, len - m.tick); m.id = "n" + std::to_string(++i); p.notes.push_back(m); }
    }
    int rollPitch = drumPitch;
    if (!part->drum) {
      rollPitch = clampPitch(part->octave + tone.root % 12);
      if (l.mode == "alter" && !p.notes.empty()) rollPitch = p.notes.front().pitch;
    }
    transform(p, l, style, tone, part->drum, grid, rollPitch, seed);
    made.push_back(std::move(p));
  }

  // The bass leaves the kick's ticks free (outside the roll): KBBB stays KBBB whatever the kick plays.
  int kick = -1;
  for (size_t k = 0; k < plans.size(); ++k) if (plans[k].part->id == "kick") { kick = (int)k; break; }
  if (kick >= 0 && clearBass) {
    std::set<int64_t> kicks;
    for (const auto& n : made[(size_t)kick].notes) kicks.insert(n.tick);
    const int64_t rollFrom = len - (int64_t)l.rollBeats * kPpq;
    for (size_t k = 0; k < plans.size(); ++k) {
      if (plans[k].part->id != "bass") continue;
      auto& v = made[k].notes;
      v.erase(std::remove_if(v.begin(), v.end(), [&](const Note& n) {
        if (n.tick >= rollFrom) return false;
        const auto it = kicks.lower_bound(n.tick - 20);
        return it != kicks.end() && *it <= n.tick + 20;
      }), v.end());
    }
  }

  // Write: the range's old notes go ("create") or are replaced by their changed selves ("alter").
  for (size_t k = 0; k < plans.size(); ++k) {
    Pattern& dst = *plans[k].t->pattern;
    dst.notes.erase(std::remove_if(dst.notes.begin(), dst.notes.end(), [&](const Note& n) { return n.tick >= start && n.tick < end; }), dst.notes.end());
    if (dst.lengthTicks < end) dst.lengthTicks = end;
    long next = 1;
    for (const auto& n : dst.notes) next = std::max(next, noteNumber(n.id) + 1);
    for (auto n : made[k].notes) {
      if (dst.notes.size() >= kMaxNotes) break;
      n.tick += start;
      n.length = std::max<int64_t>(1, std::min(n.length, dst.lengthTicks - n.tick));
      n.id = "n" + std::to_string(next++);
      dst.notes.push_back(n);
    }
    sortNotes(dst);
    PCoreResult r;
    r.trackId = plans[k].t->trackId;
    r.part = plans[k].part->id;
    r.shape = plans[k].shape ? plans[k].shape->id : "";
    r.notes = (int)made[k].notes.size();
    r.perBar = (double)r.notes / std::max(1.0, (double)len / kBar);
    results.push_back(r);
  }
  return results;
}


// ------------------------------------------------------------------------------------------------- alignment

namespace {

// The coarsest grid that explains the notes: 9 in 10 of them less than a quarter step from one of its lines.
int64_t fitGrid(const std::vector<Note*>& notes, int64_t start) {
  for (int64_t g : {kPpq / 4, kPpq / 6, kPpq / 8, kPpq / 12, kPpq / 16}) {
    size_t near = 0;
    for (const Note* n : notes) {
      const int64_t r = ((n->tick - start) % g + g) % g;
      if (std::min(r, g - r) * 4 < g) ++near;
    }
    if (near * 10 >= notes.size() * 9) return g;
  }
  return kPpq / 16;
}

int64_t snapTick(int64_t t, int64_t start, int64_t g) {
  const int64_t r = t - start;
  return start + (int64_t)std::floor((double)r / (double)g + 0.5) * g;
}

std::vector<Note*> notesBetween(Pattern& p, int64_t start, int64_t end) {
  std::vector<Note*> v;
  for (auto& n : p.notes) if (n.tick >= start && n.tick < end) v.push_back(&n);
  std::sort(v.begin(), v.end(), [](const Note* a, const Note* b) { return a->tick < b->tick || (a->tick == b->tick && a->pitch < b->pitch); });
  return v;
}

// Two notes of one sound on one tick after the snap: the louder stays.
int dropTwins(Pattern& p, int64_t start, int64_t end) {
  std::map<std::pair<int64_t, int>, size_t> best;
  std::set<size_t> gone;
  for (size_t i = 0; i < p.notes.size(); ++i) {
    const Note& n = p.notes[i];
    if (n.tick < start || n.tick >= end) continue;
    const auto key = std::make_pair(n.tick, n.pitch);
    const auto it = best.find(key);
    if (it == best.end()) { best[key] = i; continue; }
    if (p.notes[it->second].velocity >= n.velocity) gone.insert(i);
    else { gone.insert(it->second); it->second = i; }
  }
  if (gone.empty()) return 0;
  std::vector<Note> keep;
  for (size_t i = 0; i < p.notes.size(); ++i) if (!gone.count(i)) keep.push_back(p.notes[i]);
  p.notes = std::move(keep);
  return (int)gone.size();
}

bool nearKick(const std::vector<int64_t>& kicks, int64_t t, int64_t before, int64_t after) {
  const auto it = std::lower_bound(kicks.begin(), kicks.end(), t - after);
  return it != kicks.end() && *it <= t + before;
}

}  // namespace

void pcoreAlignNotes(const PCoreAlign& a, double bpm, int64_t start, int64_t end, std::vector<PCoreAlignTrack>& tracks,
                     PCoreAlignStats& st) {
  const double ticksPerMs = (bpm > 0 ? bpm : 145.0) / 60.0 * kPpq / 1000.0;
  constexpr int64_t kNear = 20;  // ticks: "on the kick"
  for (auto& t : tracks) {
    if (!t.pattern) continue;
    const std::string& part = t.part;
    if (part != "kick" && part != "bass" && part != "hats" && part != "cymbals") continue;
    Pattern& p = *t.pattern;
    std::vector<int64_t> kicks = t.kicks;
    std::sort(kicks.begin(), kicks.end());
    auto notes = notesBetween(p, start, end);
    if (notes.empty()) continue;
    // 1. Onto the grid the notes were written on.
    const int64_t g = fitGrid(notes, start);
    for (Note* n : notes) {
      const int64_t to = std::clamp<int64_t>(snapTick(n->tick, start, g), start, end - 1);
      if (to != n->tick) { n->tick = to; ++st.moved; }
    }
    st.removed += dropTwins(p, start, end);
    notes = notesBetween(p, start, end);

    if (part == "bass") {
      // 2. No bass under a kick (nor in the steps kept free after it).
      const int64_t clear = (int64_t)a.bassClearSteps * (kPpq / 4);
      const size_t before = p.notes.size();
      p.notes.erase(std::remove_if(p.notes.begin(), p.notes.end(), [&](const Note& n) {
        return n.tick >= start && n.tick < end && nearKick(kicks, n.tick, kNear, kNear + clear);
      }), p.notes.end());
      st.removed += (int)(before - p.notes.size());
      notes = notesBetween(p, start, end);
      // 3. Each note ends before the next kick and leaves a gap before the next note.
      const int64_t tail = (int64_t)std::lround(a.bassTailMs * ticksPerMs);
      for (size_t i = 0; i < notes.size(); ++i) {
        Note& n = *notes[i];
        int64_t nextBass = end;
        for (size_t k = i + 1; k < notes.size(); ++k) if (notes[k]->tick > n.tick) { nextBass = notes[k]->tick; break; }
        const auto kk = std::upper_bound(kicks.begin(), kicks.end(), n.tick);
        const int64_t nextKick = kk != kicks.end() ? *kk : std::numeric_limits<int64_t>::max();
        const int64_t next = std::min(nextBass, nextKick);
        int64_t len = n.length;
        if (next != std::numeric_limits<int64_t>::max()) len = std::min(len, (int64_t)std::floor((double)(next - n.tick) * a.bassGate));
        if (nextKick != std::numeric_limits<int64_t>::max()) len = std::min(len, nextKick - tail - n.tick);
        len = std::max<int64_t>(30, len);
        if (len < n.length) { n.length = len; ++st.shortened; }
      }
      // 4. The first note after each kick softer than the others (80 / 100 / 100).
      for (size_t i = 0; i < notes.size();) {
        const auto kk = std::upper_bound(kicks.begin(), kicks.end(), notes[i]->tick);
        const int64_t until = kk != kicks.end() ? *kk : std::numeric_limits<int64_t>::max();
        size_t j = i + 1;
        double loud = 0;
        while (j < notes.size() && notes[j]->tick < until) loud = std::max(loud, notes[j++]->velocity);
        const bool afterKick = kk != kicks.begin() && notes[i]->tick - *std::prev(kk) <= kPpq / 2;
        if (afterKick && loud > 0) {
          const double v = vel(std::min(notes[i]->velocity, loud * a.bassFirst));
          if (v < notes[i]->velocity) { notes[i]->velocity = v; ++st.softer; }
        }
        i = j;
      }
    }

    if (part == "hats") {
      // 5. A closed hat on a kick softer (or gone); off the offbeat 1/8 softer than the offbeat hats.
      double offbeat = 0, others = 0;
      for (const Note* n : notes) {
        const bool onKick = nearKick(kicks, n->tick, kNear, kNear);
        if ((n->tick - start) % (kPpq / 2) == kPpq / 4) offbeat = std::max(offbeat, n->velocity);
        else if (!onKick) others = std::max(others, n->velocity);
      }
      const double ref = offbeat > 0 ? offbeat : others;  // the offbeat hats never change: the same caps the next time
      std::set<const Note*> gone;
      for (Note* n : notes) {
        double cap = 1.0;
        if (nearKick(kicks, n->tick, kNear, kNear)) {
          if (a.hatsOnKick <= 0) { gone.insert(n); continue; }
          cap = ref * a.hatsOnKick;
        }
        if (offbeat > 0 && (n->tick - start) % (kPpq / 2) != kPpq / 4) cap = std::min(cap, offbeat * (1.0 - a.hatsAccent));
        if (ref <= 0) continue;
        const double v = vel(std::min(n->velocity, cap));
        if (v < n->velocity) { n->velocity = v; ++st.softer; }
      }
      if (!gone.empty()) {
        const size_t before = p.notes.size();
        p.notes.erase(std::remove_if(p.notes.begin(), p.notes.end(), [&](const Note& n) { return gone.count(&n) > 0; }), p.notes.end());
        st.removed += (int)(before - p.notes.size());
        notes = notesBetween(p, start, end);
      }
    }

    if (part == "hats" || part == "cymbals") {
      // 6. Swing (straight grids only) and the push, kept under a quarter step so the next align finds the grid again.
      const bool straight = g == kPpq / 4 || g == kPpq / 8;
      const int64_t swing = straight ? (int64_t)std::lround(std::min(a.swing, 0.25) * (double)g / 2.0) : 0;
      const int64_t limit = g / 8 - 1;
      const int64_t push = std::clamp<int64_t>((int64_t)std::lround(a.pushMs * ticksPerMs), -limit, limit);
      if (swing != 0 || push != 0)
        for (Note* n : notes) {
          const int64_t step = (n->tick - start) / g;
          const int64_t to = std::clamp<int64_t>(n->tick + (step % 2 == 1 ? swing : 0) + push, start, end - 1);
          if (to != n->tick) { n->tick = to; ++st.moved; }
        }
    }
    sortNotes(p);
  }
}

const PCoreAlign* pcoreAlignFor(const PCoreCatalog& c, const PCoreLayers& l) {
  const std::string id = l.align.empty() && l.alignOnly ? std::string("style") : l.align;
  if (id.empty()) return nullptr;
  if (id == "style") {
    const PCoreStyle* st = l.style.empty() ? nullptr : c.style(l.style);
    if (st && !st->align.empty()) return c.align(st->align);
    if (c.aligns.empty()) throw DocumentError("pcore.align", "the catalogue has no alignments");
    return &c.aligns.front();
  }
  const PCoreAlign* a = c.align(id);
  if (!a) throw DocumentError("pcore.align", "unknown alignment " + id);
  return a;
}

std::string patternAt(const Track& t, int64_t songTick) {
  for (const auto& c : t.clips)
    if (songTick >= c.startTick && songTick < c.startTick + c.lengthTicks && t.findPattern(c.patternId)) return c.patternId;
  return t.patterns.empty() ? std::string() : t.patterns.front().id;
}

namespace {

std::string patternIdOf(const Track& t, const Pattern* p) {
  for (const auto& x : t.patterns) if (&x == p) return x.id;
  return {};
}

// Where the kick really plays, in song ticks: every clip of the kick track, its pattern looped inside the clip only
// (a kick past the clip's end is not a kick). A track with no clips: its pattern once from the song's start.
std::vector<int64_t> kickSongTicks(const Track& t, const Pattern* fallback) {
  std::vector<int64_t> out;
  auto play = [&](const Pattern& p, int64_t clipStart, int64_t clipLen, int64_t offset) {
    const int64_t len = std::max<int64_t>(kPpq, p.lengthTicks);
    const int64_t base = clipStart - offset;
    for (int64_t m = (int64_t)std::floor((double)offset / (double)len) - 1; base + m * len < clipStart + clipLen; ++m)
      for (const auto& n : p.notes) {
        if (n.tick >= len) continue;
        const int64_t at = base + m * len + n.tick;
        if (at >= clipStart && at < clipStart + clipLen) out.push_back(at);
      }
  };
  bool any = false;
  for (const auto& c : t.clips)
    if (const Pattern* p = t.findPattern(c.patternId)) { play(*p, c.startTick, c.lengthTicks, c.offsetTicks); any = true; }
  if (!any && fallback) for (const auto& n : fallback->notes) out.push_back(n.tick);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

// The kick's song ticks in a pattern's own ticks, over every repetition that the pattern's clips play (a bass note
// at tick t sounds at t, t + length, t + 2 * length... in a clip longer than its pattern, and a collision in any of
// them is a collision). Each kick is also given one pattern length before and after, so the last note of the
// pattern sees the next repetition's first kick. A pattern without clips: song ticks = pattern ticks.
std::vector<int64_t> kicksInPattern(const Track& t, const std::string& patternId, const Pattern& p, const std::vector<int64_t>& song,
                                    int64_t from, int64_t to) {
  std::vector<int64_t> out;
  const int64_t len = std::max<int64_t>(kPpq, p.lengthTicks);
  bool any = false;
  for (const auto& c : t.clips) {
    if (c.patternId != patternId) continue;
    any = true;
    const int64_t base = c.startTick - c.offsetTicks;
    for (int64_t k : song) {
      if (k < c.startTick || k >= c.startTick + c.lengthTicks) continue;
      const int64_t at = (((k - base) % len) + len) % len;
      for (int64_t x : {at - len, at, at + len}) if (x >= from && x <= to) out.push_back(x);
    }
  }
  if (!any) for (int64_t k : song) if (k >= from && k <= to) out.push_back(k);
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Track* findTrackPtr(Document& doc, const std::string& id) {
  for (auto& x : doc.tracks) if (x.id == id) return &x;
  return nullptr;
}

// The kick track the call goes by: a selected kick track with notes, else the song's kick track (read only).
const Track* kickTrackFor(Document& doc, const std::vector<PCoreTarget>& targets, const Pattern** pattern, std::string* from) {
  for (const auto& tg : targets)
    if (tg.part == "kick" && !tg.pattern->notes.empty())
      if (const Track* t = findTrackPtr(doc, tg.trackId)) { *pattern = tg.pattern; *from = "track"; return t; }
  for (const auto& t : doc.tracks) {
    if (!playsPatterns(t) || pcoreGuessPart(t) != "kick") continue;
    bool notes = false;
    for (const auto& p : t.patterns) notes |= !p.notes.empty();
    if (!notes) continue;
    const std::string pid = patternAt(t, 0);
    *pattern = pid.empty() ? nullptr : t.findPattern(pid);
    *from = "song";
    return &t;
  }
  *from = "beat";
  return nullptr;
}

// Write (kick and bass written together): the bass leaves free the ticks where the kick really plays, in song time,
// whatever the clips' offsets and lengths (pcoreWrite alone compares pattern ticks).
void clearBassUnderKick(Document& doc, const std::vector<PCoreTarget>& made, int64_t start, int64_t end, int rollBeats) {
  const PCoreTarget* kick = nullptr;
  for (const auto& tg : made) if (tg.part == "kick") { kick = &tg; break; }
  if (!kick) return;
  const Track* kt = findTrackPtr(doc, kick->trackId);
  if (!kt) return;
  const std::vector<int64_t> song = kickSongTicks(*kt, kick->pattern);
  const int64_t rollFrom = end - (int64_t)rollBeats * kPpq;
  for (const auto& tg : made) {
    if (tg.part != "bass") continue;
    const Track* bt = findTrackPtr(doc, tg.trackId);
    if (!bt) continue;
    const auto ks = kicksInPattern(*bt, patternIdOf(*bt, tg.pattern), *tg.pattern, song, start - kPpq, end + kPpq);
    auto& v = tg.pattern->notes;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Note& n) {
      if (n.tick < start || n.tick >= end || n.tick >= rollFrom) return false;
      const auto it = std::lower_bound(ks.begin(), ks.end(), n.tick - 20);
      return it != ks.end() && *it <= n.tick + 20;
    }), v.end());
  }
}

// An empty sampler track (Diogo's samples) plays only its keys: a written note outside every sample's loKey..hiKey
// is silent. It moves by octaves to the nearest key the sampler plays, else to the first sample's root.
int fitToSampler(Document& doc, const std::vector<PCoreTarget>& targets, int64_t start, int64_t end) {
  int moved = 0;
  for (const auto& tg : targets) {
    const Track* t = findTrackPtr(doc, tg.trackId);
    if (!t || t->instrument.kind != "builtin" || t->instrument.name != "sampler" || t->instrument.samples.empty()) continue;
    auto plays = [&](int key) {
      for (const auto& s : t->instrument.samples) if (key >= s.loKey && key <= s.hiKey) return true;
      return false;
    };
    for (auto& n : tg.pattern->notes) {
      if (n.tick < start || n.tick >= end || plays(n.pitch)) continue;
      int best = -1;
      for (int o = 1; o <= 10 && best < 0; ++o) {
        if (n.pitch - 12 * o >= 0 && plays(n.pitch - 12 * o)) best = n.pitch - 12 * o;
        else if (n.pitch + 12 * o <= 127 && plays(n.pitch + 12 * o)) best = n.pitch + 12 * o;
      }
      n.pitch = best >= 0 ? best : std::clamp(t->instrument.samples.front().rootKey, 0, 127);
      ++moved;
    }
  }
  return moved;
}

PCoreAlignStats alignSong(Document& doc, const PCoreAlign& a, std::vector<PCoreTarget>& targets, int64_t songTick, int64_t start,
                          int64_t end) {
  PCoreAlignStats st;
  st.align = a.id;
  const double bpm = bpmAt(doc, songTick);
  auto trackOf = [&](const std::string& id) -> Track* { for (auto& x : doc.tracks) if (x.id == id) return &x; return nullptr; };
  // 1. The selected kick tracks first: onto their grid.
  std::vector<PCoreAlignTrack> kicks;
  for (auto& tg : targets) if (tg.part == "kick") kicks.push_back({tg.pattern, "kick", {}});
  pcoreAlignNotes(a, bpm, start, end, kicks, st);
  // 2. Where the kick plays, in song ticks: a selected kick track, else the song's kick track, else every beat.
  const Pattern* kp = nullptr;
  const Track* kt = kickTrackFor(doc, targets, &kp, &st.kickFrom);
  const std::vector<int64_t> song = kt ? kickSongTicks(*kt, kp) : std::vector<int64_t>{};
  // 3. The bass, the hats and the cymbals, each with the kick in its own pattern's time over every repetition its
  // clips play (one beat past the end too: the last bass note stops before the next bar's kick).
  std::vector<PCoreAlignTrack> others;
  for (auto& tg : targets) {
    if (tg.part == "kick") continue;
    const Track* t = trackOf(tg.trackId);
    PCoreAlignTrack at{tg.pattern, tg.part, {}};
    if (kt) {
      if (t) at.kicks = kicksInPattern(*t, patternIdOf(*t, tg.pattern), *tg.pattern, song, start - kPpq, end + kPpq);
    } else {
      for (int64_t k = start; k <= end; k += kPpq) at.kicks.push_back(k);
    }
    others.push_back(std::move(at));
  }
  pcoreAlignNotes(a, bpm, start, end, others, st);
  return st;
}

}  // namespace

std::vector<PCoreResult> pcoreApplyToSong(Document& doc, const PCoreCatalog& c, const PCoreLayers& layers,
                                          const std::vector<PCoreTrackChoice>& tracks, int64_t songTick, int fromBar, int toBar,
                                          const std::map<std::string, std::string>& patterns, PCoreAlignStats* aligned) {
  if (fromBar < 0 || toBar <= fromBar || toBar > kMaxBars) throw DocumentError("pcore.bars", "from 0..63, to after from, 64 at most");
  if (tracks.empty()) throw DocumentError("pcore.tracks", "choose at least one track");
  std::vector<PCoreTarget> targets;
  std::set<std::string> seen;
  for (const auto& ch : tracks) {
    if (!seen.insert(ch.trackId).second) throw DocumentError("pcore.tracks", "a track is named twice: " + ch.trackId);
    Track* t = nullptr;
    for (auto& x : doc.tracks) if (x.id == ch.trackId) t = &x;
    if (!t) throw DocumentError("pcore.tracks", "no track " + ch.trackId);
    if (!playsPatterns(*t)) throw DocumentError("pcore.tracks", ch.trackId + " plays audio, not notes");
    const std::string part = ch.part.empty() ? pcoreGuessPart(*t) : ch.part;
    if (part.empty()) throw DocumentError("pcore.tracks", "say which part " + ch.trackId + " plays");
    const auto named = patterns.find(t->id);
    const std::string pid = named != patterns.end() && t->findPattern(named->second) ? named->second : patternAt(*t, songTick);
    if (pid.empty()) throw DocumentError("pcore.tracks", ch.trackId + " has no pattern");
    Pattern* p = t->findPattern(pid);
    const PCorePart* pp = c.part(part);
    targets.push_back({t->id, part, ch.shape, p, pp && pp->drum ? trackRoot(*t, p) : -1});
  }
  const PCoreAlign* align = pcoreAlignFor(c, layers);  // an unknown id throws before anything changes
  const int64_t start = (int64_t)fromBar * kBar, end = (int64_t)toBar * kBar;
  if (layers.alignOnly) {  // the Align button: the notes stay, only aligned
    checkPCoreLayers(layers);
    std::vector<PCoreResult> out;
    PCoreAlignStats stats = alignSong(doc, *align, targets, songTick, start, end);
    for (const auto& tg : targets) {
      PCoreResult r{tg.trackId, tg.part, "", 0, 0};
      for (const auto& n : tg.pattern->notes) if (n.tick >= start && n.tick < end) ++r.notes;
      r.perBar = (double)r.notes / std::max(1, toBar - fromBar);
      out.push_back(r);
    }
    if (aligned) *aligned = stats;
    return out;
  }
  // A drum track whose notes are all one sound keeps that sound; an empty pattern takes the part's, except on a
  // sampler, which keeps its sample's root key (another key may be silent there).
  for (auto& tg : targets)
    if (tg.pitch >= 0 && tg.pattern->notes.empty()) {
      const Track* t = findTrackPtr(doc, tg.trackId);
      if (!t || t->instrument.name != "sampler") tg.pitch = -1;
    }
  // With "create": the tracks are written with their shape ("" = the style's first for the part); those whose
  // shape is "keep" keep their notes and are only changed. Both
  // groups are checked before either is written.
  std::vector<PCoreTarget> make, keep;
  for (auto& tg : targets) (layers.mode == "alter" || tg.shape == "keep" ? keep : make).push_back(tg);
  PCoreLayers alter = layers;
  alter.mode = "alter";
  PCoreLayers create = layers;
  create.mode = "create";
  if (!make.empty() && !keep.empty()) {  // a dry run of both on copies: nothing changes when one is wrong
    std::vector<Pattern> copies;
    copies.reserve(targets.size());
    std::vector<PCoreTarget> m2 = make, k2 = keep;
    for (auto& tg : m2) { copies.push_back(*tg.pattern); tg.pattern = &copies.back(); }
    for (auto& tg : k2) { copies.push_back(*tg.pattern); tg.pattern = &copies.back(); }
    pcoreWrite(c, create, (int64_t)fromBar * kBar, (int64_t)toBar * kBar, m2, false);
    pcoreWrite(c, alter, (int64_t)fromBar * kBar, (int64_t)toBar * kBar, k2, false);
  }
  std::vector<PCoreResult> out;
  if (!make.empty()) {
    out = pcoreWrite(c, create, (int64_t)fromBar * kBar, (int64_t)toBar * kBar, make, false);
    clearBassUnderKick(doc, make, start, end, create.rollBeats);
  }
  if (!keep.empty()) {
    // "Keep the notes there": no note is taken away (Align is the button that moves the bass off the kick).
    const auto more = pcoreWrite(c, alter, (int64_t)fromBar * kBar, (int64_t)toBar * kBar, keep, false);
    out.insert(out.end(), more.begin(), more.end());
  }
  if (align) {
    const PCoreAlignStats stats = alignSong(doc, *align, targets, songTick, start, end);
    if (aligned) *aligned = stats;
  }
  fitToSampler(doc, targets, start, end);
  return out;
}

PCoreLayers pcoreLayersFromJson(const json& j) {
  if (!j.is_object()) throw DocumentError("pcore", "the settings are an object");
  PCoreLayers l;
  l.mode = str(j, "mode", l.mode);
  l.style = str(j, "style");
  l.scale = str(j, "scale");
  l.root = str(j, "root");
  l.grid = str(j, "grid");
  const auto num = [&](const char* k, double d) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : d; };
  l.density = num("density", 0);
  l.humanize = num("humanize", -1);
  l.quantize = num("quantize", 0);
  l.shuffle = j.value("shuffle", false);
  l.reverse = j.value("reverse", false);
  // Whole numbers kept inside int before the cast (sweep: a huge or NaN value was undefined); checkPCoreLayers then
  // refuses what is out of its own range.
  const auto whole = [&](const char* k) {
    const double v = num(k, 0);
    return v == v ? (int)std::clamp(v, -1.0e6, 1.0e6) : 0;
  };
  l.variationEvery = whole("variationEvery");
  l.rollBeats = whole("rollBeats");
  l.seed = j.contains("seed") && (j["seed"].is_number_unsigned() || (j["seed"].is_number_integer() && j["seed"].get<int64_t>() >= 0)) ? j["seed"].get<uint64_t>() : 1;
  l.breakFeel = str(j, "breakFeel");
  l.breakSpacing = whole("breakSpacing");
  l.breakLength = num("breakLength", 0);
  l.align = str(j, "align");
  l.alignOnly = j.value("alignOnly", false);
  l.vary = j.value("vary", false);
  checkPCoreLayers(l);
  return l;
}

json pcoreLayersToJson(const PCoreLayers& l) {
  return {{"mode", l.mode}, {"style", l.style}, {"scale", l.scale}, {"root", l.root}, {"grid", l.grid}, {"density", l.density},
          {"humanize", l.humanize}, {"quantize", l.quantize}, {"shuffle", l.shuffle}, {"reverse", l.reverse},
          {"variationEvery", l.variationEvery}, {"rollBeats", l.rollBeats}, {"seed", l.seed},
          {"breakFeel", l.breakFeel}, {"breakSpacing", l.breakSpacing}, {"breakLength", l.breakLength},
          {"align", l.align}, {"alignOnly", l.alignOnly}, {"vary", l.vary}};
}

int pcoreShuffle(Pattern& pattern, const std::set<std::string>& ids, bool drum, uint64_t seed) {
  Pattern part;  // the chosen notes, shuffled as P-Core does, then written back by id
  part.lengthTicks = pattern.lengthTicks;
  for (const auto& n : pattern.notes) if (ids.empty() || ids.count(n.id)) part.notes.push_back(n);
  Draw draw(seed);
  shuffleNotes(part, drum, draw);
  int changed = 0;
  for (const auto& s : part.notes)
    for (auto& n : pattern.notes)
      if (n.id == s.id) {
        if (n.pitch != s.pitch || n.velocity != s.velocity) ++changed;
        n.pitch = s.pitch;
        n.velocity = s.velocity;
      }
  return changed;
}

}  // namespace core::doc
