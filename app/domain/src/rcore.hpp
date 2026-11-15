// Reverb Core (R-Core) (layer 1, a gesture): the selected tracks share one SPACE (layer 0) on a bus of their own,
// instead of a reverb on every track. A preset names the place (a SPACE preset of its style), how much each track
// sends, and how the place follows the song's sections (quieter in drops, full in breaks), written as volume
// automation on the bus, so nothing is hidden: the bus, its SPACE, the sends and the curve are all in the song.
// Several R-Core places can live side by side (one bus each); applying the same place again only updates sends.
// Delay Core (D-Core) (layer 1) is the same gesture with an ECHO on the bus: dcorePresets(), applied with applyRCore.
#pragma once
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

struct RCorePreset {
  const char* gesture;  // "R-Core" or "D-Core": the bus is "<gesture> · <name>"
  const char* effect;   // the layer 0 device on the bus: "space" or "echo"
  const char* style;  // the Style FX ids: darkpsycore, hitech, slambient, psytrance
  const char* name;   // also the bus name: "R-Core · <name>"
  const char* space;  // the device's preset (effectPresets) on the bus, played at mix 1
  const char* tip;
  double sendDb;      // each selected track's send
  double dropDb;      // the bus in sections called "drop" (0 = unchanged)
  double breakDb;     // the bus in intros, breaks, build-ups and outros
};
const std::vector<RCorePreset>& rcorePresets();
const std::vector<RCorePreset>& dcorePresets();

struct RCoreResult {
  std::string busId;
  bool newBus = false;
  int sends = 0;      // tracks now sending to the place
  int sections = 0;   // sections the bus follows (0: none, or the preset leaves them flat)
};
// One edit (one Undo when the caller wraps it in applyEdit). Tracks that cannot send (the bus itself, a loop) are
// left as they were. bySection false: no automation is written. Throws DocumentError (no tracks, unknown preset).
RCoreResult applyRCore(Document& doc, const std::vector<std::string>& trackIds, const RCorePreset& preset, bool bySection = true);
// The R-Core bus of a place already in the song ("" if none).
std::string rcoreBus(const Document& doc, const RCorePreset& preset);

}  // namespace core::doc
