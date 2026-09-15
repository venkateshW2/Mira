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
    // Share of energy below 80 Hz. spectral_centroid is a mean dominated by the mids and
    // is blind to the sub, so a track built on a 40 Hz fundamental scores like one with
    // no bottom end. See Descriptors.cpp for why the band is 20-80 and not 20-60.
    double subRatio = 0.0;
    // How fast the spectrum moves frame to frame, and how steadily. flatness says what
    // the spectrum looks like; flux says whether it is going anywhere -- a drone and a
    // continuously morphing texture can share a flatness and differ wildly here.
    double fluxMean = 0.0;
    double fluxStddev = 0.0;
    double attackTimeSeconds = 0.0;  // whole-file envelope attack; most meaningful on one-shots
    // 1.0 = purely harmonic tone, 0.0 = highly inharmonic/noisy; averaged over frames with
    // pitch confidence above kHarmonicityMinPitchConfidence (Descriptors.cpp), skipping
    // unpitched/noisy frames rather than letting them drag a meaningless average down.
    // 0.0 with harmonicityFrameCount==0 means no frame was confident enough to measure at
    // all (e.g. a rhythm stem) — a true "unmeasured", not a claim of total inharmonicity.
    double harmonicity = 0.0;
    int harmonicityFrameCount = 0;
    // Timbre (13-coefficient MFCC, mel filterbank -> DCT) and pitch-class content
    // (12-bin HPCP/chroma), both frame-averaged over the same shared spectral pass as
    // centroid/flatness/harmonicity above. For similarity search (PRD's 2D-corpus-style
    // "sounds like" comparison), not for a human to read directly.
    std::vector<double> mfcc;    // size 13 (numberCoefficients), or empty if unmeasurable
    std::vector<double> chroma;  // size 12 (HPCP), or empty if unmeasurable
};

// Computes descriptors from already-loaded stereo channels at their native sample rate.
// Caller is responsible for restricting these to active regions only where PRD §5 asks
// for it (ActiveRegions.h's extractActiveAudio, applied in main.cpp) — this function
// itself just measures whatever samples it's handed. `left`/`right` must be equal
// length; pass the same buffer twice for mono. Requires an EssentiaEngine to already exist.
// Per-frame features kept alongside the averages, for meter detection (Meter.h). The
// frames are computed by the loop below either way -- this only stops them being
// discarded. OFF by default because a 5-minute track is ~13,000 frames and keeping MFCC
// + chroma + mel bands + flux for all of them costs a few MB that nothing else wants.
//
// The hop matches the reference meter implementation's time resolution exactly
// (kHopSize/44100 == 512/22050 == 23.2 ms), which is why beat-synchronous averaging over
// these frames lines up with it rather than needing its own pass.
struct SpectralFrames {
    int frames = 0;
    double hopSeconds = 0.0;
    std::vector<float> mfcc;   // kNumMfccCoefficients rows x frames, row-major
    std::vector<float> chroma; // kChromaSize rows x frames
    std::vector<float> mel;    // melBands rows x frames
    int melBands = 0;
    std::vector<float> flux;   // one per frame -- the onset envelope the accents read
};

DspDescriptors computeDspDescriptors(const std::vector<float>& left,
                                      const std::vector<float>& right, int sampleRate,
                                      SpectralFrames* framesOut = nullptr);

std::string toJson(const DspDescriptors& d);

} // namespace mira
