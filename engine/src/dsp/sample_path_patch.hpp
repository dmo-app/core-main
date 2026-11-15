// Core Glitch · Sample Path: the map (slices, nodes, links, patterns) and the walk along it. Header-only, no audio:
// the engine plays it (sample_path.hpp), the document checks it and the window draws it, all from this one place.
//
// The track's "synth" (json) of a "samplepath" track:
// {"sync": true, "bpm": 190,
//  "layers": [ // 1..4 samplers; the sound of sampler i is the track's sample zone with loKey == i
//    {"slices": [[start, end], ...],          // seconds in the sound, at most 32
//     "pattern": "A", "mute": false,
//     "fx": [{"type": "delay"|"crush"|"filter"|"stutter"|"off", "a": 0..1, "b": 0..1, "mix": 0..1}, x2],
//     "patterns": {"A": {"nodes": [{"id", "slice", "x", "y", "startOffset", "endOffset", "pitch", "fine", "length",
//                                   "gate", "repeats", "level", "pan", "attack", "release", "reverse", "lock",
//                                   "links": [{"to": id, "weight": 0..100, "main": bool}]}],
//                        "start": id, "mode": "follow"|"weighted", "rate": 0.5|1|2, "end": "stop"|"restart"|"hold",
//                        "variation": 0..100, "seed": 0..9999, "lockPath": bool, "fade": ms, "shape": ""},
//                  "B": ..., "C": ..., "D": ...}}]}
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>
#include "../../third_party/json.hpp"

