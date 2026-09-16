#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mira {

// Groove measurement over stored onset times (PRD §12.6's "store everything, tune
// thresholds later", now that a real beat corpus has landed).
//
// It needs NO AUDIO -- everything here is arithmetic over `$.onset_times`, which
// `mira analyze --groove` already stored. That is why it lives in mira_core beside
// CueDetection rather than in the heavy `mira` target: the UI derives a grid in
// microseconds instead of shelling out, and the CLI links the same objects so a caption
// and a drawn overlay can never disagree about where the beat is.
//
// WHY THIS EXISTS AT ALL. CaptionFields.h already records the two findings that forced
// it: swing measured off `beat_this_beats` sat between 1.000 and 1.023 for every decile
// in the library, and "`beat_this_beats` holds BEATS, so 8th/16th-note swing is invisible
// to it regardless". Both turned out to be the same problem seen from two sides -- the
// beat track, not the music, was the flat thing.
//
// Measured over the 94-file Amon Tobin / Two Fingers corpus (2026-09-15), taking the
// onset phase histogram's peak/uniform ratio as the test of whether a grid is actually
// locked to the audio:
//
//     grid source            median peak/uniform   max    tracks above 1.5
//     `beat_this_beats`              1.17          1.36        0 / 94
//     fitted from onsets             1.93          6.93       76 / 94
//
// The fitted grid wins on 94 of 94 -- not a majority, all of them. A ceiling of 1.36
// across a whole catalogue is the signature of a grid that never locked: it is why a
// programmed halftime beat and an orchestral cue previously scored identically on swing,
// pocket and syncopation, which is what exposed the bug.
//
// The two estimators mira already stores were checked against the fit the same way:
//
//     `essentia_bpm`  agrees on 77/94 (82%), 49 of those at the same octave
//     `beat_this_bpm` agrees on 12/94 (13%)
//
// So `beat_this` is not dropped -- it remains the better estimator on songs, which is
// what it was chosen for -- but on this material it is the third opinion, not the first.
// The period comes from the onsets themselves; the two scalars only pick the OCTAVE,
// which is the one thing a phase-concentration search genuinely cannot do (a grid at
// twice the tempo fits the same onsets exactly as well).
//
// Nothing here re-analyzes anything. Every file that was analyzed with --groove already
// has what this reads.

// Bins the onset phase histogram is measured in. Sixteen puts one bin on each 16th-note
// cell of a 4/4 beat at the 4-beat level, which is the resolution the swing and
// syncopation questions are actually asked at.
constexpr int kGroovePhaseBins = 16;

// Search bounds for the underlying pulse, in seconds. 0.28..1.20 s is 50..214 BPM at the
// level the search actually finds, which is usually the tatum (the fastest regular
// subdivision) rather than the notated beat -- pickBeatOctave() is what turns one into
// the other.
constexpr double kGrooveMinPeriodSeconds = 0.28;
constexpr double kGrooveMaxPeriodSeconds = 1.20;

// Below this many onsets there is not enough evidence to fit a period at all; the result
// comes back invalid rather than confidently wrong.
//
// RE-MEASURED 2026-09-17, and it had to be: the number encodes a DURATION, and the
// onset rate underneath it doubled when detection moved off Essentia's `OnsetRate` to
// mira's own flux at hop 256 (Onsets.h). The intent has always been "roughly 15 seconds
// of even sparse material". Measured over the same 94-file Amon Tobin corpus:
//
//     onsets/second   min 5.61   p10 7.44   median 9.32   max 12.43   (was ~4.8 median)
//
// At 40, the floor had quietly become 4.3 seconds of median material -- permissive
// enough to fit a "grid" to a one-bar fragment. 15 s at the SPARSEST observed rate
// (5.61/s) is 84 onsets, so 80 restores the original intent at the new resolution.
//
// This is the failure mode conventions 2 and 4 in CLAUDE.md are about: a threshold is a
// statement about a distribution, and it stops being true the moment the distribution
// moves underneath it.
constexpr int kGrooveMinOnsets = 80;

// A fitted grid whose phase histogram is this flat is not a grid -- it is a null result.
// Metrics derived from it would be noise wearing a number's clothes, so they are omitted
// instead.
//
// ORIGINALLY set at the midpoint of two medians measured with Essentia's onsets: 1.17
// (the flat null `beat_this` produced on all 94 files) and 1.93 (mira's fitted grid).
//
// RE-MEASURED 2026-09-17 with the new onsets, against `beat_this_beats` on the same 94
// files: min 1.22, p33 1.94, median 2.20, p66 2.49, max 3.87. The old 1.17 "null" is now
// below the worst file in the corpus -- better onsets moved the whole distribution up,
// which is the point of them. Rejection counts at candidate thresholds:
//
//     1.3 -> 2/94     1.4 -> 6/94     1.5 -> 11/94     1.6 -> 16/94
//
// Kept at 1.50. It now sits near the 12th percentile rather than at a midpoint between
// a null and a result, which is a different justification for the same number and is
// recorded as such rather than left looking like the old one still holds.
constexpr double kGrooveMinGridStrength = 1.50;

