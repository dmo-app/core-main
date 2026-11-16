// What Core lets its AI touch (Diogo, 2026-11-16, Fase 4): ONLY music creation. The AI runs in its own process
// (core-ai) and sees Core through a catalog of actions; this file is the closed list of what goes in it. What is not
// here is not offered to the AI, and if the AI asks for it anyway Core refuses the whole proposal (ai_admit.hpp).
//
//   in   pcore (patterns), shape (the song's sections), arrange, plays (what plays where), track (a new track),
//        roll (kick rolls), stutter, voicestutter (rhythmic repeats of notes / audio)
//   out  mix (mixing), space / fx / gate / suck / pump / buildup (effects on tracks: sound design and mixing),
//        open (windows). Mix analysis, sound design, stems and training are AICORE's and are not even callable.
//
// A new Core function stays OUT until it is added here by a decision (ESTADO.md): nothing enters by default.
#pragma once
#include <string>
#include <vector>
#include "calls.hpp"

namespace core::ai {

// The closed list, by Core function id.
const std::vector<std::string>& creationScope();
bool inCreationScope(const std::string& functionId);
// Core's functions cut to the scope (the order is kept).
std::vector<control::CoreFunction> exposedFunctions(const std::vector<control::CoreFunction>& all);

}  // namespace core::ai
