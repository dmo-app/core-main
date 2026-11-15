#include "calls.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>
#include <sstream>
#include <string_view>
#include "core_plugins.hpp"
#include "edit.hpp"
#include "fcore.hpp"
#include "rcore.hpp"
#include "stutter.hpp"
#include "wcore.hpp"

namespace core::control {

namespace {

// Text folded for matching: lower case, Portuguese accents removed (UTF-8 Latin-1 letters), everything else that
// is not a letter or a digit a space.
std::string foldText(const std::string& s) {
  static const char* latin1 =
      "aaaaaaaceeeeiiii"     // C3 80..8F
      "dnooooo\0ouuuuy\0s"  // C3 90..9F
      "aaaaaaaceeeeiiii"     // C3 A0..AF
      "dnooooo\0ouuuuy\0y";  // C3 B0..BF
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) {
      out += std::isalnum(c) ? (char)std::tolower(c) : ' ';
    } else if (c == 0xC3 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0xBF) {
      const char f = latin1[(unsigned char)s[i + 1] - 0x80];
      out += f ? f : ' ';
      ++i;
    } else {
      out += ' ';
      while (i + 1 < s.size() && ((unsigned char)s[i + 1] & 0xC0) == 0x80) ++i;
    }
  }
  return out;
}

std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  std::istringstream in(s);
  for (std::string w; in >> w;) out.push_back(w);
  return out;
}

std::vector<std::string> wordsOf(const std::string& text) { return split(foldText(text)); }

std::string slugOf(const std::string& s) {
  std::string out;
  for (const auto& w : wordsOf(s)) out += (out.empty() ? "" : "-") + w;
  return out;
}

