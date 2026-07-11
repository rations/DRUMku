// WAV sample loading and sample-rate conversion for DRUMku.
//
// Loads a PCM/float .wav (16/24/32-bit int or 32-bit float, mono or stereo)
// into planar float channels, then resamples to the engine's sample rate. This
// runs on the message thread at load time only (never on the RT thread), so the
// windowed-sinc resampler's cost is not a concern; the RT thread only ever
// reads the finished float buffers.

#pragma once

#include <string>
#include <vector>

namespace DRUMku {

// A decoded, resampled drum sample. Mono samples leave ch1 empty; the engine
// reads ch1 only when channels > 1.
struct DrumSample {
    int channels = 1; // 1 or 2
    int frames = 0;   // per-channel frame count
    std::vector<float> ch0;
    std::vector<float> ch1;
};

// Load `path` and resample to `targetRate` Hz. Returns true on success and fills
// `out`; returns false (leaving `out` unspecified) on any read/parse error or an
// unsupported format. Never throws.
bool wav_load_resampled(const std::string &path, double targetRate, DrumSample &out);

} // namespace DRUMku
