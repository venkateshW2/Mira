#pragma once

#include <string>
#include <vector>

namespace mira {

// DSP descriptors, all content types (PRD §5A). Cheap, interpretable, the backbone of
// one-shot similarity.
struct DspDescriptors {
    double integratedLoudnessLufs = 0.0;
    double loudnessRangeLu = 0.0;
    double truePeakDb = 0.0;
    double crestFactor = 0.0;        // peak / mean of |signal| — "how spiky vs. flat"
    double spectralCentroidHz = 0.0; // brightness, averaged over frames
    double spectralFlatness = 0.0;   // noisiness (0=tonal, 1=noise-like), averaged over frames
    double attackTimeSeconds = 0.0;  // whole-file envelope attack; most meaningful on one-shots
    // 1.0 = purely harmonic tone, 0.0 = highly inharmonic/noisy; averaged over frames with
    // pitch confidence above kHarmonicityMinPitchConfidence (Descriptors.cpp), skipping
    // unpitched/noisy frames rather than letting them drag a meaningless average down.
    // 0.0 with harmonicityFrameCount==0 means no frame was confident enough to measure at
    // all (e.g. a rhythm stem) — a true "unmeasured", not a claim of total inharmonicity.
    double harmonicity = 0.0;
    int harmonicityFrameCount = 0;
};

// Computes descriptors from already-loaded stereo channels at their native sample rate.
// Caller is responsible for restricting these to active regions only where PRD §5 asks
// for it (ActiveRegions.h's extractActiveAudio, applied in main.cpp) — this function
// itself just measures whatever samples it's handed. `left`/`right` must be equal
// length; pass the same buffer twice for mono. Requires an EssentiaEngine to already exist.
DspDescriptors computeDspDescriptors(const std::vector<float>& left,
                                      const std::vector<float>& right, int sampleRate);

std::string toJson(const DspDescriptors& d);

} // namespace mira
