// The tools that play through the role map (roles.hpp): they put the role's plugin on the track (once) and write
// automation of its macros over a span of the song. The result is ordinary plugin effects and curves: undo, save,
// export and the engine treat them as any other (nothing runs in real time, nothing new in the engine).
//
//   SUCK      the sound is sucked into the drop: the reverb opens and grows (and the filter closes, when the role
//             is mapped) over the last bars, everything back as it was on the drop.
//   PUM       the pump: the sidechain plugin's volume dips at every note of a source track (the kick) and comes back
//             over `release`; a plugin with its own pump shape (only "depth" mapped) gets its depth over the span.
//   Build-up  (with the roll) the filter opens, the reverb and the delay grow, the drive pushes, up to the drop.
//
// Song ticks everywhere. Each function throws DocumentError (a role not mapped, a span outside the song...) before
// changing anything, so the caller's edit is one undo step or nothing.
#pragma once
#include <string>
#include <vector>
#include "gate.hpp"
#include "roles.hpp"

namespace core::doc {

struct RoleToolResult {
  std::vector<std::string> roles;    // the roles that were written (their effects are on the track)
  std::vector<std::string> keys;     // the automation keys written
  std::vector<std::string> skipped;  // optional roles left out, with why ("filter: no plugin chosen")
  std::string sentence;              // what it did, in plain words
};

// One macro of a role from `from` to `to` over [start, end), as (t)^curve, back at its rest value at `end`
// (`end` must be inside the song; the curve after `end` stays as it was). Returns the automation key.
std::string writeRoleSweep(Document& doc, const std::string& trackId, const RoleMap& map, const std::string& role,
                           const std::string& macro, int64_t start, int64_t end, double from, double to, double curve = 1.0);

// SUCK into the drop at dropTick, over the `lengthTicks` before it. amount 0..1. Needs the reverb role; the filter
// role is used when mapped.
RoleToolResult toolSuck(Document& doc, const std::string& trackId, const RoleMap& map, int64_t dropTick, int64_t lengthTicks,
                        double amount = 0.8);

// PUM on trackId from the notes of sourceTrackId in [start, end): depth 0..1 (how far the volume dips), release in
// ticks (how long it takes to come back; 0: an eighth). Needs the sidechain role with "volume" or "depth" mapped.
RoleToolResult toolPump(Document& doc, const std::string& trackId, const std::string& sourceTrackId, const RoleMap& map,
                        int64_t start, int64_t end, double depth = 0.7, int64_t releaseTicks = 0);

// The build-up's effects from start to the drop at dropTick. Each of filter / reverb / delay / drive is used when
// asked and mapped (skipped, with why, otherwise); at least one must be written.
struct BuildupFx {
  bool filter = true, reverb = true, delay = false, drive = false;
  double amount = 0.8;  // 0..1
  bool any() const { return filter || reverb || delay || drive; }
};
RoleToolResult toolBuildupFx(Document& doc, const std::string& trackId, const RoleMap& map, int64_t start, int64_t dropTick,
                             const BuildupFx& fx);

// The gate (Core AI Gate) on trackId over [start, end): each of its knobs as a flat line of automation, back at
// its rest (the gate off, depth 0) at `end`. Only the steps that are not on and the knobs that differ from the
// gate's own rest are written. Needs the gate role with every macro it writes mapped.
RoleToolResult toolGate(Document& doc, const std::string& trackId, const RoleMap& map, int64_t start, int64_t end,
                        const aifx::Settings& gate, const std::string& name = "");

}  // namespace core::doc
