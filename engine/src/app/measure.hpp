#pragma once
#include <string>
#include "core/project.hpp"

namespace core {
json demoProject(double bpm);  // kick + rolling bass + hats, cutoff envelope, kick roll (8 bars)
// Runs the measurements and returns the report. outDir: where example WAVs go ("" = none).
json runMeasure(const std::string& outDir, const json& realtime);
} // namespace core
