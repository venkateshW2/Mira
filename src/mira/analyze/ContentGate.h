#pragma once

#include <string>
#include <vector>

namespace mira {

// CED-small (PRD §2c) — "is this even music?" general-audio content gate, run before any
// music-specific classification head. The same honesty principle already applied to
// key/chords via the harmonicity gate (a field recording or noise stem pushed through a
// genre/mood classifier returns "confident nonsense" — PRD §2c), just at the whole-file
// AudioSet-taxonomy level instead of the DSP-frame level.
struct ContentGateResult {
    bool ok = false;
    bool isMusic = false; // AudioSet class 137 ("Music") score > kContentGateMusicThreshold
    double musicScore = 0.0;
    struct TopLabel {
        std::string name;
        double score = 0.0;
    };
    std::vector<TopLabel> topLabels; // top-5 AudioSet labels by score, for a human to sanity-check the gate
};

// `mono`/`sampleRate` are the already-decoded audio; resampled to 16kHz internally.
// `modelPath` is models/content-gate/ced-small/model.onnx.
ContentGateResult runContentGate(const std::vector<float>& mono, int sampleRate,
                                  const std::string& modelPath);

std::string toJson(const ContentGateResult& g);

} // namespace mira
