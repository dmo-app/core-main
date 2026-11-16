#include "project.hpp"
#include "../dsp/finite.hpp"
#include "../dsp/registry.hpp"
#include <algorithm>
#include <functional>
#include <map>
#include <cmath>
#include <regex>
#include <set>
#include <stdexcept>

namespace core {

namespace {
std::vector<Envelope> envelopesFromJson(const json& list, const std::function<bool(const std::string&)>& known);
json envelopesJson(const std::vector<Envelope>& list);
}  // namespace

namespace {
// The built-in instrument and effect parameter catalogues now live in the DSP registry (dsp/registry.hpp),
// right next to the classes they describe; the accessors below delegate to it (single source of truth).


double num(const json& v, const char* key, double fallback) {
  if (!v.contains(key) || v[key].is_null()) return fallback;
  if (!v[key].is_number()) throw std::runtime_error(std::string("'") + key + "' must be a number.");
  double x = v[key].get<double>();
  if (!finiteBits(x)) throw std::runtime_error(std::string("'") + key + "' is not finite.");
  return x;
}
// A whole number in [lo, hi], its range checked on the number as written, before the cast (a cast of 1e30 or
// 4294967356 to int is undefined, and could land in range). Truncates as before: 60.7 is 60, -0.5 is 0.
int whole(const json& v, const char* key, int fallback, int lo, int hi, const char* error) {
  const double x = num(v, key, (double)fallback);
  if (!(x > (double)lo - 1.0 && x < (double)hi + 1.0)) throw std::runtime_error(error);
  return (int)x;
}
int64_t ticks(const json& v, const char* key, int64_t fallback) {
  double x = num(v, key, (double)fallback);
  return (int64_t)std::llround(x);
}
std::string str(const json& v, const char* key, const std::string& fallback) {
  if (!v.contains(key) || v[key].is_null()) return fallback;
  if (!v[key].is_string()) throw std::runtime_error(std::string("'") + key + "' must be a string.");
  return v[key].get<std::string>();
}
double clampd(double x, double lo, double hi) { return std::min(hi, std::max(lo, x)); }

// A v1 whole number, clamped on the number as written (before any cast to int). A wrong type keeps the fallback.
int64_t v1Int(const json& v, const char* key, int64_t fallback, int64_t lo, int64_t hi) {
  if (!v.contains(key) || !v[key].is_number()) return fallback;
  const double x = v[key].get<double>();
  if (!std::isfinite(x)) return fallback;
  return (int64_t)std::clamp(x, (double)lo, (double)hi);
}
// Legacy v1 (JS editor): kind drums|synth|audio, pattern {steps, resolution, gate, notes:[{step,pitch,accent,length}]}, bars.
json trackFromV1Impl(const json& t) {
  std::string kind = t.value("kind", "");
  if (kind != "synth" && kind != "drums") throw std::runtime_error("Track kind '" + kind + "' is not supported by the C++ engine yet.");
  const json pattern = t.value("pattern", json::object());
  std::string res = pattern.value("resolution", "1/16");
  int perBeat = res == "1/8" ? 2 : res == "1/32" ? 8 : 4;
  int64_t stepTicks = kPpq / perBeat;
  int steps = (int)v1Int(pattern, "steps", 16, 1, 65536);
  double gate = pattern.value("gate", 0.5);
  double accent = pattern.value("accent", 0.3);
  json track = {{"id", t.value("id", "t1")}, {"name", t.value("name", "Track")},
                {"instrument", "none"},  // Core's own sound left (2026-11-16): the notes wait for a plugin
                {"gainDb", t.value("volumeDb", 0.0)}, {"pan", t.value("pan", 0.0)},
                {"mute", t.value("mute", false)}, {"solo", t.value("solo", false)}, {"patternTicks", steps * stepTicks},
                {"lengthTicks", v1Int(t, "bars", 16, 1, 100000) * 4 * kPpq},
                {"notes", json::array()}, {"params", json::object()}};
  for (const auto& n : pattern.value("notes", json::array())) {
    int64_t len = v1Int(n, "length", 1, 1, 65536);
    int pitch = (int)v1Int(n, "pitch", 36, -1, 128);  // out of 0..127 stays out (refused when read)
    if (kind == "drums" && pitch != 36 && pitch != 42 && pitch != 46) continue; // clap/perc: not in the prototype
    double velocity = n.value("accent", false) ? std::min(1.0, 0.75 * (1 + accent)) : 0.75;
    track["notes"].push_back({{"tick", v1Int(n, "step", 0, -1, 1 << 24) * stepTicks},
                              {"length", std::max<int64_t>(1, (int64_t)std::llround(stepTicks * ((len - 1) + std::clamp(gate, 0.05, 1.0))))},
                              {"pitch", pitch}, {"velocity", velocity}});
  }
  return track;
}

json migrateV1(const json& v1) {
  json v2 = {{"format", kProjectFormat}, {"version", 2}, {"name", v1.value("name", json())},
             {"bpm", v1.value("bpm", 145.0)}, {"tracks", json::array()}};
  int64_t longest = 0;
  for (const auto& t : v1.value("tracks", json::array())) {
    if (t.value("kind", "") == "audio") continue; // audio tracks stay in the JS engine for now
    json track = trackFromV1Impl(t);
    longest = std::max<int64_t>(longest, track["lengthTicks"].get<int64_t>());
    v2["tracks"].push_back(track);
  }
  v2["lengthTicks"] = longest > 0 ? longest : 64 * kPpq;
  return v2;
}
} // namespace

const std::vector<ParamSpec>& instrumentParams(const std::string& instrument) {
  return nativeInstrumentParams(instrument);
}
int currentInstrumentVersion(const std::string& instrument) { return nativeInstrumentVersion(instrument); }

const std::vector<ParamSpec>& effectParams(const std::string& effect) {
  return nativeEffectParams(effect);
}

int currentEffectVersion(const std::string& effect) { return nativeEffectVersion(effect); }

std::vector<std::string> effectNames() { return nativeEffectNames(); }
bool isMixParam(const std::string& p) { return p == "gainDb" || p == "pan"; }
std::vector<std::string> instrumentNames() { return nativeInstrumentNames(); }
const std::vector<std::string>& modulatorShapes() {
  static const std::vector<std::string> shapes = {"sine", "triangle", "square", "saw", "ramp", "random", "envelope", "rise", "steps"};
  return shapes;
}

bool hasNoInstrumentParams(const std::string& i) { return nativeInstrumentHasNoParams(i); }

void validateRouting(const Project& p) {
  std::map<std::string, const Track*> byId;
  for (const auto& t : p.tracks) byId[t.id] = &t;
  auto target = [&](const Track& t, const std::string& to, const char* what) {
    auto it = byId.find(to);
    if (it == byId.end()) throw std::runtime_error("Track '" + t.id + "': " + what + " goes to '" + to + "', which is not a track.");
    if (it->second->instrument != "bus") throw std::runtime_error("Track '" + t.id + "': " + what + " goes to '" + to + "', which is not a bus.");
    if (to == t.id) throw std::runtime_error("Track '" + t.id + "': " + what + " goes to itself.");
  };
  // M7c sidechains: the key comes from a track that is not a bus (it plays before the buses) and not itself.
  std::map<std::string, std::vector<const Track*>> keyed;  // source -> the tracks it keys
  for (const auto& t : p.tracks) {
    if (!t.output.empty()) target(t, t.output, "its output");
    for (const auto& s : t.sends) target(t, s.to, ("send '" + s.id + "'").c_str());
    if (!t.effects.is_array()) continue;
    for (const auto& fx : t.effects) {
      if (!fx.is_object() || !fx.contains("sidechain") || !fx["sidechain"].is_string()) continue;
      const std::string from = fx["sidechain"].get<std::string>();
      auto it = byId.find(from);
      if (it == byId.end()) throw std::runtime_error("Track '" + t.id + "': the sidechain comes from '" + from + "', which is not a track.");
      if (from == t.id) throw std::runtime_error("Track '" + t.id + "': the sidechain comes from itself.");
      if (it->second->instrument == "bus") throw std::runtime_error("Track '" + t.id + "': the sidechain comes from '" + from + "', a bus (use a track).");
      keyed[from].push_back(&t);
    }
  }
  // Modulators started by another track's notes: a track that plays notes (not a bus), not itself.
  for (const auto& t : p.tracks)
    for (const auto& m : t.modulators) {
      if (m.source.empty()) continue;
      auto it = byId.find(m.source);
      if (it == byId.end()) throw std::runtime_error("Track '" + t.id + "': modulator '" + m.id + "' starts from '" + m.source + "', which is not a track.");
      if (m.source == t.id) throw std::runtime_error("Track '" + t.id + "': modulator '" + m.id + "' starts from its own track (leave source empty).");
      if (it->second->instrument == "bus" || it->second->instrument == "audio")
        throw std::runtime_error("Track '" + t.id + "': modulator '" + m.id + "' starts from '" + m.source + "', which plays no notes.");
    }
  // No loops: a depth-first walk over the buses (0 = not seen, 1 = on the path, 2 = done).
  std::map<std::string, int> mark;
  std::function<void(const Track&)> visit = [&](const Track& t) {
    int& m = mark[t.id];
    if (m == 2) return;
    if (m == 1) throw std::runtime_error("The routing loops back into '" + t.id + "'.");
    m = 1;
    if (!t.output.empty()) visit(*byId[t.output]);
    for (const auto& s : t.sends) visit(*byId[s.to]);
    if (auto k = keyed.find(t.id); k != keyed.end()) for (const Track* d : k->second) visit(*d);
    mark[t.id] = 2;
  };
  for (const auto& t : p.tracks) visit(t);
}
json trackFromV1(const json& t) { return trackFromV1Impl(t); }

double samplesPerTick(double bpm, double sampleRate) { return sampleRate * 60.0 / (bpm * kPpq); }

const Track* Project::findTrack(const std::string& id) const {
  for (const auto& t : tracks) if (t.id == id) return &t;
  return nullptr;
}
Track* Project::findTrack(const std::string& id) {
  for (auto& t : tracks) if (t.id == id) return &t;
  return nullptr;
}

json toJson(const Project& p) {
  json out = {{"format", kProjectFormat}, {"version", kProjectVersion},
              {"name", p.name.empty() ? json() : json(p.name)}, {"bpm", p.bpm},
              {"lengthTicks", p.lengthTicks}, {"masterGainDb", p.masterGainDb}, {"masterEffects", p.masterEffects}, {"masterEnvelopes", envelopesJson(p.masterEnvelopes)}, {"ppq", kPpq},
              {"tracks", json::array()}};
  if (!p.tempoMap.empty()) {
    json map = json::array();
    for (const auto& c : p.tempoMap) {
      json point = {{"tick", c.tick}, {"bpm", c.bpm}};
      if (c.ramp) point["ramp"] = true;
      map.push_back(point);
    }
    out["tempoMap"] = map;
  }
  for (const auto& t : p.tracks) {
    json track = {{"id", t.id}, {"name", t.name}, {"instrument", t.instrument},
                  {"instrumentVersion", t.instrumentVersion}, {"params", t.params}, {"gainDb", t.gainDb}, {"pan", t.pan}, {"mute", t.mute}, {"solo", t.solo},
                  {"patternTicks", t.patternTicks}, {"lengthTicks", t.lengthTicks}, {"notes", json::array()}, {"envelopes", json::array()},
                  {"effects", t.effects}};
    if (t.instrument == "plugin") { track["plugin"] = t.plugin; track.erase("params"); track.erase("instrumentVersion"); }
    for (const auto& n : t.notes) {
      json note = {{"tick", n.tick}, {"length", n.length}, {"pitch", n.pitch}, {"velocity", n.velocity}};
      if (n.roll.count > 1)
        note["roll"] = {{"count", n.roll.count}, {"velocityFrom", n.roll.velocityFrom}, {"velocityTo", n.roll.velocityTo}};
      track["notes"].push_back(note);
    }
    if (!t.clips.empty()) {
      track["clips"] = json::array();
      for (const auto& cl : t.clips) {
        json notes = json::array();
        for (const auto& n : cl.notes) {
          json note = {{"tick", n.tick}, {"length", n.length}, {"pitch", n.pitch}, {"velocity", n.velocity}};
          if (n.roll.count > 1) note["roll"] = {{"count", n.roll.count}, {"velocityFrom", n.roll.velocityFrom}, {"velocityTo", n.roll.velocityTo}};
          notes.push_back(note);
        }
        json clip = {{"startTick", cl.startTick}, {"lengthTicks", cl.lengthTicks}, {"patternTicks", cl.patternTicks}, {"notes", notes}};
        if (cl.offsetTicks) clip["offsetTicks"] = cl.offsetTicks;
        track["clips"].push_back(clip);
      }
    }
    if (t.instrument == "audio") {
      track.erase("params");
      track.erase("instrumentVersion");
      track["audioClips"] = json::array();
      for (const auto& a : t.audioClips) {
        track["audioClips"].push_back({{"startTick", a.startTick}, {"path", a.path}, {"offsetSeconds", a.offsetSeconds},
                                       {"durationSeconds", a.durationSeconds}, {"gainDb", a.gainDb}, {"fadeInSeconds", a.fadeInSeconds},
                                       {"fadeOutSeconds", a.fadeOutSeconds}, {"mute", a.mute}});
        if (a.pitchSemitones != 0) track["audioClips"].back()["pitchSemitones"] = a.pitchSemitones;
        if (a.freezeSeconds > 0) track["audioClips"].back()["freezeSeconds"] = a.freezeSeconds;
      }
    }
    if (t.instrument == "bus") { track.erase("params"); track.erase("instrumentVersion"); track.erase("notes"); }
    if (!t.output.empty()) track["output"] = t.output;  // older songs stay as they were
    if (!t.modulators.empty()) {
      track["modulators"] = json::array();
      for (const auto& m : t.modulators) {
        json mj = {{"id", m.id}, {"param", m.param}, {"shape", m.shape}, {"rateBeats", m.rateBeats}, {"depth", m.depth}, {"phase", m.phase}};
        if (m.shape == "envelope") { mj["attackMs"] = m.attackMs; mj["decayMs"] = m.decayMs; }
        if (m.shape == "rise") { mj["curve"] = m.curve; mj["hold"] = m.hold; }
        if (m.shape == "steps") mj["steps"] = m.steps;
        if (!m.source.empty()) mj["source"] = m.source;
        if (!m.on) mj["on"] = false;  // only when off: older songs stay as they were
        track["modulators"].push_back(mj);
      }
    }
    if (!t.sends.empty()) {
      track["sends"] = json::array();
      for (const auto& sd : t.sends) track["sends"].push_back({{"id", sd.id}, {"to", sd.to}, {"gainDb", sd.gainDb}, {"pre", sd.pre}});
    }
    for (const auto& e : t.envelopes) {
      json env = {{"param", e.param}, {"points", json::array()}};
      for (const auto& pt : e.points) env["points"].push_back({{"tick", pt.tick}, {"value", pt.value}});
      track["envelopes"].push_back(env);
    }
    out["tracks"].push_back(track);
  }
  return out;
}

// A song written by hand or by an AI may hold an effect id with ':' (the app never makes one), an id longer
// than 40 characters, or the same id twice in one chain. The song still opens: the id is cut to 40, ':'
// becomes '_', and a repeated id gets _2, _3... (unique in its chain); every change is reported (audit
// 48c9c38 E6: refusing it made a whole song unplayable; audit 2026-11-08 A4: long and repeated ids were
// cut or kept in silence). Automation follows a renamed id, unless an effect before it keeps that id: then
// the automation stays with that one, as the engine always resolved it (the first in the chain), so the song
// sounds as before. A chain checked on its own (effects.set) still refuses them.
static json renameColonEffectIds(json v, std::vector<std::string>& renamed) {
  auto clean = [](const std::string& id) { return !id.empty() && id.size() <= 40 && id.find(':') == std::string::npos; };
  auto fixChain = [&](json& effects, json* envelopes) {
    if (!effects.is_array()) return;
    std::vector<std::string> ids;  // taken: every effect that keeps its id (first of each), then each new one
    std::vector<bool> keep;
    for (const auto& fx : effects) {
      const std::string id = fx.is_object() && fx.contains("id") && fx["id"].is_string() ? fx["id"].get<std::string>() : std::string();
      const bool k = clean(id) && std::find(ids.begin(), ids.end(), id) == ids.end();
      if (k) ids.push_back(id);
      keep.push_back(k);
    }
    for (size_t i = 0; i < effects.size(); ++i) {
      json& fx = effects[i];
      if (keep[i] || !fx.is_object() || !fx.contains("id") || !fx["id"].is_string()) continue;
      const std::string old = fx["id"].get<std::string>();
      if (old.empty()) continue;  // an unnamed effect stays as it is
      std::string base = old.substr(0, 40);
      std::replace(base.begin(), base.end(), ':', '_');
      std::string id = base;
      for (int n = 2; std::find(ids.begin(), ids.end(), id) != ids.end(); ++n) id = base.substr(0, 36) + "_" + std::to_string(n);
      const bool owned = std::find(ids.begin(), ids.end(), old) != ids.end();  // an effect before keeps the old id
      ids.push_back(id);
      fx["id"] = id;
      renamed.push_back(old + " -> " + id);
      if (!owned && envelopes && envelopes->is_array())
        for (auto& e : *envelopes) {
          if (!e.is_object() || !e.contains("param") || !e["param"].is_string()) continue;
          const std::string param = e["param"].get<std::string>(), prefix = "fx:" + old + ":";
          if (param.rfind(prefix, 0) == 0) e["param"] = "fx:" + id + ":" + param.substr(prefix.size());
        }
    }
  };
  if (v.contains("masterEffects")) {
    json none;
    fixChain(v["masterEffects"], v.contains("masterEnvelopes") ? &v["masterEnvelopes"] : &none);
  }
  if (v.contains("tracks") && v["tracks"].is_array())
    for (auto& t : v["tracks"])
      if (t.is_object() && t.contains("effects")) {
        json none;
        fixChain(t["effects"], t.contains("envelopes") ? &t["envelopes"] : &none);
      }
  return v;
}

Project fromJson(const json& input) {
  if (!input.is_object()) throw std::runtime_error("Not a Core project.");
  if (input.value("format", "") != kProjectFormat) throw std::runtime_error("Not a Core project file.");
  const double written = num(input, "version", 0);
  const int version = written > 1e6 ? 1000000 : written < -1e6 ? -1000000 : (int)written;  // never a cast past int
  if (version < 1 || version > kProjectVersion) throw std::runtime_error("Unsupported project version " + std::to_string(version) + ".");
  std::vector<std::string> renamed;
  const json v = renameColonEffectIds(version == 1 ? migrateV1(input) : input, renamed);

  Project p;
  p.renamedEffects = std::move(renamed);
  p.name = str(v, "name", "").substr(0, 80);
  p.bpm = num(v, "bpm", 145);
  if (!(p.bpm >= kMinBpm && p.bpm <= kMaxBpm)) throw std::runtime_error("Invalid project BPM (40–600).");
  p.lengthTicks = ticks(v, "lengthTicks", 64 * kPpq);
  if (p.lengthTicks < kPpq || p.lengthTicks > 4096LL * 4 * kPpq) throw std::runtime_error("Invalid song length.");
  // Tempo changes after the start: ticks increasing, inside the song, each BPM 40-600, at most 64.
  if (v.contains("tempoMap") && !v["tempoMap"].is_null()) {
    if (!v["tempoMap"].is_array()) throw std::runtime_error("'tempoMap' must be a list of {tick, bpm}.");
    if (v["tempoMap"].size() > 64) throw std::runtime_error("Too many tempo changes (max 64).");
    int64_t previous = 0;
    for (const auto& c : v["tempoMap"]) {
      if (!c.is_object()) throw std::runtime_error("'tempoMap' must be a list of {tick, bpm}.");
      TempoChange t;
      t.tick = ticks(c, "tick", 0);
      t.bpm = num(c, "bpm", 0);
      if (c.contains("ramp") && !c["ramp"].is_null()) {
        if (!c["ramp"].is_boolean()) throw std::runtime_error("Tempo change 'ramp' must be true or false.");
        t.ramp = c["ramp"].get<bool>();
      }
      if (t.tick <= previous || t.tick >= p.lengthTicks) throw std::runtime_error("Tempo changes must be in order, after the start and inside the song.");
      if (!(t.bpm >= kMinBpm && t.bpm <= kMaxBpm)) throw std::runtime_error("Invalid tempo change BPM (40–600).");
      previous = t.tick;
      p.tempoMap.push_back(t);
    }
  }
  p.masterGainDb = clampd(num(v, "masterGainDb", 0), -60, 12);
  if (v.contains("masterEffects") && !v["masterEffects"].is_null()) p.masterEffects = effectsFromJson(v["masterEffects"]);
  if (v.contains("masterEnvelopes") && v["masterEnvelopes"].is_array())
    p.masterEnvelopes = envelopesFromJson(v["masterEnvelopes"], [](const std::string& param) { return param == "gainDb"; });
  if (!v.contains("tracks") || !v["tracks"].is_array()) throw std::runtime_error("The project has no track list.");
  if (v["tracks"].size() > 128) throw std::runtime_error("Too many tracks (max 128).");

  std::set<std::string> ids;
  int index = 0;
  for (const auto& tv : v["tracks"]) {
    ++index;
    Track t;
    try { t = trackFromJson(tv); }
    catch (const std::exception& e) { throw std::runtime_error("Track " + std::to_string(index) + ": " + e.what()); }
    if (ids.count(t.id)) throw std::runtime_error("Track " + std::to_string(index) + ": repeated id.");
    for (const auto& n : t.loadNotes) p.conversionNotes.push_back(t.id + ": " + n);
    ids.insert(t.id);
    p.tracks.push_back(std::move(t));
  }
  for (const auto& fx : p.masterEffects)
    if (fx.contains("sidechain")) throw std::runtime_error("The master's effects take no sidechain.");
  validateRouting(p);
  return p;
}


static json validatePlugin(const json& plugin) {
  if (!plugin.is_object()) throw std::runtime_error("a plugin needs {format, id}.");
  const std::string format = plugin.value("format", "");
  if (format != "vst3" && format != "clap" && format != "core") throw std::runtime_error("plugin format must be vst3, clap or core.");
  if (!plugin.contains("id") || !plugin["id"].is_string() || plugin["id"].get<std::string>().empty())
    throw std::runtime_error("plugin id is required.");
  if (plugin.contains("version") && !plugin["version"].is_string()) throw std::runtime_error("plugin version must be text.");
  if (plugin.contains("state") && !plugin["state"].is_string()) throw std::runtime_error("plugin state must be base64 text.");
  if (plugin.contains("stateId") && !plugin["stateId"].is_string()) throw std::runtime_error("plugin stateId must be text.");
  json clean = plugin;
  clean.erase("missing");  // decided by the engine when it loads, never saved
  return clean;
}

// Notes of one pattern, checked against its length, sorted by tick.
static std::vector<Note> parseNotes(const json& list, int64_t patternTicks) {
  if (!list.is_array()) throw std::runtime_error("notes must be a list.");
  if (list.size() > 4096) throw std::runtime_error("too many notes.");
  std::vector<Note> notes;
  for (const auto& nv : list) {
    Note n;
    n.tick = ticks(nv, "tick", 0);
    n.length = ticks(nv, "length", kPpq / 4);
    n.pitch = whole(nv, "pitch", 36, 0, 127, "invalid pitch.");
    n.velocity = clampd(num(nv, "velocity", 0.8), 0, 1);
    if (n.tick < 0 || n.tick >= patternTicks) throw std::runtime_error("note outside the pattern.");
    if (n.length < 1 || n.length > patternTicks) throw std::runtime_error("invalid note length.");
    if (n.pitch < 0 || n.pitch > 127) throw std::runtime_error("invalid pitch.");
    if (nv.contains("roll") && !nv["roll"].is_null()) {
      const json& r = nv["roll"];
      n.roll.count = whole(r, "count", 0, 0, 64, "roll count must be 0–64.");
      if (n.roll.count < 0 || n.roll.count > 64) throw std::runtime_error("roll count must be 0–64.");
      n.roll.velocityFrom = clampd(num(r, "velocityFrom", n.velocity), 0, 1);
      n.roll.velocityTo = clampd(num(r, "velocityTo", n.velocity), 0, 1);
    }
    notes.push_back(n);
  }
  std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) { return a.tick < b.tick; });
  return notes;
}

