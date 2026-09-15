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
    // Every onset's position in seconds. Essentia's OnsetRate returns these on the way to
    // the count, and mira threw them away for the whole of Part 1 -- keeping them costs
    // nothing at analysis time and is the ONLY thing that makes groove measurable.
    //
    // Why they matter: every field mira captions today is a whole-file average, and
    // groove is not an average -- it is where onsets sit RELATIVE TO THE BEAT GRID.
    // `beat_this_beats` supplies the grid, these supply the events, and the phase
    // between them yields swing (do 8ths fall at 50% or 62% between beats), pocket (are
    // onsets consistently ahead of or behind the grid) and syncopation (how much lands
    // off the beat at all). CaptionFields.h deferred `swing` with "note that
    // beat_this_beats holds BEATS, so 8th/16th-note swing is invisible to it regardless"
    // -- this is what fixes the "regardless".
    std::vector<double> onsetTimes;
};

// Classifies already-decoded mono audio. Requires an EssentiaEngine to already be
// constructed (essentia::init() called) — used for the OnsetRate algorithm.
RoutingResult routeContentType(const std::vector<float>& audio, int sampleRate);

inline constexpr double kOneShotMaxSeconds = 3.0;
inline constexpr double kLoopMaxSeconds = 30.0;

} // namespace mira
