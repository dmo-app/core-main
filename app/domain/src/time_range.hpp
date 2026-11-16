#pragma once
// A piece of song time on some tracks (the arrangement's time selection, Diogo 2026-11-16: "seleção, andar no
// timeline, copiar colar, quanto mais próximo do Bitwig melhor"): copy it, clear it, paste it somewhere else,
// duplicate it right after itself. Clips that cross an edge are cut there (they keep playing what they played),
// so a copy holds exactly the music between the edges. Removing or inserting time for the whole song is
// deleteTime / insertTime (edit.hpp).
#include <string>
#include <vector>
#include "document.hpp"

namespace core::doc {

struct TimeRange {
  int64_t fromTick = 0, toTick = 0;      // [from, to)
  std::vector<std::string> trackIds;     // top to bottom, as the rows show them
  int64_t length() const { return toTick - fromTick; }
  bool empty() const { return toTick <= fromTick || trackIds.empty(); }
};

// What Ctrl+C took: per track, the clips (ticks from the range's start), the patterns they play and the
// automation inside (ticks from the start too, with a point at each edge so the curve keeps its shape).
struct RangeCopy {
  struct Part {
    std::string trackId;
    bool audio = false;                   // an audio track's part (audio clips), else pattern clips
    std::vector<Pattern> patterns;
    std::vector<Clip> clips;
    std::vector<AudioClip> audioClips;
    std::vector<Automation> automation;
  };
  int64_t lengthTicks = 0;
  std::vector<Part> parts;
  bool empty() const { return parts.empty() || lengthTicks <= 0; }
};

struct RangePasteResult {
  int clips = 0;            // clips placed
  int skippedTracks = 0;    // parts with no track to go to, or the wrong kind of track (audio on an instrument track...)
  TimeRange range;          // where it landed
};

// The track ids from `first` downwards (document order), at most `count`: where a multi-track copy is pasted.
std::vector<std::string> tracksFrom(const Document& doc, const std::string& first, size_t count);

RangeCopy copyRange(const Document& doc, const TimeRange& range);
// The music inside goes (clips cut at the edges, automation points inside removed); the time stays. Returns how many
// clips went or were shortened.
int clearRange(Document& doc, const TimeRange& range);
// Pastes part i on targets[i] at `at`, replacing what was there for the copy's length; the song grows to hold it.
// A part's patterns are copied onto another track (new ids); on its own track it plays the same patterns.
// Automation goes back only onto its own track (a parameter of one plugin means nothing on another).
RangePasteResult pasteRange(Document& doc, const RangeCopy& copy, int64_t at, const std::vector<std::string>& targets);
// The range's music again right after it (Ctrl+D), on the same tracks; returns the new range.
TimeRange duplicateRange(Document& doc, const TimeRange& range);

}  // namespace core::doc
