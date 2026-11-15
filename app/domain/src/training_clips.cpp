#include "training_clips.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <fstream>
#include <map>
#include <random>
#include <set>
#include <sstream>

#include "assets.hpp"
#include "edit.hpp"
#include "randomize.hpp"

namespace core::doc {

namespace {

constexpr int64_t kBar = 4 * kPpq;   // the templates are in 4/4
constexpr int64_t kStep = kPpq / 4;  // a 16th
const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// FNV-1a: the same on every compiler (std::hash is not), so a seed gives the same clip on any build.
uint64_t textHash(const std::string& s) {
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; }
  return h;
}

// A library file that is gone (moved since the last scan) is never picked: the clip would render without it.
bool fileExists(const std::string& utf8) {
  std::error_code ec;
  return std::filesystem::is_regular_file(std::filesystem::path(std::u8string(utf8.begin(), utf8.end())), ec);
}

std::string noteName(int pitch) { return std::string(kNoteNames[((pitch % 12) + 12) % 12]) + std::to_string(pitch / 12 - 1); }

// The track roles each kind keeps (trackSampleRole, then the id for what it does not know).
std::string trackRole(const Track& t) {
  const std::string r = trackSampleRole(t);
  return r.empty() ? t.id : r;
}
bool kindKeeps(const std::string& kind, const Track& t) {
  if (!playsPatterns(t)) return false;
  const std::string r = trackRole(t);
  if (kind == "kbbb") return r == "kick" || r == "bass";
  if (kind == "groove") return r == "kick" || r == "bass" || r == "hat" || r == "openhat" || r == "perc" || r == "accent";
  return true;  // full
}

int sectionEnergy(const Section& s) {
  const auto it = s.extra.find("energy");
  return it != s.extra.end() && it->is_number() ? it->get<int>() : -1;
}

struct PlacedNote { int64_t tick; int64_t length; int pitch; double velocity; };

// The notes a track plays in [from, to), on the song's ticks (each clip repeats its pattern).
std::vector<PlacedNote> notesIn(const Track& t, int64_t from, int64_t to) {
  std::vector<PlacedNote> out;
  for (const auto& c : t.clips) {
    const Pattern* p = t.findPattern(c.patternId);
    if (!p || p->lengthTicks <= 0) continue;
    const int64_t cs = c.startTick, ce = c.startTick + c.lengthTicks;
    if (ce <= from || cs >= to) continue;
    for (int64_t base = cs - c.offsetTicks; base < ce; base += p->lengthTicks)
      for (const auto& n : p->notes) {
        const int64_t at = base + n.tick;
        if (at < cs || at >= ce || at < from || at >= to) continue;
        out.push_back({at, n.length, n.pitch, n.velocity});
      }
  }
  std::sort(out.begin(), out.end(), [](const PlacedNote& a, const PlacedNote& b) { return a.tick < b.tick || (a.tick == b.tick && a.pitch < b.pitch); });
  return out;
}

// A bar as 16 steps: "x" where a note starts, "-" elsewhere.
std::string stepGrid(const std::vector<PlacedNote>& notes, int64_t barStart) {
  std::string g(16, '-');
  for (const auto& n : notes)
    if (n.tick >= barStart && n.tick < barStart + kBar) g[(size_t)((n.tick - barStart) / kStep)] = 'x';
  return g;
}

std::set<std::string> patternsIn(const Track& t, int64_t from, int64_t to) {
  std::set<std::string> names;
  for (const auto& c : t.clips)
    if (c.startTick < to && c.startTick + c.lengthTicks > from)
      if (const Pattern* p = t.findPattern(c.patternId)) names.insert(p->name.empty() ? p->id : p->name);
  return names;
}

json effectsJson(const json& chain) {
  json out = json::array();
  if (!chain.is_array()) return out;
  for (const auto& fx : chain) {
    json e = {{"name", effectName(fx)}, {"bypassed", effectBypassed(fx)}, {"builtin", true}};
    if (fx.contains("params") && fx["params"].is_object() && !fx["params"].empty()) e["params"] = fx["params"];
    if (fx.contains("sidechain")) e["sidechain"] = fx["sidechain"];
    out.push_back(std::move(e));
  }
  return out;
}

json instrumentJson(const Instrument& in, const SampleIndex* library) {
  json j = {{"kind", in.kind}};
  j["name"] = in.name;
  if (!in.params.empty()) j["params"] = in.params;
  if (in.name == "sampler") {
    json samples = json::array();
    for (const auto& z : in.samples) {
      json s = {{"file", z.name}, {"sha256", z.sha256}, {"rootKey", noteName(z.rootKey)}};
      const LibrarySample* ls = library ? library->findHash(z.sha256) : nullptr;
      if (ls) {
        s["name"] = shownName(*ls);
        s["pack"] = ls->pack;
        s["seconds"] = ls->seconds;
        // The catalogue's fields that say something (a one-shot has no BPM; a sample may have no tags).
        json cat = json::object();
        const json info = sampleInfoJson(ls->info);
        for (const auto& [k, v] : info.items()) {
          const bool empty = v.is_null() || (v.is_string() && v.get<std::string>().empty()) || (v.is_array() && v.empty()) ||
                             (v.is_number() && v.get<double>() == 0) || (k == "role" && v == "other");
          if (!empty) cat[k] = v;
        }
        s["catalogue"] = cat;
        s["soundWords"] = soundWords(*ls);
        if (ls->analysis.is_object()) {
          if (ls->analysis.contains("tailMs")) s["tailMs"] = ls->analysis["tailMs"];
          if (ls->analysis.contains("attackMs")) s["attackMs"] = ls->analysis["attackMs"];
        }
      }
      samples.push_back(std::move(s));
      if (samples.size() >= 8) break;  // a bass recorded note by note: the first zones say enough
    }
    j["samples"] = samples;
    if (in.samples.size() > 8) j["zones"] = in.samples.size();
  }
  return j;
}

// Bars of the region: whole bars inside [minS, maxS] from start, multiples of 4 first, the closest to the middle.
int pickBars(const Document& doc, int64_t start, double minS, double maxS) {
  const double mid = (minS + maxS) / 2;
  int best = 0;
  double bestScore = 1e18;
  for (int bars = 1; bars <= 256; ++bars) {
    const double s = secondsAt(doc, (double)(start + bars * kBar)) - secondsAt(doc, (double)start);
    if (s > maxS + 1e-9) break;
    if (s + 1e-9 < minS) continue;
    const double score = std::abs(s - mid) + (bars % 4 == 0 ? 0 : 1000);
    if (score < bestScore) { bestScore = score; best = bars; }
  }
  return best;
}

std::string fmt1(double v) {
  std::ostringstream o;
  o.setf(std::ios::fixed);
  o.precision(1);
  o << v;
  return o.str();
}

} // namespace

