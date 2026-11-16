// The synth of Core's AI (Diogo, 2026-11-16): Vital, one synth for every instrument role the AI uses (bass, lead, pad,
// pluck). AICORE's pipeline (Vita) makes Vital presets from a reference, so the AI and AICORE speak the same synth.
// Vital cannot ship inside Core's package (its licence does not allow it), so the installer and the app only look
// for it where its own installer puts it, and say where to get it when it is not there.
// Standard C++ only (no JUCE, no JSON): the installer includes this header too.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace core::ai {

struct Synth {
  const char* name = "Vital";
  const char* vendor = "Vital Audio";
  const char* file = "Vital.vst3";
  const char* download = "https://vital.audio";
  std::vector<std::string> roles{"bass", "lead", "pad", "pluck"};  // the instrument roles it plays by default
};
inline const Synth& synth() {
  static const Synth s;
  return s;
}

// Where VST3 plugins go on Windows: the system folder (%CommonProgramFiles%\VST3, Vital's installer default) and the
// user's (%LOCALAPPDATA%\Programs\Common\VST3). Empty variables are skipped.
inline std::vector<std::filesystem::path> vst3Folders() {
  std::vector<std::filesystem::path> out;
  if (const char* c = std::getenv("CommonProgramFiles"); c && *c) out.push_back(std::filesystem::path(c) / "VST3");
  if (const char* l = std::getenv("LOCALAPPDATA"); l && *l) out.push_back(std::filesystem::path(l) / "Programs" / "Common" / "VST3");
  return out;
}

// The synth's plugin in the first folder that has it (a .vst3 bundle folder or file), or none.
inline std::optional<std::filesystem::path> findSynth(const std::vector<std::filesystem::path>& folders = vst3Folders()) {
  for (const auto& f : folders) {
    std::error_code ec;
    const auto p = f / synth().file;
    if (std::filesystem::exists(p, ec)) return p;
  }
  return std::nullopt;
}

// The plugins the AI uses (Diogo, 2026-11-16): open source only, so they can later be forked into the AI's own
// tools. Kept few on purpose: one synth, one sample player, one reverb, one effects plugin. Each is a separate VST3;
// Core only loads them, never links their code (GPL stays theirs).
struct AiPlugin {
  std::string name, file, download, licence;
};
// The default role of each (Settings > Roles, roles.hpp). `match`: a word of the scanned plugin's name (case
// ignored). `effect`: for Airwindows Consolidated, the effect to choose in it for that role (one plugin, many effects).
struct AiRole {
  const char* role;
  const char* match;
  const char* effect;
};
inline const std::vector<AiRole>& aiRoles() {
  static const std::vector<AiRole> r = {{"drums", "sfizz", ""},
                                        {"reverb", "dragonfly room", ""},
                                        {"delay", "airwindows consolidated", "TapeDelay2"},
                                        {"filter", "airwindows consolidated", "Capacitor2"},
                                        {"drive", "airwindows consolidated", "Drive"},
                                        {"sidechain", "airwindows consolidated", "PurestGain"}};
  return r;
}
inline std::vector<AiPlugin> aiPlugins() {
  return {{synth().name, synth().file, synth().download, "GPL-3"},
          {"sfizz", "sfizz.vst3", "https://sfz.tools/sfizz/downloads", "BSD-2"},
          {"Dragonfly Room Reverb", "DragonflyRoomReverb.vst3", "https://michaelwillis.github.io/dragonfly-reverb/", "GPL-3"},
          {"Airwindows Consolidated", "Airwindows Consolidated.vst3", "https://github.com/baconpaul/airwin2rack/releases", "MIT"}};
}
// Where a plugin of the AI is on this computer, or none.
inline std::optional<std::filesystem::path> findAiPlugin(const AiPlugin& p, const std::vector<std::filesystem::path>& folders = vst3Folders()) {
  for (const auto& f : folders) {
    std::error_code ec;
    if (std::filesystem::exists(f / p.file, ec)) return f / p.file;
  }
  return std::nullopt;
}

}  // namespace core::ai
