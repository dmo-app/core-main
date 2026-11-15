#include "fcore.hpp"
#include "corefx_legacy.hpp"
#include "core_plugins.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include "edit.hpp"

namespace core::doc {

namespace {

[[noreturn]] void fail(const std::string& where, const std::string& message) { throw DocumentError(where, message); }

// What a style changes in every intent (the signatures of core-devices.md, section 3).
struct StyleFx {
  double echoBeats = 0.75;  // the echo's division
  double spaceSize = 0.6;   // reverb size
  double spaceMul = 1;      // how much reverb compared with the others
  double drive = 0;         // saturation in dB (0: none)
  bool drop = true;         // a build-up cuts dry at its end (false: the tail rings on)
  double pitchUp = 12;      // a build-up's climb, semitones
  double resonance = 0.707; // the build-up filter's resonance
  double introBeats = 64;   // an intro's length
};
StyleFx styleFx(const std::string& style) {
  StyleFx s;
  if (style == "darkpsycore") { s.echoBeats = 0.25; s.spaceSize = 0.35; s.spaceMul = 0.8; s.drive = 4; s.resonance = 0.9; }
  else if (style == "hitech") { s.echoBeats = 0.125; s.spaceSize = 0.25; s.spaceMul = 0.5; s.drive = 2; s.resonance = 0.8; }
  else if (style == "slambient") { s.echoBeats = 0.75; s.spaceSize = 0.95; s.spaceMul = 1.5; s.drop = false; s.pitchUp = 7; }
  else if (style == "darkpsy") { s.spaceSize = 0.55; s.drive = 2; s.resonance = 1.4; }
  else if (style == "twilight") { s.drive = 1; s.resonance = 1.4; }
  else if (style == "fullon") { s.spaceSize = 0.5; }
  else if (style == "progressive") { s.pitchUp = 7; s.introBeats = 128; }
  return s;
}

std::string fmt(const char* f, double v) {
  char b[64];
  std::snprintf(b, sizeof b, f, v);
  return b;
}
std::string hz(double v) { return v < 1000 ? fmt("%.0f Hz", v) : fmt("%.1f kHz", v / 1000); }
std::string pct(double v) { return fmt("%.0f %%", v * 100); }
std::string division(double beats) {
  if (std::fabs(beats - 0.125) < 1e-9) return "1/32";
  if (std::fabs(beats - 0.25) < 1e-9) return "1/16";
  if (std::fabs(beats - 0.5) < 1e-9) return "1/8";
  if (std::fabs(beats - 0.75) < 1e-9) return "dotted 1/8";
  if (std::fabs(beats - 1) < 1e-9) return "1/4";
  if (std::fabs(beats - 1.5) < 1e-9) return "dotted 1/4";
  return fmt("%.3g beats", beats);
}
std::string bars(double beats) {
  const double b = beats / 4;
  return std::fabs(b - std::round(b)) < 1e-9 ? fmt(b == 1 ? "%.0f bar" : "%.0f bars", b) : fmt("%.2g bars", b);
}

bool isHzParam(const std::string& effect, const std::string& param) {
  if (const EffectInfo* info = findEffect(effect))
    for (const auto& p : info->params) if (param == p.name) return std::string(p.unit) == "Hz";
  return false;
}

// Builds a plan: effects by role, static values (with what they are outside the regions) and moves.
struct Builder {
  FCorePlan plan;
  std::map<std::string, json> neutral;  // role -> {param: value outside the regions}
  FCoreEffect& fx(const std::string& role, const std::string& name) {
    for (auto& e : plan.effects) if (e.role == role) return e;
    plan.effects.push_back({role, name, json::object()});
    return plan.effects.back();
  }
  void set(const std::string& role, const std::string& name, const std::string& param, double value, std::optional<double> outside = {}) {
    fx(role, name).params[param] = value;
    if (outside) neutral[role][param] = *outside;
  }
  void move(const std::string& role, const std::string& name, const std::string& param, double from, double to, double outside,
            double curve, bool hold) {
    if (role != "sampler") fx(role, name).params[param] = from;
    plan.moves.push_back({role, param, from, to, outside, curve, hold});
  }
};

double clamp01(double v) { return std::clamp(v, 0.0, 1.0); }

}  // namespace

// ---- styles and intents

const std::vector<FCoreStyle>& fcoreStyles() {
  // The templates' styles and their tempos (templates/*.core-template.json), with a margin.
  static const std::vector<FCoreStyle> styles = {
      {"progressive", "Progressive psy", 134, 142}, {"fullon", "Full-on", 142, 150},       {"twilight", "Twilight", 144, 152},
      {"darkpsy", "Darkpsy", 150, 160},             {"hitech", "Hi-tech", 172, 225},       {"darkpsycore", "Dark psycore", 205, 260},
      {"slambient", "Slambient", 235, 460}};
  return styles;
}
const FCoreStyle* findFCoreStyle(const std::string& id) {
  for (const auto& s : fcoreStyles()) if (s.id == id) return &s;
  return nullptr;
}

const std::vector<FCoreIntent>& fcoreIntents() {
  static const std::vector<FCoreIntent> intents = {
      {"buildup", "Build-up", true, 32, "Rises and builds tension towards the drop"},
      {"intro", "Intro", true, 64, "Brings the sound in at the start"},
      {"impact", "Impact", true, 4, "One big hit on the drop"},
      {"downlifter", "Downlifter", true, 16, "Falls away after a peak"},
      {"atmosphere", "Atmosphere", false, 16, "A still, wide background"},
      {"clean", "Clean", false, 16, "Only the sample, nothing added"}};
  return intents;
}
const FCoreIntent* findFCoreIntent(const std::string& id) {
  for (const auto& i : fcoreIntents()) if (i.id == id) return &i;
  return nullptr;
}

std::vector<std::string> fcoreIntentsFor(const std::string& style) {
  static const std::map<std::string, std::vector<std::string>> first = {
      {"darkpsycore", {"buildup", "impact"}},
      {"hitech", {"buildup", "impact"}},
      {"slambient", {"atmosphere", "intro"}},
      {"darkpsy", {"buildup", "downlifter", "intro"}},
      {"twilight", {"buildup", "downlifter", "intro"}},
      {"fullon", {"intro", "buildup", "downlifter"}},
      {"progressive", {"intro", "buildup", "downlifter"}}};
  std::vector<std::string> out;
  auto it = first.find(style);
  if (it != first.end()) out = it->second;
  std::vector<std::string> warned;
  for (const auto& i : fcoreIntents()) {
    if (std::find(out.begin(), out.end(), i.id) != out.end()) continue;
    if (!fcoreIntentWarning(style, i.id).empty()) warned.push_back(i.id);
    else out.push_back(i.id);
  }
  out.insert(out.end(), warned.begin(), warned.end());  // last, with their warning
  return out;
}

std::string fcoreIntentWarning(const std::string& style, const std::string& intent) {
  if ((style == "darkpsycore" || style == "hitech") && intent == "atmosphere") return "a long, big space tends to blur this style's fast low end";
  if (style == "hitech" && intent == "downlifter") return "a slow fall loses the tension hi-tech keeps";
  return "";
}

// ---- ingredients: the parts of an intent that can be switched off

std::string fcoreIngredientLabel(const std::string& intent, const std::string& id) {
  if (id == "shape") return intent == "intro" ? "Fade-in" : intent == "downlifter" ? "Fade-out" : "Width";
  if (id == "space") return intent == "impact" ? "Tail" : "Space";
  if (id == "filter") return "Filter";
  if (id == "echo") return "Echo";
  if (id == "drive") return "Drive";
  if (id == "pitch") return intent == "atmosphere" ? "Octave down" : "Pitch";
  if (id == "attack") return "Slow attack";
  if (id == "darken") return "Darker before the drop";
  if (id == "swell") return "Reverb before the drop";
  if (id == "freeze") return "Freeze at the end";
  return "";
}

// What each part does to the sound, in plain words (shown next to its switch; no technical names).
std::string fcoreIngredientHelp(const std::string& intent, const std::string& style, const std::string& id) {
  static const std::map<std::string, std::map<std::string, std::string>> help = {
      {"buildup",
       {{"filter", "Takes the low end away bit by bit, so the drop hits harder"},
        {"drive", "Adds grit as it rises"},
        {"echo", "The sound repeats, and the repeats grow towards the drop"},
        {"space", "The sound moves into a bigger room"},
        {"shape", "The sound spreads wider, left and right"},
        {"pitch", "The sound climbs in pitch near the end"},
        {"darken", "The top closes in the last bars, so the drop feels brighter"},
        {"swell", "A big reverb swells in over the last bars"},
        {"freeze", "At the end everything stays where it got to, the note held: add more effects after it"}}},
      {"intro",
       {{"filter", "Starts muffled and opens up"},
        {"echo", "Repeats that fade away as the sound arrives"},
        {"space", "Starts far away and comes closer"},
        {"shape", "The level comes in from quiet"},
        {"drive", "A little grit comes in at the end"}}},
      {"impact",
       {{"pitch", "The hit drops in pitch"},
        {"filter", "The hit closes down and gets darker"},
        {"drive", "Grit and weight on the hit"},
        {"space", "A tail that rings after the hit"}}},
      {"downlifter",
       {{"filter", "Gets darker as it falls"},
        {"space", "Moves far away"},
        {"pitch", "Falls in pitch"},
        {"shape", "Fades out"}}},
      {"atmosphere",
       {{"filter", "Softens the bright top"},
        {"space", "A big room around it"},
        {"shape", "Wider, left and right"},
        {"attack", "Swells in instead of starting hard"},
        {"pitch", "An octave lower: darker and bigger"}}}};
  if (intent == "intro" && id == "filter" && style == "hitech") return "Starts thin; the low end only arrives at the end";
  auto i = help.find(intent);
  if (i == help.end()) return "";
  auto j = i->second.find(id);
  return j == i->second.end() ? "" : j->second;
}

std::string fcoreMainPart(const std::string& intent, const std::string& mode) {
  if (intent == "impact") return mode == "note" ? "pitch" : "filter";
  if (intent == "atmosphere") return "space";
  if (intent == "clean") return "";
  return "filter";
}

std::vector<std::string> fcoreStartOff(const std::string& intent, const std::string& style, const FCoreKnobs& knobs, const std::string& mode) {
  const std::string main = fcoreMainPart(intent, mode);
  std::vector<std::string> off;
  for (const auto& id : planFCore(intent, style, knobs, mode).ingredients) if (id != main) off.push_back(id);
  std::sort(off.begin(), off.end());
  return off;
}

const std::vector<FCorePreset>& fcorePresetsOf(const std::string& intent) {
  static const std::vector<FCorePreset> none;
  static const std::map<std::string, std::vector<FCorePreset>> all = {
      {"buildup",
       {{"full", "Full build-up", "Everything rises together: the low end goes, echo and space grow, it widens and climbs", {"filter", "drive", "echo", "space", "shape", "pitch"}},
        {"climb", "Climb", "Only the low end going and the pitch climbing: tight, no tail", {"filter", "drive", "pitch"}},
        {"wash", "Echo wash", "The echo and the space swell while the low end stays", {"echo", "space", "shape"}},
        {"frozen", "Freeze into the drop", "It rises, gets darker with a big reverb in the last bars, and holds there until the drop",
         {"filter", "echo", "space", "pitch", "darken", "swell", "freeze"}}}},
      {"intro",
       {{"full", "Full intro", "Comes in from far away: it opens, the echo and the space dry up, the level rises", {"filter", "echo", "space", "shape", "drive"}},
        {"fog", "Out of the fog", "Opens out of a wide space, no echo", {"filter", "space", "shape"}},
        {"fade", "Simple fade-in", "Only the level coming in", {"shape"}}}},
      {"impact",
       {{"full", "Full impact", "The hit falls, with grit and a tail", {"pitch", "filter", "drive", "space"}},
        {"dry", "Dry hit", "The hit falls with grit, no tail", {"pitch", "filter", "drive"}}}},
      {"downlifter",
       {{"full", "Full downlifter", "It closes, moves away, falls and fades out", {"filter", "space", "pitch", "shape"}},
        {"away", "Fade away", "It moves away and fades out, the tone stays", {"space", "shape"}}}},
      {"atmosphere",
       {{"full", "Full atmosphere", "Soft, wide, a big room and a slow swell", {"filter", "space", "shape", "attack", "pitch"}},
        {"close", "Close pad", "Soft and wide, no room", {"filter", "shape", "attack"}}}}};
  auto it = all.find(intent);
  return it == all.end() ? none : it->second;
}

std::vector<std::string> fcorePresetOff(const std::string& intent, const std::string& style, const FCoreKnobs& knobs, const std::string& mode,
                                        const std::string& preset) {
  for (const auto& p : fcorePresetsOf(intent)) {
    if (p.id != preset) continue;
    std::vector<std::string> off;
    for (const auto& id : planFCore(intent, style, knobs, mode).ingredients)
      if (std::find(p.on.begin(), p.on.end(), id) == p.on.end()) off.push_back(id);
    std::sort(off.begin(), off.end());
    return off;
  }
  fail("fcore.preset", "intent '" + intent + "' has no preset '" + preset + "'");
}

namespace {
// Parts that appear with a new style or mode (a style's drive, the sampler's pitch) start off: a step is added with +.
void newPartsOff(FCoreInstance& f, const std::vector<std::string>& partsBefore) {
  for (const auto& id : planFCore(f.intent, f.style, f.knobs, f.mode).ingredients)
    if (std::find(partsBefore.begin(), partsBefore.end(), id) == partsBefore.end() && std::find(f.off.begin(), f.off.end(), id) == f.off.end())
      f.off.push_back(id);
  std::sort(f.off.begin(), f.off.end());
}
}  // namespace

std::vector<FCoreIngredient> fcoreIngredients(const std::string& intent, const std::string& style, const FCoreKnobs& knobs,
                                              const std::string& mode) {
  std::vector<FCoreIngredient> out;
  for (const auto& id : planFCore(intent, style, knobs, mode).ingredients) out.push_back({id, fcoreIngredientLabel(intent, id), fcoreIngredientHelp(intent, style, id)});
  return out;
}

// ---- plans

FCorePlan planFCore(const std::string& intent, const std::string& style, const FCoreKnobs& kn, const std::string& mode,
                    const std::vector<std::string>& off) {
  if (!findFCoreIntent(intent)) fail("fcore.intent", "unknown intent '" + intent + "'");
  if (!findFCoreStyle(style)) fail("fcore.style", "unknown style '" + style + "'");
  if (mode != "note" && mode != "region") fail("fcore.mode", "must be \"note\" or \"region\"");
  const std::set<std::string> drops(off.begin(), off.end());
  const bool note = mode == "note";
  StyleFx st = styleFx(style);
  const double amount = clamp01(kn.amount), tone = clamp01(kn.tone), space = clamp01(kn.space);
  const double k = 0.35 + 0.65 * amount;  // even at 0 the intent is still heard
  auto spaceMix = [&](double x) { return clamp01(x * st.spaceMul * (0.4 + 1.2 * space)); };
  Builder b;
  // What it does, one piece per ingredient (a part switched off leaves its piece out).
  std::vector<std::pair<std::string, std::string>> said;
  auto say = [&](const std::string& ingredient, const std::string& text) { said.push_back({ingredient, text}); };
  std::string prefix, suffix = ".";
  const std::string length = bars(kn.lengthBeats);

  if (intent == "buildup") {
    // Freeze (a step, + Add step; Diogo 2026-11-05): at the end everything stays where it got to, the note's pitch
    // too, so more effects can go after it until the drop.
    const bool freeze = drops.count("freeze") == 0;
    const double top = 600 * std::pow(6.0, amount);  // 600 Hz .. 3.6 kHz
    b.set("filter", "filter", "mode", 1);
    b.set("filter", "filter", "resonance", st.resonance);
    b.move("filter", "filter", "cutoff", 20, top, 20, 2, freeze || !st.drop);
    if (st.drive > 0) {
      b.set("drive", "saturation", "drive", st.drive * (0.5 + amount), 0.0);
      b.set("drive", "saturation", "output", -st.drive * (0.5 + amount) / 3, 0.0);
    }
    b.set("echo", "delay", "time", st.echoBeats);
    b.set("echo", "delay", "pingPong", 1);
    b.set("echo", "delay", "tone", 0.3 + 0.6 * tone);
    b.move("echo", "delay", "mix", 0, 0.5 * k, 0, 2, freeze || !st.drop);
    b.move("echo", "delay", "feedback", 0.3, 0.3 + 0.4 * k, 0.3, 2, freeze || !st.drop);
    b.set("space", "reverb", "size", st.spaceSize);
    b.set("space", "reverb", "damping", 1 - 0.7 * tone);
    b.move("space", "reverb", "mix", spaceMix(0.1), spaceMix(0.5), 0, 2, freeze || !st.drop);
    b.move("shape", "utility", "width", 1, 1 + 0.6 * k, 1, 1, freeze || !st.drop);
    prefix = "Over " + length + ": ";
    say("filter", "the low end goes (high-pass up to " + hz(top) + ")");
    say("echo", "the echo (" + division(st.echoBeats) + ") grows to " + pct(0.5 * k));
    say("space", "the space grows to " + pct(spaceMix(0.5)));
    say("shape", "the stereo widens");
    // Darker before the drop: a low-pass that waits and closes over the last quarter (t^6 in octaves: a quarter of
    // the way at 80 %), down to 2.4 kHz .. 600 Hz with Amount. Off until it is added (+ Add step).
    const double dark = 2400 / std::pow(4.0, amount);
    b.set("darken", "filter", "mode", 0);
    b.move("darken", "filter", "cutoff", 20000, dark, 20000, 6, freeze || !st.drop);
    say("darken", "the top closes to " + hz(dark) + " in the last bars");
    // More reverb before the drop: a second space that waits and swells over the last bars.
    b.set("swell", "reverb", "size", std::max(0.7, st.spaceSize));
    b.set("swell", "reverb", "damping", 1 - 0.7 * tone);
    b.move("swell", "reverb", "mix", 0, spaceMix(0.6), 0, 6, freeze || !st.drop);
    say("swell", "a big reverb swells in the last bars");
    say("freeze", "");
    if (note) {
      b.move("sampler", "sampler", "tune", 0, std::round(st.pitchUp * k), 0, 4, freeze);
      say("pitch", fmt("the pitch climbs +%.0f st near the end", std::round(st.pitchUp * k)));
    }
    suffix = freeze ? ", and at the end it freezes: everything stays where it got to."
                    : st.drop ? ", and it cuts dry at the drop." : ", and the tail rings on at the end.";
  } else if (intent == "intro") {
    prefix = "Over " + length + ": ";
    if (style == "hitech") {  // tight entry: the low end arrives at the end
      const double from = 150 * std::pow(4.0, amount);  // 150 .. 600 Hz
      b.set("filter", "filter", "mode", 1);
      b.move("filter", "filter", "cutoff", from, 20, 20, 0.7, true);
      say("filter", "it comes in tight, the low end only arrives at the end (high-pass from " + hz(from) + ")");
    } else {
      const double base = style == "darkpsycore" ? 150 : style == "slambient" ? 800 : 250;
      const double from = base * std::pow(4.0, 1 - amount);
      const double to = style == "darkpsycore" ? 3000 * (0.5 + tone) : 20000;
      b.set("filter", "filter", "mode", 0);
      b.move("filter", "filter", "cutoff", from, to, 20000, 0.7, true);
      if (style == "darkpsycore") say("filter", "it comes out of the dark (low-pass from " + hz(from) + " to " + hz(to) + ") but never gets bright");
      else if (style == "slambient") say("filter", "it grows out of a wide fog (low-pass from " + hz(from) + " opening fully)");
      else say("filter", "it starts muffled and far away and opens (low-pass from " + hz(from) + " to fully open)");
    }
    const double echoBeats = style == "progressive" ? 1.5 : style == "hitech" ? 0.25 : 0.75;
    b.set("echo", "delay", "time", echoBeats);
    b.set("echo", "delay", "feedback", 0.4);
    b.set("echo", "delay", "tone", 0.3 + 0.6 * tone);
    b.move("echo", "delay", "mix", 0.25 * k, 0.1 * k, 0, 1, true);
    b.set("space", "reverb", "size", st.spaceSize);
    b.set("space", "reverb", "damping", 1 - 0.7 * tone);
    const double wetFrom = style == "slambient" ? spaceMix(0.55) : spaceMix(0.4);
    const double wetTo = style == "slambient" ? spaceMix(0.27) : spaceMix(0.15);
    b.move("space", "reverb", "mix", wetFrom, wetTo, 0, 1, true);
    b.move("shape", "utility", "gain", -18 * k, 0, 0, 0.5, true);
    say("space", "the space dries from " + pct(wetFrom) + " to " + pct(wetTo));
    say("echo", "the " + division(echoBeats) + " echo fades to " + pct(0.1 * k));
    say("shape", fmt("the level comes in from -%.0f dB", 18 * k));
    if (style == "darkpsycore") {
      b.move("drive", "saturation", "drive", 0, 3, 0, 3, true);
      say("drive", "a light drive comes in at the end");
    }
  } else if (intent == "impact") {
    const double drive = 4 + 8 * amount;
    b.set("drive", "saturation", "drive", drive, 0.0);
    b.set("drive", "saturation", "output", -drive / 3, 0.0);
    b.set("space", "reverb", "size", style == "slambient" ? 0.95 : 0.4);
    b.set("space", "reverb", "damping", 1 - 0.7 * tone);
    b.set("space", "reverb", "mix", spaceMix(0.25), 0.0);
    prefix = "A hit on the drop, over " + length + ": ";
    if (note) {
      const double fall = -std::round(12 * k);
      b.move("sampler", "sampler", "tune", 0, fall, 0, 0.5, true);
      say("pitch", fmt("the pitch falls %.0f st", fall));
    } else {
      const double to = 300 * std::pow(4.0, 1 - amount);
      b.set("filter", "filter", "mode", 0);
      b.move("filter", "filter", "cutoff", 20000, to, 20000, 0.5, true);
      say("filter", "the sound closes down to " + hz(to));
    }
    say("drive", "saturated (" + fmt("%.0f dB", drive) + ")");
    say("space", std::string("with a ") + (style == "slambient" ? "long, dark" : "short") + " tail (" + pct(spaceMix(0.25)) + ")");
  } else if (intent == "downlifter") {
    const double to = 400 * std::pow(4.0, 1 - amount);
    b.set("filter", "filter", "mode", 0);
    b.move("filter", "filter", "cutoff", 20000, to, 20000, 0.7, true);
    b.set("space", "reverb", "size", st.spaceSize);
    b.set("space", "reverb", "damping", 1 - 0.7 * tone);
    b.move("space", "reverb", "mix", spaceMix(0.15), spaceMix(0.6), 0, 1, true);
    b.move("shape", "utility", "gain", 0, -18 * k, 0, 1.5, true);
    prefix = "The opposite of a build-up, over " + length + ": ";
    say("filter", "it closes (low-pass down to " + hz(to) + ")");
    say("space", "it moves away (space to " + pct(spaceMix(0.6)) + ")");
    if (note) {
      b.move("sampler", "sampler", "tune", 0, -std::round(7 * k), 0, 1, true);
      say("pitch", fmt("it falls %.0f st", -std::round(7 * k)));
    }
    say("shape", "it fades out");
  } else if (intent == "atmosphere") {
    const double cut = 800 + 4200 * tone;
    b.set("filter", "filter", "mode", 0);
    b.set("filter", "filter", "cutoff", cut, 20000.0);
    b.set("space", "reverb", "size", 0.9);
    b.set("space", "reverb", "damping", 0.6);
    b.set("space", "reverb", "mix", spaceMix(0.6), 0.0);
    b.set("shape", "utility", "width", 1 + 0.6 * k, 1.0);
    prefix = "Turns the sample into a background: ";
    say("filter", "low-pass at " + hz(cut));
    say("space", "a big space (" + pct(spaceMix(0.6)) + ")");
    say("shape", "wider stereo");
    if (note) {
      b.plan.sampler["attack"] = 200;
      say("attack", "a slow attack");
      if (style == "slambient" || amount > 0.66) {
        b.plan.sampler["tune"] = -12;
        say("pitch", "an octave down");
      }
    }
  } else {  // clean
    prefix = "Only plays the sample";
  }

  // Every ingredient that sounds here, in the order the sentence says them (the drive of a build-up has no words).
  const bool driveSaid = std::any_of(said.begin(), said.end(), [](const auto& p) { return p.first == "drive"; });
  for (const auto& e : b.plan.effects)
    if (e.role == "drive" && !driveSaid) said.insert(said.begin() + (said.empty() ? 0 : 1), {"drive", ""});
  for (const auto& [ingredient, text] : said)
    if (std::find(b.plan.ingredients.begin(), b.plan.ingredients.end(), ingredient) == b.plan.ingredients.end()) b.plan.ingredients.push_back(ingredient);
  // The ingredients switched off: their effects, moves and sampler values go, and their words.
  if (!drops.empty()) {
    auto dropped = [&](const std::string& role) { return drops.count(role == "sampler" ? "pitch" : role) > 0; };
    b.plan.effects.erase(std::remove_if(b.plan.effects.begin(), b.plan.effects.end(), [&](const FCoreEffect& e) { return dropped(e.role); }),
                         b.plan.effects.end());
    b.plan.moves.erase(std::remove_if(b.plan.moves.begin(), b.plan.moves.end(), [&](const FCoreMove& m) { return dropped(m.role); }),
                       b.plan.moves.end());
    if (drops.count("pitch")) b.plan.sampler.erase("tune");
    if (drops.count("attack")) b.plan.sampler.erase("attack");
  }
  std::string body;
  for (const auto& [ingredient, text] : said) {
    if (drops.count(ingredient) || text.empty()) continue;
    body += (body.empty() ? "" : ", ") + text;
  }
  if (body.empty() && intent != "clean") { prefix = "Every part is switched off: only plays the sample"; suffix = "."; }
  b.plan.sentence = prefix + body + suffix;
  if (!b.plan.sentence.empty()) b.plan.sentence[0] = (char)std::toupper((unsigned char)b.plan.sentence[0]);

  if (note) {  // the finish: under 30 Hz goes, and nothing passes -1 dB (a rising build-up never jumps in level)
    b.set("finish", "eq", "b1Freq", 30);
    b.set("limit", "limiter", "ceiling", -1);
    b.plan.sampler["pitched"] = 1;
    b.plan.sampler["decay"] = 10000;
    if (!b.plan.sampler.contains("tune")) b.plan.sampler["tune"] = 0;
    if (!b.plan.sampler.contains("attack")) b.plan.sampler["attack"] = 0;
  }
  // The values outside the regions travel with the effects (region mode only reads them).
  for (auto& e : b.plan.effects)
    if (b.neutral.count(e.role)) e.params["__outside"] = b.neutral[e.role];
  return b.plan;
}

// ---- on a track

namespace {
FCoreKnobs knobsFrom(const json& j) {
  FCoreKnobs k;
  if (!j.is_object()) return k;
  auto num = [&](const char* key, double def, double lo, double hi) {
    return j.contains(key) && j[key].is_number() ? std::clamp(j[key].get<double>(), lo, hi) : def;
  };
  k.amount = num("amount", k.amount, 0, 1);
  k.lengthBeats = num("lengthBeats", k.lengthBeats, 1, 256);
  k.tone = num("tone", k.tone, 0, 1);
  k.space = num("space", k.space, 0, 1);
  return k;
}

json instanceJson(const FCoreInstance& f) {
  json regions = json::array();
  for (const auto& r : f.regions) regions.push_back({{"startTick", r.startTick}, {"endTick", r.endTick}});
  json j = {{"intent", f.intent}, {"style", f.style}, {"mode", f.mode}, {"bypass", f.bypass},
            {"knobs", {{"amount", f.knobs.amount}, {"lengthBeats", f.knobs.lengthBeats}, {"tone", f.knobs.tone}, {"space", f.knobs.space}}},
            {"effects", f.effects}, {"off", f.off}};
  if (f.mode == "region") j["regions"] = regions;
  if (!f.lanes.empty()) j["lanes"] = f.lanes;
  if (!f.samplerBase.empty()) {
    json base = json::object();
    for (const auto& [k, v] : f.samplerBase) base[k] = v ? json(*v) : json();
    j["samplerBase"] = base;
  }
  return corePluginState("fcore", std::move(j));
}

std::string fxKey(const std::string& id, const std::string& param) { return "fx:" + id + ":" + param; }

// The fingerprint of a lane's points: F-Core compares it with the one it wrote to see a change made by hand.
std::string laneFingerprint(const Automation& a) {
  std::string text;
  char buf[48];
  for (const auto& p : a.points) {
    std::snprintf(buf, sizeof buf, "%lld:%.6g;", (long long)p.tick, p.value);
    text += buf;
  }
  return std::to_string(std::hash<std::string>{}(text));
}

bool isHandLane(const Automation& a) { return a.extra.value("fcoreHand", false); }

// Takes away everything FCore wrote on the track (effects, modulators, automation), keeping the instance. The lanes
// changed by hand stay unless dropHand (F-Core removed: its effects go, so their lanes go too).
void clearWritten(Track& t, bool dropHand = false) {
  json chain = json::array();
  for (const auto& fx : t.effects) if (!isFCoreEffect(fx)) chain.push_back(fx);
  t.effects = chain;
  json mods = json::array();
  for (const auto& m : t.modulators) if (!isFCoreModulator(m)) mods.push_back(m);
  t.modulators = mods;
  t.automation.erase(std::remove_if(t.automation.begin(), t.automation.end(),
                                    [&](const Automation& a) { return a.extra.value("fcore", false) || (dropHand && isHandLane(a)); }),
                     t.automation.end());
}

// The value of a move at u in [0, 1] (in octaves between Hz values).
double moveAt(const FCoreMove& m, bool hzParam, double u) {
  const double w = std::pow(std::clamp(u, 0.0, 1.0), m.curve);
  if (hzParam && m.from > 0 && m.to > 0) return m.from * std::pow(m.to / m.from, w);
  return m.from + (m.to - m.from) * w;
}

void writeAutomation(Document& doc, Track& t, const std::string& key, const std::vector<AutomationPoint>& points) {
  clearAutomation(doc, t.id, key);
  for (const auto& p : points) setAutomationPoint(doc, t.id, key, p.tick, p.value);
  for (auto& a : t.automation) if (a.param == key) a.extra["fcore"] = true;
}

// Puts the sampler's own values back (those F-Core set over), and forgets them.
void restoreSampler(Track& t, FCoreInstance& f) {
  for (const auto& [k, v] : f.samplerBase) {
    if (v) t.instrument.params[k] = *v;
    else t.instrument.params.erase(k);
  }
  f.samplerBase.clear();
}

// The plan's drive, echo and space are written for the old saturation, delay and reverb: on the track they are
// Core FX, with their values, moves and rest values turned into Core FX parameters (corefx_legacy.hpp).
FCorePlan asCoreFx(FCorePlan plan, double bpm) {
  std::map<std::string, std::string> was;  // role -> old effect
  for (auto& e : plan.effects) {
    if (!isLegacyEffect(e.name)) continue;
    was[e.role] = e.name;
    json given = e.params;
    given.erase("__outside");
    bool mixMoves = e.params.contains("__outside") && e.params["__outside"].contains("mix");
    for (const auto& mv : plan.moves) mixMoves |= mv.role == e.role && mv.param == "mix";
    json params = coreFxFrom(e.name, given, bpm, mixMoves);
    if (e.params.contains("__outside")) {
      json outside = json::object();
      for (const auto& [k, v] : e.params["__outside"].items())
        if (const auto m = coreFxParamOf(e.name, k, bpm)) outside[m->param] = m->value(v.get<double>());
      params["__outside"] = outside;
    }
    e.params = params;
    e.name = "corefx";
  }
  std::vector<FCoreMove> moves;
  for (auto m : plan.moves) {
    const auto it = was.find(m.role);
    if (it != was.end()) {
      const auto map = coreFxParamOf(it->second, m.param, bpm);
      if (!map) continue;
      m.param = map->param;
      m.from = map->value(m.from); m.to = map->value(m.to); m.neutral = map->value(m.neutral);
    }
    moves.push_back(m);
  }
  plan.moves = moves;
  return plan;
}

// Writes the instance: its effects (ids "fc-<role>", so tails go on across edits), and its movement as rise
// modulators ("note") or as automation over the regions ("region").
void render(Document& doc, Track& t, FCoreInstance& f) {
  const FCorePlan plan = asCoreFx(planFCore(f.intent, f.style, f.knobs, f.mode, f.off), doc.bpm);
  // The sampler starts from its own values every time; a bypassed F-Core leaves them as they are.
  restoreSampler(t, f);
  auto setSampler = [&](const std::string& k, double v) {
    if (!f.samplerBase.count(k)) {
      const auto now = t.instrument.params.find(k);
      f.samplerBase[k] = now == t.instrument.params.end() ? std::nullopt : std::optional<double>(now->second);
    }
    t.instrument.params[k] = v;
  };
  // Lanes F-Core wrote and someone changed since: they become the user's (kept, never written over).
  for (auto& a : t.automation)
    if (a.extra.value("fcore", false)) {
      const auto it = f.lanes.find(a.param);
      if (it != f.lanes.end() && it->second != laneFingerprint(a)) {
        a.extra.erase("fcore");
        a.extra["fcoreHand"] = true;
      }
    }
  std::set<std::string> hand;
  for (const auto& a : t.automation) if (isHandLane(a)) hand.insert(a.param);
  f.lanes.clear();
  clearWritten(t);
  const int free = kMaxEffectsPerChain - (int)t.effects.size();
  if ((int)plan.effects.size() > free) fail("effects", "F-Core needs " + std::to_string(plan.effects.size()) + " places on the chain, it has " + std::to_string(free));
  f.effects.clear();
  for (const auto& e : plan.effects) {
    const EffectInfo* info = findEffect(e.name);
    if (!info) fail("fcore", "no built-in effect '" + e.name + "'");
    json params = json::object();
    for (const auto& p : info->params) params[p.name] = p.def;
    for (const auto& [k, v] : e.params.items()) if (k != "__outside") params[k] = v;
    const std::string id = "fc-" + e.role;
    t.effects.push_back({{"id", id}, {"type", "builtin"}, {"name", e.name}, {"version", info->version}, {"params", params},
                         {"bypass", f.bypass}, {"fcore", true}});
    f.effects[e.role] = id;
  }
  auto nameOf = [&](const std::string& role) {
    for (const auto& e : plan.effects) if (e.role == role) return e.name;
    return std::string();
  };
  if (f.mode == "note") {
    if (!f.bypass)
      for (const auto& [k, v] : plan.sampler.items()) setSampler(k, v.get<double>());
    if (!findFCoreIntent(f.intent)->moves) return;
    int n = 0;
    for (const auto& m : plan.moves) {
      const bool hzParam = m.role != "sampler" && isHzParam(nameOf(m.role), m.param);
      double depth = hzParam ? std::log2(m.to / m.from) : m.to - m.from;
      if (m.role == "sampler" && !f.bypass) setSampler(m.param, m.from);
      if (f.bypass) depth = 0;
      t.modulators.push_back({{"id", "fc" + std::to_string(++n)}, {"param", m.role == "sampler" ? m.param : fxKey(f.effects[m.role], m.param)},
                              {"shape", "rise"}, {"rateBeats", f.knobs.lengthBeats}, {"depth", depth}, {"curve", m.curve},
                              {"hold", m.hold}, {"fcore", true}});
    }
    return;
  }
  // region: outside the regions every FCore value is neutral; inside, its static value or its move.
  const int64_t step = kPpq / 4;
  auto lane = [&](const std::string& role, const std::string& param, double outside, const std::function<double(double)>& inside, bool moving) {
    if (hand.count(fxKey(f.effects[role], param))) return;  // the user's now
    std::vector<AutomationPoint> pts{{0, outside, json::object()}};
    for (const auto& r : f.regions) {
      const int64_t start = std::clamp<int64_t>(r.startTick, 0, doc.lengthTicks);
      const int64_t end = std::clamp<int64_t>(r.endTick, start + 1, doc.lengthTicks);
      const int64_t last = end - 1;
      while (!pts.empty() && pts.back().tick >= start) pts.pop_back();
      if (start > 0 && (pts.empty() || pts.back().tick < start - 1)) pts.push_back({start - 1, outside, json::object()});
      pts.push_back({start, inside(0), json::object()});
      if (moving)
        for (int64_t tick = start + step; tick < last; tick += step) pts.push_back({tick, inside((double)(tick - start) / (double)(last - start)), json::object()});
      if (last > start) pts.push_back({last, inside(1), json::object()});
      if (end < doc.lengthTicks) pts.push_back({end, outside, json::object()});
    }
    const std::string key = fxKey(f.effects[role], param);
    writeAutomation(doc, t, key, pts);
    for (const auto& a : t.automation) if (a.param == key) f.lanes[key] = laneFingerprint(a);
  };
  if (f.bypass) return;  // bypassed effects: nothing to write
  for (const auto& m : plan.moves) {
    if (m.role == "sampler") continue;
    const bool hzParam = isHzParam(nameOf(m.role), m.param);
    // A region's movement runs over the whole region; Length is the note's (the region is its own length).
    lane(m.role, m.param, m.neutral, [&](double u) { return moveAt(m, hzParam, u); }, true);
  }
  for (const auto& e : plan.effects) {
    if (!e.params.contains("__outside")) continue;
    for (const auto& [param, outside] : e.params["__outside"].items()) {
      const double value = e.params[param].get<double>();
      lane(e.role, param, outside.get<double>(), [value](double) { return value; }, false);
    }
  }
  // Neutral where nothing is automated and no region is set yet: effects at rest values.
  if (f.regions.empty()) {
    for (const auto& m : plan.moves)
      if (m.role != "sampler")
        for (auto& fx : t.effects)
          if (fx.value("id", std::string()) == f.effects[m.role]) fx["params"][m.param] = m.neutral;
    for (const auto& e : plan.effects)
      if (e.params.contains("__outside"))
        for (auto& fx : t.effects)
          if (fx.value("id", std::string()) == f.effects[e.role])
            for (const auto& [param, outside] : e.params["__outside"].items()) fx["params"][param] = outside;
  }
}

FCoreInstance& stored(Track& t, FCoreInstance& f) { t.extra["fcore"] = instanceJson(f); return f; }

FCoreInstance requireFCore(const Track& t) {
  auto f = fcoreOf(t);
  if (!f) fail("fcore", "track '" + t.id + "' has no F-Core");
  return *f;
}

void rewrite(Document& doc, Track& t, FCoreInstance f) {
  render(doc, t, f);
  // A lane kept by hand whose F-Core effect went (a step switched off, another intent): nothing left to move.
  t.automation.erase(std::remove_if(t.automation.begin(), t.automation.end(), [&](const Automation& a) {
                       if (!isHandLane(a)) return false;
                       for (const auto& [role, id] : f.effects)
                         if (a.param.rfind(fxKey(id, ""), 0) == 0) return false;
                       return true;
                     }),
                     t.automation.end());
  stored(t, f);
}

}  // namespace

bool isFCoreEffect(const json& fx) { return fx.is_object() && fx.value("fcore", false); }
bool isFCoreModulator(const json& m) { return m.is_object() && m.value("fcore", false); }

std::optional<FCoreInstance> fcoreOf(const Track& track) {
  if (!track.extra.contains("fcore") || !corePluginStateReadable("fcore", track.extra["fcore"])) return std::nullopt;
  const json& j = track.extra["fcore"];
  if (!j.is_object()) return std::nullopt;
  // Every field by its type: a song with "intent": 42 loaded, then threw in a panel callback (audit 2026-11-07).
  for (const char* key : {"intent", "style", "mode"}) if (j.contains(key) && !j[key].is_string()) return std::nullopt;
  auto text = [&](const char* key, const std::string& def) { return j.contains(key) ? j[key].get<std::string>() : def; };
  FCoreInstance f;
  f.intent = text("intent", f.intent);
  f.style = text("style", f.style);
  f.mode = text("mode", f.mode);
  f.bypass = j.contains("bypass") && j["bypass"].is_boolean() && j["bypass"].get<bool>();
  f.knobs = knobsFrom(j.value("knobs", json::object()));
  if (j.contains("effects") && j["effects"].is_object())
    for (const auto& [role, id] : j["effects"].items()) if (id.is_string()) f.effects[role] = id.get<std::string>();
  if (j.contains("off") && j["off"].is_array())
    for (const auto& o : j["off"]) if (o.is_string() && !fcoreIngredientLabel(f.intent, o.get<std::string>()).empty()) f.off.push_back(o.get<std::string>());
  if (j.contains("lanes") && j["lanes"].is_object())
    for (const auto& [key, print] : j["lanes"].items()) if (print.is_string()) f.lanes[key] = print.get<std::string>();
  if (j.contains("samplerBase") && j["samplerBase"].is_object())
    for (const auto& [key, v] : j["samplerBase"].items())
      if (v.is_number()) f.samplerBase[key] = v.get<double>();
      else if (v.is_null()) f.samplerBase[key] = std::nullopt;
  if (j.contains("regions") && j["regions"].is_array())
    for (const auto& r : j["regions"])
      if (r.is_object() && r.contains("startTick") && r.contains("endTick") && r["startTick"].is_number_integer() && r["endTick"].is_number_integer())
        f.regions.push_back({r["startTick"].get<int64_t>(), r["endTick"].get<int64_t>()});
  if (!findFCoreIntent(f.intent) || !findFCoreStyle(f.style) || (f.mode != "note" && f.mode != "region")) return std::nullopt;
  return f;
}

void applyFCore(Document& doc, const std::string& trackId, const std::string& intent, const std::string& style, const FCoreKnobs& knobs,
                const std::string& mode) {
  Track& t = trackById(doc, trackId);
  if (!findFCoreIntent(intent)) fail("fcore.intent", "unknown intent '" + intent + "'");
  if (!findFCoreStyle(style)) fail("fcore.style", "unknown style '" + style + "'");
  if (mode == "note") {
    if (t.instrument.kind != "builtin" || t.instrument.name != "sampler") fail("fcore.mode", "the F-Core sampler plays on a sampler track");
    t.instrument.version = std::max(t.instrument.version, kSamplerVersion);
  } else if (mode != "region") {
    fail("fcore.mode", "must be \"note\" or \"region\"");
  }
  FCoreInstance f;
  f.intent = intent;
  f.style = style;
  f.mode = mode;
  f.knobs = knobsFrom(json{{"amount", knobs.amount}, {"lengthBeats", knobs.lengthBeats}, {"tone", knobs.tone}, {"space", knobs.space}});
  const auto old = fcoreOf(t);
  if (old && old->mode == mode) f.regions = old->regions;
  if (old && old->intent == intent) {  // the same intent keeps its steps
    f.off = old->off;
    newPartsOff(f, planFCore(old->intent, old->style, old->knobs, old->mode).ingredients);
  } else {
    f.off = fcoreStartOff(intent, style, f.knobs, mode);  // one step to start; + adds the next
  }
  rewrite(doc, t, f);
}

bool fcoreLowTrack(const Track& track) {
  std::string name = track.name;
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  for (const char* word : {"kick", "bass", "sub", "bombo", "baixo"})
    if (name.find(word) != std::string::npos) return true;
  return false;
}

std::vector<FCoreApplied> applyFCoreToTracks(Document& doc, const std::vector<std::string>& trackIds, const std::string& intent,
                                             const std::string& style, const FCoreKnobs& knobs, const std::string& mode,
                                             const std::vector<FCoreRegion>& regions) {
  if (trackIds.empty()) fail("fcore", "no track selected");
  if (mode != "note" && mode != "region") fail("fcore.mode", "must be \"note\" or \"region\"");
  Document work = doc;  // all or nothing
  std::vector<FCoreApplied> out;
  std::set<std::string> seen;
  for (const auto& id : trackIds) {
    if (!seen.insert(id).second) continue;
    Track& t = trackById(work, id);
    const bool sampler = t.instrument.kind == "builtin" && t.instrument.name == "sampler";
    const std::string m = mode == "note" && sampler ? "note" : "region";
    try {
      applyFCore(work, id, intent, style, knobs, m);
      FCoreInstance f = *fcoreOf(t);
      if (m == "region" && f.regions.empty()) {
        for (const auto& r : regions) {
          const bool overlaps = std::any_of(f.regions.begin(), f.regions.end(), [&](const FCoreRegion& x) { return r.startTick < x.endTick && x.startTick < r.endTick; });
          if (!overlaps && r.endTick - r.startTick >= kPpq / 4 && r.startTick >= 0 && r.endTick <= work.lengthTicks) f.regions.push_back(r);
        }
        std::sort(f.regions.begin(), f.regions.end(), [](const FCoreRegion& a, const FCoreRegion& b) { return a.startTick < b.startTick; });
      }
      if (trackIds.size() > 1 && fcoreLowTrack(t)) {
        const auto parts = fcoreIngredients(intent, style, knobs, m);
        for (const char* part : {"shape", "space"})
          if (std::any_of(parts.begin(), parts.end(), [&](const FCoreIngredient& p) { return p.id == part; }) &&
              std::find(f.off.begin(), f.off.end(), part) == f.off.end())
            f.off.push_back(part);
        std::sort(f.off.begin(), f.off.end());
      }
      rewrite(work, t, f);
      out.push_back({id, m, f.off});
    } catch (const DocumentError& e) {
      fail("fcore", "track '" + t.name + "': " + e.what());
    }
  }
  doc = std::move(work);
  return out;
}

void setFCoreKnobs(Document& doc, const std::string& trackId, const FCoreKnobs& knobs) {
  Track& t = trackById(doc, trackId);
  FCoreInstance f = requireFCore(t);
  f.knobs = knobsFrom(json{{"amount", knobs.amount}, {"lengthBeats", knobs.lengthBeats}, {"tone", knobs.tone}, {"space", knobs.space}});
  rewrite(doc, t, f);
}

void setFCoreIntent(Document& doc, const std::string& trackId, const std::string& intent) {
  Track& t = trackById(doc, trackId);
  FCoreInstance f = requireFCore(t);
  const FCoreIntent* info = findFCoreIntent(intent);
  if (!info) fail("fcore.intent", "unknown intent '" + intent + "'");
  f.intent = intent;
  f.knobs.lengthBeats = intent == "intro" ? styleFx(f.style).introBeats : info->defaultBeats;
  f.off = fcoreStartOff(intent, f.style, f.knobs, f.mode);  // another mini plugin: its main step only
  rewrite(doc, t, f);
}

void setFCoreStyle(Document& doc, const std::string& trackId, const std::string& style) {
  Track& t = trackById(doc, trackId);
  FCoreInstance f = requireFCore(t);
  if (!findFCoreStyle(style)) fail("fcore.style", "unknown style '" + style + "'");
  const auto before = planFCore(f.intent, f.style, f.knobs, f.mode).ingredients;
  f.style = style;
  newPartsOff(f, before);
  rewrite(doc, t, f);
}

void setFCoreIngredient(Document& doc, const std::string& trackId, const std::string& ingredient, bool on) {
  Track& t = trackById(doc, trackId);
  FCoreInstance f = requireFCore(t);
  if (fcoreIngredientLabel(f.intent, ingredient).empty()) fail("fcore.ingredient", "no part '" + ingredient + "' in F-Core");
  f.off.erase(std::remove(f.off.begin(), f.off.end(), ingredient), f.off.end());
  if (!on) f.off.push_back(ingredient);
  std::sort(f.off.begin(), f.off.end());
  rewrite(doc, t, f);
}

void setFCorePreset(Document& doc, const std::string& trackId, const std::string& preset) {
  Track& t = trackById(doc, trackId);
  FCoreInstance f = requireFCore(t);
  f.off = fcorePresetOff(f.intent, f.style, f.knobs, f.mode, preset);
  rewrite(doc, t, f);
}

void setFCoreBypass(Document& doc, const std::string& trackId, bool bypass) {
  Track& t = trackById(doc, trackId);
  FCoreInstance f = requireFCore(t);
  f.bypass = bypass;
  rewrite(doc, t, f);
}

void addFCoreRegion(Document& doc, const std::string& trackId, int64_t startTick, int64_t endTick) {
  Track& t = trackById(doc, trackId);
  FCoreInstance f = requireFCore(t);
  if (f.mode != "region") fail("fcore.regions", "the F-Core sampler moves with its notes, not with regions");
  if (startTick < 0 || endTick > doc.lengthTicks || endTick - startTick < kPpq / 4) fail("fcore.regions", "a region is at least 1/16, inside the song");
  for (const auto& r : f.regions)
    if (startTick < r.endTick && r.startTick < endTick) fail("fcore.regions", "the region overlaps another");
  f.regions.push_back({startTick, endTick});
  std::sort(f.regions.begin(), f.regions.end(), [](const FCoreRegion& a, const FCoreRegion& b) { return a.startTick < b.startTick; });
  rewrite(doc, t, f);
}

int removeFCoreRegions(Document& doc, const std::string& trackId, int64_t startTick, int64_t endTick) {
  Track& t = trackById(doc, trackId);
  FCoreInstance f = requireFCore(t);
  const size_t before = f.regions.size();
  if (endTick <= startTick) f.regions.clear();
  else f.regions.erase(std::remove_if(f.regions.begin(), f.regions.end(), [&](const FCoreRegion& r) { return r.startTick < endTick && startTick < r.endTick; }),
                       f.regions.end());
  rewrite(doc, t, f);
  return (int)(before - f.regions.size());
}

void removeFCore(Document& doc, const std::string& trackId) {
  Track& t = trackById(doc, trackId);
  FCoreInstance f = requireFCore(t);
  clearWritten(t, true);
  restoreSampler(t, f);
  t.extra.erase("fcore");
}

std::string fcoreSentence(const Track& track) {
  const auto f = fcoreOf(track);
  return f ? planFCore(f->intent, f->style, f->knobs, f->mode, f->off).sentence : std::string();
}

// ---- samples

std::vector<FCoreSampleChoice> fcoreSamples(const std::vector<LibrarySample>& samples, const std::string& style, double songBpm) {
  const FCoreStyle* st = findFCoreStyle(style);
  if (!st) fail("fcore.style", "unknown style '" + style + "'");
  auto near = [](double a, double b) { return std::fabs(a - b) <= b * 0.02; };
  auto fits = [&](double bpm) {
    for (double x : {bpm, bpm * 2, bpm / 2}) {
      if (songBpm > 0 ? near(x, songBpm) : (x >= st->bpmLo * 0.98 && x <= st->bpmHi * 1.02)) return true;
    }
    return false;
  };
  std::vector<FCoreSampleChoice> first, rest;
  for (size_t i = 0; i < samples.size(); ++i) {
    const SampleInfo& info = samples[i].info;
    FCoreSampleChoice c;
    c.index = i;
    const bool tagged = std::find(info.styles.begin(), info.styles.end(), style) != info.styles.end();
    if (info.loop) {
      if (info.bpm <= 0) c.warning = "a loop with no known BPM";
      else if (!fits(info.bpm))
        c.warning = songBpm > 0 ? fmt("loop at %.4g BPM, the song is at ", info.bpm) + fmt("%.4g BPM", songBpm)
                                : fmt("loop at %.4g BPM, ", info.bpm) + st->label + fmt(" asks for %.0f", st->bpmLo) + fmt("-%.0f BPM", st->bpmHi);
    }
    c.forStyle = c.warning.empty() && (tagged || (info.loop && info.bpm > 0));
    (c.forStyle ? first : rest).push_back(c);
  }
  first.insert(first.end(), rest.begin(), rest.end());
  return first;
}

}  // namespace core::doc
