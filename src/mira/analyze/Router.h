#pragma once

#include <string>

namespace mira {

// Content-type router (PRD §5): "Duration, onset density and loop-point heuristics
// separate one-shot / loop / track." Only the duration cut is implemented so far —
// onset rate is computed and returned (so it lands in `machine` for later tuning) but
// does not yet move the boundary, and the loop-point heuristic (checking whether a
// clip's start/end line up for a tight loop) isn't implemented at all. The three
// thresholds below are a first, documented guess, not measured on real material —
// PRD §12.6's "store everything, tune thresholds later" applies here too.
struct RoutingResult {
    bool ok = false;               // false if the file couldn't be decoded
    std::string contentType;       // one_shot | loop | track
    double durationSeconds = 0.0;
    double onsetRate = 0.0;        // onsets per second — collected, not yet used to route
    int onsetCount = 0;
};

// Decodes `path` (via Essentia's MonoLoader — PRD §7 eventually moves runtime decode to
// JUCE's AudioFormatManager to drop the ffmpeg dependency; this is the analysis-engine
// path used by the CLI, not that final decision) and classifies it by duration alone:
//   duration <= kOneShotMaxSeconds        -> one_shot
//   kOneShotMaxSeconds < duration <= kLoopMaxSeconds -> loop
//   duration > kLoopMaxSeconds            -> track
// Requires an EssentiaEngine to already be constructed (essentia::init() called).
RoutingResult routeContentType(const std::string& path);

inline constexpr double kOneShotMaxSeconds = 3.0;
inline constexpr double kLoopMaxSeconds = 30.0;

} // namespace mira