namespace core::spath {

using json = nlohmann::json;

constexpr int kLayers = 4;      // samplers (tabs)
constexpr int kMaxSlices = 32;  // per sampler
constexpr int kMaxNodes = 32;   // per pattern
constexpr int kMaxLinks = 8;    // exits per node
constexpr int kPatterns = 4;    // A..D
constexpr int kFxSlots = 2;     // per sampler

// The rhythmic lengths a node can have, in beats (quarter notes).
struct Length { const char* name; double beats; };
inline const Length kLengths[] = {{"1/32", 0.125}, {"1/16T", 1.0 / 6}, {"1/16", 0.25}, {"1/8T", 1.0 / 3}, {"1/8", 0.5}, {"1/4", 1}, {"1/2", 2}};
inline double lengthBeats(const std::string& name) {
  for (const auto& l : kLengths) if (name == l.name) return l.beats;
  return 0.25;
}
inline bool knownLength(const std::string& name) {
  for (const auto& l : kLengths) if (name == l.name) return true;
  return false;
}

struct Slice { double start = 0, end = 0; };  // seconds in the sound

struct Link { int to = 0; double weight = 100; bool main = false; };  // to: the target's index in Pattern::nodes

struct Node {
  int id = 1;                 // the number the user sees
  int slice = 0;
  double x = 0, y = 0;        // where it sits on the map (no effect on the sound)
  double startOffset = 0, endOffset = 0;  // seconds added to the slice's start and end: this node's part of it
  double pitch = 0, fine = 0;             // semitones, cents
  std::string length = "1/16";
  double gate = 100;          // % of the length it sounds (at most the part's own length)
  int repeats = 1;            // steps it plays before it leaves
  double level = 0, pan = 0;  // dB, -100..100
  double attackMs = 1, releaseMs = 40;
  bool reverse = false, lock = false;
  std::vector<Link> links;
};

enum class Mode { Follow, Weighted };
enum class End { Stop, Restart, Hold };

struct Pattern {
  std::vector<Node> nodes;
  int start = 0;              // index in nodes
  Mode mode = Mode::Weighted;
  double rate = 1;            // 0.5, 1 or 2
  End end = End::Restart;
  double variation = 35;      // % (Generate Variation)
  uint32_t seed = 128;
  bool lockPath = false;
  double fadeMs = 3;          // the shortest fade in and out of every part (0: hard cut)
  std::string shape;          // the last shape applied ("" once the links were edited by hand)
  int nextId = 1;
  int find(int id) const { for (size_t i = 0; i < nodes.size(); ++i) if (nodes[i].id == id) return (int)i; return -1; }
};

enum class FxType { Off, Delay, Crush, Filter, Stutter };
inline const char* const kFxNames[] = {"off", "delay", "crush", "filter", "stutter"};
struct Fx { FxType type = FxType::Off; double a = 0.5, b = 0.5, mix = 0; };

struct Layer {
  std::vector<Slice> slices;
  Pattern patterns[kPatterns];
  bool used[kPatterns] = {true, false, false, false};  // an unused pattern starts as a copy of the current one
  int pattern = 0;
  bool mute = false;
  Fx fx[kFxSlots] = {{FxType::Delay, 0.6, 0.35, 0.2}, {FxType::Crush, 0.5, 0.4, 0}};
};

struct Patch {
  Layer layers[kLayers];
  bool sync = true;           // steps follow the song's tempo (else bpm)
  double bpm = 190;
};

// ---- random: one small generator, the same everywhere (the engine, the window, a test) ----
inline uint32_t nextRandom(uint32_t& s) {  // xorshift32
  if (s == 0) s = 0x9E3779B9u;
  s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  return s;
}
inline double random01(uint32_t& s) { return (nextRandom(s) >> 8) * (1.0 / 16777216.0); }
inline uint32_t hashText(const std::string& t, uint32_t h = 2166136261u) {
  for (unsigned char c : t) { h ^= c; h *= 16777619u; }
  return h;
}

// ---- the walk: pure, the same pattern and seed give the same steps ----
struct Walk {
  int node = -1;              // index of the node playing (-1: the walk has ended)
  int repeat = 0;             // steps the node has already played
  uint32_t rng = 1;
  void reset(const Pattern& p) {
    node = p.nodes.empty() ? -1 : std::clamp(p.start, 0, (int)p.nodes.size() - 1);
    repeat = 0;
    rng = p.seed * 2654435761u + 1u;
  }
  // After the current node's step: where the walk goes next. Returns the index of the link taken in the node's links,
  // -1 for a repeat, an end behaviour or the end.
  int advance(const Pattern& p) {
    if (node < 0 || node >= (int)p.nodes.size()) { node = -1; return -1; }
    const Node& n = p.nodes[node];
    if (++repeat < std::max(1, n.repeats)) return -1;
    repeat = 0;
    if (n.links.empty()) {
      if (p.end == End::Stop) node = -1;
      else if (p.end == End::Restart) node = std::clamp(p.start, 0, (int)p.nodes.size() - 1);
      return -1;  // Hold: the same node again
    }
    int pick = 0;
    if (p.mode == Mode::Follow) {
      for (size_t i = 0; i < n.links.size(); ++i) if (n.links[i].main) { pick = (int)i; break; }
    } else {
      double total = 0;
      for (const auto& l : n.links) total += std::max(0.0, l.weight);
      double r = random01(rng) * total;  // drawn even when the total is 0, so the sequence does not depend on weights at 0
      pick = (int)n.links.size() - 1;
      if (total > 0)
        for (size_t i = 0; i < n.links.size(); ++i) { r -= std::max(0.0, n.links[i].weight); if (r < 0) { pick = (int)i; break; } }
      else pick = 0;
    }
    node = n.links[pick].to;
    return pick;
  }
};

// A node's step, in beats.
inline double stepBeats(const Pattern& p, const Node& n) { return lengthBeats(n.length) / std::clamp(p.rate, 0.25, 4.0); }

// ---- reading and writing ----
namespace detail {
inline double num(const json& o, const char* k, double def, double lo, double hi) {
  if (!o.contains(k) || o[k].is_null()) return def;
  if (!o[k].is_number()) throw std::runtime_error(std::string(k) + " must be a number.");
  const double v = o[k].get<double>();
  if (!std::isfinite(v)) throw std::runtime_error(std::string(k) + " must be finite.");
  return std::clamp(v, lo, hi);
}
inline bool flag(const json& o, const char* k, bool def) {
  if (!o.contains(k) || o[k].is_null()) return def;
  if (!o[k].is_boolean()) throw std::runtime_error(std::string(k) + " must be true or false.");
  return o[k].get<bool>();
}
inline std::string text(const json& o, const char* k, const std::string& def) {
  if (!o.contains(k) || o[k].is_null()) return def;
  if (!o[k].is_string()) throw std::runtime_error(std::string(k) + " must be text.");
  return o[k].get<std::string>();
}
inline void fixMains(Pattern& p) {
  for (auto& n : p.nodes) {
    bool seen = false;
    for (auto& l : n.links) { if (l.main && seen) l.main = false; seen = seen || l.main; }
    if (!seen && !n.links.empty()) n.links[0].main = true;
  }
}
}  // namespace detail

inline Pattern parsePattern(const json& v, int slices) {
  using namespace detail;
  if (!v.is_object()) throw std::runtime_error("a pattern must be an object.");
  Pattern p;
  if (v.contains("nodes") && !v["nodes"].is_array()) throw std::runtime_error("nodes must be a list.");
  const json nodes = v.value("nodes", json::array());
  if (nodes.size() > (size_t)kMaxNodes) throw std::runtime_error("at most " + std::to_string(kMaxNodes) + " nodes.");
  std::vector<std::vector<std::pair<int, Link>>> rawLinks;  // target ids, resolved after every node is read
  for (const auto& nv : nodes) {
    if (!nv.is_object()) throw std::runtime_error("a node must be an object.");
    Node n;
    n.id = (int)num(nv, "id", (double)p.nodes.size() + 1, 1, 999);
    if (p.find(n.id) >= 0) throw std::runtime_error("node " + std::to_string(n.id) + " appears twice.");
    n.slice = (int)num(nv, "slice", 0, 0, kMaxSlices - 1);
    if (slices > 0) n.slice = std::min(n.slice, slices - 1);
    n.x = num(nv, "x", 100, -10000, 10000);
    n.y = num(nv, "y", 100, -10000, 10000);
    n.startOffset = num(nv, "startOffset", 0, -600, 600);
    n.endOffset = num(nv, "endOffset", 0, -600, 600);
    n.pitch = num(nv, "pitch", 0, -24, 24);
    n.fine = num(nv, "fine", 0, -100, 100);
    n.length = text(nv, "length", "1/16");
    if (!knownLength(n.length)) throw std::runtime_error("unknown length '" + n.length + "'.");
    n.gate = num(nv, "gate", 100, 5, 100);
    n.repeats = (int)num(nv, "repeats", 1, 1, 8);
    n.level = num(nv, "level", 0, -30, 6);
    n.pan = num(nv, "pan", 0, -100, 100);
    n.attackMs = num(nv, "attack", 1, 0, 50);
    n.releaseMs = num(nv, "release", 40, 1, 300);
    n.reverse = flag(nv, "reverse", false);
    n.lock = flag(nv, "lock", false);
    std::vector<std::pair<int, Link>> ls;
    if (nv.contains("links") && !nv["links"].is_null()) {
      if (!nv["links"].is_array()) throw std::runtime_error("links must be a list.");
      for (const auto& lv : nv["links"]) {
        if (!lv.is_object()) throw std::runtime_error("a link must be an object.");
        Link l;
        l.weight = num(lv, "weight", 100, 0, 100);
        l.main = flag(lv, "main", false);
        ls.push_back({(int)num(lv, "to", 0, 0, 999), l});
      }
    }
    rawLinks.push_back(std::move(ls));
    p.nodes.push_back(std::move(n));
  }
  for (size_t i = 0; i < p.nodes.size(); ++i)
    for (auto& [to, l] : rawLinks[i]) {
      const int at = p.find(to);
      if (at < 0 || at == (int)i || (int)p.nodes[i].links.size() >= kMaxLinks) continue;  // a link to nowhere is dropped
      bool dup = false;
      for (const auto& o : p.nodes[i].links) dup = dup || o.to == at;
      if (dup) continue;
      l.to = at;
      p.nodes[i].links.push_back(l);
    }
  detail::fixMains(p);
  const int startId = (int)num(v, "start", p.nodes.empty() ? 1 : p.nodes[0].id, 0, 999);
  p.start = std::max(0, p.find(startId));
  const std::string mode = text(v, "mode", "weighted");
  if (mode != "follow" && mode != "weighted") throw std::runtime_error("mode must be follow or weighted.");
  p.mode = mode == "follow" ? Mode::Follow : Mode::Weighted;
  p.rate = num(v, "rate", 1, 0.5, 2);
  p.rate = p.rate < 0.75 ? 0.5 : p.rate < 1.5 ? 1 : 2;
  const std::string end = text(v, "end", "restart");
  if (end != "stop" && end != "restart" && end != "hold") throw std::runtime_error("end must be stop, restart or hold.");
  p.end = end == "stop" ? End::Stop : end == "hold" ? End::Hold : End::Restart;
  p.variation = num(v, "variation", 35, 0, 100);
  p.seed = (uint32_t)num(v, "seed", 128, 0, 9999);
  p.lockPath = flag(v, "lockPath", false);
  p.fadeMs = num(v, "fade", 3, 0, 20);
  p.shape = text(v, "shape", "");
  int maxId = 0;
  for (const auto& n : p.nodes) maxId = std::max(maxId, n.id);
  p.nextId = std::max((int)num(v, "nextId", maxId + 1, 1, 9999), maxId + 1);
  return p;
}

inline json patternToJson(const Pattern& p) {
  json nodes = json::array();
  for (const auto& n : p.nodes) {
    json links = json::array();
    for (const auto& l : n.links) links.push_back({{"to", p.nodes[l.to].id}, {"weight", l.weight}, {"main", l.main}});
    nodes.push_back({{"id", n.id}, {"slice", n.slice}, {"x", n.x}, {"y", n.y}, {"startOffset", n.startOffset}, {"endOffset", n.endOffset},
                     {"pitch", n.pitch}, {"fine", n.fine}, {"length", n.length}, {"gate", n.gate}, {"repeats", n.repeats},
                     {"level", n.level}, {"pan", n.pan}, {"attack", n.attackMs}, {"release", n.releaseMs},
                     {"reverse", n.reverse}, {"lock", n.lock}, {"links", links}});
  }
  return {{"nodes", nodes}, {"start", p.nodes.empty() ? 1 : p.nodes[std::clamp(p.start, 0, (int)p.nodes.size() - 1)].id},
          {"mode", p.mode == Mode::Follow ? "follow" : "weighted"}, {"rate", p.rate},
          {"end", p.end == End::Stop ? "stop" : p.end == End::Hold ? "hold" : "restart"}, {"variation", p.variation},
          {"seed", p.seed}, {"lockPath", p.lockPath}, {"fade", p.fadeMs}, {"shape", p.shape}, {"nextId", p.nextId}};
}

inline Patch parsePatch(const json& v) {
  using namespace detail;
  Patch patch;
  if (v.is_null()) return patch;
  if (!v.is_object()) throw std::runtime_error("the Sample Path must be an object.");
  patch.sync = flag(v, "sync", true);
  patch.bpm = num(v, "bpm", 190, 60, 300);
  if (v.contains("layers") && !v["layers"].is_null()) {
    if (!v["layers"].is_array() || v["layers"].size() > (size_t)kLayers) throw std::runtime_error("layers must be a list of at most 4.");
    for (size_t li = 0; li < v["layers"].size(); ++li) {
      const json& lv = v["layers"][li];
      try {
        if (!lv.is_object()) throw std::runtime_error("not an object.");
        Layer& L = patch.layers[li];
        if (lv.contains("slices") && !lv["slices"].is_null()) {
          if (!lv["slices"].is_array() || lv["slices"].size() > (size_t)kMaxSlices) throw std::runtime_error("slices must be a list of at most 32.");
          for (const auto& sv : lv["slices"]) {
            if (!sv.is_array() || sv.size() != 2 || !sv[0].is_number() || !sv[1].is_number()) throw std::runtime_error("a slice is [start, end] in seconds.");
            Slice s{sv[0].get<double>(), sv[1].get<double>()};
            if (!std::isfinite(s.start) || !std::isfinite(s.end) || s.start < 0 || s.end < s.start + 0.005)
              throw std::runtime_error("a slice must end at least 5 ms after it starts.");
            L.slices.push_back(s);
          }
        }
        const std::string pat = text(lv, "pattern", "A");
        if (pat.size() != 1 || pat[0] < 'A' || pat[0] > 'D') throw std::runtime_error("pattern must be A, B, C or D.");
        L.pattern = pat[0] - 'A';
        L.mute = flag(lv, "mute", false);
        if (lv.contains("patterns") && !lv["patterns"].is_null()) {
          if (!lv["patterns"].is_object()) throw std::runtime_error("patterns must be an object (A..D).");
          for (auto it = lv["patterns"].begin(); it != lv["patterns"].end(); ++it) {
            const std::string& k = it.key();
            if (k.size() != 1 || k[0] < 'A' || k[0] > 'D') throw std::runtime_error("unknown pattern '" + k + "'.");
            try { L.patterns[k[0] - 'A'] = parsePattern(it.value(), (int)L.slices.size()); }
            catch (const std::exception& e) { throw std::runtime_error("pattern " + k + ": " + e.what()); }
            L.used[k[0] - 'A'] = true;
          }
        }
        if (!L.used[L.pattern]) L.used[L.pattern] = true;
        if (lv.contains("fx") && !lv["fx"].is_null()) {
          if (!lv["fx"].is_array() || lv["fx"].size() > (size_t)kFxSlots) throw std::runtime_error("fx must be a list of at most 2.");
          for (size_t i = 0; i < lv["fx"].size(); ++i) {
            const json& fv = lv["fx"][i];
            if (!fv.is_object()) throw std::runtime_error("an fx slot must be an object.");
            const std::string t = text(fv, "type", "off");
            Fx f;
            int found = -1;
            for (int k = 0; k < 5; ++k) if (t == kFxNames[k]) found = k;
            if (found < 0) throw std::runtime_error("unknown fx '" + t + "'.");
            f.type = (FxType)found;
            f.a = num(fv, "a", 0.5, 0, 1);
            f.b = num(fv, "b", 0.5, 0, 1);
            f.mix = num(fv, "mix", 0, 0, 1);
            L.fx[i] = f;
          }
        }
      } catch (const std::exception& e) {
        throw std::runtime_error("sampler " + std::to_string(li + 1) + ": " + e.what());
      }
    }
  }
  return patch;
}

inline json patchToJson(const Patch& patch) {
  json layers = json::array();
  for (const auto& L : patch.layers) {
    json slices = json::array(), pats = json::object(), fx = json::array();
    for (const auto& s : L.slices) slices.push_back({s.start, s.end});
    for (int k = 0; k < kPatterns; ++k) if (L.used[k]) pats[std::string(1, char('A' + k))] = patternToJson(L.patterns[k]);
    for (const auto& f : L.fx) fx.push_back({{"type", kFxNames[(int)f.type]}, {"a", f.a}, {"b", f.b}, {"mix", f.mix}});
    layers.push_back({{"slices", slices}, {"pattern", std::string(1, char('A' + L.pattern))}, {"mute", L.mute}, {"fx", fx}, {"patterns", pats}});
  }
  return {{"sync", patch.sync}, {"bpm", patch.bpm}, {"layers", layers}};
}

// ---- making and changing maps ----
// Even slices across a sound: n of them, each 86 % of its share (the brief's example: one sound, six parts).
inline std::vector<Slice> evenSlices(double seconds, int n) {
  std::vector<Slice> out;
  n = std::clamp(n, 1, kMaxSlices);
  const double share = seconds / n;
  if (share < 0.01) return out;
  for (int i = 0; i < n; ++i) out.push_back({i * share + 0.002, i * share + share * 0.86});
  return out;
}

// Slices at the hits of a sound (rises of energy), at most n, in order. left/right: frames at `rate`.
inline std::vector<Slice> transientSlices(const float* left, const float* right, int64_t frames, double rate, int n) {
  std::vector<Slice> out;
  const int hop = 256;
  const int64_t count = frames / hop;
  if (count < 2 || n < 1) return out;
  std::vector<double> energy((size_t)count);
  for (int64_t f = 0; f < count; ++f) {
    double s = 0;
    for (int64_t i = f * hop; i < (f + 1) * hop; ++i) s += (double)left[i] * left[i] + (double)right[i] * right[i];
    energy[(size_t)f] = std::sqrt(s / hop);
  }
  std::vector<std::pair<double, int64_t>> rises;
  for (int64_t f = 0; f < count; ++f) rises.push_back({std::max(0.0, energy[(size_t)f] - (f ? energy[(size_t)f - 1] : 0.0)), f});
  std::stable_sort(rises.begin(), rises.end(), [](auto& a, auto& b) { return a.first > b.first; });
  const int64_t gap = std::max<int64_t>(4, (int64_t)(0.06 * rate / hop));
  std::vector<int64_t> onsets;
  for (const auto& [v, f] : rises) {
    if ((int)onsets.size() >= std::min(n, kMaxSlices) || v <= 0) break;
    bool near = false;
    for (int64_t o : onsets) near = near || std::llabs(o - f) < gap;
    if (!near) onsets.push_back(f);
  }
  std::sort(onsets.begin(), onsets.end());
  const double seconds = frames / rate;
  for (size_t k = 0; k < onsets.size(); ++k) {
    const double s = std::max(0.0, onsets[k] * hop / rate - 0.002);
    const double next = k + 1 < onsets.size() ? onsets[k + 1] * hop / rate - 0.004 : seconds;
    out.push_back({s, std::max(s + 0.02, std::min(next, s + 1.2))});
  }
  return out;
}

inline const std::vector<std::string>& shapes() {
  static const std::vector<std::string> s = {"Loop", "Zigzag", "Cross", "Star"};
  return s;
}

// A path shape: replaces the links (in the order of the node numbers). Nothing when Lock Path or fewer than 2 nodes.
inline bool applyShape(Pattern& p, const std::string& shape) {
  const int m = (int)p.nodes.size();
  if (p.lockPath || m < 2) return false;
  std::vector<int> order(m);
  for (int i = 0; i < m; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](int a, int b) { return p.nodes[a].id < p.nodes[b].id; });
  std::vector<std::tuple<int, int, double, bool>> ls;  // positions in order
  if (shape == "Loop") for (int i = 0; i < m; ++i) ls.push_back({i, (i + 1) % m, 100, true});
  else if (shape == "Zigzag") {
    std::vector<int> seq;
    for (int a = 0, b = m - 1; a <= b;) { seq.push_back(a++); if (a <= b) seq.push_back(b--); }
    for (size_t i = 0; i < seq.size(); ++i) ls.push_back({seq[i], seq[(i + 1) % seq.size()], 100, true});
  } else if (shape == "Cross") {
    for (int i = 0; i < m; ++i) ls.push_back({i, (i + m / 2) % m, 100, true});
    for (int i = 0; i < m; ++i) ls.push_back({i, (i + 1) % m, 40, false});
  } else if (shape == "Star") for (int i = 0; i < m; ++i) ls.push_back({i, (i + 2) % m, 100, true});
  else return false;
  for (auto& n : p.nodes) n.links.clear();
  for (auto& [a, b, w, main] : ls) {
    Node& from = p.nodes[order[a]];
    const int to = order[b];
    if (to == order[a] || (int)from.links.size() >= kMaxLinks) continue;
    bool dup = false;
    for (const auto& l : from.links) dup = dup || l.to == to;
    if (!dup) from.links.push_back({to, w, main});
  }
  detail::fixMains(p);
  p.shape = shape;
  return true;
}

