// Offline time and pitch changes for audio clips (the result becomes a new file; nothing here runs in
// real time). Audio is one vector per channel, all the same length.
#pragma once
#include <vector>

namespace core::app::dsp {

using Channels = std::vector<std::vector<float>>;

// Longer or shorter, same pitch (WSOLA: overlapping pieces of the input, each placed where it best
// continues the previous one). ratio = output length / input length, 0.25 to 4.
Channels stretch(const Channels& in, double ratio);

// Longer or shorter by resampling (pitch moves with it). ratio = output length / input length.
Channels resample(const Channels& in, double ratio);

// Same length, pitch moved by semitones (-24 to 24): stretch, then resample back.
Channels shiftPitch(const Channels& in, double semitones);

} // namespace core::app::dsp