namespace {
std::vector<Envelope> envelopesFromJson(const json& list, const std::function<bool(const std::string&)>& known) {
  std::vector<Envelope> out;
  for (const auto& ev : list) {
    Envelope e;
    e.param = str(ev, "param", "");
    if (!known(e.param) && e.param.rfind("fx:", 0) != 0) throw std::runtime_error("envelope on unknown parameter '" + e.param + "'.");
    for (const auto& pv : ev.value("points", json::array())) {
      EnvelopePoint pt{ticks(pv, "tick", 0), num(pv, "value", 0)};
      if (pt.tick < 0) throw std::runtime_error("envelope point before the start.");
      e.points.push_back(pt);
    }
    if (e.points.empty()) continue;
    std::stable_sort(e.points.begin(), e.points.end(), [](auto& a, auto& b) { return a.tick < b.tick; });
    out.push_back(e);
  }
  return out;
}

json envelopesJson(const std::vector<Envelope>& list) {
  json out = json::array();
  for (const auto& e : list) {
    json env = {{"param", e.param}, {"points", json::array()}};
    for (const auto& p : e.points) env["points"].push_back({{"tick", p.tick}, {"value", p.value}});
    out.push_back(env);
  }
  return out;
}
}  // namespace

json effectsFromJson(const json& list) {
  if (!list.is_array() || list.size() > 16) throw std::runtime_error("effects must be a list (max 16).");
  json out = json::array();
  std::set<std::string> effectIds;
  for (const auto& fx : list) {
    if (!fx.is_object()) throw std::runtime_error("each effect must be an object.");
    const std::string type = fx.contains("type") && fx["type"].is_string() ? fx["type"].get<std::string>() : "builtin";
    const bool bypass = fx.contains("bypass") && fx["bypass"].is_boolean() && fx["bypass"].get<bool>();
    const std::string id = fx.contains("id") && fx["id"].is_string() ? fx["id"].get<std::string>() : std::string();
    // Automation names an effect parameter "fx:<id>:<param>", so an id with ':' could never be automated
    // (stress test 2026-11-03), and one longer than 40 or used twice would be cut or automate the wrong
    // effect (audit 2026-11-08 A4). fromJson renames such ids before this (audit 48c9c38 E6); a chain
    // checked on its own (effects.set) refuses them.
    if (id.size() > 40) throw std::runtime_error("effect id '" + id + "' is longer than 40 characters.");
    if (id.find(':') != std::string::npos) throw std::runtime_error("effect id '" + id + "' must not contain ':'.");
    if (!id.empty() && !effectIds.insert(id).second) throw std::runtime_error("effect id '" + id + "' is used twice in one chain.");
    if (type == "plugin") {
      json clean = fx;
      clean["plugin"] = validatePlugin(fx.value("plugin", json()));
      clean["bypass"] = bypass;
      if (!id.empty()) clean["id"] = id;
      out.push_back(clean);
      continue;
    }
    const std::string name = fx.contains("name") && fx["name"].is_string() ? fx["name"].get<std::string>() : std::string();
    const auto& specs = effectParams(name);
    // One version of each effect (2026-11-09, Diogo: "não existem versões agora, só 1"): an older one, or none
    // written, plays the current one. A newer one than this Core knows stays silent.
    int version = 1;
    if (fx.contains("version") && fx["version"].is_number()) {  // past int: unknown (newer), kept silent
      const double x = fx["version"].get<double>();
      version = std::isfinite(x) && x < 1e6 && x > -1e6 ? (int)x : 1000000;
    }
    if (!specs.empty() && version >= 1 && version < currentEffectVersion(name)) version = currentEffectVersion(name);
    // Not one of ours (another type, an unknown name, a newer version): kept as it is and silent. The song
    // still opens and plays.
    if (type != "builtin" || specs.empty() || version < 1 || version > currentEffectVersion(name)) {
      json kept = fx;
      kept["unsupported"] = true;
      out.push_back(kept);
      continue;
    }
    json params = json::object();
    for (const auto& s : specs) params[s.name] = s.def;
    if (fx.contains("params") && fx["params"].is_object())
      for (auto it = fx["params"].begin(); it != fx["params"].end(); ++it) {
        auto spec = std::find_if(specs.begin(), specs.end(), [&](const ParamSpec& s) { return it.key() == s.name; });
        if (spec != specs.end() && it.value().is_number()) params[it.key()] = clampd(it.value().get<double>(), spec->min, spec->max);
      }  // unknown or odd parameters: left out (the defaults play)
    json clean = {{"type", "builtin"}, {"name", name}, {"version", version}, {"params", params}, {"bypass", bypass}};
    if (!id.empty()) clean["id"] = id;
    // M7c: a compressor keyed by another track (its signal after inserts, before its fader: a muted
    // "ghost" kick still ducks). Checked against the tracks by validateRouting.
    if (fx.contains("sidechain") && !fx["sidechain"].is_null()) {
      if (name != "compressor") throw std::runtime_error("only a compressor takes a sidechain.");
      if (!fx["sidechain"].is_string() || fx["sidechain"].get<std::string>().empty()) throw std::runtime_error("sidechain must be a track id.");
      clean["sidechain"] = fx["sidechain"];
    }
    out.push_back(clean);
  }
  return out;
}

