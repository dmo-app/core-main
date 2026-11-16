#include "bounce.hpp"
#include <algorithm>
#include "assets.hpp"
#include "edit.hpp"

namespace core::doc {

namespace {

void checkRegion(const Document& doc, int64_t startTick, int64_t endTick) {
  if (startTick < 0 || endTick <= startTick) throw DocumentError("region", "is empty: set a loop or use the whole song");
  if (startTick >= doc.lengthTicks) throw DocumentError("region", "starts after the song's end");
}

// The file as a new audio track at startTick, cut at endTick; the new track goes right after `after` (or last).
Printed place(Document& doc, const std::filesystem::path& file, const std::filesystem::path& projectDir, const std::string& name,
              const std::string& after, int64_t startTick, int64_t endTick) {
  Printed out;
  out.assetId = importAudio(doc, file, projectDir);
  out.trackId = addAudioTrack(doc, name);
  out.clipId = addAudioClip(doc, out.trackId, out.assetId, startTick);
  trimAudioClipEnd(doc, out.trackId, out.clipId, secondsBetween(doc, (double)startTick, (double)endTick));
  if (!after.empty()) {
    Track moved = std::move(doc.tracks.back());
    doc.tracks.pop_back();
    auto at = std::find_if(doc.tracks.begin(), doc.tracks.end(), [&](const Track& t) { return t.id == after; });
    doc.tracks.insert(at == doc.tracks.end() ? at : at + 1, std::move(moved));
  }
  return out;
}

void label(Document& doc, const std::string& assetId, const std::string& what) {
  // Cut at 80 bytes on a character boundary: cut inside a "ç" the text is not UTF-8 and Save and autosave throw
  // (sweep 2026-11-14 D4).
  size_t n = std::min<size_t>(what.size(), 80);
  if (n < what.size()) while (n > 0 && ((unsigned char)what[n] & 0xC0) == 0x80) --n;
  for (auto& a : doc.assets)  // the same audio printed before keeps its first label
    if (a.id == assetId && a.made.empty() && a.madeFrom.empty()) a.made = what.substr(0, n);
}

} // namespace

std::string cannotBounce(const Document& doc, const std::string& trackId) {
  const Track* t = doc.findTrack(trackId);
  if (!t) return "There is no such track.";
  if (t->mix.mute) return "The track is muted: its bounce would be silence. Unmute it first.";
  if (t->instrument.kind == "bus") {
    bool fed = false;
    for (const auto& o : doc.tracks) {
      if (o.mix.output == trackId) fed = true;
      for (const auto& s : o.mix.sends) fed = fed || s.to == trackId;
    }
    if (!fed) return "Nothing plays into this bus.";
  }
  return "";
}

Printed placeBounce(Document& doc, const std::string& sourceTrackId, const std::filesystem::path& file,
                    const std::filesystem::path& projectDir, int64_t startTick, int64_t endTick) {
  if (const std::string why = cannotBounce(doc, sourceTrackId); !why.empty()) throw DocumentError("tracks", why);
  checkRegion(doc, startTick, endTick);
  endTick = std::min(endTick, doc.lengthTicks);
  Document next = doc;  // all or nothing
  const Track source = *next.findTrack(sourceTrackId);
  // The name fits in 80 bytes with " (bounce)" kept: a long name loses its end, not the suffix, and never half a
  // character (deep hunt P01: a track name of 72+ bytes must not make the song unsavable).
  const std::string suffix = " (bounce)";
  std::string base = source.name;
  if (base.size() > 80 - suffix.size()) {
    size_t n = 80 - suffix.size();
    while (n > 0 && ((unsigned char)base[n] & 0xC0) == 0x80) --n;
    base.resize(n);
  }
  Printed out = place(next, file, projectDir, base + suffix, sourceTrackId, startTick, endTick);
  Track& printed = trackById(next, out.trackId);
  printed.color = source.color;
  // The render already has the fader, the pan, the sends and the buses: the new track adds nothing.
  printed.mix = Mix{};
  label(next, out.assetId, "Bounce of " + source.name);
  trackById(next, sourceTrackId).mix.mute = true;
  doc = std::move(next);
  return out;
}

Printed placeResample(Document& doc, const std::filesystem::path& file, const std::filesystem::path& projectDir,
                      int64_t startTick, int64_t endTick) {
  checkRegion(doc, startTick, endTick);
  endTick = std::min(endTick, doc.lengthTicks);
  Document next = doc;
  int n = 1;
  for (const auto& t : next.tracks) if (t.name.rfind("Resample", 0) == 0) ++n;
  Printed out = place(next, file, projectDir, n == 1 ? std::string("Resample") : "Resample " + std::to_string(n), "", startTick, endTick);
  Track& printed = trackById(next, out.trackId);
  printed.mix.mute = true;  // the mix already plays it
  label(next, out.assetId, "Resample of the mix");
  doc = std::move(next);
  return out;
}

} // namespace core::doc