// Generate Variation: repeats, pitch, reverse and (unless Lock Path) the links of the unlocked nodes change within
// Variation %, from the seed and the pattern itself: the same pattern and seed give the same variation, and pressing
// it again gives the next one.
inline void generateVariation(Pattern& p, bool path = true, bool repeats = true, bool pitch = true, bool reverse = true) {
  if (p.nodes.empty()) return;
  uint32_t r = hashText(patternToJson(p).dump(), p.seed * 2654435761u + 7u);
  const double amount = p.variation / 100.0;
  static const double kPitch[] = {-12, -7, -5, 0, 0, 3, 5, 7, 12};
  for (auto& n : p.nodes) {
    if (n.lock) continue;
    if (random01(r) < amount && repeats) n.repeats = 1 + (int)(random01(r) * 3);
    if (random01(r) < amount && pitch) n.pitch = kPitch[(int)(random01(r) * 9) % 9];
    if (random01(r) < amount * 0.6 && reverse) n.reverse = !n.reverse;
  }
  if (path && !p.lockPath) {
    bool changed = false;
    for (int i = 0; i < (int)p.nodes.size(); ++i) {
      Node& n = p.nodes[i];
      if (n.lock || random01(r) >= amount * 0.7) continue;
      std::vector<int> extra;
      for (int k = 0; k < (int)n.links.size(); ++k) if (!n.links[k].main) extra.push_back(k);
      if (!extra.empty() && random01(r) < 0.4) {
        n.links.erase(n.links.begin() + extra[(size_t)(random01(r) * extra.size()) % extra.size()]);
        changed = true;
      } else if ((int)n.links.size() < kMaxLinks) {
        std::vector<int> free;
        for (int k = 0; k < (int)p.nodes.size(); ++k) {
          bool linked = k == i;
          for (const auto& l : n.links) linked = linked || l.to == k;
          if (!linked) free.push_back(k);
        }
        if (free.empty()) continue;
        n.links.push_back({free[(size_t)(random01(r) * free.size()) % free.size()], 15 + std::floor(random01(r) * 30), n.links.empty()});
        changed = true;
      }
    }
    if (changed) p.shape.clear();
  }
  detail::fixMains(p);
}

