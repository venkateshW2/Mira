#include "Mir.h"

#include <essentia/algorithmfactory.h>
#include <beat_this_api.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <sstream>
#include <string>

namespace mira {

namespace {

// A beat interval within this fraction of the median counts as "even" for
// gridStability. 0.25 is the collaborator's STABILITY_TOL, kept as-is: it is wide enough
// that ordinary human push-and-pull passes and narrow enough that a doubled or halved
// pulse (a 2x error, i.e. 100% off) never can.
constexpr double kGridStabilityTol = 0.25;

// Below this stability the DBN grid is uneven enough to be worth a second opinion, so
// the minimal grid is decoded too and the steadier one wins. The collaborator's
// CROSS_CHECK_BELOW, unchanged. On the six ground-truth tracks it fires on exactly the
// three that are wrong (0.64, 0.67, 0.85) and on none of the three that are right
// (0.99, 1.00, 1.00).
constexpr double kGridCrossCheckBelow = 0.90;

// A grid with fewer beats than this is not a grid. Guards the cross-check against
// swapping a real, slightly-uneven DBN grid for a peak-picker that found almost nothing
// and is therefore trivially "even". The collaborator's MIN_BEATS.
constexpr size_t kGridMinBeats = 12;

// A beat gap must fall this close to a whole number of seed periods to join the
// least-squares fit. Wide enough to keep quantisation noise (+/-0.02 s on a 0.35 s beat
// is 0.057 of a period) and ordinary drift, narrow enough that a beat sitting on an
// off-grid subdivision is excluded rather than averaged in.
constexpr double kGridSnapTol = 0.25;



// Beat-to-beat intervals, in seconds. Shared by the two measurements below, which
// must see exactly the same numbers to be talking about the same grid.
std::vector<double> beatIntervals(const std::vector<float>& beats) {
    std::vector<double> intervals;
    if (beats.size() < 2) return intervals;
    intervals.reserve(beats.size() - 1);
    for (size_t i = 1; i < beats.size(); ++i) intervals.push_back(beats[i] - beats[i - 1]);
    return intervals;
}

// The MEDIAN inter-beat interval, not the mean.
//
// This was the mean until 2026-09-17, and that was a real error, not a preference. A DBN
// grid is allowed to change pulse level partway through a track -- 55-215 BPM is one
// state space, so 86 and its double 172 are both reachable and the model will move
// between them where the activations support it. Two Fingers' "Smurf" is exactly that:
// the beat list holds a 0.70 s cluster (86 BPM, the tempo the user knows it to be) and a
// 0.35 s cluster (172). The MEAN of those is 0.4725 s -> 127.0 BPM, a tempo that occurs
// in neither cluster and nowhere in the song, reported with no hint that anything was
// wrong. The median lands inside whichever cluster is dominant, which is always a tempo
// the track actually plays.
//
// This is `bpm_of` from the collaborator's bar-detection tool, which is the same three
// lines. Its whole robustness lives in the median plus `gridStability` below -- not in
// anything clever downstream.
double medianBeatIntervalBpm(const std::vector<float>& beats) {
    auto intervals = beatIntervals(beats);
    if (intervals.empty()) return 0.0;
    auto mid = intervals.begin() + static_cast<long>(intervals.size() / 2);
    std::nth_element(intervals.begin(), mid, intervals.end());
    double medianInterval = *mid;
    if (intervals.size() % 2 == 0) {
        auto lower = std::max_element(intervals.begin(), mid);
        medianInterval = (medianInterval + *lower) * 0.5;
    }
    return medianInterval > 0.0 ? 60.0 / medianInterval : 0.0;
}

// Fit ONE beat period to the whole beat list, after folding the intervals into a single
// octave. Replaces reporting the median interval, which cannot be right on this data for
// two independent reasons:
//
// 1. QUANTISATION. The network runs at 50 fps, so every beat time is a multiple of
//    0.02 s and so is every interval. At a 0.35 s beat that is +/-5.7%, i.e. +/-9 BPM.
//    Two Fingers' "Smurf" is really 172 BPM (0.349 s); its intervals can only land on
//    0.34 (176.5) or 0.36 (166.7), straddling the truth, and the median picks a bin. No
//    amount of care about WHICH interval you take fixes this -- the answer is not in any
//    single interval. A least-squares fit of beat time against beat index over the whole
//    file averages the quantisation away and resolves the period to under a millisecond.
//    "Surge" is the clean demonstration: it is 87 BPM = 0.690 s, exactly between the
//    0.68 and 0.70 bins, so the median could never report it and the fit does.
//
// 2. OCTAVE MIXING. The DBN's state space spans 55-215 BPM, so it is free to move
//    between a pulse and its double partway through a track, and on halftime electronic
//    it does. "Smurf" holds 165 intervals near 0.35 s and 74 near 0.70 s. Folding every
//    interval into one octave first means the fit sees one consistent grid instead of
//    two interleaved ones.
//
// Returns 0 if there is not enough to fit. `usedFraction` reports the share of beats that
// landed on the fitted grid -- a second, finer confidence number than gridStability,
// since it is measured against the fit rather than against the median.
double fitBeatPeriod(const std::vector<float>& beats, double* usedFraction = nullptr) {
    if (usedFraction) *usedFraction = 0.0;
    auto intervals = beatIntervals(beats);
    if (intervals.size() < kGridMinBeats) return 0.0;

    std::vector<double> sorted = intervals;
    auto mid = sorted.begin() + static_cast<long>(sorted.size() / 2);
    std::nth_element(sorted.begin(), mid, sorted.end());
    double median = *mid;
    if (median <= 0.0) return 0.0;

    // Fold each interval towards the median's octave. Bounds are 0.75x/1.5x rather than
    // the symmetric 0.707x/1.414x so that a genuine 2:1 always folds and ordinary
    // push-and-pull never does.
    std::vector<double> folded;
    folded.reserve(intervals.size());
    for (double interval : intervals) {
        if (interval <= 0.0) continue;
        for (int i = 0; i < 4 && interval < median * 0.75; ++i) interval *= 2.0;
        for (int i = 0; i < 4 && interval > median * 1.5; ++i) interval *= 0.5;
        folded.push_back(interval);
    }
    if (folded.size() < kGridMinBeats) return 0.0;
    auto fmid = folded.begin() + static_cast<long>(folded.size() / 2);
    std::nth_element(folded.begin(), fmid, folded.end());
    double seed = *fmid;
    if (seed <= 0.0) return 0.0;

    // Walk the beats, giving each one an integer index on a `seed` grid. A beat whose
    // gap is not close to a whole number of seeds is off the grid -- excluded from the
    // fit rather than allowed to drag it.
    std::vector<double> x, y;
    double index = 0.0;
    x.push_back(0.0);
    y.push_back(beats.front());
    for (size_t i = 1; i < beats.size(); ++i) {
        double steps = (beats[i] - beats[i - 1]) / seed;
        double rounded = std::round(steps);
        index += rounded;
        if (rounded >= 1.0 && std::abs(steps - rounded) < kGridSnapTol) {
            x.push_back(index);
            y.push_back(beats[i]);
        }
    }
    if (x.size() < kGridMinBeats) return 0.0;
    if (usedFraction) *usedFraction = static_cast<double>(x.size()) / static_cast<double>(beats.size());

    // Least squares: beatTime = period * index + phase. Slope is the period.
    double n = static_cast<double>(x.size());
    double sx = std::accumulate(x.begin(), x.end(), 0.0);
    double sy = std::accumulate(y.begin(), y.end(), 0.0);
    double sxx = 0.0, sxy = 0.0;
    for (size_t i = 0; i < x.size(); ++i) { sxx += x[i] * x[i]; sxy += x[i] * y[i]; }
    double denom = n * sxx - sx * sx;
    if (std::abs(denom) < 1e-12) return 0.0;
    double period = (n * sxy - sx * sy) / denom;
    return period > 0.0 ? period : 0.0;
}

// How even the beat spacing is: the share of intervals within kGridStabilityTol of the
// median. This is mira's first honest confidence number about its own beat grid, and it
// is the measurement the whole 2026-09-16 tempo bug went unnoticed for want of.
//
// Measured against six tracks whose tempo the user knows, it separates them completely:
//
//     Marine Machines   97 BPM   stability 0.99   grid correct
//     Surge             87 BPM   stability 1.00   grid correct
//     Deep Jinx         86 BPM   stability 1.00   grid correct
//     Crunch Rhythm     87 BPM   stability 0.85   grid WRONG (reported 149.4)
//     Smurf (feat)      86 BPM   stability 0.64   grid WRONG (reported 127.0)
//     Smurf (instr)     86 BPM   stability 0.67   grid WRONG (reported 130.2)
//
// A grid that changes pulse level partway through scores low, and nothing built on such
// a grid can be trusted: a bar takes 4 slots in one passage and 8 in another, so meter,
// swing and pocket are all measuring across a seam. Note this says nothing about whether
// the OCTAVE is right -- a perfectly even grid at double the true tempo scores 1.00. It
// says the grid is internally consistent, which is a precondition, not a verdict.
double gridStability(const std::vector<float>& beats) {
    auto intervals = beatIntervals(beats);
    if (intervals.size() < 2) return 0.0;
    std::vector<double> sorted = intervals;
    auto mid = sorted.begin() + static_cast<long>(sorted.size() / 2);
    std::nth_element(sorted.begin(), mid, sorted.end());
    double median = *mid;
    if (median <= 0.0) return 0.0;
    double tolerance = kGridStabilityTol * median;
    size_t even = 0;
    for (double interval : intervals) {
        if (std::abs(interval - median) <= tolerance) ++even;
    }
    return static_cast<double>(even) / static_cast<double>(intervals.size());
}

constexpr double kTempoWindowSeconds = 60.0;
constexpr int kTempoMinBeatsPerWindow = 2;
// First-pass, unmeasured threshold (PRD §12.6) — not derived from any labeled dataset,
// just "a score mix drifting 60->175 BPM should trip this, a stable club track shouldn't."
constexpr double kTempoUnstableStddevBpm = 10.0;

struct TempoStability {
    double stddevBpm = 0.0;
    double rangeBpm = 0.0;
    int windowCount = 0;
    bool unstable = false;
};

// Local BPM per fixed-size window over the full beat timeline, then the spread across
// those windows — a whole-file BPM (medianBeatIntervalBpm above) is silent about
// whether the piece actually holds one tempo or the average is smearing together a slow
// intro and a fast finale.
TempoStability computeTempoStability(const std::vector<double>& beats) {
    TempoStability result;
    if (beats.size() < 2) return result;

    double end = beats.back();
    std::vector<double> windowBpms;
    for (double t = 0.0; t < end; t += kTempoWindowSeconds) {
        std::vector<double> windowBeats;
        for (double b : beats) {
            if (b >= t && b < t + kTempoWindowSeconds) windowBeats.push_back(b);
        }
        if (static_cast<int>(windowBeats.size()) < kTempoMinBeatsPerWindow) continue;
        std::vector<double> intervals;
        for (size_t i = 1; i < windowBeats.size(); ++i) intervals.push_back(windowBeats[i] - windowBeats[i - 1]);
        double meanInterval = std::accumulate(intervals.begin(), intervals.end(), 0.0) / intervals.size();
        if (meanInterval > 0.0) windowBpms.push_back(60.0 / meanInterval);
    }

    // <3 windows (e.g. a short cue under ~3 minutes) isn't enough to tell real tempo
    // drift from beat-tracking noise at the file's edges — leave it unmeasured rather
    // than risk a false "unstable" flag off a two-point spread.
    if (windowBpms.size() < 3) return result;
    result.windowCount = static_cast<int>(windowBpms.size());

    double mean = std::accumulate(windowBpms.begin(), windowBpms.end(), 0.0) / windowBpms.size();
    double variance = 0.0;
    for (double b : windowBpms) variance += (b - mean) * (b - mean);
    variance /= windowBpms.size();
    result.stddevBpm = std::sqrt(variance);

    auto minmax = std::minmax_element(windowBpms.begin(), windowBpms.end());
    result.rangeBpm = *minmax.second - *minmax.first;
    result.unstable = result.stddevBpm > kTempoUnstableStddevBpm;
    return result;
}
} // namespace

RhythmResult analyzeRhythm(const std::vector<float>& mono, int sampleRate,
                            const std::string& beatThisModelPath, bool runRecheck) {
    RhythmResult result;
    if (mono.empty() || sampleRate <= 0) return result;

    std::vector<essentia::Real> audio(mono.begin(), mono.end());

    // --- beat_this_cpp: the default, primary tempo/beat/downbeat estimator ---
    //
    // The DBN grid is the default, and the minimal peak-picked grid is the cross-check:
    // when the DBN's grid comes out UNEVEN -- which is what it looks like from outside
    // when its tempo state has moved to a different pulse level partway through the
    // track -- the steadier of the two wins. Both are decoded from one model pass
    // (process_audio_both), so the check costs a Viterbi and a peak-pick, not a second
    // trip over the audio.
    //
    // Straight from the collaborator's bar-detection tool, whose choose_grid() does
    // exactly this at CROSS_CHECK_BELOW = 0.90. That tool is in production use and gets
    // these tracks right; mira had both postprocessors vendored all along and simply
    // never looked at the second one.
    try {
        BeatThis::BeatThis beatThis(beatThisModelPath, /*use_dbn=*/true);
        auto both = beatThis.process_audio_both(mono, sampleRate, /*channels=*/1);

        const auto* chosen = &both.dbn;
        result.beatGridSource = "dbn";
        result.beatGridStability = gridStability(both.dbn.beats);
        result.beatGridStabilityDbn = result.beatGridStability;
        result.beatGridStabilityMinimal = -1.0; // not decoded unless the cross-check runs

        if (result.beatGridStability < kGridCrossCheckBelow) {
            double minimalStability = gridStability(both.minimal.beats);
            result.beatGridStabilityMinimal = minimalStability;
            // The minimal grid has to be BOTH steadier and long enough to be a grid at
            // all -- a peak-picker that found nine peaks can be trivially "even".
            if (minimalStability > result.beatGridStability
                && both.minimal.beats.size() >= kGridMinBeats) {
                chosen = &both.minimal;
                result.beatGridSource = "minimal";
                result.beatGridStability = minimalStability;
            }
        }

        const auto& beatResult = *chosen;
        result.beatThisBeats.assign(beatResult.beats.begin(), beatResult.beats.end());
        result.beatThisDownbeats.assign(beatResult.downbeats.begin(), beatResult.downbeats.end());

        // The reference's `bpm_of`, and nothing else: 60 / median beat interval.
        //
        // Between 2026-09-17 morning and evening this was an octave-folded least-squares
        // fit, with the octave then chosen by a log-normal prior centred on 120 BPM. Both
        // were mine, neither was in the tool being copied, and the prior in particular
        // was a guess wearing arithmetic: it decided the octave by a knife edge at
        // 120*sqrt(2) = 169.7 BPM, so a 4 BPM difference in the fit flipped a track
        // between 86 and 165. That hit 28 of 94 Amon Tobin files.
        //
        // The median is what the working tool reports. It keeps the fitted-grid numbers
        // beside it (below) so nothing is lost, but the number mira states is the
        // reference's number.
        result.beatThisBpm = medianBeatIntervalBpm(beatResult.beats);
        double usedFraction = 0.0;
        double period = fitBeatPeriod(beatResult.beats, &usedFraction);
        result.beatFitUsedFraction = usedFraction;
        result.beatThisMedianBpm = result.beatThisBpm;
        result.beatFittedBpmRaw = period > 0.0 ? 60.0 / period : 0.0;

        auto stability = computeTempoStability(result.beatThisBeats);
        result.tempoStabilityBpmStddev = stability.stddevBpm;
        result.tempoRangeBpm = stability.rangeBpm;
        result.tempoWindowCount = stability.windowCount;
        result.tempoUnstable = stability.unstable;
    } catch (const std::exception&) {
        // beatThis* fields stay at their defaults — essentia's recheck below can still stand alone.
    }

    // --- Danceability (cheap, independent of which tempo estimator runs) ---
    try {
        auto& factory = essentia::standard::AlgorithmFactory::instance();
        std::unique_ptr<essentia::standard::Algorithm> danceability(
            factory.create("Danceability", "sampleRate", static_cast<essentia::Real>(sampleRate)));
        essentia::Real value = 0;
        std::vector<essentia::Real> dfa;
        danceability->input("signal").set(audio);
        danceability->output("danceability").set(value);
        danceability->output("dfa").set(dfa);
        danceability->compute();
        result.danceability = value;
    } catch (const essentia::EssentiaException&) {
        // stays 0
    }

    // --- Essentia: RhythmExtractor2013 (multifeature) — opt-in recheck/comparison only.
    // Measured ~3.4x cheaper than beat_this_cpp (2.7s vs 9.2s on a 5:08 song) but also
    // the less accurate of the two (no downbeats, weaker on syncopated material), so it's
    // not worth its cost by default — only run it when a recheck against beat_this_cpp is
    // explicitly requested.
    if (runRecheck) {
        try {
            auto& factory = essentia::standard::AlgorithmFactory::instance();
            std::unique_ptr<essentia::standard::Algorithm> rhythm(
                factory.create("RhythmExtractor2013", "method", "multifeature"));

            essentia::Real bpm = 0, confidence = 0;
            std::vector<essentia::Real> ticks, estimates, bpmIntervals;
            rhythm->input("signal").set(audio);
            rhythm->output("bpm").set(bpm);
            rhythm->output("ticks").set(ticks);
            rhythm->output("confidence").set(confidence);
            rhythm->output("estimates").set(estimates);
            rhythm->output("bpmIntervals").set(bpmIntervals);
            rhythm->compute();

            result.essentiaBpm = bpm;
            result.essentiaConfidence = confidence;
            result.essentiaBeatTicks.assign(ticks.begin(), ticks.end());

            // --- BeatsLoudness, at the beats RhythmExtractor2013 just found ---
            std::unique_ptr<essentia::standard::Algorithm> beatsLoudness(
                factory.create("BeatsLoudness", "sampleRate", static_cast<essentia::Real>(sampleRate),
                                "beats", ticks));
            std::vector<essentia::Real> loudness;
            std::vector<std::vector<essentia::Real>> loudnessBand;
            beatsLoudness->input("signal").set(audio);
            beatsLoudness->output("loudness").set(loudness);
            beatsLoudness->output("loudnessBandRatio").set(loudnessBand);
            beatsLoudness->compute();
            if (!loudness.empty()) {
                result.beatsLoudnessMean =
                    std::accumulate(loudness.begin(), loudness.end(), 0.0) / loudness.size();
            }
        } catch (const essentia::EssentiaException&) {
            // essentia recheck fields stay at 0 — beat_this_cpp's estimate above still stands.
        }

        if (result.beatThisBpm > 0.0 && result.essentiaBpm > 0.0) {
            result.bpmRatio = result.essentiaBpm / result.beatThisBpm;
        }
    }

    result.ok = (result.essentiaBpm > 0.0 || result.beatThisBpm > 0.0);
    return result;
}

namespace {
std::string arrayJson(const std::vector<double>& values) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) oss << ",";
        oss << values[i];
    }
    oss << "]";
    return oss.str();
}
} // namespace

