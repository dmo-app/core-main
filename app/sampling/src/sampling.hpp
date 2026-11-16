// Making a sample from audio (Save as sample, Render to sample): plain buffers, no file, no JUCE. The clip's own
// shape (gain, fades), cutting the silence, the peak, a safe file name and the catalogue's role from a name.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace core::sampling {

// A clip as it sounds, for "Save clip as sample": its gain and its linear fades (the same shape as the
// engine), applied in place to `frames` frames of each channel, read at `sampleRate`.
void shapeClip(std::vector<std::vector<float>>& channels, double sampleRate, double gainDb, double fadeInSeconds,
               double fadeOutSeconds);

// A clip with VOICE STUTTER's pitch / freeze, as the engine plays it (the same grains, dsp/clip_grains.hpp): `file` holds the
// file's channels from the clip's offset on (at least frames * ratio, or the freeze span, when the file has them);
// returns `frames` frames per channel. Without pitch and freeze, the first `frames` frames as they are.
std::vector<std::vector<float>> processClip(const std::vector<std::vector<float>>& file, double sampleRate, int64_t frames,
                                            double pitchSemitones, double freezeSeconds);
// How much of the file such a clip reads, in seconds from its offset (pitch up reads faster than it plays).
double clipSourceSeconds(double durationSeconds, double pitchSemitones, double freezeSeconds);

// A file name for a sample that is safe on Windows: no \ / : * ? " < > |, no trailing dots or spaces,
// never empty, at most 80 characters.
std::string safeFileName(const std::string& name);


// ---- making a sample (Save as sample, Render to sample): plain buffers, no file, no JUCE

using Channels = std::vector<std::vector<float>>;
// The frames [from, to) where the sound is: from the first frame louder than thresholdDb (on any channel),
// minus startMarginMs (so the attack stays whole), to the last frame louder than it, plus endMarginMs (the tail
// fades on its own). to <= from when everything is under the threshold (silence).
struct AudibleRange { int64_t from = 0, to = 0; };
AudibleRange audibleRange(const Channels& channels, double sampleRate, double thresholdDb = -60, double startMarginMs = 1,
                          double endMarginMs = 20);
// Cuts the silence before and after the sound (audibleRange) and puts a short fade (fadeMs) where a cut lands in
// sound, so there is no click. Returns false (nothing changed) when everything is silent.
bool trimSilence(Channels& channels, double sampleRate, double thresholdDb = -60, double fadeMs = 2);
// True when any sample is NaN or Inf: a damaged file, not silence (deep hunt P08: audibleRange calls it silent,
// so check this first and say the file is damaged).
bool hasBrokenSamples(const Channels& channels);
// The peak of all channels, as a level (0..); 0 for silence.
float peakOf(const Channels& channels);
// Gain so the peak reaches targetDb (dBFS). Returns the gain in dB; silence and broken samples (hasBrokenSamples)
// are left as they are (returns 0).
double normalizePeak(Channels& channels, double targetDb = -0.3);
// The catalogue's role for a sample made from this name (a track, a stem, a clip): "Kick 2" -> kick,
// "Bass" -> bass; "other" when the name says nothing (doc::classifySample's rules).
std::string roleFromName(const std::string& name);
}  // namespace core::sampling
