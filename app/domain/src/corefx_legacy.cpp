#include "corefx_legacy.hpp"
#include "edit.hpp"
#include <algorithm>
#include <cmath>

namespace core::doc {

namespace {

const char* moduleOf(const std::string& name) {
  if (name == "saturation") return "sat";
  if (name == "reverb" || name == "space") return "rev";
  if (name == "delay" || name == "echo" || name == "spiral") return "del";
  if (name == "flux") return "mod";
  return nullptr;
}

// The old effects' defaults (their parameter catalogues before Core FX), so a value left out sounds as before.
json oldDefaults(const std::string& name) {
  if (name == "saturation") return {{"drive", 6}, {"mix", 1}, {"output", 0}};
  if (name == "delay") return {{"time", 0.75}, {"feedback", 0.35}, {"mix", 0.25}, {"pingPong", 0}, {"tone", 0.6}};
  if (name == "reverb") return {{"size", 0.6}, {"damping", 0.5}, {"mix", 0.2}, {"width", 1}, {"preDelay", 10}};
  if (name == "space") return {{"size", 0.6}, {"decay", 2}, {"predelay", 20}, {"tone", 7000}, {"lowcut", 300}, {"motion", 0.3}, {"mix", 0.25}, {"shimmer", 0}};
  if (name == "echo") return {{"time", 2}, {"feedback", 0.4}, {"tone", 5000}, {"lowcut", 250}, {"pingpong", 0.5}, {"duck", 0}, {"mix", 0.25}};
  if (name == "spiral") return {{"time", 1}, {"shift", 8}, {"feedback", 0.6}, {"tone", 5000}, {"lowcut", 300}, {"mix", 0.25}};
  if (name == "flux") return {{"mode", 0}, {"rate", 0.5}, {"depth", 0.6}, {"feedback", 0.4}, {"mix", 0.5}};
  return json::object();
}

constexpr double kOldReverbGainDb = 2.0;

double num(const json& j, const char* k, double def) { return j.contains(k) && j[k].is_number() ? j[k].get<double>() : def; }

// The note value (ECHO's eight: 1/16 .. 1 bar) nearest to a time in beats.
double timeIndex(double beats) {
  static const double b[8] = {0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0};
  int best = 0;
  for (int i = 1; i < 8; ++i) if (std::fabs(std::log2(beats / b[i])) < std::fabs(std::log2(beats / b[best]))) best = i;
  return best;
}
// Modulate's rate (1/16 .. 4 bars) nearest to a rate in Hz at the song's tempo.
double rateIndex(double hz, double bpm) {
  static const double beats[7] = {0.25, 0.5, 1, 2, 4, 8, 16};
  const double cycle = bpm / 60.0 / std::max(0.01, hz);  // beats per cycle
  int best = 0;
  for (int i = 1; i < 7; ++i) if (std::fabs(std::log2(cycle / beats[i])) < std::fabs(std::log2(cycle / beats[best]))) best = i;
  return best;
}
// A region between two frequencies (a low cut and a tone): centre and width in octaves; the whole range
// (width 8) when it is open at both ends.
void region(json& out, const std::string& m, double lo, double hi) {
  lo = std::clamp(lo, 20.0, 20000.0);
  hi = std::clamp(hi, lo * 1.25, 20000.0);
  if (lo <= 40 && hi >= 15000) { out[m + "R1Freq"] = 630.0; out[m + "R1Width"] = 8.0; return; }
  out[m + "R1Freq"] = std::sqrt(lo * hi);
  out[m + "R1Width"] = std::clamp(std::log2(hi / lo), 0.3, 7.9);
}

}  // namespace

bool isLegacyEffect(const std::string& name) { return moduleOf(name) != nullptr; }

json coreFxFrom(const std::string& name, const json& given, double bpm, bool mixMoves) {
  const EffectInfo* info = findEffect("corefx");
  json out = json::object();
  if (info) for (const auto& p : info->params) out[p.name] = p.def;
  const char* mod = moduleOf(name);
  if (!mod) return out;
  json v = oldDefaults(name);
  if (given.is_object()) for (const auto& [k, x] : given.items()) if (x.is_number()) v[k] = x;
  const std::string m = mod;
  for (const char* o : {"sat", "rev", "del", "mod", "lofi"}) out[std::string(o) + "On"] = m == o ? 1 : 0;
  out[m + "R1Amount"] = 1.0;
  out[m + "Return"] = 0.0;
  out[m + "Fit"] = 0;
  out["limiter"] = 0;
  const double mix = std::clamp(num(v, "mix", 1), 0.0, 1.0);
  if (mixMoves) {  // a lane or an attachment moves the mix: Amount follows it, the dry sound stays
    out["amount"] = mix;
    out["fxOnly"] = 0;  // the dry stays whole: fxOnly never moves, so 1 here silenced the track wherever Amount is 0 (sweep PL2)
  } else {
    // The old effect's own mix law, exactly: dry * D + effect * W. Core FX plays Output * (dry + Amount * Return *
    // effect), so Output = D and Amount * Return = W / D (Return up to +6 dB); no dry at all is FX Only.
    double dry = 1.0 - mix, wet = mix;                                              // a crossfade (FLUX, the old ones)
    if (name == "space" || name == "echo" || name == "spiral") dry = std::min(1.0, 2.0 * (1.0 - mix));  // the dry stays to the middle
    if (name == "delay") dry = 1.0;                                                // the dry always whole
    if (dry < 1e-3) {
      out["fxOnly"] = 1;
      out["amount"] = wet;
    } else {
      const double ratio = wet / dry;
      out["fxOnly"] = 0;
      out["output"] = std::clamp(20.0 * std::log10(dry), -24.0, 6.0);
      out["amount"] = std::min(1.0, ratio);
      if (ratio > 1.0) out[m + "Return"] = std::min(24.0, 20.0 * std::log10(ratio));
    }
  }
  region(out, m, 20, 20000);
  if (name == "saturation") {
    out["satType"] = 0;
    out["satDrive"] = num(v, "drive", 6);
    out["output"] = std::clamp(out.value("output", 0.0) + num(v, "output", 0), -24.0, 6.0);
    out["satTone"] = 1.0;

  } else if (name == "space" || name == "reverb") {
    if (name == "space") {
      // SPACE's types become Core FX reverb types: Plate 3, Spring 4, Reverse 5, Cathedral 6 (Stone church). Air has no Core FX knob.
      static const int fromKind[5] = {-1, 3, 4, 6, 5};
      const int kind = (int)std::lround(std::clamp(num(v, "kind", 0), 0.0, 4.0));
      out["revType"] = kind > 0 ? fromKind[kind] : num(v, "shimmer", 0) > 0.3 ? 2 : num(v, "size", 0.6) >= 0.85 ? 1 : 0;
      out["revDecay"] = std::clamp(num(v, "decay", 2), 0.2, 20.0);
      out["revPreDelay"] = std::clamp(num(v, "predelay", 20), 0.0, 250.0);
      region(out, m, num(v, "lowcut", 300), num(v, "tone", 7000) * 1.5);
    } else {
      const double size = std::clamp(num(v, "size", 0.6), 0.0, 1.0);
      out["revType"] = size >= 0.85 ? 1 : 0;
      out["revDecay"] = 0.4 + 6.0 * size * size;
      out["revPreDelay"] = std::clamp(num(v, "preDelay", 10), 0.0, 250.0);
      region(out, m, 20, 20000.0 * (1.0 - 0.8 * std::clamp(num(v, "damping", 0.5), 0.0, 1.0)));
      // The old reverb's tail was louder than SPACE's at the same mix (measured on the templates' hats and impacts).
      out["revReturn"] = std::clamp(out.value("revReturn", 0.0) + kOldReverbGainDb, -24.0, 24.0);
    }
  } else if (name == "echo" || name == "spiral") {
    out["delType"] = name == "spiral" ? 2 : num(v, "pingpong", 0.5) >= 0.5 ? 0 : 1;
    out["delTime"] = std::clamp(std::round(num(v, "time", 2)), 0.0, 7.0);
    out["delFeedback"] = std::clamp(num(v, "feedback", 0.4), 0.0, 0.95);
    region(out, m, num(v, "lowcut", 250), num(v, "tone", 5000) * 1.5);
  } else if (name == "delay") {
    out["delType"] = num(v, "pingPong", 0) >= 0.5 ? 0 : 1;
    out["delTime"] = timeIndex(num(v, "time", 0.75));
    out["delFeedback"] = std::clamp(num(v, "feedback", 0.35), 0.0, 0.95);
    region(out, m, 20, 500 + 15000 * std::clamp(num(v, "tone", 0.6), 0.0, 1.0));
  } else if (name == "flux") {
    out["modType"] = std::clamp(std::round(num(v, "mode", 0)), 0.0, 2.0);
    out["modDepth"] = std::clamp(num(v, "depth", 0.6), 0.0, 1.0);
    out["modRate"] = rateIndex(num(v, "rate", 0.5), bpm);
  }
  return out;
}

std::optional<CoreFxParamMap> coreFxParamOf(const std::string& name, const std::string& param, double bpm) {
  const char* mod = moduleOf(name);
  if (!mod) return std::nullopt;
  const std::string m = mod;
  const auto same = [](double x) { return x; };
  if (param == "mix") return CoreFxParamMap{"amount", [](double x) { return std::clamp(x, 0.0, 1.0); }};
  if (name == "saturation") {
    if (param == "drive") return CoreFxParamMap{"satDrive", same};
    if (param == "output") return CoreFxParamMap{"output", [](double x) { return std::clamp(x, -24.0, 6.0); }};
  }
  if (name == "space") {
    if (param == "decay") return CoreFxParamMap{"revDecay", [](double x) { return std::clamp(x, 0.2, 20.0); }};
    if (param == "predelay") return CoreFxParamMap{"revPreDelay", same};
    if (param == "tone") return CoreFxParamMap{"revR1Freq", [](double x) { return std::clamp(x * 0.5, 20.0, 20000.0); }, true};
  }
  if (name == "reverb") {
    if (param == "size") return CoreFxParamMap{"revDecay", [](double x) { return 0.4 + 6.0 * x * x; }};
    if (param == "preDelay") return CoreFxParamMap{"revPreDelay", same};
  }
  if (name == "echo" || name == "spiral") {
    if (param == "time") return CoreFxParamMap{"delTime", [](double x) { return std::clamp(std::round(x), 0.0, 7.0); }};
    if (param == "feedback") return CoreFxParamMap{"delFeedback", [](double x) { return std::clamp(x, 0.0, 0.95); }};
    if (param == "tone") return CoreFxParamMap{"delR1Freq", [](double x) { return std::clamp(x * 0.5, 20.0, 20000.0); }, true};
  }
  if (name == "delay") {
    if (param == "time") return CoreFxParamMap{"delTime", [](double x) { return timeIndex(x); }};
    if (param == "feedback") return CoreFxParamMap{"delFeedback", [](double x) { return std::clamp(x, 0.0, 0.95); }};
  }
  if (name == "flux") {
    if (param == "depth") return CoreFxParamMap{"modDepth", same};
    if (param == "mode") return CoreFxParamMap{"modType", [](double x) { return std::clamp(std::round(x), 0.0, 2.0); }};
    if (param == "rate") return CoreFxParamMap{"modRate", [bpm](double x) { return rateIndex(x, bpm); }};
  }
  return std::nullopt;
}

std::pair<std::string, double> coreFxAttachSwitch(const std::string& name, double on, json& params) {
  const char* module = moduleOf(name);
  if (!module) return {"amount", on};
  params["amount"] = std::clamp(on, 0.0, 1.0);
  params["fxOnly"] = 0;
  params[std::string(module) + "On"] = 0;
  return {std::string(module) + "On", 1.0};
}

namespace {
// The five-knob EQ (version 1) becomes the Core EQ (version 2): a rename only, bands 1-5 are the same filters.
const char* coreEqParamOf(const std::string& old) {
  static const std::pair<const char*, const char*> map[] = {
      {"highPass", "b1Freq"}, {"lowGain", "b2Gain"}, {"lowFreq", "b2Freq"}, {"midGain", "b3Gain"}, {"midFreq", "b3Freq"},
      {"midQ", "b3Q"}, {"highGain", "b4Gain"}, {"highFreq", "b4Freq"}, {"lowPass", "b5Freq"}};
  for (const auto& [o, n] : map) if (old == o) return n;
  return nullptr;
}

bool convertEq(json& fx, std::vector<Automation>& lanes, json* modulators) {
  if (fx.value("name", "") != "eq" || (fx.contains("version") && fx["version"].is_number() && fx["version"].get<double>() >= 2)) return false;
  const EffectInfo* info = findEffect("eq");
  json out = json::object();
  if (info) for (const auto& p : info->params) out[p.name] = p.def;
  const json given = fx.value("params", json::object());
  if (given.is_object())
    for (const auto& [k, x] : given.items()) if (const char* n = coreEqParamOf(k); n && x.is_number()) out[n] = x;
  fx["params"] = out;
  fx["version"] = 2;
  const std::string id = fx.value("id", "");
  if (id.empty()) return true;
  const std::string prefix = "fx:" + id + ":";
  auto rename = [&](std::string& param) {
    if (param.rfind(prefix, 0) != 0) return;
    if (const char* n = coreEqParamOf(param.substr(prefix.size()))) param = prefix + n;
  };
  for (auto& l : lanes) rename(l.param);
  if (modulators && modulators->is_array())
    for (auto& md : *modulators)
      if (md.is_object() && md.contains("param") && md["param"].is_string()) {
        std::string p = md["param"].get<std::string>();
        rename(p);
        md["param"] = p;
      }
  if (fx.contains("attach") && fx["attach"].is_object() && fx["attach"].contains("param") && fx["attach"]["param"].is_string())
    if (const char* n = coreEqParamOf(fx["attach"]["param"].get<std::string>())) fx["attach"]["param"] = n;
  return true;
}

// One chain: each old effect becomes a Core FX; its lanes and modulators follow it.
int convertChain(json& chain, std::vector<Automation>& lanes, json* modulators, double bpm, const std::string& where,
                 std::vector<std::string>* notes) {
  if (!chain.is_array()) return 0;
  const EffectInfo* info = findEffect("corefx");
  int n = 0;
  for (auto& fx : chain) {
    if (!fx.is_object() || fx.value("type", "") != "builtin") continue;
    if (convertEq(fx, lanes, modulators)) { ++n; continue; }
    const std::string name = fx.value("name", "");
    if (!isLegacyEffect(name)) continue;
    const std::string id = fx.value("id", "");
    bool mixMoves = fx.contains("attach");
    for (const auto& l : lanes) mixMoves |= !id.empty() && l.param == "fx:" + id + ":mix";
    if (modulators && modulators->is_array())
      for (const auto& md : *modulators) mixMoves |= !id.empty() && md.is_object() && md.value("param", "") == "fx:" + id + ":mix";
    fx["params"] = coreFxFrom(name, fx.value("params", json::object()), bpm, mixMoves);
    fx["name"] = "corefx";
    fx["version"] = info ? info->version : 1;
    ++n;
    // An attachment switches its effect on through one parameter (0 off, "on" when it plays).
    const std::string prefix = "fx:" + id + ":";
    std::string switchLane;  // the attachment's lane, already moved to the module's On
    if (fx.contains("attach") && fx["attach"].is_object() && fx["attach"].contains("param") && fx["attach"]["param"].is_string()) {
      auto& a = fx["attach"];
      const std::string oldParam = a["param"].get<std::string>();
      const double level = a.contains("on") && a["on"].is_number() ? a["on"].get<double>() : 1.0;
      const auto [param, on] = coreFxAttachSwitch(name, level, fx["params"]);
      a["param"] = param;
      a["on"] = on;
      a.erase("tail");  // a module switched off always lets its tail ring out
      // Its curve: where the sound went in (Input, when the tail rang out) or else where the Mix was up.
      const std::string inKey = prefix + "input", mixKey = prefix + oldParam;
      bool hasInput = false;
      for (const auto& l : lanes) hasInput |= !id.empty() && l.param == inKey;
      switchLane = prefix + param;
      for (auto it = lanes.begin(); it != lanes.end();) {
        if (id.empty() || (it->param != inKey && it->param != mixKey)) { ++it; continue; }
        if (it->param == (hasInput ? inKey : mixKey)) {
          it->param = switchLane;
          for (auto& p : it->points) p.value = p.value > 1e-9 ? on : 0.0;
          ++it;
        } else {
          it = lanes.erase(it);
        }
      }
    }
    for (auto it = lanes.begin(); it != lanes.end();) {
      if (id.empty() || it->param.rfind(prefix, 0) != 0 || it->param == switchLane) { ++it; continue; }
      const std::string old = it->param.substr(prefix.size());
      const auto map = coreFxParamOf(name, old, bpm);
      if (!map) {
        if (notes) notes->push_back(where + ": the automation of " + name + " " + old + " has no Core FX counterpart and was removed");
        it = lanes.erase(it);
        continue;
      }
      it->param = prefix + map->param;
      for (auto& p : it->points) p.value = map->value(p.value);
      ++it;
    }
    if (modulators && modulators->is_array())
      for (auto it = modulators->begin(); it != modulators->end();) {
        const std::string param = it->is_object() ? it->value("param", "") : "";
        if (id.empty() || param.rfind(prefix, 0) != 0) { ++it; continue; }
        const std::string old = param.substr(prefix.size());
        // A note value is not a continuous scale: a modulated time or rate has nothing to move in Core FX.
        const auto map = old == "time" || old == "rate" ? std::nullopt : coreFxParamOf(name, old, bpm);
        if (!map) {
          if (notes) notes->push_back(where + ": the modulation of " + name + " " + old + " has no Core FX counterpart and was removed");
          it = modulators->erase(it);
          continue;
        }
        (*it)["param"] = prefix + map->param;
        ++it;
      }
  }
  return n;
}
}  // namespace

int convertLegacyEffects(Document& doc, std::vector<std::string>* notes) {
  int n = convertChain(doc.masterEffects, doc.masterAutomation, nullptr, doc.bpm, "Master", notes);
  for (auto& t : doc.tracks) n += convertChain(t.effects, t.automation, &t.modulators, doc.bpm, t.name, notes);
  return n;
}

}  // namespace core::doc
