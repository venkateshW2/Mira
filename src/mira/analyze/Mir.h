#pragma once

#include <string>
#include <vector>

namespace mira {

// Rhythm/MIR (PRD §5B, §14.1). beat_this_cpp (neural beat/downbeat tracker) is the
// primary and default tempo estimator — measured more accurate (it's the only one that
// gives downbeats, and handles syncopated/complex material better) and, on a real 5:08
// song, ~3.4x cheaper to skip than to run relative to the alternative (9.2s vs 2.7s).
// Essentia's RhythmExtractor2013 is now an opt-in recheck: pass runRecheck=true to also
// run it (plus BeatsLoudness at its beats) purely for comparison against beat_this_cpp —
// bpmRatio is that comparison signal, not resolved by picking a winner.
// Caller is responsible for gating this to loops/tracks/stems, not one-shots (§5).
struct RhythmResult {
    bool ok = false;

    // beat_this_cpp — the default, primary tempo/beat/downbeat estimator
    double beatThisBpm = 0.0;              // derived from mean beat interval; 0 if <2 beats found
    std::vector<double> beatThisBeats;      // seconds
    std::vector<double> beatThisDownbeats;  // seconds

    // RhythmExtractor2013 (multifeature) — only populated when runRecheck=true
    double essentiaBpm = 0.0;
    double essentiaConfidence = 0.0;
    std::vector<double> essentiaBeatTicks; // seconds — the full array, not just the BPM scalar (§6)
    double beatsLoudnessMean = 0.0;        // only populated when runRecheck=true (needs essentia's beats)

    // essentiaBpm / beatThisBpm — near 1.0 is agreement; near 2.0 or 0.5 is the classic
    // octave (double/half-time) error PRD §10's risk table flags. Only meaningful when
    // runRecheck=true (stays 0 otherwise). A first-pass raw signal, not a resolved
    // verdict — rendering/thresholding is a later phase's job (§12.6: "store everything,
    // tune thresholds later").
    double bpmRatio = 0.0;

    double danceability = 0.0;
};

// Requires an EssentiaEngine already constructed. `beatThisModelPath` is
// vendor/beat_this_cpp/onnx/beat_this.onnx — not vendored into mira's own repo (PRD §7:
// "79 MB ONNX committed in-tree" refers to beat_this_cpp's own repo, which
// scripts/fetch-vendor.sh clones). `runRecheck` opts into also running Essentia's
// RhythmExtractor2013 for comparison (off by default — it's the slower, less accurate
// estimator of the two; see struct comment above).
RhythmResult analyzeRhythm(const std::vector<float>& mono, int sampleRate,
                            const std::string& beatThisModelPath, bool runRecheck = false);

std::string toJson(const RhythmResult& r);

} // namespace mira