// The brief's example on a sound of `seconds`: six slices, 1-2-3-4-5-6-1, node 2 also goes to 5 (25 %), node 3
// repeats twice at +7 st, node 4 plays reversed.
inline Patch examplePatch(double seconds) {
  Patch patch;
  Layer& L = patch.layers[0];
  L.slices = evenSlices(seconds, 6);
  Pattern& p = L.patterns[0];
  const double pos[6][2] = {{95, 52}, {290, 165}, {495, 52}, {695, 165}, {900, 52}, {1095, 165}};
  for (int i = 0; i < 6; ++i) {
    Node n;
    n.id = i + 1; n.slice = i; n.x = pos[i][0]; n.y = pos[i][1];
    p.nodes.push_back(n);
  }
  p.nodes[1].length = "1/32"; p.nodes[2].pitch = 7; p.nodes[2].repeats = 2; p.nodes[3].reverse = true; p.nodes[3].length = "1/32";
  p.nodes[5].length = "1/32";
  const int links[7][4] = {{0, 1, 100, 1}, {1, 2, 75, 1}, {1, 4, 25, 0}, {2, 3, 100, 1}, {3, 4, 100, 1}, {4, 5, 100, 1}, {5, 0, 100, 1}};
  for (const auto& l : links) p.nodes[l[0]].links.push_back({l[1], (double)l[2], l[3] != 0});
  p.nextId = 7;
  return patch;
}

}  // namespace core::spath
