// Plugin qualification (plugin protection, step 1): before a plugin, or a new version of one, is trusted
// in a song, it plays on its own for a few seconds: silence, notes or noise, extremes (lowest and highest
// notes, full-scale and DC input), then its tail. At the engine's rate, in the graph's sub-blocks of 32,
// through the same checks as a song (exception, crash, NaN/Inf, runaway level). The app runs it in a
// process of its own (core-engine qualify <file>), so a plugin that crashes takes only that process down.
#pragma once
#include "plugins.hpp"

namespace core {

// {verdict: "ok" | "unsafe", plugin, version, installed, instrument, problems: [..], warnings: [..],
//  cpuPct (% of one core at this rate), peakDb, seconds}
json qualifyPlugin(PluginProvider& provider, const json& plugin, double sampleRate = 48000, double seconds = 4);

// The plugin matrix (M10): how a plugin behaves in Core, measured offline with fresh instances at the
// engine's rate. Not a verdict: numbers to keep per plugin and version.
//  latency: {reported (what the graph lines the song up by: the plugin's own plus the host's gathering into
//           blocks of 128), measured (impulse or note-on to the first sound; a reverb or a slow attack starts
//           later than its latency), changed (the plugin reported another latency while playing)}
//  tailSeconds: from the end of the input (effect) or the note-off (instrument) to below -90 dBFS
//           (tailCapped: still sounding at the end of the window)
//  differences in dB relative to the reference render's peak (-200 = identical), each lined up by latency:
//    repeatDb (the same render twice: the plugin's own randomness, the floor for the others),
//    callSizesDb (calls of 1..512 samples, as the live graph splits them at events),
//    realtimeDb (a live instance: blocks of 128, not marked offline)
//  monoRightDb (effects): the right output with only the left input, relative to the left output
//  installed, cpuPct, problems (anything the qualification would call unsafe), plugin, version, instrument
json pluginMatrix(PluginProvider& provider, const json& plugin, double sampleRate = 48000, double tailWindow = 20);

} // namespace core
