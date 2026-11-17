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

// ================================================================================================================
// The song tools the AI uses to reach the plugins (Diogo 2026-11-16: "têm de ser feitos a pensar na AI como caminho
// para usar os plugins"). Diogo plays the plugins by hand; these are for the AI first, so each one has few
// parameters, plain intent words and the same function for the AI and for any button. They never name a plugin:
// they ask for a role and Settings > Roles says which plugin plays it. songToolCatalog() describes them for the AI.

// Where a tool works: a section by name ("Drop 1", "the break": the first section whose name holds the words, case
// ignored) or the whole song (""). Throws DocumentError when no section matches.
struct SongSpan { int64_t start = 0, end = 0; std::string name; };
SongSpan sectionSpan(const Document& doc, const std::string& section);

// ---- R-Core / D-Core (toolSharedBus): tracks share one reverb or one delay on a bus, and it follows the sections
// The bus plays the role's plugin fully wet; each track sends to it; the bus is quieter in the drops and opens in
// the intros, breaks, build-ups and outros. The same place again only updates the sends. kind: "reverb" | "delay".
struct SpacePreset {
  std::string kind, id, what;
  double sendDb;          // each track's send
  double dropDb, openDb;  // the bus in drops / in intros, breaks, build-ups and outros (0 = the same everywhere)
  double size;            // reverb: size 0..1 (the "size" macro); delay: feedback 0..1 (the "feedback" macro)
};
const std::vector<SpacePreset>& spacePresets();  // reverb: room, hall, plate, huge, tight; delay: throw, dotted, eighths, sixteenths, slow
const SpacePreset* findSpacePreset(const std::string& kind, const std::string& id);
// sections: the sections where the bus level moves. sizeSet: the preset's size (feedback) was written; false when
// the role has no such macro. skipped: tracks that could not feed the bus, "name (reason)", left as they were.
struct SharedBusResult {
  std::string busId;
  bool newBus = false, sizeSet = false;
  int sends = 0, sections = 0;
  std::vector<std::string> skipped;
  std::string sentence;
};
SharedBusResult toolSharedBus(Document& doc, const std::vector<std::string>& trackIds, const RoleMap& map, const std::string& kind,
                              const std::string& preset = "");  // preset "": room (reverb) or dotted (delay)

// ---- FX by intent (was F-Core): what the moment is for, over a span, on one track
// intent: buildup (filter opens, reverb grows), suck (into the drop at the span's end), intro (dark to bright),
// outro (bright to dark, the reverb grows), impact (a burst of reverb and drive at the start that dies away),
// break (darker and wider, steady). amount 0..1. Roles that are not mapped are skipped and said.
const std::vector<std::string>& fxIntents();
RoleToolResult toolIntentFx(Document& doc, const std::string& trackId, const RoleMap& map, const std::string& intent,
                            int64_t start, int64_t end, double amount = 0.7);

// The same intent over [start, end) on several tracks at once (Diogo 2026-11-17: "ao selecionar a zona aplicava nas
// pistas que queria ou em todas"). Only tracks that play notes take it; a bus or an audio track is skipped and said.
// The span and the intent are checked before anything is written, so a refusal changes nothing (one undo step).
// Roles and skipped reasons are gathered across the tracks; the sentence says how many tracks were written.
RoleToolResult toolIntentFxTracks(Document& doc, const std::vector<std::string>& trackIds, const RoleMap& map,
                                  const std::string& intent, int64_t start, int64_t end, double amount = 0.7);

// ---- ROLL: notes that speed up into the drop on a track (a snare or a kick roll), in a new pattern placed on the
// span. style: "steps" (1/4, 1/8, 1/16, then 1/32) | "gradual" (speeding up the whole way). pitch -1: the track's
// key (or 38, a snare). rise: semitones the roll climbs by the drop (0-12). gapBeats: silence before the drop (0-4).
// Returns the notes written; the clips already on the track over the span are cut out of it first.
struct RollResult { int notes = 0; std::string patternId, sentence; };
RollResult toolRoll(Document& doc, const std::string& trackId, int64_t start, int64_t dropTick, const std::string& style = "steps",
                    int pitch = -1, int rise = 0, int gapBeats = 0);

// ---- the catalog: what the AI reads to choose a tool
struct SongToolParam { std::string name, kind, values, what; };  // kind: track | tracks | section | choice | number
struct SongToolInfo { std::string id, what; std::vector<std::string> roles; std::vector<SongToolParam> params; };
const std::vector<SongToolInfo>& songToolCatalog();  // space, fx, roll, suck, pump, buildup
json songToolCatalogJson();

}  // namespace core::doc
