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

}  // namespace core::ai
