// What a sample sounds like, measured from its audio (plan A1, "physical descriptors"): where the sound
// starts and ends, how fast it hits, how long its tail is, how bright it is and how much sub it has.
// Measurements only: no guess of role, BPM or style. Offline, never in the audio callback. No GUI.
// Contract: docs/contracts/core-samples.md ("Análise").
#pragma once
#include <atomic>
#include <filesystem>
#include <string>
#include "document.hpp"

namespace core::doc {

// A new version when a definition, a window or a threshold changes: results of another version are measured again.
constexpr const char* kAnalyzerVersion = "core-dsp-3";  // 2: pitchHz, 3: peakDb
constexpr double kAnalysisMaxSeconds = 30;  // longer files: the first 30 s (coverage says so)

struct SampleAnalysis {
  std::string version = kAnalyzerVersion;
  std::string sha256;              // of the file that was measured
  double coveredSeconds = 0;       // how much of the file was read
  bool complete = false;           // the whole file was read
  bool silent = false;             // nothing above the silence threshold: the rest is not measured
  // Times in ms, from the file's start. The silence threshold is the louder of -60 dBFS and 50 dB under the peak.
  double leadingSilenceMs = 0;     // before the first sound
  double activeMs = 0;             // from the first sound to the last
  double attackMs = 0;             // from the first sound to 90% of the loudest point of its first 250 ms
  double tailMs = -1;              // from the loudest point to 40 dB under it (-1: several hits, a tail would mislead)
  int hits = 0;                    // separate hits (the envelope falls under 25% of the peak, then rises over 50%)
  // Spectrum of the sounding part (the channels' power added, so stereo never cancels): 2048-point Hann windows.
  double centroidHz = 0;           // brightness: the spectrum's centre of mass
  double subRatio = 0;             // share of the power under 120 Hz (0..1)
  double highRatio = 0;            // share of the power over 5 kHz (0..1)
  // The note it plays, from its audio (YIN, 25..1000 Hz, on the sound 30 ms after it starts, past the attack):
  // 0 when no clear pitch (noise, a hat, a chord). A bass sample's name often gives a wrong note or octave
  // ("BASS Cm" that plays an F, "A#1" that plays A#2), so the sampler tunes a bass by this when known.
  double pitchHz = 0;
  // The loudest sample of any channel, in dBFS (of the part read): the sampler plays every library sample at
  // the same peak (kSamplePeakDb), so one kick is not 12 dB quieter than another.
  double peakDb = -120;
};
// The MIDI note nearest to a measured pitch, -1 when there is none.
int pitchToMidi(double hz);

// Measures a WAV file (the formats Core plays). stop (may be null): set it to give up; throws DocumentError
// ("analysis stopped") then, and when the file cannot be read.
SampleAnalysis analyzeSample(const std::filesystem::path& file, const std::string& sha256, const std::atomic<bool>* stop = nullptr);

json analysisToJson(const SampleAnalysis& a);
// Null or another version: false (to be measured again).
bool analysisFromJson(const json& j, SampleAnalysis& out);

// Words for the Library's filters, from the measurements: "short" (sounds under 300 ms), "long" (over 2 s),
// "bright" (centroid over 3 kHz), "dark" (under 800 Hz), "sub" (over 40% of the power under 120 Hz).
std::vector<std::string> analysisWords(const SampleAnalysis& a);

}  // namespace core::doc