const std::vector<std::string>& trainingClipKinds() {
  static const std::vector<std::string> k = {"kbbb", "groove", "full"};
  return k;
}

TrainingClip buildTrainingClip(const SongTemplate& tmpl, const TrainingClipRequest& rq) {
  const auto& kinds = trainingClipKinds();
  if (std::find(kinds.begin(), kinds.end(), rq.kind) == kinds.end()) throw DocumentError("kind", "must be kbbb, groove or full");
  if (!(rq.minSeconds > 0 && rq.maxSeconds >= rq.minSeconds && rq.maxSeconds <= 120))
    throw DocumentError("seconds", "must be 0 < min <= max <= 120");

  TrainingClip clip;
  clip.doc = documentFromTemplate(tmpl);
  Document& doc = clip.doc;
  const std::string style = !tmpl.info.style.empty() ? tmpl.info.style : songStyle(doc);
  // Each template and kind draws its own numbers from the seed: the same seed in two styles (or a kbbb and a groove
  // clip) is not the same samples at the same bar.
  const uint64_t salt = textHash(tmpl.info.id + "/" + rq.kind);
  std::mt19937_64 rng(rq.seed * 0x9E3779B97F4A7C15ull + 17 + salt);

  json facts = {{"format", kTrainingClipFormat}, {"version", kTrainingClipVersion}};
  std::vector<std::string> changes;
  // Samples of the library: any good one for the role (not only those near the best, as File > New does), so a batch
  // covers the library. The "-builtin" templates keep their Core instruments: they are there for those sounds.
  const bool builtinTemplate = tmpl.info.id.size() > 8 && tmpl.info.id.compare(tmpl.info.id.size() - 8, 8, "-builtin") == 0;
  bool samplesUsed = false;
  if (rq.library && rq.swapSamples && !builtinTemplate) {
    std::vector<Track*> order;
    for (auto& t : doc.tracks) if (trackSampleRole(t) == "bass") order.push_back(&t);
    for (auto& t : doc.tracks) if (trackSampleRole(t) != "bass") order.push_back(&t);
    std::set<std::string> used;
    for (Track* t : order) {
      const std::string role = trackSampleRole(*t);
      if (role.empty() || !playsPatterns(*t)) continue;
      std::vector<const LibrarySample*> pool;
      for (const auto& c : suggestSamples(*rq.library, role, style, 200, anchorPack(doc, *rq.library)))
        if (c.score >= kGoodSuggestion && !used.count(c.sample->sha256) && fileExists(c.sample->file)) pool.push_back(c.sample);
      if (pool.empty()) continue;
      std::mt19937_64 pickRng(rq.seed * 0xD1B54A32D192ED03ull + salt + textHash(t->id));
      const LibrarySample* pick = pool[(size_t)(pickRng() % pool.size())];
      setTrackSample(doc, t->id, *rq.library, *pick);
      used.insert(pick->sha256);
      samplesUsed = true;
      changes.push_back("Sample: " + t->name + ": " + shownName(*pick) + " (1 of " + std::to_string(pool.size()) + " that fit)");
    }
  }
  bool randomized = false;
  if (rq.randomize) {
    try {
      RandomizeOptions ro;
      ro.seed = rq.seed;
      ro.axes = rq.axes.empty() ? std::vector<std::string>{"bass", "gate", "hats", "kick", "roles", "fills", "sound"} : rq.axes;
      for (auto& line : randomizeStyle(doc, ro)) changes.push_back(line);
      randomized = true;
    } catch (const DocumentError&) {
      // A style Randomize does not know (full-on, twilight...): the template's own groove.
    }
  }

  // The tracks of the kind play; the rest is muted (buses stay: what plays may go through them).
  std::vector<const Track*> playing;
  for (auto& t : doc.tracks) {
    if (!playsPatterns(t)) continue;
    if (kindKeeps(rq.kind, t)) {
      if (!t.mix.mute) playing.push_back(&t);
    } else {
      t.mix.mute = true;
    }
    t.mix.solo = false;
  }
  if (playing.empty()) throw DocumentError("kind", "the template has no unmuted tracks for " + rq.kind);

  // The section: a high-energy one (any one when the template says none), the seed picks.
  std::vector<const Section*> sections;
  for (const auto& s : doc.sections) if (sectionEnergy(s) >= 3) sections.push_back(&s);
  if (sections.empty()) for (const auto& s : doc.sections) sections.push_back(&s);
  int64_t start = 0;
  const Section* section = nullptr;
  if (!sections.empty()) {
    section = sections[(size_t)(rng() % sections.size())];
    start = section->startTick;
  }
  int bars = pickBars(doc, start, rq.minSeconds, rq.maxSeconds);
  if (bars == 0) throw DocumentError("seconds", "no whole number of bars at this tempo lasts " + fmt1(rq.minSeconds) + "-" + fmt1(rq.maxSeconds) + " s");
  // Somewhere inside the section when it is longer than the clip (on a 4-bar line), never past the song's end.
  if (section) {
    const int64_t room = section->startTick + section->lengthTicks - (start + bars * kBar);
    if (room >= 4 * kBar) start += (int64_t)(rng() % (uint64_t)(room / (4 * kBar) + 1)) * 4 * kBar;
  }
  if (start + bars * kBar > doc.lengthTicks) start = std::max<int64_t>(0, (doc.lengthTicks - bars * kBar) / kBar * kBar);
  bars = pickBars(doc, start, rq.minSeconds, rq.maxSeconds);
  if (bars == 0 || start + bars * kBar > doc.lengthTicks) throw DocumentError("seconds", "the song is too short for a clip this long");
  clip.startTick = start;
  clip.endTick = start + bars * kBar;
  clip.bars = bars;
  clip.seconds = secondsAt(doc, (double)clip.endTick) - secondsAt(doc, (double)clip.startTick);
  clip.id = tmpl.info.id + "-" + rq.kind + "-s" + std::to_string(rq.seed);

  // Tempo: at the start, and every change inside the region.
  double startBpm = doc.bpm;
  json tempoChanges = json::array();
  for (const auto& p : doc.tempoMap) {
    if (p.tick <= clip.startTick) startBpm = p.bpm;
    else if (p.tick < clip.endTick) tempoChanges.push_back({{"bar", (p.tick - clip.startTick) / kBar + 1}, {"bpm", p.bpm}, {"ramp", p.ramp}});
  }

  json sectionsJson = json::array();
  for (const auto& s : doc.sections) {
    const int64_t a = std::max(s.startTick, clip.startTick), b = std::min(s.startTick + s.lengthTicks, clip.endTick);
    if (b > a) sectionsJson.push_back({{"name", s.name}, {"energy", sectionEnergy(s)}, {"bars", (b - a) / kBar}});
  }

  json tracks = json::array();
  std::string bassRoot;
  for (const Track* t : playing) {
    const auto notes = notesIn(*t, clip.startTick, clip.endTick);
    json tj = {{"id", t->id}, {"name", t->name}, {"role", trackRole(*t)}, {"instrument", instrumentJson(t->instrument, rq.library)},
               {"effects", effectsJson(t->effects)}, {"mix", {{"gainDb", t->mix.gainDb}, {"pan", t->mix.pan}}}};
    if (!t->mix.sends.empty()) {
      json sends = json::array();
      for (const auto& s : t->mix.sends) {
        const Track* bus = doc.findTrack(s.to);
        sends.push_back({{"to", bus ? bus->name : s.to}, {"gainDb", s.gainDb}, {"effects", bus ? effectsJson(bus->effects) : json::array()}});
      }
      tj["sends"] = sends;
    }
    json np = {{"count", notes.size()}, {"patterns", patternsIn(*t, clip.startTick, clip.endTick)}};
    if (!notes.empty()) {
      np["firstBar"] = stepGrid(notes, clip.startTick);
      std::map<int, int> classes;
      int lo = 127, hi = 0;
      double lengthMs = 0;
      for (const auto& n : notes) {
        ++classes[((n.pitch % 12) + 12) % 12];
        lo = std::min(lo, n.pitch);
        hi = std::max(hi, n.pitch);
        lengthMs += (secondsAt(doc, (double)(n.tick + n.length)) - secondsAt(doc, (double)n.tick)) * 1000;
      }
      np["lowest"] = noteName(lo);
      np["highest"] = noteName(hi);
      np["averageLengthMs"] = std::round(lengthMs / notes.size() * 10) / 10;
      const auto root = std::max_element(classes.begin(), classes.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
      np["mostPlayed"] = kNoteNames[root];
      if (trackRole(*t) == "bass" && bassRoot.empty()) bassRoot = kNoteNames[root];
      if (t->automation.size()) {
        json autos = json::array();
        for (const auto& a : t->automation)
          for (const auto& p : a.points)
            if (p.tick >= clip.startTick && p.tick < clip.endTick) { autos.push_back(a.param); break; }
        if (!autos.empty()) tj["automated"] = autos;
      }
    }
    tj["notes"] = np;
    tracks.push_back(std::move(tj));
  }

  facts["id"] = clip.id;
  facts["kind"] = rq.kind;
  facts["seed"] = rq.seed;
  facts["style"] = style;
  facts["template"] = {{"id", tmpl.info.id}, {"version", tmpl.info.version}, {"name", tmpl.info.name}, {"description", tmpl.info.description}};
  facts["bpm"] = startBpm;
  if (!tempoChanges.empty()) facts["tempoChanges"] = tempoChanges;
  facts["timeSignature"] = "4/4";
  facts["bars"] = bars;
  facts["seconds"] = std::round(clip.seconds * 1000) / 1000;
  facts["region"] = {{"startBar", clip.startTick / kBar + 1}, {"startTick", clip.startTick}, {"endTick", clip.endTick}};
  if (!bassRoot.empty()) facts["bassRoot"] = bassRoot;
  facts["sections"] = sectionsJson;
  facts["tracks"] = tracks;
  facts["master"] = {{"gainDb", doc.masterGainDb}, {"effects", effectsJson(doc.masterEffects)}};
  // No voice: what plays is drums, bass and synths (a vox sample would say so in its role). ACE wants this stated.
  bool vocal = false;
  for (const auto& t : facts["tracks"])
    for (const auto& smp : t["instrument"].value("samples", json::array()))
      if (smp.value("catalogue", json::object()).value("role", "") == "vox") vocal = true;
  for (const auto& t : facts["tracks"])
    if (t.value("id", "").find("vox") != std::string::npos || t.value("name", "").find("Vox") != std::string::npos) vocal = true;
  facts["instrumental"] = !vocal;
  facts["randomized"] = randomized;
  // Randomize says what it did per section ("Groove: bass kbb, gate 40 ms"): only the sections of the clip.
  json kept = json::array();
  for (const auto& line : changes) {
    const auto colon = line.find(": ");
    bool sectionLine = false, inClip = false;
    if (colon != std::string::npos)
      for (const auto& sec : doc.sections)
        if (line.compare(0, colon, sec.name) == 0) {
          sectionLine = true;
          for (const auto& c : sectionsJson) inClip = inClip || c["name"] == sec.name;
        }
    if (!sectionLine || inClip) kept.push_back(line);
  }
  facts["changes"] = kept;
  facts["samplesSwapped"] = samplesUsed;
  facts["description"] = describeTrainingClip(facts);
  clip.facts = std::move(facts);
  return clip;
}

std::string describeTrainingClip(const json& f) {
  std::ostringstream o;
  static const std::map<std::string, std::string> kindText = {
      {"kbbb", "kick and bass loop"}, {"groove", "drums and bass groove"}, {"full", "full mix excerpt"}};
  const std::string kind = f.value("kind", "full");
  o << f.value("style", "Unknown style") << " " << (kindText.count(kind) ? kindText.at(kind) : kind) << ", " << (int)std::lround(f.value("bpm", 0.0))
    << " BPM";
  if (f.contains("bassRoot")) o << ", bass root " << f["bassRoot"].get<std::string>();
  o << ", " << f.value("bars", 0) << " bars (" << fmt1(f.value("seconds", 0.0)) << " s), 4/4.";
  if (f.contains("tempoChanges"))
    for (const auto& c : f["tempoChanges"]) o << " Tempo " << (c.value("ramp", false) ? "glides" : "changes") << " to " << (int)std::lround(c.value("bpm", 0.0)) << " BPM at bar " << c.value("bar", 0) << ".";
  if (f.contains("sections") && !f["sections"].empty()) {
    o << " Section" << (f["sections"].size() > 1 ? "s" : "") << ":";
    bool first = true;
    for (const auto& s : f["sections"]) { o << (first ? " " : ", ") << s.value("name", "") << " (energy " << s.value("energy", -1) << ")"; first = false; }
    o << ".";
  }
  for (const auto& t : f.value("tracks", json::array())) {
    o << " " << t.value("name", t.value("id", "")) << ": ";
    const json& in = t["instrument"];
    if (in.value("name", "") == "sampler" && in.contains("samples") && !in["samples"].empty()) {
      const json& s = in["samples"][0];
      o << "sample \"" << s.value("name", s.value("file", "")) << "\"";
      if (s.contains("tailMs") && s["tailMs"].is_number()) o << " (tail " << (int)std::lround(s["tailMs"].get<double>()) << " ms)";
    } else o << "Core " << in.value("name", "") << " synth";
    const json& n = t["notes"];
    if (n.contains("firstBar")) o << ", rhythm " << n["firstBar"].get<std::string>();
    if (t.value("role", "") == "bass" && n.contains("averageLengthMs")) o << ", notes " << (int)std::lround(n["averageLengthMs"].get<double>()) << " ms";
    std::vector<std::string> fx;
    for (const auto& e : t.value("effects", json::array()))
      if (!e.value("bypassed", false)) fx.push_back(e.value("name", ""));
    if (!fx.empty()) {
      o << ", effects ";
      for (size_t i = 0; i < fx.size(); ++i) o << (i ? " > " : "") << fx[i];
    }
    for (const auto& s : t.value("sends", json::array())) o << ", send to " << s.value("to", "") << " " << fmt1(s.value("gainDb", 0.0)) << " dB";
    o << ".";
  }
  std::vector<std::string> mfx;
  for (const auto& e : f.value("master", json::object()).value("effects", json::array()))
    if (!e.value("bypassed", false)) mfx.push_back(e.value("name", ""));
  if (!mfx.empty()) {
    o << " Master: ";
    for (size_t i = 0; i < mfx.size(); ++i) o << (i ? " > " : "") << mfx[i];
    o << ".";
  }
  return o.str();
}

void fadeWavEdges(const std::filesystem::path& wav, double fadeMs) {
  const WavInfo info = readWavInfo(wav);
  const int bytes = info.bits / 8;
  if (!(info.bits == 16 || info.bits == 24 || info.bits == 32) || info.channels <= 0 || info.frames <= 0 || info.dataOffset <= 0)
    throw DocumentError("wav", "cannot fade this format");
  const int64_t fade = std::min<int64_t>(info.frames / 2, (int64_t)std::llround(fadeMs / 1000 * info.sampleRate));
  if (fade <= 0) return;
  std::fstream f(wav, std::ios::in | std::ios::out | std::ios::binary);
  if (!f) throw DocumentError("wav", "cannot open for writing");
  const int64_t frameBytes = (int64_t)bytes * info.channels;
  std::vector<unsigned char> buf((size_t)(fade * frameBytes));
  auto apply = [&](int64_t firstFrame, bool in) {
    const int64_t at = info.dataOffset + firstFrame * frameBytes;
    f.seekg(at);
    f.read((char*)buf.data(), (std::streamsize)buf.size());
    if (!f) throw DocumentError("wav", "short read");
    for (int64_t i = 0; i < fade; ++i) {
      const double g = in ? (double)i / fade : (double)(fade - 1 - i) / fade;
      for (int c = 0; c < info.channels; ++c) {
        unsigned char* p = buf.data() + i * frameBytes + c * bytes;
        if (info.isFloat && bytes == 4) {
          float v;
          std::memcpy(&v, p, 4);
          v = (float)(v * g);
          std::memcpy(p, &v, 4);
        } else {
          int64_t v = 0;
          for (int b = 0; b < bytes; ++b) v |= (int64_t)p[b] << (8 * b);
          const int64_t sign = (int64_t)1 << (8 * bytes - 1);
          if (v & sign) v -= sign << 1;
          v = (int64_t)std::llround((double)v * g);
          for (int b = 0; b < bytes; ++b) p[b] = (unsigned char)((v >> (8 * b)) & 0xff);
        }
      }
    }
    f.seekp(at);
    f.write((const char*)buf.data(), (std::streamsize)buf.size());
    if (!f) throw DocumentError("wav", "short write");
  };
  apply(0, true);
  apply(info.frames - fade, false);
}

std::string checkTrainingClip(const std::filesystem::path& wav, json& facts, const json& missing) {
  if (missing.is_array() && !missing.empty()) return "did not load: " + missing.dump();
  double peakDb = kNotMeasured, loudDb = kNotMeasured;
  measureLoudness(wav, peakDb, loudDb);
  facts["audio"] = {{"peakDb", std::round(peakDb * 10) / 10}, {"loudDb", std::round(loudDb * 10) / 10}};
  if (peakDb < -60) return "silent (peak " + fmt1(peakDb) + " dBFS)";
  if (loudDb < -40) return "too quiet (loudest 100 ms at " + fmt1(loudDb) + " dBFS)";
  if (peakDb > -0.01) return "clips (peak " + fmt1(peakDb) + " dBFS)";
  return "";
}

} // namespace core::doc
