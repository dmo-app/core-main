// The arrange's clips show the sound their track makes (Diogo 2026-11-16: "ainda vejo as tracks com blocos em vez de
// waves"; as in Bitwig). The sound comes from plugins, so the engine renders each track's stem over the song in the
// background (as an export, with its own plugin instances) and a clip draws the part of its track's stem under it.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "document.hpp"

namespace core::doc {

// The tracks whose clips can show a stem: they play patterns and at least one of their clips plays a pattern with
// notes. In the song's order.
std::vector<std::string> tracksForWaves(const Document& doc);
// The seconds of a stem (which starts at the song's tick 0) from one song tick to another, with the tempo map.
std::pair<double, double> stemSeconds(const Document& doc, double fromTick, double toTick);
// A stem's file name: the track id kept to letters, digits, '-' and '_', then the revision ("t1-42.wav").
std::string waveFileName(const std::string& trackId, int64_t revision);

}  // namespace core::doc