struct GrooveGrid {
    bool valid = false;
    double periodSeconds = 0.0; // ONE BEAT, after octave selection
    double phaseSeconds = 0.0;  // where the grid's first line falls, in file time
    double bpm = 0.0;
    // Peak/uniform ratio of the onset phase histogram on this grid. 1.0 is perfectly
    // flat (onsets fall anywhere, i.e. no grid); the calibration corpus's most rigidly
    // programmed tracks reach 6.9. This doubles as a genuinely new caption axis --
    // "programmed" versus "free" -- which none of the existing fields capture: in the
    // corpus the ambient pieces (Foley Room, Dream Sequence, One Small Step) sit near
    // 1.1 while the beat tracks (El Wraith, Stude, Smurf) sit above 4.
    double strength = 0.0;
    double resultant = 0.0; // circular concentration |R| at this period, the search's own objective
    // Which scalar chose the octave: "essentia" | "beat_this" | "onsets" (neither agreed,
    // so the fit's own most plausible multiple was taken). Carried so a surprising BPM is
    // accountable rather than anonymous.
    std::string octaveSource = "onsets";
    // Normalised so the mean bin is 1.0 -- i.e. these ARE the peak/uniform ratios, per
    // bin. Drawn directly by the UI's groove overlay.
    std::vector<double> phaseHistogram;
};

struct GrooveResult {
    GrooveGrid grid;

    // Where the off-beat 8th actually lands inside the beat. 0.5 is dead straight,
    // 0.667 is full triplet swing, and anything above ~0.54 is audible. Measured as the
    // mean phase of onsets in the middle half of the beat, so it reads the off-beats
    // themselves rather than an inter-beat interval ratio -- which is precisely what
    // `beat_this_beats` could not see, since it holds beats and never 8ths.
    std::optional<double> swing;

    // Mean SIGNED distance from the nearest 16th-note grid line, in fractions of a beat.
    // Negative is ahead of the grid (rushing), positive is behind it (laid back). Zero
    // is machine-quantised. This is the axis that separates a producer who plays behind
    // the beat from one who programs on it.
    std::optional<double> pocket;

    // Share of onsets whose nearest 16th cell is NOT a beat (cell 0, 4, 8, 12 of the bar
    // level this measures at). A four-on-the-floor kick pattern drives this down; a
    // broken-beat pattern drives it up.
    std::optional<double> syncopation;

    // Onsets per beat on the fitted grid. Reported because it is the sanity check that
    // caught the original bug -- densities were fine (1.7..3.0) while the phase histogram
    // was flat, which is what proved the onsets were good and the grid was wrong.
    double onsetsPerBeat = 0.0;

    // Why a metric is missing, when one is. Empty when everything was measurable.
    std::string omittedReason;
};

// `essentiaBpm` and `beatThisBpm` are `$.rhythm.essentia_bpm` / `$.rhythm.beat_this_bpm`;
// either or both may be absent. They are used ONLY to choose the octave -- see the header
// comment for why the period itself never comes from them.
// How well a set of beat times explains the onsets: the peak/uniform ratio of the onset
// phase histogram against that beat sequence. Same yardstick analyzeGroove uses on its own
// fitted grid, so the two are directly comparable -- which is the point. It answers "does
// the audio support this grid" for ANY grid, including one that came from a tracker.
//
// Built because drawing meter bars on beat_this's beats put them on a grid the audio does
// not support: on NIN's La Mer the onsets score 1.79x against essentia's 85.4 BPM and only
// 1.12x against beat_this's 93.0, and the bars visibly failed to line up with anything.
// A grid has to be checked before it is drawn, not assumed because a tracker produced it.
double gridConcentration(const std::vector<double>& onsetTimes,
                         const std::vector<double>& beats);

GrooveResult analyzeGroove(const std::vector<double>& onsetTimes,
                           std::optional<double> essentiaBpm,
                           std::optional<double> beatThisBpm);

// Same measurements, but against a beat grid the caller already has (`beats` in seconds,
// ascending) rather than a period fitted from the onsets.
//
// ADDED 2026-09-17, and it REVERSES Phase 6. Phase 6 measured `beat_this_beats` flat at
// 1.17 median peak/uniform against mira's fitted grid's 1.93, and concluded the fitted
// grid won 94 of 94. Re-measured on the same 94 files with the onsets corrected
// (Onsets.h -- the old ones came from Essentia and were tracking its tempo error):
//
//     beat_this_beats   min 1.22   p33 1.92   median 2.18   p66 2.46   max 3.84
//     fitted grid       min 1.04   p33 1.16   median 1.23   p66 1.34   max 2.94
//
// Exactly backwards. The fitted grid only ever looked better because it was fitting the
// artefact: Essentia's onsets clustered near 159 BPM on five unrelated songs, and a
// constant-period search finds a clean period in that because the artefact IS periodic.
// Give the fitter real onsets and it loses, because a constant period cannot follow a
// performance and the detected beats can.
//
// The practical consequence was 72 of 94 files falling below kGrooveMinGridStrength, so
// two thirds of the corpus lost `groove` and `swing` entirely.
//
// Phase 6's other findings stand -- the flat-histogram bug was real and the tempo it
// produced was wrong. What is retracted is the conclusion about WHICH grid to prefer,
// which was drawn from a measurement whose input was broken.
//
// The grid returned carries the MEDIAN beat interval as its period so callers that draw
// it keep working, but every phase is measured against the actual bracketing beats, so
// drift is followed rather than averaged away.
GrooveResult analyzeGrooveOnGrid(const std::vector<double>& onsetTimes,
                                 const std::vector<double>& beats);

} // namespace mira
