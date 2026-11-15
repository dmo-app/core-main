// FOLLOW (Diogo, 2026-11-09): "o bass acompanhar o kick e o kick a acompanhar o stutter". Two ways, the same rules:
//
// - A zone, once (like Normalize): the notes of a stutter you select, or the bars shown, and the kick and the bass
//   are written there. One undo step; the result is ordinary notes.
// - On ("ligado"): a track keeps following. While it is on, every edit rewrites it where the song has STUTTER notes
//   (the notes the STUTTER tool wrote, marked "stutter"): the kick plays the stutter's onsets, the bass fills the
//   steps after each of those kicks. Run by refreshFollows, in the same undo step as the edit that changed things.
//
// "Not 100% perfect": every follow has an offset (late +, early -) and a looseness (each note moved at random, from a
// seed, by up to that many ticks), so the kick can sit a bit behind or ahead of the stutter.
//
// Writing a range of the song makes that range its own clips with its own, non-looping patterns (the clips around it
// are split at its edges; where the track has no clip, a new one is added). Nothing outside the range changes.
#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

struct FollowSettings {
  int64_t offsetTicks = 0;   // + later, - earlier (30 = 1/128, 60 = 1/64, 120 = 1/32); -240..240
  int64_t looseTicks = 0;    // each note moves at random by up to this (0: exact); 0..120
  uint64_t seed = 1;
  // The bass: a note on each grid step after a kick, at most afterKick of them (KBBB: 3), never on the next kick.
  int64_t grid = kPpq / 4;
  int afterKick = 3;
  double gate = 0.9;         // a note's length as a share of its step
};
void checkFollow(const FollowSettings& s);  // throws DocumentError on a value out of range

// The song ticks where a track's notes start in [from, to) (to <= from: the whole song), every clip and every repeat
// of a looping pattern; sorted, each once. stutterOnly: only the notes the STUTTER tool wrote.
std::vector<int64_t> songOnsets(const Document& doc, const std::string& trackId, int64_t from, int64_t to, bool stutterOnly = false);
// The song ticks where these notes of a pattern sound (every clip that plays the pattern).
std::vector<int64_t> songOnsetsOfNotes(const Document& doc, const std::string& trackId, const std::string& patternId,
                                       const std::set<std::string>& noteIds);
// Where pattern ticks [from, to) of a pattern sound in the song (every clip that plays it, every repeat), merged.
std::vector<std::pair<int64_t, int64_t>> songRangesOfPattern(const Document& doc, const std::string& trackId,
                                                              const std::string& patternId, int64_t from, int64_t to);
// The STUTTER notes of every track but `except`, in [from, to) (to <= from: the whole song).
std::vector<int64_t> stutterOnsets(const Document& doc, const std::string& except, int64_t from, int64_t to);

// A stutter's zones: onsets less than a beat apart are one zone, from the first to one step after the last.
struct FollowZone { int64_t start = 0, end = 0; std::vector<int64_t> onsets; };
std::vector<FollowZone> followZones(std::vector<int64_t> onsets);

// The kick plays each onset of each zone (moved by offset and looseness); its other notes in the zones go.
// Returns the notes written plus those taken out (0: nothing changed). Throws DocumentError (a track that plays no
// notes, settings out of range).
int kickFollow(Document& doc, const std::string& kickTrackId, const std::vector<FollowZone>& zones, const FollowSettings& s);
// The bass in [from, to): a note on each step after every kick of kickTrackId there (up to afterKick, never on the
// next kick); where the kick is too fast for that (a stutter, a roll), the bass plays with each kick instead. Its other
// notes there go. Pitches: the bass's own note at that point (its last note before), else its
// most used pitch. Returns the notes written plus those taken out.
int bassFollow(Document& doc, const std::string& bassTrackId, const std::string& kickTrackId, int64_t from, int64_t to,
               const FollowSettings& s);
// The same over each zone.
int bassFollowZones(Document& doc, const std::string& bassTrackId, const std::string& kickTrackId,
                    const std::vector<FollowZone>& zones, const FollowSettings& s);

// The tracks a follow uses when none is named: the kick (the built-in kick, else a track named "kick") and the bass
// (a track named "bass" / "kbbb"). "" when there is none.
std::string findKickTrack(const Document& doc);
std::string findBassTrack(const Document& doc);

// --- On ("ligado"): saved on the track as extra "follow" {mode, source, offset, loose, seed}. mode "stutter" (a
// kick: follows the STUTTER notes of every other track) or "kick" (a bass: follows the kick `source`, "" = findKickTrack,
// inside the stutter zones). Old files have none: off.
struct FollowLink {
  std::string mode;          // "stutter" | "kick"
  std::string source;
  FollowSettings settings;
};
void setFollowLink(Document& doc, const std::string& trackId, const FollowLink& link);  // throws DocumentError
void removeFollowLink(Document& doc, const std::string& trackId);
bool followLink(const Track& track, FollowLink* out = nullptr);
// Rewrites every track that is on (the kicks first, then the basses). Nothing changes when the notes are already what
// it would write, so running it after every edit costs nothing and keeps the undo history clean. Returns the tracks changed.
int refreshFollows(Document& doc);

}  // namespace core::doc