std::string toJson(const RhythmResult& r) {
    std::ostringstream oss;
    oss << "{\"essentia_bpm\":" << r.essentiaBpm
        << ",\"essentia_confidence\":" << r.essentiaConfidence
        // Full beat array, not just the BPM scalar (PRD §6: "required to draw beat
        // markers in the UI... lets a human see why a tempo estimate is wrong").
        << ",\"essentia_beat_ticks\":" << arrayJson(r.essentiaBeatTicks)
        << ",\"beat_this_bpm\":" << r.beatThisBpm
        << ",\"beat_median_bpm\":" << r.beatThisMedianBpm
        << ",\"beat_fitted_bpm_raw\":" << r.beatFittedBpmRaw
        << ",\"beat_fit_used_fraction\":" << r.beatFitUsedFraction
        << ",\"beat_grid_stability\":" << r.beatGridStability
        << ",\"beat_grid_source\":\"" << r.beatGridSource << "\""
        << ",\"beat_grid_stability_dbn\":" << r.beatGridStabilityDbn
        // null, not a number, when the cross-check never ran -- the DBN grid was steady
        // enough that the minimal grid was never decoded, so there is no value to report.
        // Writing 0 here would read as "the minimal grid was hopeless", which is a claim
        // nothing measured.
        << ",\"beat_grid_stability_minimal\":"
        << (r.beatGridStabilityMinimal < 0.0 ? std::string("null")
                                             : std::to_string(r.beatGridStabilityMinimal))
        << ",\"beat_this_beats\":" << arrayJson(r.beatThisBeats)
        << ",\"beat_this_downbeats\":" << arrayJson(r.beatThisDownbeats)
        << ",\"tempo_stability_bpm_stddev\":" << r.tempoStabilityBpmStddev
        << ",\"tempo_range_bpm\":" << r.tempoRangeBpm
        << ",\"tempo_window_count\":" << r.tempoWindowCount
        << ",\"tempo_unstable\":" << (r.tempoUnstable ? "true" : "false")
        << ",\"bpm_ratio\":" << r.bpmRatio
        << ",\"beats_loudness_mean\":" << r.beatsLoudnessMean
        << ",\"danceability\":" << r.danceability
        << "}";
    return oss.str();
}

} // namespace mira
