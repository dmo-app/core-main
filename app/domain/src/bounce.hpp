// Bounce and Resample (Ableton plan AB1, Diogo chose it on 2026-11-06): what the engine rendered goes back into the
// song as audio, in one undo step.
//   - Bounce: one track as it is heard (its instrument, effects, fader and pan, and the buses and sends it feeds,
//     before the master) from startTick to endTick. The audio goes on a new audio track right below the source, to
//     the master with no sends (they are already in the sound), and the source is muted, so the song sounds the same.
//   - Resample: the whole mix (after the master chain) of a region, on a new audio track at the region's start. It
//     starts muted, because the mix already plays it: it is material to cut, reverse and process again.
// The app renders the file (the export job's stems), then calls these. Nothing here talks to the engine.
#pragma once
#include <filesystem>
#include <string>
#include "document.hpp"

namespace core::doc {

struct Printed {
  std::string trackId, clipId, assetId;
};

// Throws DocumentError (no such track, the track is muted or silent by design, an empty region) before changing
// anything; `file` is copied into <projectDir>/Audio like an import. The clip is cut at endTick (the render's tail
// past it is left out, so the source's muted part and the new clip line up).
Printed placeBounce(Document& doc, const std::string& sourceTrackId, const std::filesystem::path& file,
                    const std::filesystem::path& projectDir, int64_t startTick, int64_t endTick);
Printed placeResample(Document& doc, const std::filesystem::path& file, const std::filesystem::path& projectDir,
                      int64_t startTick, int64_t endTick);

// Why a track cannot be bounced ("" = it can): a missing track, a muted one (its render is silence), a bus with
// nothing feeding it.
std::string cannotBounce(const Document& doc, const std::string& trackId);

} // namespace core::doc
