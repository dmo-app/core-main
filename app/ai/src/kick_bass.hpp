// What the AI hears of the kick and the bass (Diogo, 2026-11-16, after Kick/Bass Scope): how the two sit together
// in a stretch of the song, computed by Core itself from the two tracks as rendered (their plugins and effects).
// The AI reads it with "ler_kick_bass" and decides the cut (gate psy-bass-gap, a duck, a shorter bass note).
// std only: tested in tests/ai_tests.cpp.
#pragma once
#include <string>
#include <vector>
#include "json.hpp"

namespace core::ai {

struct KickBass {
  double correlation = 0;     // -1..+1 of the low end (below ~150 Hz) of the two: < 0 = they cancel each other
  double overlapMs = 0;       // per beat, how long both sound at once (each above -18 dB of its own peak)
  double kickPeakDb = -120, bassPeakDb = -120, sumPeakDb = -120;  // dBFS
  bool silentKick = false, silentBass = false;
  nlohmann::json toJson() const;  // with "advice": what to do, in a few words (Portuguese, as the AI's prompt)
};

// Mono buffers of the same stretch, from the same start, at sampleRate; bpm for the beats.
KickBass analyzeKickBass(const std::vector<float>& kick, const std::vector<float>& bass, double sampleRate, double bpm);

}  // namespace core::ai
