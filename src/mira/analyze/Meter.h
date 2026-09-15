#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mira {

// Meter detection -- how many beats make a bar (TASKS.md Phase 7).
//
// mira has always known where the beats are and never how many make a bar, so a 6/8
// piece and a 4/4 piece came out of analysis described identically. This fills that in.
//
// The method is ported from a bar-detection tool shared by a collaborator
// (2026-09-16), whose meter layer turned out to be separable from its heavy half: its
// `detect_meter(feats, env, beats)` takes the beat times as an ARGUMENT, so it needs
// neither its DBN nor madmom nor any model. That is why this can live in mira_core with
// CueDetection and Groove -- given the features and the beats it is pure arithmetic, no
// audio and no ONNX, and the UI can run it inline.
//
// HOW IT WORKS, in the order the code runs:
//
//   1. BEAT-SYNCHRONOUS FEATURES. Average each per-frame feature across the frames
//      spanned by each beat, giving one vector per beat. Bars repeat, so the question
//      "how long is a bar" becomes "at what lag does a beat resemble an earlier beat".
//   2. SELF-SIMILARITY. Cosine similarity of every beat against every other.
//   3. AUTOCORRELATION over that matrix: the mean of the k-th diagonal is how alike
//      beats k apart are, for k = 1..16. The peak is the bar length. Lags 1-3 are
//      excluded -- a bar is never that short.
//   4. THREE FEATURES, INDEPENDENTLY. MFCC (timbre), chroma (harmony) and mel (spectral
//      shape) each vote. Agreement is the common case and is left alone.
//   5. ACCENTS ARBITRATE, but only on a genuine disagreement about a bar length worth
//      arguing over. The onset envelope is sampled at each beat, folded onto each
//      candidate period, and scored by ANOVA F -- how much louder some bar positions are
//      than others, relative to the within-position spread. F is used rather than plain
//      variance because it is comparable across different periods and does not depend on
//      where beat 1 happens to fall.
//   6. SNAP. 8 is two bars of 4 and 12 is two bars of 6; the autocorrelation legitimately
//      prefers the longer repeat, so these fold down.
//
// Verified against the collaborator's own tool before porting: NIN's La Mer comes out 6
// (correct, and with near-perfect bar spread), The Frail reports itself uneven rather
// than asserting a meter, and March Of The Pigs is missed -- it alternates 7/8 and 4/4,
// so arguably no single meter is right for it.

// Longest bar considered, in beats.
constexpr int kMeterMaxLag = 16;
// A bar is never 1, 2 or 3 beats -- these lags are excluded from the peak search.
constexpr int kMeterLagSkip = 3;
// Bar lengths worth letting the accents arbitrate over. A disagreement that involves
// none of these is not a real argument.
constexpr int kMeterConfusionSet[] = { 3, 4, 6, 8, 12 };
// A period needs this many whole bars in the track before it is believable.
constexpr int kMeterMinCycles = 4;
// Below this many beats there is no point trying.
constexpr int kMeterMinBeats = 12;
// Above this the accent evidence is strong; below it the accents still decide, but the
// result is reported as weak.
constexpr double kMeterAccentWeakF = 1.0;
// Bar-length spread (longest/shortest) above which the grid is reported as uneven. The
// collaborator's own threshold, and the reason this is worth having at all: mira has had
// no confidence signal on its beat grid, which is exactly how the flat-groove bug
// (Phase 6) survived unnoticed.
constexpr double kMeterSpreadWarn = 1.5;

// One per-frame feature matrix: `dims` rows by `frames` columns, stored row-major as
// `values[dim * frames + frame]`. The analyzer fills these from the frames it already
// computes -- see Descriptors.cpp, whose single Windowing->Spectrum pass already
// produces MFCC, chroma and the mel bands, and whose hop happens to match the reference
// implementation's time resolution exactly (1024/44100 == 512/22050 == 23.2 ms).
struct MeterFeature {
    std::string name;
    int dims = 0;
    int frames = 0;
    std::vector<float> values;
};

struct MeterResult {
    bool valid = false;
    int meter = 0;          // beats per bar, after snapping
    int meterPreSnap = 0;   // what the evidence said before 8->4 / 12->6
    int acfMeter = 0;       // the strongest feature's own answer, before any arbitration
    std::vector<int> featureWinners; // one per feature, in the order supplied
    std::string strongestFeature;
    bool arbitrated = false;         // did the accents settle a disagreement
    double accentF = 0.0;            // ANOVA F of the winning period, when arbitrated
    double accentMargin = 0.0;       // F ratio over the runner-up; 0 when there was none
    // Longest bar / shortest bar. Above kMeterSpreadWarn the meter may still be right but
    // the beat grid drifted -- report it, do not hide it.
    double barSpread = 0.0;
    int barCount = 0;
    std::string reason; // why this meter, in words -- carried so a surprise is accountable
};

// `features` are the per-frame matrices (MFCC, chroma, mel -- any number, but they must
// share a hop); `onsetEnvelope` is one value per frame, used only for the accent
// arbitration; `beats` are beat times in seconds, from whichever grid the caller trusts.
// `hopSeconds` converts frame index to time.
//
// Phase 6 measured mira's own fitted grid better than `beat_this_beats` on beat-driven
// material, so the fitted grid is the better input where onsets locked -- but this
// function takes whatever it is given and says nothing about where the beats came from.
MeterResult detectMeter(const std::vector<MeterFeature>& features,
                        const std::vector<float>& onsetEnvelope,
                        const std::vector<double>& beats,
                        double hopSeconds);

} // namespace mira
