#pragma once

#include <string>
#include <vector>

namespace mira {

// Rhythm/MIR (PRD §5B, §14.1). Two independent tempo estimators run and their
// *disagreement* is stored as the confidence signal — not resolved by picking one.
// Caller is responsible for gating this to loops/tracks/stems, not one-shots (§5).
struct RhythmResult {
    bool ok = false;

    // RhythmExtractor2013 (multifeature)
    double essentiaBpm = 0.0;
    double essentiaConfidence = 0.0;
    std::vector<double> essentiaBeatTicks; // seconds — the full array, not just the BPM scalar (§6)

    // beat_this_cpp
    double beatThisBpm = 0.0;              // derived from mean beat interval; 0 if <2 beats found
    std::vector<double> beatThisBeats;      // seconds
    std::vector<double> beatThisDownbeats;  // seconds

    // essentiaBpm / beatThisBpm — near 1.0 is agreement; near 2.0 or 0.5 is the classic
    // octave (double/half-time) error PRD §10's risk table flags. A first-pass raw
    // signal, not a resolved verdict — rendering/thresholding is a later phase's job
    // (§12.6: "store everything, tune thresholds later").
    double bpmRatio = 0.0;

    double beatsLoudnessMean = 0.0;
    double danceability = 0.0;
};

// Requires an EssentiaEngine already constructed. `beatThisModelPath` is
// vendor/beat_this_cpp/onnx/beat_this.onnx — not vendored into mira's own repo (PRD §7:
// "79 MB ONNX committed in-tree" refers to beat_this_cpp's own repo, which
// scripts/fetch-vendor.sh clones).
RhythmResult analyzeRhythm(const std::vector<float>& mono, int sampleRate,
                            const std::string& beatThisModelPath);

std::string toJson(const RhythmResult& r);

} // namespace mira
