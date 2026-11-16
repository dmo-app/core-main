// Quality of life taken from other DAWs (Diogo, 2026-11-16: "aplicar as QoL do estudo"; study in
// project-files core/qol-outras-daws.md). Rules and data only: no GUI, no audio, no engine code. Each call is one
// undo step in the app and changes nothing when it throws.
//   - Sections as markers (Bitwig cue markers, Ableton locators): jump to the next / previous one, loop one.
//   - Double (Ableton "Duplicate loop"): a pattern of the Patterns window twice as long, its notes repeated.
//   - Shift a row (FL Studio's shift in the channel rack): one track's steps one 1/16 left or right, wrapping.
//   - Nudge (Ableton / Bitwig arrows): the selected clips or notes one grid step earlier or later.
//   - Freeze (Bitwig "Bounce in place", Ableton "Freeze"): a track's plugins rendered to audio on a track below; the
//     source keeps its notes and plugins but they are not loaded while it is frozen. Unfreeze puts it back.
#pragma once
#include <cstdint>
#include <filesystem>
#include <set>
#include <string>
#include <vector>
#include "bounce.hpp"
#include "document.hpp"

namespace core::doc {

// ---- sections as markers
// The start of the first section that starts after `tick`, or -1. The song's end is not a section.
int64_t nextSectionStart(const Document& doc, int64_t tick);
// The start of the last section that starts before `tick` (a little slack, so pressing it twice while playing
// goes back one more), or -1.
int64_t previousSectionStart(const Document& doc, int64_t tick, int64_t slackTicks = 0);
// The section's span as an enabled loop (cut at the song's end). Throws DocumentError for an unknown id.
Loop sectionLoop(const Document& doc, const std::string& sectionId);
// The section that holds `tick`, or nullptr.
const Section* sectionAt(const Document& doc, int64_t tick);

// ---- Patterns window
// The slot twice as long (1 -> 2 -> 4 -> 8 bars), its notes repeated into the new half. Throws at 8 bars.
// Returns the new number of bars.
int doubleRackSlot(Document& doc, const std::string& slotId);
// One track's pattern in a slot rotated by `steps` sixteenths (negative: earlier); a note pushed past an end comes
// back at the other. Returns how many notes moved (0 when the row is empty: nothing changes).
int shiftRackRow(Document& doc, const std::string& slotId, const std::string& trackId, int steps);
// The same on one pattern, by ticks (the piano roll's rows use this too).
int rotatePatternNotes(Pattern& pattern, int64_t deltaTicks, const std::set<std::string>& noteIds = {});

// ---- nudge
struct ClipRef {
  std::string trackId, clipId;
  bool audio = false;  // an audio clip (Track::audioClips), else a pattern clip
};
// Every clip by deltaTicks. All or nothing: throws DocumentError when one would start before 0 or is not there.
// The song grows to the next bar when a clip ends past it. Returns how many moved.
int nudgeClips(Document& doc, const std::vector<ClipRef>& clips, int64_t deltaTicks);
// The notes by deltaTicks inside their pattern. All or nothing: throws when one would leave the pattern.
int nudgeNotes(Pattern& pattern, const std::set<std::string>& noteIds, int64_t deltaTicks);

// ---- freeze
// The audio track a frozen track plays from ("" when it is not frozen).
std::string frozenInto(const Track& track);
bool isFrozen(const Track& track);
// Why a track cannot be frozen ("" = it can): not there, already frozen, an audio track or a bus, nothing loaded
// to save, or one of the reasons a bounce is refused.
std::string cannotFreeze(const Document& doc, const std::string& trackId);
// The render (a stem of the track, as for a bounce) goes on a new audio track right below, named "<name> (frozen)";
// the source is muted and marked frozen: the engine gets it without its instrument and effects, so its plugins are
// unloaded. Its notes, plugins and their state stay in the song.
Printed freezeTrack(Document& doc, const std::string& trackId, const std::filesystem::path& file,
                    const std::filesystem::path& projectDir);
// The render started from `start` still fits `now` (a freeze finishing after an edit would put old audio in): the
// track sounds the same (its notes, instrument, effects, mix) and the song is as long.
bool freezeStillMatches(const Document& start, const Document& now, const std::string& trackId);
// The frozen audio track goes (its file too when nothing else uses it, at the next save), the source plays again.
void unfreezeTrack(Document& doc, const std::string& trackId);

} // namespace core::doc
