#include "bitwig.hpp"
#include <algorithm>
#include <cstdio>
#include "edit.hpp"

namespace core::doc {

std::string safeFilePart(const std::string& text) {
  std::string out;
  for (unsigned char c : text) {
    if (c < 32 || c == 127 || std::string("\\/:*?\"<>|").find((char)c) != std::string::npos) out += '_';
    else out += (char)c;
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();  // Windows drops them
  if (out.size() > 60) {
    size_t cut = 60;
    while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) --cut;  // not inside a character
    out.resize(cut);
  }
  return out.empty() ? std::string("_") : out;
}

namespace {
std::string twoDigits(int n) {
  char b[8];
  std::snprintf(b, sizeof b, "%02d", n);
  return b;
}
std::string pluginName(const json& plugin) {
  const std::string n = plugin.value("name", std::string());
  return n.empty() ? plugin.value("id", std::string("plugin")) : n;
}
// The plugin effects of a chain, as presets named "<prefix> - fx<n> <plugin>.vstpreset".
void effectPresets(const json& chain, const std::string& owner, const std::string& prefix, std::vector<BitwigPreset>& out) {
  int n = 0;
  for (const auto& fx : chain) {
    if (!fx.is_object() || fx.value("type", "") != "plugin" || !fx.contains("plugin")) continue;
    ++n;
    const std::string id = fx.value("id", std::string());
    out.push_back({pluginKey(owner, id), prefix + " - fx" + std::to_string(n) + " " + safeFilePart(pluginName(fx["plugin"])) + ".vstpreset",
                   "effect", fx["plugin"]});
  }
}
}  // namespace

BitwigPlan bitwigPlan(const Document& doc, const MidiExportOptions& options) {
  BitwigPlan plan;
  plan.midiFile = safeFilePart(doc.name.empty() ? std::string("song") : doc.name) + ".mid";
  // The MIDI tracks: the same choice as songToMidi (its order is the order of doc.tracks).
  const MidiFile midi = songToMidi(doc, options);
  size_t nextMidi = 0;
  int number = 0;
  for (const auto& t : doc.tracks) {
    BitwigTrack bt;
    bt.trackId = t.id;
    bt.name = t.name;
    if (nextMidi < midi.tracks.size() && t.instrument.kind != "audio" && t.instrument.kind != "bus" &&
        (!t.mix.mute || options.mutedTracks) &&
        (options.onlyTracks.empty() || std::find(options.onlyTracks.begin(), options.onlyTracks.end(), t.id) != options.onlyTracks.end()))
      bt.midiTrack = (int)++nextMidi;
    const std::string prefix = twoDigits(++number) + " " + safeFilePart(t.name);
    if (t.instrument.kind == "plugin")
      bt.presets.push_back({pluginKey(t.id), prefix + " - " + safeFilePart(pluginName(t.instrument.plugin)) + ".vstpreset", "instrument", t.instrument.plugin});
    effectPresets(t.effects, t.id, prefix, bt.presets);
    if (bt.midiTrack > 0 || !bt.presets.empty()) plan.tracks.push_back(std::move(bt));
  }
  effectPresets(doc.masterEffects, "", "Master", plan.master);
  return plan;
}

std::string bitwigReadme(const Document& doc, const BitwigPlan& plan, const std::vector<std::string>& missing) {
  auto isMissing = [&](const std::string& key) { return std::find(missing.begin(), missing.end(), key) != missing.end(); };
  auto line = [&](const BitwigPreset& p) {
    std::string s = "    " + std::string(p.role == "instrument" ? "Instrument: " : "Effect: ") + pluginName(p.plugin);
    const std::string vendor = p.plugin.value("vendor", std::string());
    if (!vendor.empty()) s += " (" + vendor + ")";
    s += isMissing(p.key) ? "  -> no preset (the plugin was not loaded or is not VST3): choose it by hand\n" : "  -> " + p.file + "\n";
    return s;
  };
  char bpm[32];
  std::snprintf(bpm, sizeof bpm, "%g", doc.bpm);
  std::string out = "Core -> Bitwig: " + (doc.name.empty() ? std::string("song") : doc.name) + "\n\n";
  out += "1. Drag " + plan.midiFile + " into the Bitwig: one track per MIDI track below, in this order (the tempo";
  out += doc.tempoMap.empty() ? std::string(" is ") + bpm + " BPM).\n" : std::string(" starts at ") + bpm + " BPM and changes: it is in the file).\n";
  out += "2. On each track drop its .vstpreset: the same plugin opens with the same sound.\n\n";
  for (const auto& t : plan.tracks) {
    out += t.midiTrack > 0 ? "MIDI track " + std::to_string(t.midiTrack) + ": " + t.name + "\n" : "No notes: " + t.name + "\n";
    for (const auto& p : t.presets) out += line(p);
  }
  if (!plan.master.empty()) {
    out += "Master\n";
    for (const auto& p : plan.master) out += line(p);
  }
  return out;
}

}  // namespace core::doc
