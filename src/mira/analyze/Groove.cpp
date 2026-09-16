#include "mira/analyze/Groove.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace mira {
namespace {

constexpr double kTwoPi = 6.283185307179586;

// Circular concentration of the onset phases at period P: |mean(e^(i*2*pi*t/P))|.
// Used as the search objective rather than a histogram peak because it is smooth in P
// (so a coarse sweep cannot step over a narrow optimum) and needs no bin-edge choice.
double resultantAt(const std::vector<double>& onsets, double period)
{
    if (period <= 0.0 || onsets.empty())
        return 0.0;

    const double w = kTwoPi / period;
    double cosSum = 0.0, sinSum = 0.0;
    for (double t : onsets)
    {
        const double a = w * t;
        cosSum += std::cos(a);
        sinSum += std::sin(a);
    }
    return std::hypot(cosSum, sinSum) / static_cast<double>(onsets.size());
}

// Mean phase angle at period P, turned back into the seconds offset of the grid's first
// line. Taking the phase from the same circular mean the search maximised means the drawn
// grid sits on the onsets rather than on an arbitrary t=0.
double phaseAt(const std::vector<double>& onsets, double period)
{
    const double w = kTwoPi / period;
    double cosSum = 0.0, sinSum = 0.0;
    for (double t : onsets)
    {
        const double a = w * t;
        cosSum += std::cos(a);
        sinSum += std::sin(a);
    }
    double angle = std::atan2(sinSum, cosSum); // -pi..pi
    if (angle < 0.0)
        angle += kTwoPi;
    return angle / kTwoPi * period;
}

// Sweep coarsely, then refine around the winner. The coarse step is 0.5 ms, which is
// well under the ~11.6 ms quantisation of Essentia's own onset hop (512 samples at
// 44.1 kHz) -- there is no finer structure in the input to resolve.
double searchPeriod(const std::vector<double>& onsets)
{
    double bestPeriod = kGrooveMinPeriodSeconds;
    double bestScore = -1.0;
    for (double p = kGrooveMinPeriodSeconds; p <= kGrooveMaxPeriodSeconds; p += 0.0005)
    {
        const double r = resultantAt(onsets, p);
        if (r > bestScore)
        {
            bestScore = r;
            bestPeriod = p;
        }
    }
    return bestPeriod;
}

// The fit lands on whatever subdivision is most regular, which on programmed music is
// usually the tatum and not the notated beat. A grid at twice the tempo fits the same
// onsets exactly as well, so the onsets themselves can never settle the octave -- that is
// what the stored BPM scalars are for, and the only thing they are trusted with here.
//
// Candidates are integer multiples of the fitted period (plus a half, for the case where
// the fit locked onto the bar rather than the beat), scored by how close they land to an
// anchor in log-tempo space. essentia_bpm is preferred over beat_this_bpm because it
// agreed with the fit on 82% of the calibration corpus against beat_this's 13%.
struct OctaveChoice
{
    double period = 0.0;
    std::string source = "onsets";
};

OctaveChoice pickBeatOctave(const std::vector<double>& onsets, double fittedPeriod,
                            std::optional<double> essentiaBpm, std::optional<double> beatThisBpm)
{
    struct Candidate { double period; double bpm; double resultant; };
    std::vector<Candidate> candidates;
    // 1.5 and 2/3 are here because the search genuinely locks onto 3-against-2
    // subdivisions. Measured case: a 100 BPM drum loop (DKP_100_drum_full_million_dollar,
    // and the filename is ground truth) fitted at 0.4015 s -- exactly 2/3 of the 0.600 s
    // beat -- and with only integer multiples on offer, NOTHING in the candidate list
    // could reach the 99.9 BPM that both estimators independently reported. The grid then
    // fell through to the tempo prior and published 149.4 BPM at 2.33x strength.
    for (double mult : { 0.5, 2.0 / 3.0, 0.75, 1.0, 1.5, 2.0, 3.0, 4.0 })
    {
        const double period = fittedPeriod * mult;
        const double bpm = 60.0 / period;
        // Outside this range a "beat" is not one a person counts; it is the bar or the
        // tatum wearing the beat's name.
        if (bpm < 55.0 || bpm > 200.0)
            continue;
        candidates.push_back({ period, bpm, resultantAt(onsets, period) });
    }
    if (candidates.empty())
        return { fittedPeriod, "onsets" };

    auto nearestTo = [&candidates](double anchorBpm) -> std::optional<Candidate> {
        const Candidate* best = nullptr;
        double bestDistance = 0.0;
        for (const auto& candidate : candidates)
        {
            const double distance = std::abs(std::log2(candidate.bpm / anchorBpm));
            if (best == nullptr || distance < bestDistance)
            {
                best = &candidate;
                bestDistance = distance;
            }
        }
        // Within 3% -- the tolerance the corpus comparison used. Beyond that the scalar
        // is not confirming the fit, it is disagreeing with it, and a disagreeing
        // estimator must not be allowed to move the grid.
        if (best != nullptr && bestDistance < 0.0431) // log2(1.03)
            return *best;
        return std::nullopt;
    };

    if (essentiaBpm && *essentiaBpm > 0.0)
        if (auto pick = nearestTo(*essentiaBpm))
            return { pick->period, "essentia" };
    if (beatThisBpm && *beatThisBpm > 0.0)
        if (auto pick = nearestTo(*beatThisBpm))
            return { pick->period, "beat_this" };

    // Neither scalar landed on any multiple of the fitted period. The old code picked
    // whichever candidate sat nearest 110 BPM and labelled it "onsets", which read like
    // evidence when it was a tempo prior -- 23% of the library was getting its grid that
    // way. It is now reported as uncorroborated so analyzeGroove can decline to derive
    // anything from it (see kGrooveMinGridStrength's caller).
    //
    // Strength alone cannot rescue this. On the measured 100 BPM loop the circular
    // concentration was HIGHER at the wrong 149.4 BPM subdivision (0.168) than at the
    // true beat (0.150): a regular subdivision of a regular grid is also regular. Only
    // corroboration separates them, which is why its absence now means "unknown".
    const Candidate* best = &candidates.front();
    for (const auto& candidate : candidates)
        if (std::abs(std::log2(candidate.bpm / 110.0)) < std::abs(std::log2(best->bpm / 110.0)))
            best = &candidate;
    return { best->period, "uncorroborated" };
}

// Phase of an onset within the beat, in [0,1).
double beatPhase(double t, double period, double gridPhase)
{
    double phase = std::fmod(t - gridPhase, period);
    if (phase < 0.0)
        phase += period;
    return phase / period;
}

} // namespace

