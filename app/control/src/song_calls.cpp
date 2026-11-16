#include "song_calls.hpp"
#include <array>
#include <cstdio>
#include "gate.hpp"
#include "role_tools.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include "edit.hpp"
#include "samples.hpp"
#include "score.hpp"

namespace core::control::songcalls {

namespace {

std::string fold(const std::string& s) {
  std::string out;
  for (unsigned char ch : s)
    if (std::isalnum(ch)) out += (char)std::tolower(ch);
  return out;
}
bool same(const std::string& a, const std::string& b) { return !a.empty() && fold(a) == fold(b); }

std::string str(const json& o, const char* k) {
  return o.is_object() && o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string();
}

std::string listOf(const std::vector<std::string>& v) {
  std::string l;
  for (const auto& x : v) l += (l.empty() ? "" : ", ") + x;
  return l;
}

std::string num(double v) {
  char b[32];
  std::snprintf(b, sizeof b, "%g", v);
  return b;
}

const std::vector<std::string>& roots() {
  static const std::vector<std::string> r = {"C", "C#", "Db", "D", "D#", "Eb", "E", "F", "F#", "Gb", "G", "G#", "Ab", "A", "A#", "Bb", "B"};
  return r;
}

// A text choice of the call, from the call itself or from its "args" (as AICORE writes them).
json textOf(const json& c, const std::string& key) {
  if (c.contains(key)) return c[key];
  if (c.contains("args") && c["args"].is_object() && c["args"].contains(key)) return c["args"][key];
  return nullptr;
}

// Checks a text choice against the function's options (any value when the function has none for it).
bool takeText(const json& c, const CoreFunction& f, const std::string& key, bool required, Call& call, std::vector<std::string>& problems) {
  const json v = textOf(c, key);
  if (v.is_null() || (v.is_string() && v.get<std::string>().empty())) {
    if (required) problems.push_back(f.name + ": \"" + key + "\" is needed: left out");
    return !required;
  }
  if (!v.is_string()) {
    problems.push_back(f.name + " " + key + ": not a text: left out");
    return false;
  }
  const std::string want = v.get<std::string>();
  const auto opt = f.options.find(key);
  if (opt == f.options.end() || opt->second.empty()) {
    call.texts[key] = want;
    return true;
  }
  for (const auto& o : opt->second)
    if (o == want || same(o, want)) {
      call.texts[key] = o;
      return true;
    }
  problems.push_back(f.name + " " + key + " \"" + want + "\" is not one of: " + listOf(opt->second) + ": left out");
  return false;
}

// Checks the numbers of the call against the function's knobs.
bool takeArgs(const json& c, const CoreFunction& f, Call& call, std::vector<std::string>& problems) {
  if (!c.contains("args") || !c["args"].is_object()) return true;
  bool ok = true;
  for (auto kv = c["args"].begin(); kv != c["args"].end(); ++kv) {
    if (kv.value().is_string() && f.options.count(kv.key())) continue;  // a text choice
    const FunctionArg* a = f.findArg(kv.key());
    if (!a) {
      problems.push_back(f.name + ": no knob \"" + kv.key() + "\": left out");
      ok = false;
      continue;
    }
    if (!kv.value().is_number()) {
      problems.push_back(f.name + " " + kv.key() + ": not a number: left out");
      ok = false;
      continue;
    }
    const double v = kv.value().get<double>();
    if (!std::isfinite(v) || v < a->min || v > a->max) {
      problems.push_back(f.name + " " + kv.key() + " " + num(v) + " is outside " + num(a->min) + ".." + num(a->max) + ": left out");
      ok = false;
      continue;
    }
    call.args[a->id] = v;
  }
  return ok;
}

const doc::Section* sectionNamed(const doc::Document& song, const std::string& name) {
  for (const auto& s : song.sections)
    if (s.id == name || same(s.name, name)) return &s;
  return nullptr;
}

double arg(const Call& c, const char* k, double def) {
  const auto it = c.args.find(k);
  return it == c.args.end() ? def : it->second;
}
std::string text(const Call& c, const char* k) {
  const auto it = c.texts.find(k);
  return it == c.texts.end() ? std::string() : it->second;
}

doc::PCoreLayers layersOf(const doc::Document& song, const Call& c, const doc::PCoreCatalog& cat) {
  doc::PCoreLayers l;
  l.mode = text(c, "mode").empty() ? "create" : text(c, "mode");
  l.style = text(c, "style");
  if (l.style.empty()) {
    if (const doc::PCoreStyle* s = doc::pcoreStyleFor(cat, doc::songStyle(song))) l.style = s->id;
    else if (const doc::PCoreStyle* s2 = doc::pcoreStyleFor(cat, song.name)) l.style = s2->id;
    else if (!cat.styles.empty()) l.style = cat.styles.front().id;
  }
  l.scale = text(c, "scale");
  l.root = text(c, "root");
  l.grid = text(c, "grid");
  l.density = arg(c, "density", 0);
  l.humanize = arg(c, "humanize", -1);
  l.variationEvery = (int)arg(c, "variationEvery", 0);
  l.rollBeats = (int)arg(c, "rollBeats", 0);
  l.seed = (uint64_t)arg(c, "seed", 1);
  l.align = text(c, "align").empty() ? std::string("style") : text(c, "align") == "off" ? std::string() : text(c, "align");
  return l;
}

// Two P-Core calls go together when only their track, part and shape differ.
std::string groupKey(const Call& c) {
  std::string k = c.sectionName + "|";
  for (const auto& [n, v] : c.args) k += n + "=" + num(v) + ";";
  for (const auto& [n, v] : c.texts)
    if (n != "part" && n != "shape") k += n + "=" + v + ";";
  return k;
}

}  // namespace

void addSongTools(std::vector<CoreFunction>& out);
const doc::SongToolInfo* songTool(const std::string& id);

bool isSongFamily(const std::string& family) {
  return family == "track" || family == "pcore" || family == "shape" || family == "arrange" || family == "plays" || family == "gate" ||
         family == "songtool";
}

void addFunctions(std::vector<CoreFunction>& out, const doc::PCoreCatalog* pcore) {
  {
    CoreFunction f;
    f.id = "track";
    f.family = "track";
    f.name = "New track";
    f.description = "a new track for notes, for a part of the song (\"part\"); \"name\" is shown (else the part's name). Its plugin is "
                    "chosen by hand; later calls of the same proposal name it by its name. A build-up track is the part riser";
    f.tags = {"track", "new", "add", "pista", "nova"};
    f.options["part"] = doc::scoreParts();
    out.push_back(std::move(f));
  }
  {
    CoreFunction f;
    f.id = "pcore";
    f.family = "pcore";
    f.name = "P-Core";
    f.description = "writes a part's notes into the track's pattern (the one it plays in \"section\", else its first): \"part\" "
                    "(else from the track's name), \"shape\" (else the style's first for the part), \"style\" (else the song's), "
                    "\"scale\", \"root\", \"grid\", \"mode\" create (new notes) or alter (changes the notes there), \"align\" "
                    "(kick, bass and hats locked together: style or off); knobs: bars, density, humanize, variationEvery, "
                    "rollBeats, seed. Calls of several tracks with the same knobs are written together (the bass leaves the kick free)";
    f.tags = {"pattern", "notes", "beat", "groove", "kick", "bass", "hats", "padrao", "batida", "notas"};
    f.args = {{"bars", "Bars", "the pattern's length in bars", "bars", 1, 64, 4},
              {"density", "Density", "fewer or more notes (the ones on the beat stay)", "%", -90, 200, 0},
              {"humanize", "Humanize", "-1: the style's", "", -1, 1, -1},
              {"variationEvery", "Variation every", "0, 1, 2, 4, 8 or 16 bars: a small fill at the end", "bars", 0, 16, 0},
              {"rollBeats", "Roll", "the last beats become a roll", "beats", 0, 16, 0},
              {"seed", "Seed", "another take of the same shape", "", 1, 9999, 1}};
    f.options["mode"] = {"create", "alter"};
    f.options["grid"] = {"1/4", "1/8", "1/8t", "1/16", "1/16t", "1/32", "1/64"};
    f.options["align"] = {"style", "off"};
    f.options["root"] = roots();
    if (pcore) {
      for (const auto& p : pcore->parts) f.options["part"].push_back(p.id);
      for (const auto& s : pcore->shapes) f.options["shape"].push_back(s.id);
      for (const auto& s : pcore->styles) f.options["style"].push_back(s.id);
      for (const auto& s : pcore->scales) f.options["scale"].push_back(s.id);
      for (const auto& a : pcore->aligns) f.options["align"].push_back(a.id);
    }
    out.push_back(std::move(f));
  }
  {
    CoreFunction f;
    f.id = "shape";
    f.family = "shape";
    f.name = "Song shape";
    f.description = "S-Core: the song's sections become the shape's (intro, build-up, drop, break, outro...) from bar 0; the clips stay";
    f.tags = {"structure", "sections", "form", "estrutura", "secções"};
    for (const auto& s : doc::scoreShapes()) f.options["shape"].push_back(s.id);
    out.push_back(std::move(f));
  }
  {
    CoreFunction f;
    f.id = "arrange";
    f.family = "arrange";
    f.name = "Arrange";
    f.description = "S-Core: places the track's pattern over the sections by the style's rules (\"on\" a track, else every track; "
                    "\"section\" one section, else all)";
    f.tags = {"arrange", "arrangement", "sections", "arranjo"};
    out.push_back(std::move(f));
  }
  {
    CoreFunction f;
    f.id = "plays";
    f.family = "plays";
    f.name = "Plays";
    f.description = "S-Core: the track plays the whole \"section\" (\"play\": on) or rests in it (off)";
    f.tags = {"mute", "remove", "add", "section", "tirar", "por"};
    f.options["play"] = {"on", "off"};
    out.push_back(std::move(f));
  }
  {
    CoreFunction f;
    f.id = "gate";
    f.family = "gate";
    f.name = "Gate";
    f.description = "Core AI Gate (our own plugin) chops the track in time over \"section\": a \"preset\" (the easy way), or a "
                    "\"pattern\" of 16 steps (x on, h half, . off; \"x.xx\" repeats) at a \"rate\"; knobs change the preset. "
                    "duck: a dip at every beat (the pump against the kick). Outside the section the gate does nothing";
    f.tags = {"gate", "trance gate", "chop", "duck", "pump", "sidechain", "cortar", "ritmo"};
    f.args = {{"depth", "Depth", "how far the closed steps go down (0: no chop, only the duck)", "", 0, 1, 1},
              {"length", "Length", "how much of each step is open", "", 0.1, 1, 0.5},
              {"smooth", "Smooth", "soft or hard edges", "ms", 0.5, 30, 3},
              {"duck", "Duck", "the dip at every beat", "", 0, 1, 0},
              {"mix", "Mix", "", "", 0, 1, 1}};
    f.options["preset"] = aifx::presetIds();
    f.options["rate"] = aifx::rateNames();
    f.options["pattern"] = {};  // any text: checked by gate.hpp parsePattern
    for (const auto& pr : aifx::presets()) f.extra["presetInfo"][pr.id] = pr.what;
    out.push_back(std::move(f));
  }
  addSongTools(out);
}

// The song tools of the cards (role_tools.hpp songToolCatalog: space, fx, roll, suck, pump, buildup), as the AI's
// functions, one each, with their parameters: "track" is the call's "on", a "section" its section, a "choice" a
// text option, a "number" a knob; "tracks" and "from" name other tracks (texts).
void addSongTools(std::vector<CoreFunction>& out) {
  for (const auto& t : doc::songToolCatalog()) {
    CoreFunction f;
    f.id = t.id;
    f.family = "songtool";
    f.name = t.id == "fx" ? "FX by intent" : t.id == "space" ? "Space (shared reverb / delay)" : t.id == "roll" ? "Roll" :
             t.id == "suck" ? "SUCK" : t.id == "pump" ? "PUM" : t.id == "buildup" ? "Build-up FX" : t.id;
    f.description = t.what + " (its plugins come from Settings > Roles: " + (t.roles.empty() ? std::string("none, notes only") : listOf(t.roles)) + ")";
    f.tags = {t.id};
    for (const auto& p : t.params) {
      if (p.kind == "choice") {
        std::vector<std::string> values;
        for (size_t a = 0; a <= p.values.size();) {
          const size_t bar = std::min(p.values.find('|', a), p.values.size());
          if (bar > a) values.push_back(p.values.substr(a, bar - a));
          a = bar + 1;
        }
        f.options[p.name] = values;
      } else if (p.kind == "number") {
        double lo = 0, hi = 1;
        std::sscanf(p.values.c_str(), "%lf..%lf", &lo, &hi);
        const double def = p.name == "amount" || p.name == "depth" ? 0.7 : 0;
        f.args.push_back({p.name, p.name, p.what, "", lo, hi, def});
      } else if (p.kind == "tracks" || (p.kind == "track" && p.name != "track")) {
        f.options[p.name] = {};  // track names, checked against the song
      }
      f.tags.push_back(p.name);
    }
    out.push_back(std::move(f));
  }
}

const doc::SongToolInfo* songTool(const std::string& id) {
  for (const auto& t : doc::songToolCatalog())
    if (t.id == id) return &t;
  return nullptr;
}

// The gate a call asks for: its preset (or the plain gate), then its pattern, rate and knobs.
aifx::Settings gateOf(const Call& c) {
  aifx::Settings g;
  if (const aifx::Preset* pr = aifx::findPreset(text(c, "preset"))) g = aifx::fromPreset(*pr);
  if (!text(c, "pattern").empty()) aifx::parsePattern(text(c, "pattern"), g.steps);
  if (!text(c, "rate").empty()) g.rate = std::max(0, aifx::rateIndex(text(c, "rate")));
  g.depth = arg(c, "depth", g.depth);
  g.length = arg(c, "length", g.length);
  g.smoothMs = arg(c, "smooth", g.smoothMs);
  g.duck = arg(c, "duck", g.duck);
  g.mix = arg(c, "mix", g.mix);
  return g;
}

const std::pair<std::string, std::string>* Pending::newTrack(const std::string& name) const {
  for (const auto& t : newTracks)
    if (same(t.first, name)) return &t;
  return nullptr;
}

std::string Pending::section(const std::string& name) const {
  for (const auto& s : sections)
    if (same(s, name)) return s;
  return "";
}

Pending startPending(const doc::Document& song) {
  Pending p;
  for (const auto& s : song.sections) p.sections.push_back(s.name);
  return p;
}

bool validate(const json& c, const CoreFunction& f, const doc::Document& song, const doc::Track* track, Call& call,
              Pending& pending, std::vector<std::string>& problems) {
  if (call.trackId == "master") {
    problems.push_back(f.name + " does not go on the master: left out");
    return false;
  }
  if (!takeArgs(c, f, call, problems)) return false;
  // A section, by its name now or after an earlier "shape" of the same proposal.
  const std::string section = str(c, "section");
  if (!section.empty()) {
    std::string known = pending.section(section);
    if (known.empty())
      if (const doc::Section* s = sectionNamed(song, section)) known = pending.section(s->name);
    if (known.empty()) {
      problems.push_back("no section called \"" + section + "\": " + f.name + " left out " +
                         (pending.sections.empty() ? std::string("(the song has no sections yet: call shape first, then this again)")
                                                   : "(sections: " + listOf(pending.sections) + ")"));
      return false;
    }
    call.sectionName = known;
  }
  const bool newTrack = call.trackId.rfind("new:", 0) == 0;
  auto needsTrack = [&]() {
    if (!call.trackId.empty()) return true;
    problems.push_back(f.name + ": no track given (\"on\"): left out");
    return false;
  };
  if (f.family == "track") {
    // A build-up track is the riser part: the models ask for "build" (benches of 16/11), so the word is taken.
    json withPart = c;
    if (withPart.contains("part") && withPart["part"].is_string()) {
      std::string p = withPart["part"].get<std::string>();
      for (auto& ch : p) ch = (char)std::tolower((unsigned char)ch);
      if (p == "build" || p == "buildup" || p == "build-up" || p == "build up" || p == "rise") withPart["part"] = "riser";
    }
    if (!takeText(withPart, f, "part", true, call, problems)) return false;
    std::string name = str(c, "name");
    if (name.empty()) name = doc::scorePartName(call.texts["part"]);
    std::string unique = name;
    for (int n = 2; pending.newTrack(unique) || std::any_of(song.tracks.begin(), song.tracks.end(), [&](const doc::Track& t) { return same(t.name, unique); }); ++n)
      unique = name + " " + std::to_string(n);
    if (unique != name) call.warnings.push_back("a track called " + name + " is there already: the new one is " + unique);
    call.texts["name"] = unique;
    call.trackId = "song";
    call.trackName = unique;
    pending.newTracks.push_back({unique, call.texts["part"]});
    return true;
  }
  if (f.family == "shape") {
    if (!takeText(c, f, "shape", true, call, problems)) return false;
    const doc::ScoreShape* s = doc::findScoreShape(call.texts["shape"]);
    pending.sections.clear();
    if (s)
      for (const auto& st : s->stages) pending.sections.push_back(st.name);
    call.trackId = "song";
    call.trackName = "the song";
    return true;
  }
  if (f.family == "arrange") {
    if (call.trackId.empty()) {
      call.trackId = "song";
      call.trackName = "every track";
    }
    if (track && !doc::playsPatterns(*track)) {
      problems.push_back("Arrange: " + track->name + " plays no notes: left out");
      return false;
    }
    return true;
  }
  if (f.family == "plays") {
    if (!needsTrack()) return false;
    if (call.sectionName.empty()) {
      problems.push_back("Plays: no \"section\" given: left out");
      return false;
    }
    if (!takeText(c, f, "play", true, call, problems)) return false;
    if (track && !doc::playsPatterns(*track)) {
      problems.push_back("Plays: " + track->name + " plays no notes: left out");
      return false;
    }
    return true;
  }
  if (f.family == "pcore") {
    if (!needsTrack()) return false;
    if (track && !doc::playsPatterns(*track)) {
      problems.push_back("P-Core: " + track->name + " plays no notes (an audio track or a bus): left out");
      return false;
    }
    // A style's id as "align" (models write "darkpsycore"): align as that style does.
    json pc = c;
    if (const auto st = f.options.find("style"); pc.contains("align") && pc["align"].is_string() && st != f.options.end()) {
      const std::string a = pc["align"].get<std::string>();
      const auto al = f.options.find("align");
      const bool isAlign = al != f.options.end() && std::find(al->second.begin(), al->second.end(), a) != al->second.end();
      if (!isAlign && std::find(st->second.begin(), st->second.end(), a) != st->second.end()) {
        if (!pc.contains("style")) pc["style"] = a;
        if (pc["style"] == a) pc["align"] = "style";
      }
    }
    for (const char* k : {"part", "shape", "style", "scale", "root", "grid", "mode", "align"})
      if (!takeText(pc, f, k, false, call, problems)) return false;
    if (!call.texts.count("part")) {
      std::string part = track ? doc::pcoreGuessPart(*track) : std::string();
      if (part.empty() && newTrack)
        if (const auto* nt = pending.newTrack(call.trackName)) part = nt->second == "openhats" ? "cymbals" : nt->second == "riser" || nt->second == "impact" ? "fx" : nt->second;
      const auto opt = f.options.find("part");
      if (!part.empty() && opt != f.options.end() && std::find(opt->second.begin(), opt->second.end(), part) == opt->second.end()) part.clear();
      if (part.empty()) {
        problems.push_back("P-Core: which part " + call.trackName + " plays is not clear (\"part\"): left out");
        return false;
      }
      call.texts["part"] = part;
    }
    static const std::set<int> every = {0, 1, 2, 4, 8, 16};
    if (!every.count((int)arg(call, "variationEvery", 0))) {
      problems.push_back("P-Core variationEvery: 0, 1, 2, 4, 8 or 16: left out");
      return false;
    }
    return true;
  }
  if (f.family == "gate") {
    if (!needsTrack()) return false;
    if (call.sectionName.empty() && !call.zoneSection) {
      problems.push_back("Gate: no \"section\" given (where it chops): left out");
      return false;
    }
    for (const char* k : {"preset", "rate", "pattern"})
      if (!takeText(c, f, k, false, call, problems)) return false;
    std::array<int, aifx::kSteps> steps{};
    if (!text(call, "pattern").empty() && !aifx::parsePattern(text(call, "pattern"), steps)) {
      problems.push_back("Gate pattern \"" + text(call, "pattern") + "\": 1, 2, 4, 8 or 16 of x (on), h (half), . (off): left out");
      return false;
    }
    if (text(call, "preset").empty() && text(call, "pattern").empty()) {
      problems.push_back("Gate: give a \"preset\" or a \"pattern\": left out");
      return false;
    }
    const aifx::Settings g = gateOf(call);
    if (g.depth <= 0 && g.duck <= 0) {
      problems.push_back("Gate: depth 0 and no duck does nothing: left out");
      return false;
    }
    return true;
  }
  if (f.family == "songtool") {
    const doc::SongToolInfo* t = songTool(f.id);
    if (!t) return false;
    // A track of the song, or one an earlier "track" call of this proposal makes.
    auto known = [&](const std::string& name) {
      return pending.newTrack(name) || std::any_of(song.tracks.begin(), song.tracks.end(), [&](const doc::Track& tr) { return same(tr.name, name) || tr.id == name; });
    };
    for (const auto& p : t->params) {
      if (p.kind == "track" && p.name == "track") {
        if (!needsTrack()) return false;
      } else if (p.kind == "section") {
        const bool required = f.id == "roll" || f.id == "suck" || f.id == "buildup";
        if (required && call.sectionName.empty() && !call.zoneSection) {
          problems.push_back(f.name + ": no \"section\" given: left out");
          return false;
        }
      } else if (p.kind == "choice") {
        if (!takeText(c, f, p.name, p.name == "kind" || p.name == "intent", call, problems)) return false;
      } else if (p.kind == "track" || p.kind == "tracks") {
        json v = textOf(c, p.name);
        std::vector<std::string> names;
        if (v.is_string()) {
          const std::string all = v.get<std::string>();
          for (size_t a = 0; a <= all.size();) {
            size_t comma = std::min(all.find(',', a), all.size());
            std::string one = all.substr(a, comma - a);
            while (!one.empty() && one.front() == ' ') one.erase(0, 1);
            while (!one.empty() && one.back() == ' ') one.pop_back();
            if (!one.empty()) names.push_back(one);
            a = comma + 1;
          }
        } else if (v.is_array()) {
          for (const auto& n : v) if (n.is_string()) names.push_back(n.get<std::string>());
        }
        if (p.kind == "tracks" && !call.trackId.empty() && call.trackId != "song") names.insert(names.begin(), call.trackName);
        if (names.empty()) {
          problems.push_back(f.name + ": \"" + p.name + "\" is needed (" + p.what + "): left out");
          return false;
        }
        std::string joined;
        for (const auto& n : names) {
          if (!known(n)) {
            problems.push_back(f.name + ": no track called \"" + n + "\": left out");
            return false;
          }
          joined += (joined.empty() ? "" : ",") + n;
        }
        call.texts[p.name] = joined;
        if (p.kind == "tracks" && call.trackId.empty()) {
          call.trackId = "song";
          call.trackName = joined;
        }
      }
    }
    return true;
  }
  problems.push_back(f.name + " is not a song function: left out");
  return false;
}

std::string resolveTrack(const std::string& trackId, const std::map<std::string, std::string>& made) {
  if (trackId.rfind("new:", 0) != 0) return trackId;
  const auto it = made.find(trackId.substr(4));
  if (it == made.end()) throw doc::DocumentError("calls", "the track " + trackId.substr(4) + " was not made");
  return it->second;
}

std::vector<std::string> apply(doc::Document& song, const Proposal& p, size_t& i, std::map<std::string, std::string>& made,
                               const ApplyEnv& env) {
  const Call& c = p.calls[i];
  const std::string family = c.fn->family;
  auto sectionId = [&](const std::string& name) {
    const doc::Section* s = sectionNamed(song, name);
    if (!s) throw doc::DocumentError("calls", "no section " + name);
    return s->id;
  };
  if (family == "track") {
    ++i;
    const std::string id = doc::addTrack(song, text(c, "part"), text(c, "name"));
    doc::trackById(song, id).extra["scorePart"] = text(c, "part");
    made[text(c, "name")] = id;
    return {"New track " + text(c, "name") + " (" + doc::scorePartName(text(c, "part")) + "): choose its plugin"};
  }
  if (family == "shape") {
    ++i;
    doc::setScoreShape(song, text(c, "shape"));
    return {describe(c)};
  }
  if (family == "arrange") {
    ++i;
    std::vector<std::string> ids;
    if (c.trackId == "song") {
      for (const auto& t : song.tracks)
        if (doc::playsPatterns(t) && !t.patterns.empty()) ids.push_back(t.id);
    } else {
      ids.push_back(resolveTrack(c.trackId, made));
    }
    std::vector<std::string> sections;
    if (!c.sectionName.empty()) sections.push_back(sectionId(c.sectionName));
    const int n = ids.empty() ? 0 : doc::scoreArrange(song, ids, sections);
    return {describe(c) + " (" + std::to_string(n) + " clips)"};
  }
  if (family == "plays") {
    ++i;
    doc::scoreSetPlays(song, resolveTrack(c.trackId, made), sectionId(c.sectionName), text(c, "play") == "on");
    return {describe(c)};
  }
  if (family == "pcore") {
    if (!env.pcore) throw doc::DocumentError("calls", "the P-Core catalogue (pcore/styles.json) is not loaded");
    const std::string key = groupKey(c);
    std::vector<doc::PCoreTrackChoice> tracks;
    std::vector<const Call*> group;
    while (i < p.calls.size() && p.calls[i].fn->family == "pcore" && groupKey(p.calls[i]) == key) {
      const Call& g = p.calls[i++];
      tracks.push_back({resolveTrack(g.trackId, made), text(g, "part"), text(g, "shape")});
      group.push_back(&g);
    }
    const doc::PCoreLayers layers = layersOf(song, c, *env.pcore);
    int64_t at = 0;
    if (!c.sectionName.empty())
      if (const doc::Section* s = sectionNamed(song, c.sectionName)) at = s->startTick;
    const int bars = (int)arg(c, "bars", 4);
    std::map<std::string, std::string> patterns;
    if (c.endTick > c.startTick) {  // the selection (calls.hpp Zone): each track gets its own clip and pattern there
      at = c.startTick;
      for (const auto& t : tracks) {
        doc::Track& track = doc::trackById(song, t.trackId);
        if (doc::playsPatterns(track)) patterns[t.trackId] = doc::zonePattern(track, c.startTick, c.endTick);
      }
    }
    const auto results = doc::pcoreApplyToSong(song, *env.pcore, layers, tracks, at, 0, bars, patterns);
    std::vector<std::string> lines;
    for (size_t k = 0; k < group.size(); ++k) {
      std::string line = describe(*group[k]);
      for (const auto& r : results)
        if (r.trackId == tracks[k].trackId) line += " (" + r.shape + ", " + std::to_string(r.notes) + " notes)";
      lines.push_back(line);
    }
    return lines;
  }
  if (family == "gate") {
    ++i;
    if (!env.roles) throw doc::DocumentError("calls", "the role map is not loaded (Settings > Roles)");
    int64_t a = c.startTick, b = c.endTick;
    if (b <= a) {
      const doc::Section* s = sectionNamed(song, c.sectionName);
      if (!s) throw doc::DocumentError("calls", "no section " + c.sectionName);
      a = s->startTick;
      b = std::min(s->startTick + s->lengthTicks, song.lengthTicks);
    }
    const auto r = doc::toolGate(song, resolveTrack(c.trackId, made), *env.roles, a, b, gateOf(c), text(c, "preset"));
    return {describe(c) + ": " + r.sentence};
  }
  if (family == "songtool") {
    ++i;
    if (!env.roles) throw doc::DocumentError("calls", "the role map is not loaded (Settings > Roles)");
    const doc::RoleMap& roles = *env.roles;
    // The span: the selection, else the section, else the whole song.
    int64_t a = c.startTick, b = c.endTick;
    if (b <= a) {
      const doc::SongSpan span = doc::sectionSpan(song, c.sectionName);
      a = span.start;
      b = span.end;
    }
    auto idOf = [&](const std::string& name) {
      for (const auto& t : song.tracks)
        if (same(t.name, name) || t.id == name) return t.id;
      const auto it = made.find(name);
      if (it != made.end()) return it->second;
      throw doc::DocumentError("calls", "no track " + name);
    };
    const std::string id = c.fn->id;
    const double amount = arg(c, "amount", 0.7);
    std::string sentence;
    if (id == "space") {
      std::vector<std::string> ids;
      const std::string all = text(c, "tracks");
      for (size_t at = 0; at <= all.size();) {
        const size_t comma = std::min(all.find(',', at), all.size());
        if (comma > at) ids.push_back(idOf(all.substr(at, comma - at)));
        at = comma + 1;
      }
      sentence = doc::toolSharedBus(song, ids, roles, text(c, "kind"), text(c, "preset")).sentence;
    } else {
      const std::string track = resolveTrack(c.trackId, made);
      if (id == "fx") sentence = doc::toolIntentFx(song, track, roles, text(c, "intent"), a, b, amount).sentence;
      else if (id == "roll")
        sentence = doc::toolRoll(song, track, a, b, text(c, "style").empty() ? "steps" : text(c, "style"), -1, (int)arg(c, "rise", 0),
                                 (int)arg(c, "gap", 0)).sentence;
      else if (id == "suck") sentence = doc::toolSuck(song, track, roles, b, b - a, amount).sentence;
      else if (id == "pump") sentence = doc::toolPump(song, track, idOf(text(c, "from")), roles, a, b, arg(c, "depth", 0.7)).sentence;
      else if (id == "buildup") {
        doc::BuildupFx fx;
        fx.amount = amount;
        sentence = doc::toolBuildupFx(song, track, roles, a, b, fx).sentence;
      } else throw doc::DocumentError("calls", "Core cannot apply " + c.fn->name + " yet");
    }
    return {describe(c) + ": " + sentence};
  }
  throw doc::DocumentError("calls", "Core cannot apply " + c.fn->name + " yet");
}

std::string describe(const Call& c) {
  const std::string family = c.fn->family;
  const std::string where = c.trackName.empty() ? std::string("the track you choose") : c.trackName;
  std::string out;
  if (family == "track") return "New track " + text(c, "name") + " (" + doc::scorePartName(text(c, "part")) + ")";
  if (family == "shape") {
    const doc::ScoreShape* s = doc::findScoreShape(text(c, "shape"));
    return std::string("Song shape: ") + (s ? s->name : text(c, "shape").c_str());
  }
  if (family == "arrange") out = "Arrange " + where;
  else if (family == "plays") out = where + (text(c, "play") == "on" ? " plays in " : " rests in ") + c.sectionName;
  else if (family == "songtool") {
    out = c.fn->name + " on " + (c.fn->id == "space" ? text(c, "tracks") : where);
    for (const char* k : {"kind", "preset", "intent", "style"})
      if (!text(c, k).empty()) out += " · " + text(c, k);
    if (!text(c, "from").empty()) out += " · from " + text(c, "from");
    for (const auto& [k, v] : c.args) out += " · " + k + " " + num(v);
  }
  else if (family == "gate") {
    out = "Gate " + (text(c, "preset").empty() ? std::string() : text(c, "preset") + " ") +
          (text(c, "pattern").empty() ? std::string() : "[" + text(c, "pattern") + "] ") + (text(c, "rate").empty() ? "" : text(c, "rate") + " ") + "on " + where;
    for (const auto& [k, v] : c.args) out += " · " + k + " " + num(v);
  }
  else if (family == "pcore") {
    out = "P-Core " + text(c, "part") + (text(c, "shape").empty() ? std::string() : " · " + text(c, "shape")) + " on " + where;
    for (const char* k : {"style", "scale", "root", "grid", "mode"})
      if (!text(c, k).empty()) out += " · " + std::string(k) + " " + text(c, k);
    if (!c.args.empty()) {
      out += " (";
      bool first = true;
      for (const auto& [k, v] : c.args) {
        out += (first ? "" : ", ") + k + " " + num(v);
        first = false;
      }
      out += ")";
    }
  } else out = c.fn->name + " on " + where;
  if (!c.sectionName.empty() && family != "plays") out += ", in " + c.sectionName;
  return out;
}

}  // namespace core::control::songcalls
