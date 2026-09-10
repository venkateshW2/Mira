#pragma once

#include <string>
#include <vector>

namespace mira {

struct ActiveSpan {
    double startSeconds;
    double endSeconds;
};

struct ActiveRegionResult {
    double activeRatio = 0.0;              // active seconds / total duration
    std::vector<ActiveSpan> spans;
};

// Frame-energy gate finding the non-silent spans of a file (PRD §5: "required, not an
// optimisation" — delivery stems can be a 3-minute file holding 40 seconds of signal).
// No model, no Essentia algorithm needed beyond basic RMS — a plain sliding-window
// energy gate over the raw samples.
//
// frameSize/hopSize, the -60dB silence threshold, and the 300ms gap-bridging tolerance
// (so a phrase's natural micro-pauses don't fragment into dozens of spans) are a
// first-pass, documented guess, not measured on real material — same caveat as the
// router's thresholds (PRD §12.6).
ActiveRegionResult detectActiveRegions(const std::vector<float>& audio, int sampleRate);

// PRD §5: "always for anything routed as a stem, regardless of duration... For every
// other content type, when duration exceeds 5 minutes."
bool shouldRunActiveRegionDetection(const std::string& contentType, double durationSeconds);

} // namespace mira
