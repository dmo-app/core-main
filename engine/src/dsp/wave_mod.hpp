// W-Core modulation (Diogo 2026-11-08, "Brief de evolução do synth modular"; the Serum screen as the minimum): one
// modulation system inside the synth, per layer (each coloured principal of the panel is one layer):
//   sources  LFO 1..4, ENV 1..3 (ENV 1 is always the volume), Velocity, Note, Macro 1..4;
//   links    source -> destination, with an amount (+ or -), optionally scaled by a second source (aux), on/off.
// The knobs of the sources (ENV times, LFO rates, macro values) are ordinary parameters of the layer, so they can be
// automated like any other; what is structural (which LFO shape, which link goes where) lives here, as data of the
// track ("synth" in the document), with stable names, never positions on the screen.
//
// A destination is a parameter of the layer, by name, or one of the virtual ones below. Hz parameters move in
// octaves (as the track's modulation does), the rest in their own units. The old fixed movements (LFO 1/2 with one
// destination each, Drift, the filter envelope, Zap, Velocity, Key follow) are links of this system: a song made
// before plays the same (the document converts them, then they are gone).
#pragma once
#include <algorithm>
#include <array>
#include <stdexcept>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>
#include "../../third_party/json.hpp"

namespace core::wmod {

using json = nlohmann::json;

constexpr int kLfos = 4, kEnvs = 3, kMacros = 4, kLinks = 32, kPoints = 32;

enum Source : uint8_t {
  SrcNone = 0,
  SrcLfo1, SrcLfo2, SrcLfo3, SrcLfo4,
  SrcEnv1, SrcEnv2, SrcEnv3,
  SrcVelocity,  // the note's velocity, 0..1
  SrcNote,      // the key, (pitch - 60) / 60: -1 at C-1, 0 at C4, +1 at C9
  SrcMacro1, SrcMacro2, SrcMacro3, SrcMacro4,
  SrcModWheel,  // the mod wheel (the knob "modWheel", automated like any knob), 0..1
  SrcCount
};
// Bipolar sources run -1..1 (the LFOs, Note); the rest 0..1.
inline bool bipolar(Source s) { return (s >= SrcLfo1 && s <= SrcLfo4) || s == SrcNote; }

// Virtual destinations (not parameters): negative ids.
enum Virtual : int {
  DstPitch = -1,   // semitones, every oscillator of the voice (vibrato, zap, dive)
  DstVolume = -2,  // the voice's volume: amount 1 = the source's lowest point is silence (tremolo, velocity)
  DstPan = -3,     // -1 (left) .. 1 (right)
  DstDetune = -4,  // the Spread's detune, as a factor: amount 0.3 = +-30 % (Drift)
};

enum class LfoMode : uint8_t {
  Song,  // placed by the song's position: every pass of a loop the same, every voice together (as before)
  Note,  // starts again on each note, each voice its own
  Once,  // starts on each note and stops at the end of its cycle (an envelope drawn as an LFO)
};
enum class LfoShape : uint8_t { Sine, Triangle, Saw, Square, Random, Points };

struct Point { double x = 0, y = 0, curve = 0; };  // x 0..1, y -1..1, curve -1..1 (0 straight) towards the next point

struct Lfo {
  LfoShape shape = LfoShape::Sine;
  LfoMode mode = LfoMode::Song;
  bool sync = true;  // rate in beats per cycle (lfoNRate), else in Hz (lfoNHz); both locked to 4 bars in Song mode
  int points = 0;
  Point point[kPoints];
};

struct Link {
  std::string id;          // stable ("m1", "m2"...): what the matrix and the modules edit
  Source source = SrcNone;
  Source aux = SrcNone;    // scales the link: 1 - auxDepth + auxDepth * aux (the velocity on the filter envelope)
  double auxDepth = 1;
  std::string target;      // a parameter of the layer ("tone") or a virtual one ("pitch")
  double amount = 0;       // in the target's units (octaves for Hz), + or -
  bool unipolar = false;   // a -1..1 source read as 0..1 (an LFO that only opens, never closes)
  bool on = true;
};

struct Layer {
  Lfo lfo[kLfos];
  std::vector<Link> links;  // at most kLinks
};

struct Patch {
  Layer layer[4];
  bool empty() const;
};


// ---- what the voice runs (no allocation; filled on the control thread from a Patch)
struct RtLink {
  Source source = SrcNone, aux = SrcNone;
  double auxDepth = 1, amount = 0;
  bool unipolar = false;
  int dst = 0;           // a slot of the layer's parameters (>= 0) or a Virtual (< 0)
  bool octaves = false;  // a Hz parameter: the amount is in octaves
};
struct RtLayer {
  Lfo lfo[kLfos];
  RtLink link[kLinks];
  int links = 0;
  bool usesSource[SrcCount] = {};
};

// One LFO shape's value at phase p (0..1), -1..1. `held` is the random step of this cycle.
inline double lfoShape(const Lfo& l, double p, double held) {
  constexpr double kTau = 6.283185307179586;
  switch (l.shape) {
    case LfoShape::Triangle: return 1 - 4 * std::fabs(p - 0.5);  // from the bottom
    case LfoShape::Saw: return 1 - 2 * p;                        // falling
    case LfoShape::Square: return p < 0.5 ? 1.0 : -1.0;
    case LfoShape::Random: return held;
    case LfoShape::Points: {
      if (l.points <= 0) return 0;
      if (l.points == 1) return l.point[0].y;
      // The segment p falls in: from the last point at or before p to the next one (after the last point the curve
      // runs on to the first point, one cycle later). The points are sorted by x.
      int i = -1;
      for (int k = 0; k < l.points; ++k) if (l.point[k].x <= p) i = k;
      const Point& a = i < 0 ? l.point[l.points - 1] : l.point[i];
      const Point& b = (i >= 0 && i + 1 < l.points) ? l.point[i + 1] : l.point[0];
      const double ax = i < 0 ? a.x - 1 : a.x;
      const double bx = (i >= 0 && i + 1 < l.points) ? b.x : b.x + (i < 0 ? 0 : 1);
      const double span = bx - ax;
      double t = span > 1e-9 ? (p - ax) / span : 0;
      t = std::fmin(1, std::fmax(0, t));
      if (a.curve != 0) {  // bend: curve > 0 slow then fast, < 0 fast then slow
        const double e = std::pow(2.0, 4 * a.curve);
        t = std::pow(t, e);
      }
      return a.y + (b.y - a.y) * t;
    }
    default: return std::sin(kTau * p);
  }
}

// ---- names, reading and writing
// The document's "synth" of a W-Core track: {"layers": [{"lfos": [...], "links": [...]}, ...]}. parsePatch throws
// std::runtime_error on anything it cannot read; a target that is not one of `paramNames` (nor virtual) too.

namespace detail {
inline const char* const kSourceNames[SrcCount] = {"", "lfo1", "lfo2", "lfo3", "lfo4", "env1", "env2", "env3", "velocity", "note",
                                            "macro1", "macro2", "macro3", "macro4", "modwheel"};
inline const char* const kShapes[] = {"sine", "triangle", "saw", "square", "random", "points"};
inline const char* const kModes[] = {"song", "note", "once"};
struct VirtualName { int id; const char* name; };
inline const VirtualName kVirtual[] = {{DstPitch, "pitch"}, {DstVolume, "volume"}, {DstPan, "pan"}, {DstDetune, "detune"}};

[[noreturn]] inline void fail(const std::string& what) { throw std::runtime_error("W-Core modulation: " + what); }
inline double num(const json& j, const char* key, double def) {
  if (!j.contains(key)) return def;
  if (!j[key].is_number()) fail(std::string(key) + " must be a number");
  const double v = j[key].get<double>();
  if (!std::isfinite(v)) fail(std::string(key) + " must be finite");
  return v;
}
}  // namespace detail

// The parameters of a layer a link can move (the rest are switches, table numbers, times: they would click or mean
// nothing). "tone" and "tone2" move in octaves; the others in their own units, added to the knob and kept inside its range.
inline const std::vector<std::string>& targets() {
  static const std::vector<std::string> t = {
      "wave", "uniDetune", "uniBlend", "uniWidth", "subLevel", "tone", "bite", "level", "noise", "noiseColor", "fm", "fmRatio", "tune", "grain", "grainPos",
      "osc2", "osc2Wave", "osc2Semi", "osc2Fine", "osc1Pos", "osc2Pos", "drive", "filterDrive", "filterShape", "osc1WarpAmt", "osc2WarpAmt",
      "tone2", "bite2", "filter2Drive", "filter2Shape",
      "osc1Level", "subPan", "filterMix", "filter2Mix", "driveColour", "driveMix",
      "lfo1Depth", "lfo2Depth", "lfo3Depth", "lfo4Depth", "outPan", "grainPitch"};
  return t;
}

inline const char* sourceName(Source s) { return s < SrcCount ? detail::kSourceNames[s] : ""; }
inline Source sourceFromName(const std::string& name) {
  for (int i = 1; i < SrcCount; ++i) if (name == detail::kSourceNames[i]) return (Source)i;
  return SrcNone;
}
inline const char* virtualName(int dst) {
  for (const auto& v : detail::kVirtual) if (v.id == dst) return v.name;
  return nullptr;
}
inline int virtualFromName(const std::string& name) {
  for (const auto& v : detail::kVirtual) if (name == v.name) return v.id;
  return 0;
}

inline bool Patch::empty() const {
  for (const auto& l : layer) {
    if (!l.links.empty()) return false;
    for (const auto& o : l.lfo) if (o.shape != LfoShape::Sine || o.mode != LfoMode::Song || !o.sync || o.points) return false;
  }
  return true;
}

inline Patch parsePatch(const json& j, const std::vector<std::string>& paramNames) {
  Patch p;
  if (j.is_null()) return p;
  if (!j.is_object()) detail::fail("synth must be an object");
  if (!j.contains("layers")) return p;
  if (!j["layers"].is_array() || j["layers"].size() > 4) detail::fail("layers must be a list of at most 4");
  for (size_t li = 0; li < j["layers"].size(); ++li) {
    const json& lj = j["layers"][li];
    if (!lj.is_object()) detail::fail("a layer must be an object");
    Layer& L = p.layer[li];
    if (lj.contains("lfos")) {
      if (!lj["lfos"].is_array() || lj["lfos"].size() > (size_t)kLfos) detail::fail("lfos must be a list of at most 4");
      for (size_t k = 0; k < lj["lfos"].size(); ++k) {
        const json& o = lj["lfos"][k];
        if (!o.is_object()) detail::fail("an LFO must be an object");
        Lfo& lfo = L.lfo[k];
        const std::string shape = o.value("shape", "sine"), mode = o.value("mode", "song");
        bool found = false;
        for (int s = 0; s < 6; ++s) if (shape == detail::kShapes[s]) { lfo.shape = (LfoShape)s; found = true; }
        if (!found) detail::fail("unknown LFO shape '" + shape + "'");
        found = false;
        for (int m = 0; m < 3; ++m) if (mode == detail::kModes[m]) { lfo.mode = (LfoMode)m; found = true; }
        if (!found) detail::fail("unknown LFO mode '" + mode + "'");
        lfo.sync = o.value("sync", true);
        if (o.contains("points")) {
          if (!o["points"].is_array() || o["points"].size() > (size_t)kPoints) detail::fail("an LFO has at most 32 points");
          double lastX = -1;
          for (const auto& pt : o["points"]) {
            if (!pt.is_array() || pt.size() < 2 || pt.size() > 3) detail::fail("a point is [x, y] or [x, y, curve]");
            for (const auto& v : pt) if (!v.is_number() || !std::isfinite(v.get<double>())) detail::fail("a point holds numbers");
            Point q{std::clamp(pt[0].get<double>(), 0.0, 1.0), std::clamp(pt[1].get<double>(), -1.0, 1.0),
                    pt.size() > 2 ? std::clamp(pt[2].get<double>(), -1.0, 1.0) : 0.0};
            if (q.x < lastX) detail::fail("the points of an LFO go from left to right");
            lastX = q.x;
            lfo.point[lfo.points++] = q;
          }
        }
        if (lfo.shape == LfoShape::Points && lfo.points == 0) detail::fail("a drawn LFO needs its points");
      }
    }
    if (lj.contains("links")) {
      if (!lj["links"].is_array() || lj["links"].size() > (size_t)kLinks) detail::fail("a layer has at most 32 links");
      for (const auto& k : lj["links"]) {
        if (!k.is_object()) detail::fail("a link must be an object");
        Link link;
        link.id = k.value("id", "");
        if (link.id.empty()) detail::fail("a link needs its id");
        for (const auto& other : L.links) if (other.id == link.id) detail::fail("two links named '" + link.id + "'");
        link.source = sourceFromName(k.value("source", ""));
        if (link.source == SrcNone) detail::fail("link '" + link.id + "': unknown source '" + k.value("source", "") + "'");
        if (k.contains("aux")) {
          link.aux = sourceFromName(k.value("aux", ""));
          if (link.aux == SrcNone) detail::fail("link '" + link.id + "': unknown aux '" + k.value("aux", "") + "'");
        }
        link.auxDepth = std::clamp(detail::num(k, "auxDepth", 1), 0.0, 1.0);
        link.target = k.value("target", "");
        if (!virtualFromName(link.target) &&
            std::find(paramNames.begin(), paramNames.end(), link.target) == paramNames.end())
          detail::fail("link '" + link.id + "': no destination '" + link.target + "'");
        link.amount = detail::num(k, "amount", 0);
        link.unipolar = k.value("unipolar", false);
        link.on = k.value("on", true);
        L.links.push_back(std::move(link));
      }
    }
  }
  return p;
}

inline json patchToJson(const Patch& p) {
  json layers = json::array();
  int last = -1;
  for (int i = 0; i < 4; ++i) {
    const Layer& L = p.layer[i];
    json lfos = json::array();
    int lastLfo = -1;
    for (int k = 0; k < kLfos; ++k) {
      const Lfo& o = L.lfo[k];
      json oj = {{"shape", detail::kShapes[(int)o.shape]}, {"mode", detail::kModes[(int)o.mode]}, {"sync", o.sync}};
      if (o.points) {
        json pts = json::array();
        for (int q = 0; q < o.points; ++q) {
          json pt = {o.point[q].x, o.point[q].y};
          if (o.point[q].curve != 0) pt.push_back(o.point[q].curve);
          pts.push_back(pt);
        }
        oj["points"] = pts;
      }
      if (o.shape != LfoShape::Sine || o.mode != LfoMode::Song || !o.sync || o.points) lastLfo = k;
      lfos.push_back(oj);
    }
    while ((int)lfos.size() > lastLfo + 1) lfos.erase(lfos.size() - 1);
    json links = json::array();
    for (const auto& k : L.links) {
      json kj = {{"id", k.id}, {"source", sourceName(k.source)}, {"target", k.target}, {"amount", k.amount}};
      if (k.aux != SrcNone) { kj["aux"] = sourceName(k.aux); kj["auxDepth"] = k.auxDepth; }
      if (k.unipolar) kj["unipolar"] = true;
      if (!k.on) kj["on"] = false;
      links.push_back(kj);
    }
    json lj = json::object();
    if (!lfos.empty()) lj["lfos"] = lfos;
    if (!links.empty()) lj["links"] = links;
    if (!lj.empty()) last = i;
    layers.push_back(lj);
  }
  while ((int)layers.size() > last + 1) layers.erase(layers.size() - 1);
  if (layers.empty()) return json();
  return json{{"layers", layers}};
}

}  // namespace core::wmod
