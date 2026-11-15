// A track's colour, shared by every view of the track (lanes, clips, Mixer): the same track keeps the same colour
// wherever it is shown (Diogo 2026-11-08: "cada um com a cor da sua entidade").
#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
#include "document.hpp"
#include "Theme.h"

namespace core::app {

// One role, one colour, in every song (CORE-MANUAL app/ui-colours.md, Diogo 2026-11-09): the role read from the
// name or instrument (kick, bass, hats…) wins over a colour saved in the song, so the same role never shows two
// colours. A track with no role keeps its saved colour, or one of the others by its id. A bus (a group) keeps its
// own group colour (doc::kGroupColors, vivid), or one of them by its id.
inline juce::Colour trackColour(const doc::Track& t) {
  auto hex = [](const std::string& c) { return juce::Colour::fromString("ff" + juce::String(c.substr(1))); };
  if (t.instrument.kind == "bus") {
    for (const char* g : doc::kGroupColors)  // only a group colour counts (an older song's blue-grey does not)
      if (juce::String(t.color).equalsIgnoreCase(g)) return hex(t.color);
    return hex(doc::kGroupColors[(juce::uint32)juce::String(t.id).hashCode() % (juce::uint32)std::size(doc::kGroupColors)]);
  }
  // The track's name is read first, then its instrument: "Accents" on a kick engine is percussion, not the kick.
  const juce::String keys[] = {juce::String::fromUTF8(t.name.c_str()).toLowerCase(),
                               juce::String::fromUTF8(t.instrument.name.c_str()).toLowerCase()};
  struct Role { const char* words; juce::uint32 argb; };
  static const Role roles[] = {{"kick", 0xffe06a6a}, {"bass sub 303 acid", 0xff6a9cf0}, {"hat hats ride cymbal", 0xffe0c070},
                               {"clap snare", 0xffd9845a}, {"perc tom conga shaker accent", 0xffddb07c}, {"lead synth arp pluck", 0xffa98bd0},
                               {"pad atmo drone ambient texture", 0xff6fb08f}, {"fx riser impact crash sweep noise", 0xffc47fa8},
                               {"vocal voice chop", 0xff9c8193}};
  for (const auto& key : keys)
    for (const auto& r : roles)
      for (const auto& w : juce::StringArray::fromTokens(r.words, " ", ""))
        if (key.contains(w)) return juce::Colour(r.argb);
  if (t.color.size() == 7) return hex(t.color);
  static const juce::uint32 others[] = {0xff7fa9c6, 0xff8a8fc0, 0xffb9a56a, 0xff6f8fb0, 0xff8fb3c9, 0xff7a9ab8};
  return juce::Colour(others[(juce::uint32)juce::String(t.id).hashCode() % 6u]);
}

// The Master has no track colour: the official base blue-grey (core-improvements, Core Studio UI palette).
inline const juce::Colour kMasterColour{0xff5e6d96};

// A panel that edits one track (its effects, its sound, its modulation) carries the track's colour, so its knobs
// and value bars draw in it (theme::setIdentity). An empty id is the Master (its colour); a missing track: none.
inline void identifyWithTrack(juce::Component& c, const doc::Document& d, const std::string& trackId) {
  const doc::Track* t = trackId.empty() ? nullptr : d.findTrack(trackId);
  theme::setIdentity(c, t ? trackColour(*t) : trackId.empty() ? kMasterColour : juce::Colour());
}

}  // namespace core::app
