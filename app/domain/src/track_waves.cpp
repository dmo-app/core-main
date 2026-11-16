#include "track_waves.hpp"
#include "edit.hpp"

namespace core::doc {

std::vector<std::string> tracksForWaves(const Document& doc) {
  std::vector<std::string> out;
  for (const auto& t : doc.tracks) {
    if (!playsPatterns(t)) continue;
    for (const auto& c : t.clips) {
      const Pattern* p = t.findPattern(c.patternId);
      if (p && !p->notes.empty() && c.lengthTicks > 0) { out.push_back(t.id); break; }
    }
  }
  return out;
}

std::pair<double, double> stemSeconds(const Document& doc, double fromTick, double toTick) {
  return {secondsAt(doc, fromTick), secondsAt(doc, toTick)};
}

std::string waveFileName(const std::string& trackId, int64_t revision) {
  std::string name;
  for (const char ch : trackId) {
    const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
    name += ok ? ch : '_';
  }
  return (name.empty() ? std::string("track") : name) + "-" + std::to_string(revision) + ".wav";
}

}  // namespace core::doc