std::string lowerCopy(std::string s) {
  for (auto& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

bool sameName(const std::string& a, const std::string& b) { return lowerCopy(a) == lowerCopy(b) || slugOf(a) == slugOf(b); }

bool hasWord(const std::vector<std::string>& words, std::initializer_list<const char*> any) {
  for (const auto& w : words)
    for (const char* a : any)
      if (w == a) return true;
  return false;
}

// 0.25, 12, -3.5: at most 3 decimals, no trailing zeros.
std::string num(double v) {
  std::ostringstream o;
  o.setf(std::ios::fixed);
  o.precision(3);
  o << v;
  std::string t = o.str();
  if (t.find('.') != std::string::npos) {
    while (!t.empty() && t.back() == '0') t.pop_back();
    if (!t.empty() && t.back() == '.') t.pop_back();
  }
  return t == "-0" || t.empty() ? "0" : t;
}

// What the built-in effects are for (search words, Portuguese and English).
const char* builtinTags(const std::string& name) {
  static const std::map<std::string, std::string> t = {
      {"eq", "eq core eq equalizer tone low high cut shelf bell notch dynamic mid side boost clean equalizacao graves agudos"},
      {"filter", "filter sweep high pass low pass buildup riser transition filtro subida"},
      {"compressor", "compressor dynamics punch glue sidechain ducking compressao"},
      {"corefx", "core fx effects saturation distortion drive grit reverb space room hall tail delay echo repeats ping pong "
                 "phaser flanger chorus modulation lofi lo-fi bitcrush regions frequency distorcao saturacao reverberacao espaco eco"},
      {"limiter", "limiter loudness master ceiling clipping volume"},
      {"utility", "utility gain volume pan width mono stereo largura"}};
  const auto it = t.find(name);
  return it == t.end() ? "" : it->second.c_str();
}

// The words of a rack's role and what track types they suit.
const std::vector<std::pair<const char*, std::vector<const char*>>>& roleTypes() {
  static const std::vector<std::pair<const char*, std::vector<const char*>>> r = {
      {"kick", {"kick"}},
      {"bass", {"bass"}},
      {"hat", {"hat", "openhat"}},
      {"hats", {"hat", "openhat"}},
      {"perc", {"perc", "accent", "drums"}},
      {"accent", {"accent", "perc", "impact", "fx"}},
      {"accents", {"accent", "perc", "impact", "fx"}},
      {"impact", {"impact", "fx"}},
      {"impacts", {"impact", "fx"}},
      {"lead", {"lead"}},
      {"leads", {"lead"}},
      {"pad", {"pad"}},
      {"pads", {"pad"}},
      {"atmos", {"pad", "fx"}},
      {"drone", {"pad"}},
      {"drones", {"pad"}},
      {"texture", {"lead", "pad", "fx"}},
      {"voice", {"vox"}},
      {"voices", {"vox"}},
      {"fx", {"fx", "riser", "impact", "perc", "accent"}},
      {"fills", {"fx", "perc", "accent"}}};
  return r;
}

}  // namespace

namespace {

// ---- Core's own plugins as functions: one entry each (the common plug, 2026-11-06)
//
// Each entry says how the plugin shows itself to AICORE (describe), what a call of it needs from the track, and how
// it is applied. Its id, name and layer come from doc::corePlugins(), so a new Core plugin is one entry there and one
// here, never a new branch in validateProposal or applyProposal. The order is the order of the function list.
struct PluginCalls {
  std::string id;                      // doc::corePlugins() id = the function id
  std::function<std::optional<CoreFunction>()> describe;
  bool needsNotes = false;             // the track must play notes (an instrument, STUTTER)
  bool needsClips = false;             // the track must have audio clips (VOICE STUTTER)
  const char* presetNeeded = nullptr;  // "the recipe", "the place": a call without a preset is left out
  bool quietWithoutSection = false;    // rises, but no section is fine (no "nothing rises" warning)
  // Extra checks after the generic ones; false = left out (the reason is in problems).
  std::function<bool(const CoreFunction&, const doc::Track*, Call&, std::vector<std::string>& problems)> check;
  // Makes the call on a track (never the master); returns the line shown after Apply.
  std::function<std::string(doc::Document&, const Call&, const std::string& trackId)> apply;
};

CoreFunction pluginFunction(const std::string& id, const char* family) {
  CoreFunction f;
  f.id = id;
  f.family = family;
  const doc::CorePluginInfo* info = doc::findCorePlugin(id);
  f.name = info ? doc::corePluginCallName(*info) : id;
  return f;
}

// A Core synth: an instrument with layers, presets and moods.
std::optional<CoreFunction> describeSynth(const std::string& id, const char* instrument, const std::vector<doc::SoundLayer>& layers,
                                          bool shortHints) {
  const doc::InstrumentInfo* info = doc::findInstrumentInfo(instrument);
  if (!info) return std::nullopt;
  CoreFunction f = pluginFunction(id, "instrument");
  for (const auto& prm : info->params) {
    // W-Core's waves 2..4 and the cards' lights are the window's: the function sets wave 1 (the state stays small).
    if (doc::isWaveLayerParam(prm.name) || std::string(prm.name).rfind("sec_", 0) == 0) continue;
    std::string hint;
    for (const auto& l : layers)
      for (const auto& k : l.knobs)
        if (prm.name == std::string(k.param)) {
          if (!shortHints) {
            hint = std::string(l.name) + ": " + k.info;
          } else {  // its first sentence: the state stays small
            const std::string text = k.info;
            hint = text.substr(0, std::min(text.size(), text.find(". ") + 1));
            if (hint.size() > 72) hint = hint.substr(0, hint.rfind(' ', 70)) + "...";
          }
        }
    f.args.push_back({prm.name, prm.label, hint, prm.unit, prm.min, prm.max, prm.def});
  }
  return f;
}

bool synthCheck(const char* instrument, const CoreFunction& f, const doc::Track* track, Call& call) {
  if (!track) return true;
  const bool already = track->instrument.kind == "builtin" && track->instrument.name == instrument;
  if (!already) call.warnings.push_back(f.name + " replaces the instrument of " + track->name + " (its notes stay)");
  return true;
}

const std::vector<PluginCalls>& pluginCalls() {
  static const std::vector<PluginCalls> list = [] {
    std::vector<PluginCalls> v;
    {
      PluginCalls w;
      w.id = "wcore";
      w.needsNotes = true;
      w.describe = [] {
        auto f = describeSynth("wcore", doc::kWaveInstrument, doc::waveLayers(), true);
        if (!f) return f;
        f->role = "lead";
        f->description = "Core's own synth (bass, lead, pad, drone): turns a note track into W-Core (notes kept) or changes the W-Core already "
                         "there; \"preset\": a whole sound, \"args\": its knobs on top, \"mood\": a mood on top (\"dry\" takes it away)";
        f->tags = {"bass", "baixo", "lead", "pad", "drone", "synth", "pluck", "fm", "noise", "w-core", "wave", "melodia"};
        for (const auto& pr : doc::wavePresets()) f->presets.push_back(pr.id);
        for (const auto& m : doc::waveMoods()) f->options["mood"].push_back(m.id);
        for (const auto& pr : doc::wavePresets()) {  // its first sentence too
          const std::string what = pr.info;
          f->extra["presetInfo"][pr.id] = {{"name", pr.name}, {"style", pr.style}, {"what", what.substr(0, std::min(what.size(), what.find(". ") + 1))}};
        }
        return f;
      };
      w.check = [](const CoreFunction& f, const doc::Track* t, Call& call, std::vector<std::string>&) { return synthCheck(doc::kWaveInstrument, f, t, call); };
      w.apply = [](doc::Document& song, const Call& c, const std::string& where) {
        doc::setWave(song, where, c.preset);  // keeps W-Core when it is there; the preset's values, if any
        if (c.texts.count("mood")) doc::setWaveMood(song, {where}, c.texts.at("mood"));
        for (const auto& [k, val] : c.args) doc::setInstrumentParam(song, where, k, val);  // the knobs given win over preset and mood
        return describeCall(c);
      };
      v.push_back(std::move(w));
    }
    {
      PluginCalls st;
      st.id = "stutter";
      st.needsNotes = true;
      st.presetNeeded = "the recipe";
      st.quietWithoutSection = true;
      st.describe = [] {
        CoreFunction f = pluginFunction("stutter", "stutter");
        f.description = "the track's notes become repeats inside each note's length (ordinary notes, editable); \"preset\": the recipe "
                        "(required); \"section\": only the notes that start there (else the whole song)";
        f.tags = {"stutter", "repeat", "repeats", "roll", "gate", "glitch", "repeticoes", "repetições"};
        f.rises = true;  // "section" limits it to a part of the song
        for (const auto& pr : doc::stutterPresets()) {
          f.presets.push_back(pr.id);
          f.extra["presetInfo"][pr.id] = {{"name", pr.name}, {"style", pr.style}, {"what", pr.info}};
        }
        return std::optional<CoreFunction>(std::move(f));
      };
      st.apply = [](doc::Document& song, const Call& c, const std::string& where) {
        const doc::StutterPreset* pr = doc::findStutterPreset(c.preset);
        if (!pr) throw doc::DocumentError("calls", c.fn->name + " has no recipe " + c.preset);
        const int n = doc::stutterTrack(song, where, c.startTick, c.endTick, *pr);
        return describeCall(c) + (n ? "" : " (no note long enough to repeat)");
      };
      v.push_back(std::move(st));
    }
    {
      PluginCalls vs;
      vs.id = "voicestutter";
      vs.needsClips = true;
      vs.presetNeeded = "the recipe";
      vs.quietWithoutSection = true;
      vs.describe = [] {
        CoreFunction f = pluginFunction("voicestutter", "voicestutter");
        f.description = "each audio clip of the track (a recorded word) becomes fragment clips at the recipe's rhythm, then the "
                        "whole word (ordinary clips, editable); the fragment is the clip's first \"fragmentMs\"; \"preset\": "
                        "the recipe (required); \"section\": only the clips that start there";
        f.tags = {"voice", "vocal", "word", "stutter", "fragment", "voz", "palavra"};
        f.rises = true;  // "section" limits it to a part of the song
        f.args.push_back({"fragmentMs", "Fragment", "How much of the start of the word repeats, in ms (mark it exactly in the clip editor).",
                          "ms", 10, 1000, 120});
        for (const auto& pr : doc::voiceStutterPresets()) {
          f.presets.push_back(pr.id);
          f.extra["presetInfo"][pr.id] = {{"name", pr.name}, {"style", pr.style}, {"what", pr.info}};
        }
        return std::optional<CoreFunction>(std::move(f));
      };
      vs.apply = [](doc::Document& song, const Call& c, const std::string& where) {
        const doc::VoiceStutterPreset* pr = doc::findVoiceStutterPreset(c.preset);
        if (!pr) throw doc::DocumentError("calls", c.fn->name + " has no recipe " + c.preset);
        const double fragment = (c.args.count("fragmentMs") ? c.args.at("fragmentMs") : 120) / 1000.0;
        std::vector<std::pair<std::string, double>> clips;  // chosen first: the recipe moves the words it makes
        for (const auto& x : doc::trackById(song, where).audioClips)
          if (c.endTick <= c.startTick || (x.startTick >= c.startTick && x.startTick < c.endTick)) clips.push_back({x.id, x.offsetSeconds});
        for (const auto& [id, offset] : clips) doc::voiceStutter(song, where, id, offset, fragment, *pr);
        return describeCall(c) + (clips.empty() ? " (no audio clip there)" : "");
      };
      v.push_back(std::move(vs));
    }
    {
      PluginCalls fc;
      fc.id = "fcore";
      fc.quietWithoutSection = true;
      fc.describe = [] {
        CoreFunction f = pluginFunction("fcore", "fcore");
        f.description = "effects by intent (build-up, intro, impact...): puts F-Core on a track or changes the one there; \"args\": its knobs, "
                        "text \"intent\", \"style\", \"mode\" (note: moves with each sampler note; region: over \"section\"); "
                        "\"preset\": one of the intent's presets (which steps are on)";
        f.tags = {"fcore", "f-core", "buildup", "build-up", "intro", "impact", "downlifter", "atmosphere", "riser", "subida"};
        f.args = {{"amount", "Amount", "subtle .. extreme", "", 0, 1, 0.6}, {"length", "Length", "how long the movement lasts", "beats", 1, 256, 32},
                  {"tone", "Tone", "dark .. bright", "", 0, 1, 0.5}, {"space", "Space", "dry .. huge", "", 0, 1, 0.5}};
        for (const auto& i : doc::fcoreIntents()) {
          f.options["intent"].push_back(i.id);
          for (const auto& pr : doc::fcorePresetsOf(i.id)) {
            f.extra["presetsByIntent"][i.id].push_back(pr.id);
            if (std::find(f.presets.begin(), f.presets.end(), pr.id) == f.presets.end()) f.presets.push_back(pr.id);
          }
        }
        for (const auto& s : doc::fcoreStyles()) f.options["style"].push_back(s.id);
        f.options["mode"] = {"note", "region"};
        f.rises = true;  // "section": a region over it (on a track that is not a sampler)
        return std::optional<CoreFunction>(std::move(f));
      };
      fc.check = [](const CoreFunction& f, const doc::Track* track, Call& call, std::vector<std::string>& problems) {
        // The intent: the one given, else the one already on the track, else a build-up.
        const auto there = track ? doc::fcoreOf(*track) : std::nullopt;
        const std::string intent = call.texts.count("intent") ? call.texts["intent"] : there ? there->intent : std::string("buildup");
        if (!call.preset.empty()) {
          const auto& list = doc::fcorePresetsOf(intent);
          if (std::none_of(list.begin(), list.end(), [&](const doc::FCorePreset& x) { return x.id == call.preset; })) {
            problems.push_back(f.name + ": the " + intent + " intent has no preset \"" + call.preset + "\": left out");
            return false;
          }
        }
        if (there) call.warnings.push_back(f.name + " on " + track->name + " is changed (it is there already)");
        if (there && call.texts.count("mode") && call.texts["mode"] != there->mode)
          call.warnings.push_back(f.name + " keeps its mode (" + there->mode + ") on " + track->name + ": remove it to change the mode");
        return true;
      };
      fc.apply = [](doc::Document& song, const Call& c, const std::string& where) {
        const auto there = doc::fcoreOf(doc::trackById(song, where));
        auto knobsFrom = [&](doc::FCoreKnobs k) {
          if (c.args.count("amount")) k.amount = c.args.at("amount");
          if (c.args.count("length")) k.lengthBeats = c.args.at("length");
          if (c.args.count("tone")) k.tone = c.args.at("tone");
          if (c.args.count("space")) k.space = c.args.at("space");
          return k;
        };
        if (there) {
          if (c.texts.count("intent") && c.texts.at("intent") != there->intent) doc::setFCoreIntent(song, where, c.texts.at("intent"));
          if (c.texts.count("style") && c.texts.at("style") != there->style) doc::setFCoreStyle(song, where, c.texts.at("style"));
          if (!c.args.empty()) doc::setFCoreKnobs(song, where, knobsFrom(doc::fcoreOf(doc::trackById(song, where))->knobs));
          if (c.endTick > c.startTick && there->mode == "region") doc::addFCoreRegion(song, where, c.startTick, c.endTick);
        } else {
          const std::string intent = c.texts.count("intent") ? c.texts.at("intent") : std::string("buildup");
          std::string style = c.texts.count("style") ? c.texts.at("style") : std::string();
          if (style.empty()) {
            const std::string k = songStyleKey(song);  // the song's style, as F-Core names it
            style = k == "psytrance" ? std::string("fullon") : doc::findFCoreStyle(k) ? k : std::string("darkpsycore");
          }
          doc::FCoreKnobs k;
          if (const doc::FCoreIntent* in = doc::findFCoreIntent(intent)) k.lengthBeats = in->defaultBeats;
          std::vector<doc::FCoreRegion> regions;
          if (c.endTick > c.startTick) regions.push_back({c.startTick, c.endTick});
          doc::applyFCoreToTracks(song, {where}, intent, style, knobsFrom(k), c.texts.count("mode") ? c.texts.at("mode") : std::string("note"), regions);
        }
        if (!c.preset.empty()) doc::setFCorePreset(song, where, c.preset);
        return describeCall(c);
      };
      v.push_back(std::move(fc));
    }
    for (int d = 0; d < 2; ++d) {
      PluginCalls g;
      g.id = d ? "dcore" : "rcore";
      g.presetNeeded = "the place";
      g.describe = [d] {
        CoreFunction f = pluginFunction(d ? "dcore" : "rcore", "gesture");
        f.description = d ? "the track sends to one shared echo on a bus (\"preset\": the echo, required), which follows the sections"
                          : "the track sends to one shared reverb on a bus (\"preset\": the place, required), which follows the sections";
        f.tags = d ? std::vector<std::string>{"delay", "echo", "eco", "d-core", "send", "bus"} : std::vector<std::string>{"reverb", "space", "espaco", "r-core", "send", "bus"};
        for (const auto& pr : d ? doc::dcorePresets() : doc::rcorePresets()) {
          f.presets.push_back(pr.name);
          f.extra["presetInfo"][pr.name] = {{"style", pr.style}, {"what", pr.tip}};
        }
        return std::optional<CoreFunction>(std::move(f));
      };
      g.apply = [d](doc::Document& song, const Call& c, const std::string& where) {
        const doc::RCorePreset* pr = nullptr;
        for (const auto& x : d ? doc::dcorePresets() : doc::rcorePresets()) if (c.preset == x.name) pr = &x;
        if (!pr) throw doc::DocumentError("calls", c.fn->name + " has no preset " + c.preset);
        doc::applyRCore(song, {where}, *pr);
        return describeCall(c);
      };
      v.push_back(std::move(g));
    }
    return v;
  }();
  return list;
}

const PluginCalls* pluginCallsOf(const CoreFunction& f) {
  for (const auto& pc : pluginCalls())
    if (pc.id == f.id) return &pc;
  return nullptr;
}

}  // namespace

const FunctionArg* CoreFunction::findArg(const std::string& id) const {
  for (const auto& a : args)
    if (a.id == id) return &a;
  return nullptr;
}

std::vector<CoreFunction> buildFunctions(const std::vector<doc::RackEntry>& racks) {
  std::vector<CoreFunction> out;
  for (const auto& fx : doc::builtinEffects()) {
    CoreFunction f;
    f.id = std::string("fx:") + fx.name;
    f.family = "effect";
    f.name = fx.label;
    f.version = fx.version;
    f.description = std::string("Core's built-in ") + fx.label + ", at the end of the chain (a fixed setting: the whole track)";
    f.tags = split(builtinTags(fx.name));
    for (const auto& p : fx.params) f.args.push_back({p.name, p.label, "", p.unit, p.min, p.max, p.def});
    for (const auto& pr : doc::effectPresets())
      if (pr.effect == std::string(fx.name)) f.presets.push_back(pr.name);
    if (!f.presets.empty()) f.description += "; \"preset\": one of its presets, then \"args\" change knobs on top";
    out.push_back(std::move(f));
  }
  for (const auto& e : racks) {
    const doc::RackDef& r = e.def;
    CoreFunction f;
    f.id = "rack:" + r.id;
    f.family = "rack";
    f.name = r.name;
    f.role = r.role;
    f.description = r.description;
    f.version = r.version;
    f.styles = r.styles;
    f.tags = r.tags;
    f.instrument = r.kind == "instrument";
    // Core's own devices with their presets (no macros); an F-Core build-up finds the song's build-ups itself, so
    // no section is asked for.
    f.native = true;
    if (!doc::nativeRackReady(r)) {
      f.available = false;
      f.unavailable = "needs a Core plugin not in this Core yet";
    }
    f.rack = r;
    out.push_back(std::move(f));
  }
  CoreFunction s;
  s.id = "sample";
  s.family = "sample";
  s.name = "Sample";
  s.description = "puts a library sample on a track (Core Sampler; its notes, mix and effects stay); give \"sample\": its s: id";
  s.tags = {"sample", "samples", "amostra", "kick", "bombo", "bass", "baixo", "hat", "clap", "snare", "perc", "impact", "riser", "one-shot", "loop"};
  out.push_back(std::move(s));
  // Core's own plugins (2026-11-06): one entry each in pluginCalls(), named and placed by doc::corePlugins().
  for (const auto& pc : pluginCalls())
    if (auto f = pc.describe()) out.push_back(std::move(*f));
  CoreFunction set;
  set.id = "set";
  set.family = "setting";
  set.name = "Set an effect";
  set.description = "changes an effect already on a track or the master (mastering): \"effect\": its id (\"fx2\") or its name "
                    "(\"space\", the first one); \"preset\": one of that effect's presets; \"args\": its knobs (the ranges of fx:<name>)";
  set.tags = {"settings", "preset", "knob", "master", "mastering", "definições", "masterização"};
  out.push_back(std::move(set));
  CoreFunction mix;
  mix.id = "mix";
  mix.family = "mix";
  mix.name = "Mix";
  mix.description = "a track's volume and pan, or the master's volume";
  mix.tags = {"volume", "level", "pan", "master", "mix", "nível"};
  mix.args = {{"gainDb", "Volume", "", "dB", -60, 12, 0}, {"pan", "Pan", "-1 left, 1 right", "", -1, 1, 0}};
  out.push_back(std::move(mix));
  CoreFunction open;
  open.id = "open";
  open.family = "open";
  open.name = "Open";
  open.description = "after Apply, opens a window: \"what\": \"effects\" (the track's or master's chain, F-Core, R-Core and D-Core: the default), "
                     "\"instrument\" (W-Core on a synth track, \"wcore\" too, or a plugin's own window), \"patterns\" (P-Core on the track, \"pcore\" too), "
                     "\"mixer\", \"score\" (Stage Core, the song's parts), or an effect's id or name (a plugin effect's window). Changes nothing";
  open.tags = {"open", "window", "plugin", "abrir"};
  out.push_back(std::move(open));
  return out;
}

const CoreFunction* findFunction(const std::vector<CoreFunction>& functions, const std::string& id) {
  for (const auto& f : functions)
    if (f.id == id) return &f;
  // "buildup/roll" or "reverb" without the family: one match only.
  const CoreFunction* only = nullptr;
  for (const auto& f : functions) {
    const size_t colon = f.id.find(':');
    if (colon != std::string::npos && f.id.substr(colon + 1) == id) {
      if (only) return nullptr;
      only = &f;
    }
  }
  return only;
}

std::string styleKey(const std::string& text) {
  const std::string s = foldText(text);
  auto has = [&](const char* w) { return s.find(w) != std::string::npos; };
  if (has("slambient")) return "slambient";
  if (has("hi tech") || has("hitech")) return "hitech";
  if (has("psycore") || has("darkpsy") || has("dark psy")) return "darkpsycore";
  if (has("psytrance") || has("psy trance") || has("twilight") || has("full on") || has("fullon") || has("progressive")) return "psytrance";
  return "";
}

std::string songStyleKey(const doc::Document& song) { return styleKey(doc::songStyle(song)); }

std::string trackType(const doc::Track& t) {
  // The name first: a track's id often comes from the instrument it started with ("bass-2" named "Acid lead").
  const auto ws = wordsOf(t.name);
  if (hasWord(ws, {"lead", "acid", "synth", "pluck", "arp", "seq", "melody", "melodia", "stab"})) return "lead";
  if (hasWord(ws, {"pad", "atmo", "atmos", "drone", "ambient", "texture", "textura"})) return "pad";
  if (hasWord(ws, {"vox", "voice", "vocal", "voz"})) return "vox";
  if (hasWord(ws, {"fx", "sfx", "noise", "fill", "glitch", "zap"})) return "fx";
  const std::string sampleRole = doc::trackSampleRole(t);
  if (!sampleRole.empty()) return sampleRole;
  if (t.instrument.kind == "builtin") {
    if (t.instrument.name == "kick") return "kick";
    if (t.instrument.name == "hats") return "hat";
    if (t.instrument.name == "drums") return "drums";
  }
  return "";
}

bool roleFits(const std::string& functionRole, const std::string& type) {
  if (functionRole.empty() || type.empty()) return true;
  const std::string folded = foldText(functionRole);
  if (folded.find("build") != std::string::npos) return true;  // a build-up suits every track
  bool known = false;
  for (const auto& w : split(folded))
    for (const auto& [word, types] : roleTypes())
      if (w == word) {
        known = true;
        for (const char* x : types)
          if (type == x) return true;
      }
  return !known;
}

std::string sampleKey(const doc::LibrarySample& sample) { return "s:" + sample.sha256.substr(0, std::min<size_t>(10, sample.sha256.size())); }

const doc::LibrarySample* findSampleKey(const doc::SampleIndex& index, const std::string& key) {
  std::string k = key.rfind("s:", 0) == 0 ? key.substr(2) : key;
  if (k.size() < 6) return index.find(key);
  for (const auto& s : index.samples)
    if (s.sha256.rfind(k, 0) == 0) return &s;
  return index.find(key);
}

// Compact: what is empty, false or the usual value is left out (AICORE reads a missing field as that), so the
// state stays a few lines per function however many effects and racks Core has.
json functionsJson(const std::vector<CoreFunction>& functions) {
  json out = json::array();
  for (const auto& f : functions) {
    json args = json::array();
    for (const auto& a : f.args) {
      json arg = {{"id", a.id}, {"name", a.name}, {"min", a.min}, {"max", a.max}, {"default", a.def}};
      if (!a.hint.empty()) arg["hint"] = a.hint;
      if (!a.unit.empty()) arg["unit"] = a.unit;
      args.push_back(std::move(arg));
    }
    json fn = {{"id", f.id}, {"family", f.family}, {"name", f.name}, {"available", f.available}, {"args", std::move(args)}};
    if (!f.role.empty()) fn["role"] = f.role;
    if (!f.description.empty()) fn["description"] = f.description;
    if (f.version != 1) fn["version"] = f.version;
    if (!f.styles.empty()) fn["styles"] = f.styles;
    if (!f.tags.empty()) fn["tags"] = f.tags;
    if (f.instrument) fn["instrument"] = true;
    if (f.rises) fn["rises"] = true;
    if (f.native) fn["native"] = true;
    if (!f.presets.empty()) fn["presets"] = f.presets;
    if (!f.options.empty()) fn["options"] = f.options;
    if (f.extra.is_object()) for (auto it = f.extra.begin(); it != f.extra.end(); ++it) fn[it.key()] = it.value();
    if (!f.unavailable.empty()) fn["unavailable"] = f.unavailable;
    out.push_back(std::move(fn));
  }
  return out;
}

namespace {
// The Core effects of a chain as they are now, for "set" ({id, fn, args: every knob's value, ranges, on}); a rack's
// own effects and F-Core's are left out (their knobs belong to the rack or to F-Core: "set" would be overwritten).
// ranges: every knob's [min, max, default], so an AI moves a knob inside what it takes instead of guessing (audit
// 2026-11-05).
json chainJson(const json& effects) {
  json out = json::array();
  if (!effects.is_array()) return out;
  for (const auto& fx : effects) {
    if (!fx.is_object() || fx.value("type", std::string()) != "builtin" || fx.value("fcore", false)) continue;
    const doc::EffectInfo* info = doc::findEffect(doc::effectName(fx));
    if (!info) continue;
    json args = json::object();
    json ranges = json::object();
    for (const auto& prm : info->params) {
      args[prm.name] = doc::effectParam(fx, prm.name, prm.def);
      ranges[prm.name] = json::array({prm.min, prm.max, prm.def});
    }
    json e = {{"id", fx.value("id", std::string())}, {"fn", std::string("fx:") + info->name}, {"args", args}, {"ranges", ranges}};
    if (doc::effectBypassed(fx)) e["on"] = false;
    if (fx.contains(doc::kWaveMoodKey)) e["part"] = "wcore mood";
    out.push_back(std::move(e));
  }
  return out;
}
}  // namespace

json songJson(const doc::Document& song, const doc::SampleIndex* samples) {
  json sections = json::array();
  for (const auto& s : song.sections)
    sections.push_back({{"name", s.name}, {"startBar", (double)s.startTick / (4.0 * doc::kPpq)},
                        {"endBar", (double)(s.startTick + s.lengthTicks) / (4.0 * doc::kPpq)}});
  json tracks = json::array();
  for (const auto& t : song.tracks) {
    std::string sound;
    if (t.instrument.kind == "builtin" && t.instrument.name == "sampler" && !t.instrument.samples.empty())
      sound = "sampler: " + (samples ? doc::shownName(*samples, t.instrument.samples.front()) : t.instrument.samples.front().name);
    else sound = t.instrument.kind + (t.instrument.name.empty() ? "" : " " + t.instrument.name);
    json devices = json::array();
    for (const auto& fx : t.effects)
      if (fx.value("type", std::string()) == "builtin") devices.push_back("fx:" + doc::effectName(fx));
    const auto fcore = doc::fcoreOf(t);
    const bool wcore = t.instrument.kind == "builtin" && t.instrument.name == doc::kWaveInstrument;
    if (wcore) devices.push_back("wcore");
    if (fcore) devices.push_back("fcore");
    json tj = {{"id", t.id}, {"name", t.name}, {"type", trackType(t)}, {"sound", sound}, {"plays", doc::playsPatterns(t)}, {"devices", devices},
               {"mix", {{"gainDb", t.mix.gainDb}, {"pan", t.mix.pan}}}};
    const json chain = chainJson(t.effects);
    if (!chain.empty()) tj["chain"] = chain;
    if (!t.audioClips.empty()) tj["audioClips"] = (int)t.audioClips.size();  // recorded words VOICE STUTTER can cut
    if (wcore) {
      json args = json::object();
      // Waves 2..4 only when added, and a card's light only when it is off: the state stays small.
      if (const doc::InstrumentInfo* info = doc::findInstrumentInfo(doc::kWaveInstrument))
        for (const auto& prm : info->params) {
          const std::string name = prm.name;
          if (doc::isWaveLayerParam(name) && !doc::waveLayerExists(t, name[1] - '1')) continue;
          const auto it = t.instrument.params.find(prm.name);
          const double v = it == t.instrument.params.end() ? prm.def : it->second;
          if (name.find("sec_") != std::string::npos && v >= 0.5) continue;
          args[prm.name] = v;
        }
      tj["wcore"] = {{"args", args}};
      // Its movements (ENV 2/3, LFO 1..4, Velocity, Note, Macros -> knobs, pitch, volume, pan, detune), per wave.
      tj["wcore"]["synth"] = doc::waveSynth(t);
      const std::string mood = doc::waveMoodOf(t);
      if (!mood.empty()) tj["wcore"]["mood"] = mood;
    }
    if (fcore) {
      json regions = json::array();
      for (const auto& r : fcore->regions) regions.push_back({(double)r.startTick / (4.0 * doc::kPpq), (double)r.endTick / (4.0 * doc::kPpq)});
      tj["fcore"] = {{"intent", fcore->intent}, {"style", fcore->style}, {"mode", fcore->mode}, {"on", !fcore->bypass},
                     {"args", {{"amount", fcore->knobs.amount}, {"length", fcore->knobs.lengthBeats}, {"tone", fcore->knobs.tone}, {"space", fcore->knobs.space}}},
                     {"stepsOff", fcore->off}};
      if (!regions.empty()) tj["fcore"]["regionBars"] = regions;
    }
    tracks.push_back(std::move(tj));
  }
  json master = json::array();
  for (const auto& fx : song.masterEffects)
    if (fx.value("type", std::string()) == "builtin") master.push_back("fx:" + doc::effectName(fx));
  json out = {{"name", song.name}, {"style", songStyleKey(song)}, {"bpm", song.bpm}, {"bars", (double)song.lengthTicks / (4.0 * doc::kPpq)},
              {"sections", sections}, {"tracks", tracks}, {"master", master}, {"masterGainDb", song.masterGainDb}};
  const json masterChain = chainJson(song.masterEffects);
  if (!masterChain.empty()) out["masterChain"] = masterChain;
  return out;
}

json coreState(const std::vector<CoreFunction>& functions, const doc::Document& song, const doc::SampleIndex* samples) {
  json lib = nullptr;
  if (samples) {
    json roles = json::object();
    for (const auto& s : samples->samples) roles[s.info.role] = roles.value(s.info.role, 0) + 1;
    lib = {{"count", samples->samples.size()}, {"roles", roles}};
  }
  return {{"format", "core-state"}, {"version", kCallsFormatVersion}, {"functions", functionsJson(functions)},
          {"song", songJson(song, samples)}, {"samples", lib}};
}

// ---------------------------------------------------------------- proposals

std::vector<OpenRequest> opensOf(const Proposal& p, const std::string& chosenTrack) {
  std::vector<OpenRequest> out;
  for (const auto& c : p.calls)
    if (c.fn && c.fn->family == "open") out.push_back({c.trackId.empty() ? chosenTrack : c.trackId, c.open});
  return out;
}

bool Proposal::needsTrack() const {
  return std::any_of(calls.begin(), calls.end(), [](const Call& c) { return c.trackId.empty(); });
}

std::vector<std::string> Proposal::warnings() const {
  std::vector<std::string> out;
  for (const auto& c : calls) out.insert(out.end(), c.warnings.begin(), c.warnings.end());
  return out;
}

namespace {

std::string str(const json& o, const char* k) { return o.is_object() && o.contains(k) && o[k].is_string() ? o[k].get<std::string>() : std::string(); }


}  // namespace

Proposal validateProposal(const json& raw, const std::vector<CoreFunction>& functions, const doc::Document& song,
                          const doc::SampleIndex* samples) {
  Proposal p;
  p.raw = raw;
  if (!raw.is_object()) {
    p.problems.push_back("the proposal is not readable");
    return p;
  }
  p.id = str(raw, "id");
  p.summary = str(raw, "summary");
  json calls = raw.contains("calls") && raw["calls"].is_array() ? raw["calls"] : json::array();
  if (calls.empty()) {
    if (p.problems.empty()) p.problems.push_back("the proposal has no calls");
    return p;
  }
  const std::string style = songStyleKey(song);
  for (const auto& c : calls) {
    const std::string fnId = str(c, "fn");
    const CoreFunction* f = fnId.empty() ? nullptr : findFunction(functions, fnId);
    if (!f) {
      p.problems.push_back(fnId.empty() ? std::string("a call without \"fn\" was left out") : "\"" + fnId + "\" is not a function of Core: left out");
      continue;
    }
    if (!f->available) {
      p.problems.push_back(f->name + " cannot run here (" + f->unavailable + "): left out");
      continue;
    }
    Call call;
    call.fn = f;
    // Where.
    const doc::Track* track = nullptr;
    const std::string on = str(c, "on").empty() ? str(c, "target") : str(c, "on");
    if (!on.empty()) {
      if (lowerCopy(on) == "master") {
        if (f->family != "effect" && f->family != "setting" && f->family != "mix" && f->family != "open") {
          p.problems.push_back(f->name + " goes on a track, not the master: left out");
          continue;
        }
        call.trackId = "master";
        call.trackName = "Master";
      } else {
        for (const auto& t : song.tracks)
          if (t.id == on || sameName(t.name, on)) track = &t;
        if (track) {
          call.trackId = track->id;
          call.trackName = track->name;
        } else {
          call.warnings.push_back("no track called \"" + on + "\": choose one when applying");
        }
      }
    }
    // "set": the effect already there, its preset and knobs (checked against what that effect is).
    if (f->family == "setting") {
      if (call.trackId.empty()) {
        p.problems.push_back("Set an effect: no track or master given (\"on\"): left out");
        continue;
      }
      const std::string want = str(c, "effect");
      const json& chain = call.trackId == "master" ? song.masterEffects : track->effects;
      const json* found = nullptr;
      for (const auto& fx : chain)
        if (fx.is_object() && !want.empty() && !fx.value("fcore", false) &&
            (fx.value("id", std::string()) == want || sameName(doc::effectName(fx), want))) { found = &fx; break; }
      const doc::EffectInfo* info = found ? doc::findEffect(doc::effectName(*found)) : nullptr;
      if (!info) {
        p.problems.push_back(want.empty() ? std::string("Set an effect: no \"effect\" given: left out")
                                          : "no Core effect \"" + want + "\" on " + call.trackName + ": left out (add it with fx:<name>)");
        continue;
      }
      call.effectId = found->value("id", std::string());
      call.effectName = info->label;
      const std::string preset = str(c, "preset");
      if (!preset.empty()) {
        const doc::EffectPreset* pr = nullptr;
        for (const auto& x : doc::effectPresets())
          if (x.effect == std::string(info->name) && sameName(x.name, preset)) pr = &x;
        if (!pr) {
          p.problems.push_back(std::string(info->label) + ": no preset \"" + preset + "\": left out");
          continue;
        }
        call.preset = pr->name;
      }
      if (c.contains("args") && c["args"].is_object())
        for (auto kv = c["args"].begin(); kv != c["args"].end(); ++kv) {
          const doc::EffectParamInfo* a = nullptr;
          for (const auto& prm : info->params) if (kv.key() == prm.name) a = &prm;
          if (!a || !kv.value().is_number()) {
            p.problems.push_back(std::string(info->label) + ": no knob \"" + kv.key() + "\" (or not a number): left out");
            continue;
          }
          const double v = kv.value().get<double>();
          if (!std::isfinite(v) || v < a->min || v > a->max) {
            p.problems.push_back(std::string(info->label) + " " + kv.key() + " " + num(v) + " is outside " + num(a->min) + ".." + num(a->max) + ": left out");
            continue;
          }
          call.args[a->name] = v;
        }
      if (call.preset.empty() && call.args.empty()) {
        p.problems.push_back("Set " + std::string(info->label) + " on " + call.trackName + ": nothing to change: left out");
        continue;
      }
      p.calls.push_back(std::move(call));
      continue;
    }
    if (f->family == "open") {
      call.open = str(c, "what").empty() ? std::string("effects") : str(c, "what");
      // A Core plugin by any of its names opens the window that shows it (doc::corePlugins: "wcore" -> "instrument",
      // "stage" -> "score", "stutter" -> "patterns": Edit > STUTTER is in the piano roll).
      if (const doc::CorePluginInfo* plugin = doc::findCorePlugin(call.open); plugin && !plugin->opens.empty()) call.open = plugin->opens;
      if (call.open == "score" && call.trackId.empty()) { call.trackId = "master"; call.trackName = "the song"; }  // no track needed
      if (call.open != "effects" && call.open != "instrument" && call.open != "patterns" && call.open != "mixer" && call.open != "score") {
        const json* chain = call.trackId == "master" ? &song.masterEffects : track ? &track->effects : nullptr;
        std::string id;
        if (chain)
          for (const auto& fx : *chain)
            if (fx.is_object() && (fx.value("id", std::string()) == call.open || sameName(doc::effectName(fx), call.open))) {
              id = fx.value("id", std::string());
              break;
            }
        if (id.empty()) {
          p.problems.push_back("Open: no effect \"" + call.open + "\" on " + (call.trackName.empty() ? std::string("that track") : call.trackName) + ": left out");
          continue;
        }
        call.open = id;
      }
      if ((call.open == "instrument" || call.open == "patterns") && call.trackId == "master") {
        p.problems.push_back(std::string("Open: the master has no ") + (call.open == "patterns" ? "notes" : "instrument") + ": left out");
        continue;
      }
      p.calls.push_back(std::move(call));
      continue;
    }
    // Core's own plugins: what the track must have, a preset by its id or name, text choices ("intent", "style", "mode",
    // "mood"), then the plugin's own check (pluginCalls()).
    const PluginCalls* plugin = pluginCallsOf(*f);
    if (plugin) {
      if (track && plugin->needsNotes && !doc::playsPatterns(*track)) {
        p.problems.push_back(f->name + ": " + track->name + " plays no notes (an audio track or a bus): left out");
        continue;
      }
      if (track && plugin->needsClips && track->audioClips.empty()) {
        p.problems.push_back(f->name + ": " + track->name + " has no audio clip (a recorded word): left out");
        continue;
      }
      bool bad = false;
      auto takeText = [&](const std::string& key, const json& v) {
        const auto opt = f->options.find(key);
        if (opt == f->options.end()) return false;
        const std::string want = v.is_string() ? v.get<std::string>() : std::string();
        std::string found;
        for (const auto& o : opt->second) if (sameName(o, want)) found = o;
        // F-Core's style: the song's style words are accepted too ("Dark psycore" -> darkpsycore).
        if (found.empty() && key == "style" && !want.empty()) {
          const std::string k = styleKey(want);
          for (const auto& o : opt->second) if (o == k) found = o;
        }
        if (found.empty()) {
          p.problems.push_back(f->name + " " + key + " \"" + want + "\" is not one of: " + [&] { std::string l; for (const auto& o : opt->second) l += (l.empty() ? "" : ", ") + o; return l; }() + ": left out");
          bad = true;
        } else {
          call.texts[key] = found;
        }
        return true;
      };
      for (const auto& [key, values] : f->options)
        if (c.contains(key)) takeText(key, c[key]);
      if (c.contains("args") && c["args"].is_object())
        for (auto kv = c["args"].begin(); kv != c["args"].end(); ++kv)
          if (kv.value().is_string()) takeText(kv.key(), kv.value());
      if (bad) continue;
      const std::string preset = str(c, "preset");
      if (!preset.empty()) {
        for (const auto& id : f->presets) if (sameName(id, preset)) call.preset = id;
        if (call.preset.empty() && f->extra.contains("presetInfo"))
          for (auto it = f->extra["presetInfo"].begin(); it != f->extra["presetInfo"].end(); ++it)
            if (sameName(it.value().value("name", std::string()), preset)) call.preset = it.key();
        if (call.preset.empty()) {
          p.problems.push_back(f->name + ": no preset \"" + preset + "\": left out");
          continue;
        }
      }
      if (plugin->presetNeeded && call.preset.empty()) {
        p.problems.push_back(f->name + ": \"preset\" (" + plugin->presetNeeded + ") is needed: left out");
        continue;
      }
      if (plugin->check && !plugin->check(*f, track, call, p.problems)) continue;
    }
    if (f->family == "effect" && !str(c, "preset").empty()) {
      const std::string preset = str(c, "preset");
      for (const auto& name : f->presets) if (sameName(name, preset)) call.preset = name;
      if (call.preset.empty()) {
        p.problems.push_back(f->name + ": no preset \"" + preset + "\": left out");
        continue;
      }
    }
    if (f->family == "mix" && call.trackId == "master" && c.contains("args") && c["args"].is_object() && c["args"].contains("pan"))
      p.problems.push_back("Mix: the master has no pan: left out");
    // The knobs.
    if (c.contains("args") && c["args"].is_object())
      for (auto kv = c["args"].begin(); kv != c["args"].end(); ++kv) {
        const FunctionArg* a = f->findArg(kv.key());
        if (f->family == "mix" && call.trackId == "master" && kv.key() == "pan") continue;  // told above
        if (kv.value().is_string() && f->options.count(kv.key())) continue;  // a text choice, taken above
        if (!a) {
          p.problems.push_back(f->name + ": no knob \"" + kv.key() + "\": left out");
          continue;
        }
        if (!kv.value().is_number()) {
          p.problems.push_back(f->name + " " + kv.key() + ": not a number: left out");
          continue;
        }
        const double v = kv.value().get<double>();
        if (!std::isfinite(v) || v < a->min || v > a->max) {
          p.problems.push_back(f->name + " " + kv.key() + " " + num(v) + " is outside " + num(a->min) + ".." + num(a->max) + ": left out");
          continue;
        }
        call.args[a->id] = v;
      }
    // A section: a rise.
    const std::string section = str(c, "section");
    if (!section.empty()) {
      const doc::Section* s = nullptr;
      for (const auto& x : song.sections)
        if (x.id == section || sameName(x.name, section)) s = &x;
      if (!f->rises) {
        call.warnings.push_back(f->name + " does not rise over a section: it acts on the whole track (automate it by hand for one part)");
      } else if (!s) {
        p.problems.push_back("no section called \"" + section + "\": " + f->name + " left out (add the section on the ruler and ask again)");
        continue;
      } else {
        call.sectionName = s->name;
        call.startTick = s->startTick;
        call.endTick = s->startTick + s->lengthTicks;
      }
    } else if (f->rises && !(plugin && plugin->quietWithoutSection)) {
      call.warnings.push_back("no section given: the knobs are set, nothing rises");
    }
    // A sample: a value of the library.
    if (f->family == "sample") {
      const std::string key = str(c, "sample");
      const doc::LibrarySample* s = samples && !key.empty() ? findSampleKey(*samples, key) : nullptr;
      if (!s) {
        p.problems.push_back(key.empty() ? std::string("a sample call without \"sample\" was left out") : "no sample \"" + key + "\" in the library: left out");
        continue;
      }
      call.sample = s->sha256;
      call.sampleName = doc::shownName(*s);
      if (track) {
        const doc::SampleFit fit = doc::sampleFit(*s, doc::trackSampleRole(*track), doc::fastestTempo(song));
        if (fit.known && !fit.fits && !fit.why.empty()) call.warnings.push_back(call.sampleName + " on " + track->name + ": " + fit.why);
      }
    }
    // Types: a warning, never a refusal.
    if (track) {
      const std::string type = trackType(*track);
      if (!roleFits(f->role, type)) call.warnings.push_back(f->name + " is made for " + f->role + "; " + track->name + " looks like a " + type);
      if (f->instrument && !doc::playsPatterns(*track)) {
        p.problems.push_back(f->name + " needs a track that plays notes: left out");
        continue;
      }
      if (f->instrument) call.warnings.push_back(f->name + " replaces the instrument of " + track->name);
    }
    if (!style.empty() && !f->styles.empty() && std::find(f->styles.begin(), f->styles.end(), style) == f->styles.end())
      call.warnings.push_back(f->name + " is made for " + f->styles.front() + (f->styles.size() > 1 ? " and others" : "") + ", the song is " + style + ": try it by ear");
    if (c.contains("version") && c["version"].is_number_integer() && c["version"].get<int>() != f->version)
      call.warnings.push_back(f->name + " is version " + std::to_string(f->version) + " now (the proposal named " + std::to_string(c["version"].get<int>()) + ")");
    p.calls.push_back(std::move(call));
  }
  return p;
}

std::string describeCall(const Call& c) {
  std::string out = c.fn->name;
  if (c.fn->family == "sample") out += " " + c.sampleName;
  if (c.fn->family == "setting") out = "Set " + c.effectName;
  if (c.fn->family == "open") out = "Open " + (c.open == "effects" ? std::string("the effects") : c.open == "instrument" ? std::string("the instrument")
                                               : c.open == "patterns" ? std::string("the notes") : c.open == "mixer" ? std::string("the mixer")
                                               : c.open == "score" ? std::string("Stage Core") : "effect " + c.open);
  if (!c.preset.empty()) out += " \u00b7 " + c.preset;
  for (const auto& [k, v] : c.texts) out += " \u00b7 " + k + " " + v;
  out += " on " + (c.trackName.empty() ? std::string("the track you choose") : c.trackName);
  if (!c.args.empty()) {
    out += " (";
    bool first = true;
    for (const auto& [k, v] : c.args) {
      const FunctionArg* a = c.fn->findArg(k);
      out += (first ? "" : ", ") + (a && !a->name.empty() ? a->name : k) + " " + num(v) + (a ? a->unit : std::string());
      first = false;
    }
    out += ")";
  }
  if (c.endTick > c.startTick) out += ", rising over " + c.sectionName;
  return out;
}

std::vector<std::string> applyProposal(doc::Document& song, const Proposal& p, const std::string& chosenTrack, const ApplyEnv& env) {
  std::vector<std::string> lines;
  for (const auto& c : p.calls) {
    const std::string where = c.trackId.empty() ? chosenTrack : c.trackId;
    if (where.empty()) throw doc::DocumentError("calls", "choose a track for " + c.fn->name);
    const bool master = where == "master";
    const doc::Track* t = master ? nullptr : song.findTrack(where);
    if (!master && !t) throw doc::DocumentError("calls", "no track '" + where + "'");
    const std::string name = master ? std::string("Master") : t->name;
    if (c.fn->family == "effect") {
      const std::string chain = master ? std::string() : where;
      const std::string effect = c.fn->id.substr(3);
      const doc::EffectPreset* preset = c.preset.empty() ? nullptr : doc::findEffectPreset(effect, c.preset);
      const int at = preset ? doc::addEffect(song, chain, *preset) : doc::addEffect(song, chain, effect);
      for (const auto& [k, v] : c.args) doc::setEffectParam(song, chain, at, k, v);
      lines.push_back(c.fn->name + (preset ? std::string(" \u00b7 ") + preset->name : std::string()) + " on " + name);
    } else if (c.fn->family == "setting") {
      const std::string chain = master ? std::string() : where;
      const json& list = doc::effectChain(song, chain);
      int at = -1;
      for (size_t i = 0; i < list.size(); ++i)
        if (list[i].is_object() && list[i].value("id", std::string()) == c.effectId) at = (int)i;
      if (at < 0) throw doc::DocumentError("calls", c.effectName + " is not on " + name + " any more");
      if (!c.preset.empty()) {
        const doc::EffectPreset* pr = doc::findEffectPreset(doc::effectName(list[(size_t)at]), c.preset);
        if (!pr) throw doc::DocumentError("calls", c.effectName + " has no preset " + c.preset);
        doc::applyEffectPreset(song, chain, at, *pr);
      }
      for (const auto& [k, v] : c.args) doc::setEffectParam(song, chain, at, k, v);
      lines.push_back(describeCall(c));
    } else if (c.fn->family == "mix") {
      if (master) {
        if (c.args.count("gainDb")) song.masterGainDb = c.args.at("gainDb");
      } else {
        for (auto& tr : song.tracks)
          if (tr.id == where) {
            if (c.args.count("gainDb")) tr.mix.gainDb = c.args.at("gainDb");
            if (c.args.count("pan")) tr.mix.pan = c.args.at("pan");
          }
      }
      lines.push_back(describeCall(c));
    } else if (const PluginCalls* plugin = pluginCallsOf(*c.fn)) {
      if (master) throw doc::DocumentError("calls", c.fn->name + " goes on a track, not the master");
      lines.push_back(plugin->apply(song, c, where));
    } else if (c.fn->family == "open") {
      lines.push_back(describeCall(c));  // nothing changes: the window opens it after Apply (opensOf)
    } else if (c.fn->family == "rack") {
      if (master) throw doc::DocumentError("racks", c.fn->name + " goes on a track, not the master");
      const std::vector<std::string> later = doc::applyNativeRack(song, {where}, *c.fn->rack);
      std::string line = c.fn->name + " on " + name + " (Core's own)";
      for (size_t i = 0; i < later.size(); ++i) line += (i ? ", " : "; not in this Core yet: ") + later[i];
      lines.push_back(line);
    } else if (c.fn->family == "sample") {
      if (master) throw doc::DocumentError("samples", "a sample goes on a track, not the master");
      if (!env.samples) throw doc::DocumentError("samples", "the sample library is not loaded");
      const doc::LibrarySample* s = env.samples->findHash(c.sample);
      if (!s) throw doc::DocumentError("samples", "the sample " + c.sampleName + " is not in the library any more");
      if (!doc::playsPatterns(*t)) throw doc::DocumentError("samples", name + " plays no notes (an audio track or a bus)");
      doc::setTrackSample(song, where, *env.samples, *s);
      lines.push_back(c.sampleName + " on " + name);
    } else {
      throw doc::DocumentError("calls", "Core cannot apply " + c.fn->name + " yet");
    }
  }
  return lines;
}

}  // namespace core::control
