#pragma once

#include <string>
#include <vector>

namespace mira {

// Content-type router (PRD §5): "Duration, onset density and loop-point heuristics
// separate one-shot / loop / track." Only the duration cut is implemented so far —
// onset rate is computed and returned (so it lands in `machine` for later tuning) but
// does not yet move the boundary, and the loop-point heuristic (checking whether a
// clip's start/end line up for a tight loop) isn't implemented at all. The three
// thresholds below are a first, documented guess, not measured on real material —
// PRD §12.6's "store everything, tune thresholds later" applies here too.
struct RoutingResult {
    std::string contentType;       // one_shot | loop | track
    double durationSeconds = 0.0;
    double onsetRate = 0.0;        // onsets per second — collected, not yet used to route
    int onsetCount = 0;
};

// Classifies already-decoded mono audio. Requires an EssentiaEngine to already be
// constructed (essentia::init() called) — used for the OnsetRate algorithm.
RoutingResult routeContentType(const std::vector<float>& audio, int sampleRate);

inline constexpr double kOneShotMaxSeconds = 3.0;
inline constexpr double kLoopMaxSeconds = 30.0;

} // namespace mira