double gridConcentration(const std::vector<double>& onsetTimes,
                         const std::vector<double>& beats)
{
    if (onsetTimes.size() < 8 || beats.size() < 4) return 0.0;

    std::vector<int> histogram(kGroovePhaseBins, 0);
    int counted = 0;
    for (double onset : onsetTimes)
    {
        // Find the beat this onset follows. Real tracker beats are unevenly spaced, so the
        // phase is taken against the LOCAL interval rather than a global period -- that is
        // the whole reason to test detected beats separately from a fitted grid.
        auto next = std::lower_bound(beats.begin(), beats.end(), onset);
        if (next == beats.begin() || next == beats.end()) continue;
        const double before = *(next - 1);
        const double interval = *next - before;
        if (interval <= 0.0) continue;
        int bin = static_cast<int>((onset - before) / interval * kGroovePhaseBins);
        histogram[static_cast<size_t>(std::clamp(bin, 0, kGroovePhaseBins - 1))] += 1;
        ++counted;
    }
    if (counted < 8) return 0.0;
    const int peak = *std::max_element(histogram.begin(), histogram.end());
    return static_cast<double>(peak) * kGroovePhaseBins / counted;
}

GrooveResult analyzeGroove(const std::vector<double>& onsetTimes,
                           std::optional<double> essentiaBpm,
                           std::optional<double> beatThisBpm)
{
    GrooveResult result;

    if (static_cast<int>(onsetTimes.size()) < kGrooveMinOnsets)
    {
        result.omittedReason = "too few onsets";
        return result;
    }

    // The search assumes ascending times; stored onsets are, but a sort here costs
    // nothing and removes the assumption.
    std::vector<double> onsets = onsetTimes;
    std::sort(onsets.begin(), onsets.end());

    const double fitted = searchPeriod(onsets);
    const auto octave = pickBeatOctave(onsets, fitted, essentiaBpm, beatThisBpm);

    GrooveGrid& grid = result.grid;
    grid.periodSeconds = octave.period;
    grid.phaseSeconds = phaseAt(onsets, octave.period);
    grid.bpm = 60.0 / octave.period;
    grid.resultant = resultantAt(onsets, octave.period);
    grid.octaveSource = octave.source;
    grid.valid = true;

    // Histogram, normalised to mean 1 so each bin IS its own peak/uniform ratio.
    grid.phaseHistogram.assign(kGroovePhaseBins, 0.0);
    for (double t : onsets)
    {
        int bin = static_cast<int>(beatPhase(t, grid.periodSeconds, grid.phaseSeconds) * kGroovePhaseBins);
        bin = std::clamp(bin, 0, kGroovePhaseBins - 1);
        grid.phaseHistogram[static_cast<size_t>(bin)] += 1.0;
    }
    const double perBin = static_cast<double>(onsets.size()) / kGroovePhaseBins;
    for (double& count : grid.phaseHistogram)
        count /= perBin;
    grid.strength = *std::max_element(grid.phaseHistogram.begin(), grid.phaseHistogram.end());

    const double span = onsets.back() - onsets.front();
    if (span > 0.0)
        result.onsetsPerBeat = static_cast<double>(onsets.size()) / (span / grid.periodSeconds);

    // A grid this flat is the null result -- see kGrooveMinGridStrength. The grid itself
    // is still returned (the UI draws it, and seeing a bad grid is how the original bug
    // was found), but nothing is derived from it.
    if (grid.strength < kGrooveMinGridStrength)
    {
        result.omittedReason = "no grid lock";
        return result;
    }

    // A grid no independent estimator agrees with is a hypothesis, not a measurement.
    // Swing and pocket are defined RELATIVE TO THE BEAT, so deriving them from a grid
    // that may be a subdivision of the real beat produces confident nonsense -- which is
    // exactly what the 100 BPM loop did at 2.33x strength.
    if (grid.octaveSource == "uncorroborated")
    {
        result.omittedReason = "grid not corroborated by any tempo estimate";
        return result;
    }

    // SWING. Mean phase of the onsets that fall in the middle half of the beat, i.e. the
    // off-beat 8ths. The window is deliberately wide (0.25..0.75) so a swung off-beat at
    // 0.667 is inside it rather than cut off by the very asymmetry being measured.
    double offBeatSum = 0.0;
    int offBeatCount = 0;
    // POCKET and SYNCOPATION share one pass over the 16th grid.
    double signedOffsetSum = 0.0;
    int offBeatCells = 0;
    for (double t : onsets)
    {
        const double phase = beatPhase(t, grid.periodSeconds, grid.phaseSeconds);
        if (phase > 0.25 && phase < 0.75)
        {
            offBeatSum += phase;
            ++offBeatCount;
        }

        const double cells = phase * 4.0;                 // 16th cells within the beat
        const double nearest = std::round(cells);
        signedOffsetSum += (cells - nearest) / 4.0;       // back into fractions of a beat
        if (static_cast<int>(nearest) % 4 != 0)
            ++offBeatCells;
    }

    if (offBeatCount >= kGrooveMinOnsets / 4)
        result.swing = offBeatSum / offBeatCount;
    result.pocket = signedOffsetSum / static_cast<double>(onsets.size());
    result.syncopation = static_cast<double>(offBeatCells) / static_cast<double>(onsets.size());

    return result;
}