Track trackFromJson(const json& input) {
  const json& tv = input;
  static const std::regex idPattern("^[A-Za-z0-9_-]{1," + std::to_string(kMaxTrackIdLength) + "}$");
  if (!tv.is_object()) throw std::runtime_error("not an object.");
  Track t;
  t.id = str(tv, "id", "");
  if (!std::regex_match(t.id, idPattern)) throw std::runtime_error("invalid id.");
  t.name = str(tv, "name", t.id).substr(0, 80);
  t.instrument = str(tv, "instrument", "");
  // Core's own instruments left the engine (2026-11-16): an older project's kick, W-Core, sampler... is a track with
  // its notes and no sound until a plugin plays it.
  bool ownInstrumentLeft = false;
  if (instrumentParams(t.instrument).empty()) {
    if (t.instrument.empty()) throw std::runtime_error("instrument is required.");
    t.loadNotes.push_back("Core's own '" + t.instrument + "' is not played any more: the track waits for a plugin");
    t.instrument = "none";
    ownInstrumentLeft = true;
  }
  const auto& specs = instrumentParams(t.instrument);
  {  // past int: newer than this engine (checked below), never wrapped
    const double iv = num(tv, "instrumentVersion", 1);
    t.instrumentVersion = iv >= 1e6 ? 1000000 : iv <= -1e6 ? -1000000 : (int)iv;
  }
  if (!hasNoInstrumentParams(t.instrument) && (t.instrumentVersion < 1 || t.instrumentVersion > currentInstrumentVersion(t.instrument)))
    throw std::runtime_error("instrument '" + t.instrument + "' version " + std::to_string(t.instrumentVersion) + " is newer than this engine.");
  if (t.instrument == "plugin") {
    t.plugin = validatePlugin(tv.value("plugin", json()));
  } else if (!hasNoInstrumentParams(t.instrument)) {
    for (const auto& s : specs) t.params[s.name] = s.def;
  }
  if (tv.contains("effects") && !tv["effects"].is_null()) t.effects = effectsFromJson(tv["effects"]);
  // One version of each instrument (2026-11-09): what an older version needed was migrated above; it plays the current sound.
  if (!hasNoInstrumentParams(t.instrument) && t.instrumentVersion >= 1 && t.instrumentVersion < currentInstrumentVersion(t.instrument))
    t.instrumentVersion = currentInstrumentVersion(t.instrument);
  t.gainDb = clampd(num(tv, "gainDb", 0), -60, 12);
  t.pan = clampd(num(tv, "pan", 0), -1, 1);
  t.mute = tv.value("mute", false);
  t.solo = tv.value("solo", false);
  t.patternTicks = ticks(tv, "patternTicks", 4 * kPpq);
  t.lengthTicks = std::max<int64_t>(0, ticks(tv, "lengthTicks", 0));
  if (t.patternTicks < kPpq / 8 || t.patternTicks > 64LL * 4 * kPpq) throw std::runtime_error("invalid pattern length.");
  if (tv.contains("notes") && !tv["notes"].is_array()) throw std::runtime_error("notes must be a list.");
  t.notes = parseNotes(tv.value("notes", json::array()), t.patternTicks);
  if (tv.contains("clips") && !tv["clips"].is_null()) {
    if (!tv["clips"].is_array() || tv["clips"].size() > 256) throw std::runtime_error("clips must be a list (max 256).");
    if (!t.notes.empty()) throw std::runtime_error("a track has notes or clips, not both.");
    int index = 0;
    for (const auto& cv : tv["clips"]) {
      ++index;
      try {
        if (!cv.is_object()) throw std::runtime_error("not an object.");
        Clip c;
        c.startTick = ticks(cv, "startTick", 0);
        c.lengthTicks = ticks(cv, "lengthTicks", 4 * 4 * kPpq);
        c.patternTicks = ticks(cv, "patternTicks", 4 * kPpq);
        c.offsetTicks = ticks(cv, "offsetTicks", 0);
        if (c.startTick < 0 || c.startTick >= 4096LL * 4 * kPpq) throw std::runtime_error("invalid start.");
        if (c.lengthTicks < 1 || c.lengthTicks > 4096LL * 4 * kPpq) throw std::runtime_error("invalid length.");
        if (c.patternTicks < kPpq / 8 || c.patternTicks > 64LL * 4 * kPpq) throw std::runtime_error("invalid pattern length.");
        if (c.offsetTicks < 0 || c.offsetTicks >= c.patternTicks) throw std::runtime_error("offsetTicks must be inside the pattern.");
        c.notes = parseNotes(cv.value("notes", json::array()), c.patternTicks);
        t.clips.push_back(std::move(c));
      } catch (const std::exception& e) {
        throw std::runtime_error("clip " + std::to_string(index) + ": " + e.what());
      }
    }
  }
  if (tv.contains("audioClips") && !tv["audioClips"].is_null()) {
    if (t.instrument != "audio") throw std::runtime_error("audioClips belong to an audio track.");
    if (!tv["audioClips"].is_array() || tv["audioClips"].size() > 1024) throw std::runtime_error("audioClips must be a list (max 1024).");
    int index = 0;
    for (const auto& av : tv["audioClips"]) {
      ++index;
      try {
        if (!av.is_object()) throw std::runtime_error("not an object.");
        AudioClip a;
        a.startTick = ticks(av, "startTick", 0);
        a.path = str(av, "path", "");
        a.offsetSeconds = num(av, "offsetSeconds", 0);
        a.durationSeconds = num(av, "durationSeconds", 0);
        a.gainDb = clampd(num(av, "gainDb", 0), -60, 24);
        a.fadeInSeconds = num(av, "fadeInSeconds", 0);
        a.fadeOutSeconds = num(av, "fadeOutSeconds", 0);
        a.mute = av.value("mute", false);
        a.pitchSemitones = num(av, "pitchSemitones", 0);
        a.freezeSeconds = num(av, "freezeSeconds", 0);
        if (!finiteBits(a.pitchSemitones) || a.pitchSemitones < -24 || a.pitchSemitones > 24) throw std::runtime_error("pitchSemitones must be -24..24.");
        if (!finiteBits(a.freezeSeconds) || a.freezeSeconds < 0 || a.freezeSeconds > 10) throw std::runtime_error("freezeSeconds must be 0..10.");
        if (a.path.empty()) throw std::runtime_error("path is required.");
        if (a.startTick < 0 || a.startTick >= 4096LL * 4 * kPpq) throw std::runtime_error("invalid start.");
        if (a.offsetSeconds < 0 || !(a.durationSeconds > 0) || a.durationSeconds > 24 * 3600) throw std::runtime_error("invalid offset or duration.");
        if (a.fadeInSeconds < 0 || a.fadeOutSeconds < 0) throw std::runtime_error("fades must be >= 0.");
        t.audioClips.push_back(std::move(a));
      } catch (const std::exception& e) {
        throw std::runtime_error("audio clip " + std::to_string(index) + ": " + e.what());
      }
    }
  }
  if (tv.contains("samples") && tv["samples"].is_array() && !tv["samples"].empty())
    t.loadNotes.push_back("the sampler's sounds are not played any more (Core's sound comes from plugins)");
  if (t.instrument == "audio" && (!t.notes.empty() || !t.clips.empty())) throw std::runtime_error("an audio track has audio clips, not notes.");
  if (t.instrument == "bus" && (!t.notes.empty() || !t.clips.empty() || !t.audioClips.empty() || !t.samples.empty()))
    throw std::runtime_error("a bus plays what is routed to it: no notes, clips or samples.");
  // M7a routing (the targets are checked against the whole project by validateRouting).
  if (tv.contains("output") && !tv["output"].is_null()) {
    if (!tv["output"].is_string()) throw std::runtime_error("output must be a bus id (or \"\" for the master).");
    t.output = tv["output"].get<std::string>();
    if (t.output == "master") t.output.clear();
  }
  if (tv.contains("sends") && !tv["sends"].is_null()) {
    if (!tv["sends"].is_array()) throw std::runtime_error("sends must be a list.");
    if (tv["sends"].size() > (size_t)kMaxSends) throw std::runtime_error("too many sends (max " + std::to_string(kMaxSends) + ").");
    std::set<std::string> sendIds;
    for (const auto& sv : tv["sends"]) {
      if (!sv.is_object() || !sv.contains("to") || !sv["to"].is_string()) throw std::runtime_error("a send needs {to: <bus id>}.");
      Send s;
      s.id = str(sv, "id", "s" + std::to_string(t.sends.size() + 1));
      if (s.id.empty() || s.id.size() > 40 || !sendIds.insert(s.id).second) throw std::runtime_error("send ids must be unique.");
      s.to = sv["to"].get<std::string>();
      s.gainDb = clampd(num(sv, "gainDb", 0), -60, 12);
      s.pre = sv.value("pre", false);
      t.sends.push_back(std::move(s));
    }
  }
  if (tv.contains("modulators") && !tv["modulators"].is_null()) {
    if (!tv["modulators"].is_array()) throw std::runtime_error("modulators must be a list.");
    if (tv["modulators"].size() > (size_t)kMaxModulators) throw std::runtime_error("too many modulators (max " + std::to_string(kMaxModulators) + ").");
    std::set<std::string> modIds;
    for (const auto& mv : tv["modulators"]) {
      if (!mv.is_object()) throw std::runtime_error("a modulator must be an object.");
      Modulator m;
      m.id = str(mv, "id", "m" + std::to_string(t.modulators.size() + 1));
      if (m.id.empty() || m.id.size() > 40 || !modIds.insert(m.id).second) throw std::runtime_error("modulator ids must be unique.");
      m.param = str(mv, "param", "");
      m.shape = str(mv, "shape", "sine");
      const auto& shapes = modulatorShapes();
      if (std::find(shapes.begin(), shapes.end(), m.shape) == shapes.end()) throw std::runtime_error("modulator '" + m.id + "': unknown shape '" + m.shape + "'.");
      // FCore: a built-in insert effect's parameter, "fx:<id>:<param>". The effect must be one of the track's,
      // built in, with that parameter (a plugin's parameters are automated, never modulated).
      const ParamSpec* fxSpec = nullptr;
      if (m.param.rfind("fx:", 0) == 0) {
        const size_t colon = m.param.find(':', 3);
        const std::string fxId = colon == std::string::npos ? std::string() : m.param.substr(3, colon - 3);
        const std::string fxParam = colon == std::string::npos ? std::string() : m.param.substr(colon + 1);
        for (const auto& fx : t.effects)
          if (fx.value("id", std::string()) == fxId && fx.value("type", std::string("builtin")) == "builtin" && !fx.value("unsupported", false))
            for (const auto& sp : effectParams(fx.value("name", std::string())))
              if (fxParam == sp.name) fxSpec = &sp;
        if (!fxSpec) { t.loadNotes.push_back("modulator '" + m.id + "' on '" + m.param + "' is not played (Core's own effects left)"); continue; }
      }
      const auto spec = std::find_if(specs.begin(), specs.end(), [&](const ParamSpec& s) { return m.param == s.name; });
      const bool known = fxSpec || isMixParam(m.param) || (!hasNoInstrumentParams(t.instrument) && spec != specs.end());
      if (!known) { t.loadNotes.push_back("modulator '" + m.id + "' on '" + m.param + "' is not played (Core's own instruments left)"); continue; }
      m.rateBeats = clampd(num(mv, "rateBeats", 1), 1.0 / 64, m.shape == "rise" ? 256 : 64);
      const char* unit = fxSpec ? fxSpec->unit : (spec != specs.end() && !isMixParam(m.param) ? spec->unit : "");
      const bool hz = std::string(unit) == "Hz";
      const double span = m.param == "gainDb" ? 72 : m.param == "pan" ? 2 : hz ? 8 : fxSpec ? fxSpec->max - fxSpec->min : (spec != specs.end() ? spec->max - spec->min : 0);
      m.depth = clampd(num(mv, "depth", 0), -span, span);
      m.phase = clampd(num(mv, "phase", 0), 0, 1);
      m.attackMs = clampd(num(mv, "attackMs", 0), 0, 2000);
      m.decayMs = clampd(num(mv, "decayMs", 150), 1, 10000);
      m.curve = clampd(num(mv, "curve", 1), 0.25, 4);
      if (mv.contains("on") && !mv["on"].is_null()) {
        if (!mv["on"].is_boolean()) throw std::runtime_error("modulator '" + m.id + "': on must be true or false.");
        m.on = mv["on"].get<bool>();
      }
      if (mv.contains("hold") && !mv["hold"].is_null()) {
        if (!mv["hold"].is_boolean()) throw std::runtime_error("modulator '" + m.id + "': hold must be true or false.");
        m.hold = mv["hold"].get<bool>();
      }
      if (mv.contains("source") && !mv["source"].is_null()) {
        if (!mv["source"].is_string()) throw std::runtime_error("modulator '" + m.id + "': source must be a track id.");
        m.source = mv["source"].get<std::string>();
        if (!m.source.empty() && m.shape != "envelope" && m.shape != "rise")
          throw std::runtime_error("modulator '" + m.id + "': only an envelope or a rise starts from another track's notes.");
      }
      if (m.shape == "steps") {
        const json& sv = mv.contains("steps") ? mv["steps"] : json();
        if (!sv.is_array() || sv.empty() || sv.size() > (size_t)kMaxModSteps)
          throw std::runtime_error("modulator '" + m.id + "': steps must be 1 to " + std::to_string(kMaxModSteps) + " numbers.");
        for (const auto& v : sv) {
          if (!v.is_number()) throw std::runtime_error("modulator '" + m.id + "': steps must be numbers from -1 to 1.");
          m.steps.push_back(clampd(v.get<double>(), -1, 1));
        }
      }
      t.modulators.push_back(std::move(m));
    }
  }
  json envelopeList = tv.value("envelopes", json::array());
  // The automation of an instrument that left (a W-Core "tone", a kick "ampDecay"...) has nothing to move any more:
  // it is left out with a note, so the song still opens (before, "envelope on unknown parameter" refused the song).
  if (ownInstrumentLeft && envelopeList.is_array()) {
    json kept = json::array();
    for (const auto& ev : envelopeList) {
      const std::string param = ev.is_object() ? str(ev, "param", "") : std::string();
      if (isMixParam(param) || param.rfind("fx:", 0) == 0) kept.push_back(ev);
      else t.loadNotes.push_back("the automation of '" + param + "' left with Core's own instrument");
    }
    envelopeList = std::move(kept);
  }
  t.envelopes = envelopesFromJson(envelopeList, [&](const std::string& param) {
    return isMixParam(param) || std::any_of(specs.begin(), specs.end(), [&](const ParamSpec& s) { return param == s.name; }) ||
           (t.instrument == "plugin" && param.rfind("vst:", 0) == 0 && param.size() > 4);  // the plugin's own parameter, 0..1
  });
  return t;
}

} // namespace core
