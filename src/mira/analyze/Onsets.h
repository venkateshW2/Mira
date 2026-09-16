#pragma once

#include <vector>

namespace mira {

// Onset detection from a spectral-flux envelope, computed here rather than taken from
// Essentia's OnsetRate.
//
// WHY THIS EXISTS (2026-09-17). Onsets used to come from Essentia's `OnsetRate`, and
// they were wrong in a way that was invisible from inside the pipeline. Measured against
// six Amon Tobin / Two Fingers tracks whose tempo the user knows, the stored onsets
// phase-locked to the true tempo at R = 0.024, 0.006, 0.022, 0.011, 0.001, 0.006 -- that
// is zero, on 599 to 1278 onsets per file. They instead peaked at 158-178 BPM on five
// different songs, a near-constant answer across unrelated material, which is the
// signature of an algorithm artefact rather than of music. Everything downstream --
// groove, swing, pocket, syncopation, the fitted grid -- was measuring that artefact.
//
// The user's own diagnosis was exact: "onset cant come from essentia because it
// detecting tempo wrong period."
//
// WHAT THIS DOES INSTEAD. One STFT, a log-mel spectrogram, the positive first difference
// averaged across bands (the standard onset detection function), then adaptive-threshold
// peak picking. It is the same front end as the collaborator's bar-detection tool, which
// is in production use and gets these tracks right, and it carries no tempo model at
// all: an onset here cannot inherit a tempo error, because nothing upstream of it has an
// opinion about tempo.
//
// PRECISION IS THE OTHER HALF. `OnsetRate` quantised onsets to ~11.6 ms, which is
// exactly why `pocket` is measured and never captioned -- the middle two thirds of the
// Amon Tobin corpus spans -2.4 to +2.0 ms, i.e. inside a single step (ANALYSIS.md §5).
// At kOnsetHopSize the step is 5.8 ms at 44.1 kHz. That is the difference between
// "pocket is rounding" and "pocket is a measurement".
//
// Note this is a SEPARATE pass from computeSpectralAverages' shared Windowing->Spectrum
// loop, which runs at hop 1024 (23.2 ms) because that is the resolution beat-synchronous
// meter features want. Reusing it would have made the onsets coarser than the Essentia
// pass being removed, which is the opposite of the point. The two hops answer different
// questions and neither is free to move for the other's sake.

// 5.8 ms at 44.1 kHz. Half the frame advance of Essentia's OnsetRate.
constexpr int kOnsetFrameSize = 2048;
constexpr int kOnsetHopSize = 256;

struct OnsetResult {
    bool ok = false;
    std::vector<double> times;   // seconds
    double rate = 0.0;           // onsets per second over the analysed duration
    // The envelope itself, for drawing and for debugging a wrong answer by eye. One
    // value per hop; `hopSeconds` says what a step is worth.
    std::vector<float> envelope;
    double hopSeconds = 0.0;
};

// `mono` at `sampleRate`. Requires an EssentiaEngine to already exist (it uses Essentia's
// Windowing/Spectrum/MelBands for the transform, not its onset detectors).
OnsetResult detectOnsets(const std::vector<float>& mono, int sampleRate);

} // namespace mira
