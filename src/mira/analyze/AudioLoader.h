#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mira {

struct LoadedAudio {
    std::vector<float> mono;    // (left+right)/2, or the single channel if numChannels==1
    std::vector<float> left;
    std::vector<float> right;   // == left when numChannels==1
    int sampleRate = 0;         // the file's own rate — no forced resample
    int numChannels = 0;
    double durationSeconds = 0.0;
};

// Loads `path` via Essentia's AudioLoader, at the file's native sample rate (no forced
// resample — PRD §7 eventually moves runtime decode to JUCE's AudioFormatManager to drop
// the ffmpeg dependency; this is the analysis-engine path used by the CLI). Mono files
// get `right` duplicated from `left` (Essentia's AudioLoader leaves it unset). Only
// mono/stereo are handled correctly — AudioLoader itself doesn't support more channels.
// Returns nullopt rather than throwing if the file can't be opened or decoded.
std::optional<LoadedAudio> loadAudio(const std::string& path);

} // namespace mira
