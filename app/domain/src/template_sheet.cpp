#include "template_sheet.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>
#include "edit.hpp"

namespace core::doc {

namespace {

constexpr int64_t kBarTicks = 4 * kPpq;

std::string num(double v) {
  if (!std::isfinite(v)) return "?";
  char buf[48];
  // 2 decimals at most, no exponent: 20000 Hz, 0.707, 251.9 ms.
  if (std::fabs(v) >= 1000) std::snprintf(buf, sizeof buf, "%.0f", v);
  else std::snprintf(buf, sizeof buf, "%.4g", v);
  return buf;
}

std::string bars(int64_t ticks) {  // 1-based bar and beat, as on the ruler: 9.1 = bar 9, beat 1
  const int64_t bar = ticks / kBarTicks + 1, beat = (ticks % kBarTicks) / kPpq + 1;
  return std::to_string(bar) + "." + std::to_string(beat);
}

std::string length(int64_t ticks) { return num((double)ticks / kBarTicks) + " compassos"; }

const char* kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
std::string noteName(int pitch) { return std::string(kNoteNames[((pitch % 12) + 12) % 12]) + std::to_string(pitch / 12 - 1); }

std::string withUnit(double v, const char* unit) { return num(v) + (unit && *unit ? std::string(" ") + unit : std::string()); }

// A W-Core layer 2-4 parameter ("l3_...") of a layer that is off is left out (it sounds nowhere).
bool layerOff(const Track& t, const std::string& name) {
  if (name.size() < 4 || name[0] != 'l' || name[2] != '_' || name[1] < '2' || name[1] > '4') return false;
  const std::string onKey = name.substr(0, 3) + "on";
  if (name == onKey) return false;
  auto it = t.instrument.params.find(onKey);
  return it == t.instrument.params.end() || it->second < 0.5;
}

void instrumentPart(std::ostringstream& o, const Track& t) {
  const Instrument& ins = t.instrument;
  const InstrumentInfo* info = ins.kind == "builtin" ? findInstrumentInfo(ins.name) : nullptr;
  o << "- Instrumento: " << (info ? info->label : ins.name.empty() ? ins.kind : ins.name) << " (`" << ins.kind;
  if (!ins.name.empty()) o << ":" << ins.name;
  o << "`, versão " << ins.version << ")\n";
  if (!ins.samples.empty()) {
    o << "- Samples:\n";
    for (const auto& s : ins.samples) {
      o << "  - " << (s.name.empty() ? s.file : s.name) << ": `" << s.file << "`, raiz " << noteName(s.rootKey) << ", teclas "
        << noteName(s.loKey) << "–" << noteName(s.hiKey);
      if (s.loops()) o << ", loop " << num(s.loopStart) << "–" << num(s.loopEnd) << " s";
      if (!s.sha256.empty()) o << ", sha256 " << s.sha256;
      o << "\n";
    }
  }
  if (info) {
    o << "\n| Parâmetro | Valor | Padrão | Faixa |\n|---|---|---|---|\n";
    for (const auto& p : info->params) {
      if (layerOff(t, p.name)) continue;
      const double v = instrumentParamValue(t, p);
      const bool set = ins.params.count(p.name) && std::fabs(v - p.def) > 1e-9;
      o << "| " << p.label << " (`" << p.name << "`) | " << (set ? "**" : "") << withUnit(v, p.unit) << (set ? "**" : "") << " | "
        << withUnit(p.def, p.unit) << " | " << num(p.min) << "–" << num(p.max) << " |\n";
    }
    // Values the catalogue does not know (newer app, a hand edit): listed as they are.
    for (const auto& [k, v] : ins.params)
      if (!findInstrumentParam(t, k)) o << "| `" << k << "` | " << num(v) << " | | |\n";
  } else if (!ins.params.empty()) {
    o << "\n| Parâmetro | Valor |\n|---|---|\n";
    for (const auto& [k, v] : ins.params) o << "| `" << k << "` | " << num(v) << " |\n";
  }
  if (!ins.synth.is_null() && !(ins.synth.is_object() && ins.synth.empty()))
    o << "\nModulação (W-Core):\n\n```json\n" << ins.synth.dump() << "\n```\n";
}

void effectsPart(std::ostringstream& o, const json& effects) {
  if (!effects.is_array() || effects.empty()) return;
  o << "\nEfeitos (pela ordem da cadeia):\n";
  for (const auto& fx : effects) {
    const std::string name = effectName(fx);
    const EffectInfo* info = findEffect(name);
    o << "\n- **" << (info ? info->label : name.c_str()) << "** (`" << name << "`, id `" << fx.value("id", std::string()) << "`)"
      << (effectBypassed(fx) ? " DESLIGADO" : "") << "\n";
    if (info) {
      o << "\n  | Parâmetro | Valor | Padrão |\n  |---|---|---|\n";
      for (const auto& p : info->params) {
        const double v = effectParam(fx, p.name, p.def);
        std::string shown = withUnit(v, p.unit);
        if (p.step == 1 && !p.choices.empty()) {
          const long i = std::lround(v - p.min);
          if (i >= 0 && i < (long)p.choices.size()) shown += " (" + std::string(p.choices[(size_t)i]) + ")";
        }
        const bool set = std::fabs(v - p.def) > 1e-9;
        o << "  | " << p.label << " (`" << p.name << "`) | " << (set ? "**" : "") << shown << (set ? "**" : "") << " | "
          << withUnit(p.def, p.unit) << " |\n";
      }
    } else if (fx.contains("params") && fx["params"].is_object()) {
      o << "\n  | Parâmetro | Valor |\n  |---|---|\n";
      for (const auto& [k, v] : fx["params"].items()) o << "  | `" << k << "` | " << v.dump() << " |\n";
    }
  }
}

void automationPart(std::ostringstream& o, const std::vector<Automation>& list) {
  if (list.empty()) return;
  o << "\nAutomação:\n\n| Parâmetro | Pontos | Mín. | Máx. | Começa | Acaba |\n|---|---|---|---|---|---|\n";
  for (const auto& a : list) {
    if (a.points.empty()) continue;
    double lo = a.points.front().value, hi = lo;
    for (const auto& p : a.points) { lo = std::min(lo, p.value); hi = std::max(hi, p.value); }
    o << "| `" << a.param << "` | " << a.points.size() << " | " << num(lo) << " | " << num(hi) << " | " << num(a.points.front().value)
      << " em " << bars(a.points.front().tick) << " | " << num(a.points.back().value) << " em " << bars(a.points.back().tick) << " |\n";
  }
}

void notesPart(std::ostringstream& o, const Track& t) {
  int64_t count = 0;
  int lo = 127, hi = 0;
  for (const auto& c : t.clips) {
    const Pattern* p = t.findPattern(c.patternId);
    if (!p || p->lengthTicks <= 0) continue;
    for (int64_t at = -c.offsetTicks; at < c.lengthTicks; at += p->lengthTicks)
      for (const auto& n : p->notes)
        if (at + n.tick >= 0 && at + n.tick < c.lengthTicks) { ++count; lo = std::min(lo, n.pitch); hi = std::max(hi, n.pitch); }
  }
  if (count) o << "- Notas: " << count << ", de " << noteName(lo) << " a " << noteName(hi) << ", em " << t.clips.size() << " clips e "
               << t.patterns.size() << " padrões\n";
  if (!t.audioClips.empty()) o << "- Clips de áudio: " << t.audioClips.size() << " (o template não leva áudio)\n";
}

} // namespace

std::string templateCompanionPath(const std::string& templateFile, const std::string& ending) {
  const std::string ext = kTemplateExtension;
  std::string base = templateFile;
  if (base.size() >= ext.size() && base.compare(base.size() - ext.size(), ext.size(), ext) == 0) base.resize(base.size() - ext.size());
  return base + ending;
}

std::string templateSheet(const SongTemplate& t) {
  const Document& d = t.document;
  std::ostringstream o;
  o << "# " << t.info.name << "\n\n";
  o << "Ficha do template `" << t.info.id << "` (versão " << t.info.version << "). Estilo: " << (t.info.style.empty() ? "—" : t.info.style) << ".\n";
  if (!t.info.description.empty()) o << "\n" << t.info.description << "\n";
  if (t.info.source.contains("from"))
    o << "\nFeito a partir de \"" << t.info.source.value("from", std::string()) << "\", do tick " << t.info.source.value("startTick", (int64_t)0)
      << " ao " << t.info.source.value("endTick", (int64_t)0) << " (compassos " << bars(t.info.source.value("startTick", (int64_t)0)) << " a "
      << bars(t.info.source.value("endTick", (int64_t)0)) << ").\n";

  o << "\n## Tempo e forma\n\n- BPM: " << num(d.bpm) << "\n- Duração: " << length(d.lengthTicks) << " (" << d.lengthTicks << " ticks, "
    << kPpq << " por semínima)\n";
  for (const auto& p : d.tempoMap) o << "- Tempo " << num(p.bpm) << " BPM em " << bars(p.tick) << (p.ramp ? " (em rampa)" : "") << "\n";
  if (!d.sections.empty()) {
    o << "\n| Secção | Começa | Duração |\n|---|---|---|\n";
    for (const auto& s : d.sections) o << "| " << s.name << " | " << bars(s.startTick) << " | " << length(s.lengthTicks) << " |\n";
  }

  o << "\n## Master\n\n- Ganho: " << num(d.masterGainDb) << " dB\n";
  effectsPart(o, d.masterEffects);
  automationPart(o, d.masterAutomation);

  o << "\n## Pistas\n";
  for (const auto& tr : d.tracks) {
    o << "\n### " << tr.name << " (`" << tr.id << "`)\n\n";
    instrumentPart(o, tr);
    o << "\n- Mistura: ganho " << num(tr.mix.gainDb) << " dB, pan " << num(tr.mix.pan) << (tr.mix.mute ? ", mudo" : "")
      << (tr.mix.output.empty() ? "" : ", saída para `" + tr.mix.output + "`") << "\n";
    for (const auto& s : tr.mix.sends)
      o << "- Envio para `" << s.to << "`: " << num(s.gainDb) << " dB" << (s.pre ? " (antes do fader)" : "") << "\n";
    notesPart(o, tr);
    effectsPart(o, tr.effects);
    if (tr.modulators.is_array() && !tr.modulators.empty()) {
      o << "\nLFOs da pista:\n\n| Parâmetro | Forma | Batidas | Profundidade | Fase |\n|---|---|---|---|---|\n";
      for (const auto& m : tr.modulators)
        o << "| `" << m.value("param", std::string()) << "` | " << m.value("shape", std::string()) << " | " << num(m.value("rateBeats", 0.0))
          << " | " << num(m.value("depth", 0.0)) << " | " << num(m.value("phase", 0.0)) << " |\n";
    }
    automationPart(o, tr.automation);
  }
  return o.str();
}

} // namespace core::doc
