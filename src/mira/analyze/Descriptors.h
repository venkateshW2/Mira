#pragma once

#include <string>
#include <vector>

namespace mira {

// DSP descriptors, all content types (PRD §5A). Cheap, interpretable, the backbone of
// one-shot similarity. `harmonicity` from the PRD list is NOT implemented — it needs a
// pitch+harmonic-peaks pipeline (PitchYinFFT -> HarmonicPeaks -> Inharmonicity) that
// hasn't been built yet; everything else in the PRD's list is here.
struct DspDescriptors {
    double integratedLoudnessLufs = 0.0;
    double loudnessRangeLu = 0.0;
    double truePeakDb = 0.0;
    double crestFactor = 0.0;        // peak / mean of |signal| — "how spiky vs. flat"
    double spectralCentroidHz = 0.0; // brightness, averaged over frames
    double spectralFlatness = 0.0;   // noisiness (0=tonal, 1=noise-like), averaged over frames
    double attackTimeSeconds = 0.0;  // whole-file envelope attack; most meaningful on one-shots
};

// Computes descriptors from already-loaded stereo channels at their native sample rate
// (PRD §5 note: on stems this should eventually run over active regions only, once a
// caller threads spans through — see TASKS.md). `left`/`right` must be equal length;
// pass the same buffer twice for mono. Requires an EssentiaEngine to already exist.
DspDescriptors computeDspDescriptors(const std::vector<float>& left,
                                      const std::vector<float>& right, int sampleRate);

std::string toJson(const DspDescriptors& d);

} // namespace mira
