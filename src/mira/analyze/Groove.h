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
// comes back invalid rather than confidently wrong. 40 is roughly 15 seconds of even
// sparse material, and the sparsest track in the calibration corpus carried 1.7 onsets
// per beat.
constexpr int kGrooveMinOnsets = 40;

// A fitted grid whose phase histogram is this flat is not a grid -- it is the null
// result that `beat_this` produced on all 94 files. Metrics derived from it would be
// noise wearing a number's clothes, so they are omitted instead. Set at the midpoint of
// the two measured medians (1.17 flat, 1.93 fitted).
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
GrooveResult analyzeGroove(const std::vector<double>& onsetTimes,
                           std::optional<double> essentiaBpm,
                           std::optional<double> beatThisBpm);

} // namespace mira