GrooveResult analyzeGrooveOnGrid(const std::vector<double>& onsetTimes,
                                 const std::vector<double>& beats)
{
    GrooveResult result;

    if (static_cast<int>(onsetTimes.size()) < kGrooveMinOnsets)
    {
        result.omittedReason = "too few onsets";
        return result;
    }
    if (beats.size() < 4)
    {
        result.omittedReason = "no beat grid";
        return result;
    }

    std::vector<double> onsets = onsetTimes;
    std::sort(onsets.begin(), onsets.end());

    // Phase of each onset WITHIN ITS OWN BEAT: which two beats bracket it, and how far
    // between them it sits. This is the whole difference from the fitted-period version
    // -- a constant period drifts away from a performance, and this cannot, because each
    // onset is measured against the beats actually either side of it.
    std::vector<double> phases;
    phases.reserve(onsets.size());
    for (double t : onsets)
    {
        auto next = std::upper_bound(beats.begin(), beats.end(), t);
        if (next == beats.begin() || next == beats.end()) continue;
        const double lo = *(next - 1), hi = *next;
        const double width = hi - lo;
        if (width <= 0.0) continue;
        phases.push_back((t - lo) / width);
    }
    if (static_cast<int>(phases.size()) < kGrooveMinOnsets)
    {
        result.omittedReason = "too few onsets inside the beat grid";
        return result;
    }

    std::vector<double> intervals;
    for (size_t i = 1; i < beats.size(); ++i) intervals.push_back(beats[i] - beats[i - 1]);
    std::sort(intervals.begin(), intervals.end());
    const double medianInterval = intervals[intervals.size() / 2];

    GrooveGrid& grid = result.grid;
    grid.periodSeconds = medianInterval;
    grid.phaseSeconds = beats.front();
    grid.bpm = medianInterval > 0.0 ? 60.0 / medianInterval : 0.0;
    grid.octaveSource = "beat_grid";
    grid.valid = true;

    grid.phaseHistogram.assign(kGroovePhaseBins, 0.0);
    for (double phase : phases)
    {
        int bin = std::clamp(static_cast<int>(phase * kGroovePhaseBins), 0, kGroovePhaseBins - 1);
        grid.phaseHistogram[static_cast<size_t>(bin)] += 1.0;
    }
    const double perBin = static_cast<double>(phases.size()) / kGroovePhaseBins;
    for (double& count : grid.phaseHistogram) count /= perBin;
    grid.strength = *std::max_element(grid.phaseHistogram.begin(), grid.phaseHistogram.end());

    // Circular concentration over the same phases, for parity with the fitted version's
    // own objective.
    double sumSin = 0.0, sumCos = 0.0;
    for (double phase : phases)
    {
        sumSin += std::sin(2.0 * M_PI * phase);
        sumCos += std::cos(2.0 * M_PI * phase);
    }
    grid.resultant = std::hypot(sumSin, sumCos) / static_cast<double>(phases.size());

    result.onsetsPerBeat = static_cast<double>(phases.size())
                         / static_cast<double>(beats.size() - 1);

    if (grid.strength < kGrooveMinGridStrength)
    {
        result.omittedReason = "no grid lock";
        return result;
    }

    // Identical arithmetic to analyzeGroove below this line -- only the source of `phase`
    // differs, which is the point.
    double offBeatSum = 0.0;
    int offBeatCount = 0;
    double signedOffsetSum = 0.0;
    int offBeatCells = 0;
    for (double phase : phases)
    {
        if (phase > 0.25 && phase < 0.75)
        {
            offBeatSum += phase;
            ++offBeatCount;
        }
        const double cells = phase * 4.0;
        const double nearest = std::round(cells);
        signedOffsetSum += (cells - nearest) / 4.0;
        if (static_cast<int>(nearest) % 4 != 0) ++offBeatCells;
    }

    if (offBeatCount >= kGrooveMinOnsets / 4)
        result.swing = offBeatSum / offBeatCount;
    result.pocket = signedOffsetSum / static_cast<double>(phases.size());
    result.syncopation = static_cast<double>(offBeatCells) / static_cast<double>(phases.size());

    return result;
}

} // namespace mira
