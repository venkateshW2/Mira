#include "mira/analyze/Meter.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <map>
#include <set>
#include <sstream>

namespace mira {
namespace {

// 8 is two bars of 4 and 12 is two bars of 6. The autocorrelation legitimately prefers
// the longer repeat -- a four-bar phrase really does resemble itself at lag 8 -- so these
// fold down to the meter a person would count.
int snapMeter(int meter)
{
    if (meter == 8 || meter == 16) return 4;
    if (meter == 12) return 6;
    return meter;
}

bool inConfusionSet(int value)
{
    for (int c : kMeterConfusionSet)
        if (c == value) return true;
    return false;
}

// Average each feature dimension across the frames one beat spans. The result is one
// vector per beat interval, which is what turns "how long is a bar" into "at what lag
// does a beat resemble an earlier one".
std::vector<std::vector<double>> beatSync(const MeterFeature& feature,
                                          const std::vector<double>& beats,
                                          double hopSeconds)
{
    std::vector<std::vector<double>> out;
    if (feature.frames <= 0 || beats.size() < 2) return out;

    auto frameOf = [&](double seconds) {
        int f = static_cast<int>(std::floor(seconds / hopSeconds));
        return std::clamp(f, 0, feature.frames - 1);
    };

    out.reserve(beats.size() - 1);
    for (size_t i = 0; i + 1 < beats.size(); ++i)
    {
        const int a = frameOf(beats[i]);
        const int b = std::max(frameOf(beats[i + 1]), a + 1);
        const int span = std::min(b, feature.frames) - a;
        if (span <= 0) { out.emplace_back(feature.dims, 0.0); continue; }

        std::vector<double> mean(static_cast<size_t>(feature.dims), 0.0);
        for (int d = 0; d < feature.dims; ++d)
        {
            const float* row = feature.values.data() + static_cast<size_t>(d) * feature.frames;
            double sum = 0.0;
            for (int f = a; f < a + span; ++f) sum += row[f];
            mean[static_cast<size_t>(d)] = sum / span;
        }
        out.push_back(std::move(mean));
    }
    return out;
}

// Mean of the k-th diagonal of the beat self-similarity matrix, for k = 1..kMeterMaxLag.
// The matrix itself is never materialised: only the diagonals are needed, and computing
// them directly is O(beats * lags * dims) instead of O(beats^2 * dims).
std::vector<double> autocorrelation(const std::vector<std::vector<double>>& synced)
{
    std::vector<double> acf(kMeterMaxLag, -std::numeric_limits<double>::infinity());
    const int n = static_cast<int>(synced.size());
    if (n < 2) return acf;

    // Cosine similarity needs unit vectors; normalise once rather than per pair.
    std::vector<std::vector<double>> unit = synced;
    for (auto& v : unit)
    {
        double norm = 0.0;
        for (double x : v) norm += x * x;
        norm = std::sqrt(norm);
        if (norm < 1e-12) norm = 1e-12;
        for (double& x : v) x /= norm;
    }

    const int maxLag = std::min(kMeterMaxLag, n - 1);
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double sum = 0.0;
        int count = 0;
        for (int i = 0; i + lag < n; ++i)
        {
            double dot = 0.0;
            const auto& a = unit[static_cast<size_t>(i)];
            const auto& b = unit[static_cast<size_t>(i + lag)];
            for (size_t d = 0; d < a.size(); ++d) dot += a[d] * b[d];
            sum += dot;
            ++count;
        }
        if (count > 0) acf[static_cast<size_t>(lag - 1)] = sum / count;
    }
    return acf;
}

// `beatCount` gates the search: a period needs kMeterMinCycles whole repetitions before
// it means anything. Without this a 10-second loop (16 beats) can "detect" a bar of 6
// from 2.7 repetitions, which is what a 100 BPM Dark Pop drum loop did -- it came back 6
// where it is plainly 4. The reference applies the same constant, but only to the
// arbitration contenders; applying it to the peak search as well is a deliberate
// addition, not a porting slip.
int acfPeak(const std::vector<double>& acf, int beatCount)
{
    int best = -1;
    double bestValue = -std::numeric_limits<double>::infinity();
    for (int i = kMeterLagSkip; i < static_cast<int>(acf.size()); ++i)
    {
        const int lag = i + 1;
        if (beatCount / lag < kMeterMinCycles) continue;
        if (acf[static_cast<size_t>(i)] > bestValue) { bestValue = acf[static_cast<size_t>(i)]; best = i; }
    }
    // Nothing had enough repetitions to judge -- the caller reports no meter rather than
    // picking the least bad lag.
    return best < 0 ? 0 : best + 1;
}

// The onset envelope sampled at each beat, normalised to its own maximum.
std::vector<double> accentsAtBeats(const std::vector<float>& envelope,
                                   const std::vector<double>& beats, double hopSeconds)
{
    std::vector<double> acc;
    if (envelope.empty()) return acc;
    acc.reserve(beats.size());
    for (double b : beats)
    {
        int f = static_cast<int>(std::lround(b / hopSeconds));
        f = std::clamp(f, 0, static_cast<int>(envelope.size()) - 1);
        acc.push_back(envelope[static_cast<size_t>(f)]);
    }
    const double peak = *std::max_element(acc.begin(), acc.end());
    if (peak > 0.0) for (double& v : acc) v /= peak;
    return acc;
}

// ANOVA F over bar positions: how much louder some positions are than others, relative to
// the spread within each position. Chosen over plain variance because it is comparable
// across different candidate periods and does not depend on where beat 1 falls.
double accentFScore(const std::vector<double>& accents, int period)
{
    const int n = static_cast<int>(accents.size());
    if (period < 2 || n - period <= 0) return std::nan("");

    std::vector<double> sums(static_cast<size_t>(period), 0.0);
    std::vector<int> counts(static_cast<size_t>(period), 0);
    for (int i = 0; i < n; ++i) { sums[static_cast<size_t>(i % period)] += accents[static_cast<size_t>(i)]; ++counts[static_cast<size_t>(i % period)]; }

    const double grand = std::accumulate(accents.begin(), accents.end(), 0.0) / n;
    double ssBetween = 0.0;
    std::vector<double> means(static_cast<size_t>(period), 0.0);
    for (int p = 0; p < period; ++p)
    {
        if (counts[static_cast<size_t>(p)] == 0) continue;
        means[static_cast<size_t>(p)] = sums[static_cast<size_t>(p)] / counts[static_cast<size_t>(p)];
        const double d = means[static_cast<size_t>(p)] - grand;
        ssBetween += counts[static_cast<size_t>(p)] * d * d;
    }
    double ssWithin = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double d = accents[static_cast<size_t>(i)] - means[static_cast<size_t>(i % period)];
        ssWithin += d * d;
    }
    if (ssWithin <= 0.0) return std::nan("");
    return (ssBetween / (period - 1)) / (ssWithin / (n - period));
}

} // namespace

MeterResult detectMeter(const std::vector<MeterFeature>& features,
                        const std::vector<float>& onsetEnvelope,
                        const std::vector<double>& beats,
                        double hopSeconds)
{
    MeterResult result;
    if (features.empty() || static_cast<int>(beats.size()) < kMeterMinBeats || hopSeconds <= 0.0)
    {
        result.reason = "too few beats";
        return result;
    }

    // Each feature votes independently; the one whose autocorrelation peaks highest is
    // the one trusted when the accents are not consulted.
    int bestFeature = 0;
    double bestPeakValue = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < features.size(); ++i)
    {
        const auto acf = autocorrelation(beatSync(features[i], beats, hopSeconds));
        const int lag = acfPeak(acf, static_cast<int>(beats.size()));
        if (lag == 0) continue; // too few bars for this feature to have an opinion
        result.featureWinners.push_back(lag);
        const double value = acf[static_cast<size_t>(lag - 1)];
        if (value > bestPeakValue) { bestPeakValue = value; bestFeature = static_cast<int>(i); }
    }
    if (result.featureWinners.empty())
    {
        result.reason = "too few bars to judge a meter";
        return result;
    }
    result.strongestFeature = features[static_cast<size_t>(bestFeature)].name;
    result.acfMeter = result.featureWinners[static_cast<size_t>(bestFeature)];

    // Do the features actually disagree, and about something worth arguing over? If they
    // agree, leave it alone -- even by a hair.
    std::set<int> distinct(result.featureWinners.begin(), result.featureWinners.end());
    bool involvesConfusable = false;
    for (int d : distinct) if (inConfusionSet(d)) involvesConfusable = true;

    std::vector<int> contenders;
    for (int d : distinct)
        if (d >= 3 && d <= kMeterMaxLag
            && static_cast<int>(beats.size()) / d >= kMeterMinCycles)
            contenders.push_back(d);

    bool confused = distinct.size() > 1 && involvesConfusable && contenders.size() >= 2;

    std::ostringstream reason;
    if (distinct.size() == 1)
        reason << "all " << features.size() << " features agree on " << *distinct.begin();
    else
    {
        reason << "features split";
        for (int d : distinct) reason << " " << d;
        if (!confused) reason << " (nothing foldable to arbitrate)";
    }

    int meter = result.acfMeter;
    if (confused)
    {
        const auto accents = accentsAtBeats(onsetEnvelope, beats, hopSeconds);
        std::vector<std::pair<double, int>> scored;
        for (int c : contenders)
        {
            const double f = accentFScore(accents, c);
            if (std::isfinite(f)) scored.emplace_back(f, c);
        }
        if (!scored.empty())
        {
            std::sort(scored.rbegin(), scored.rend());
            meter = scored.front().second;
            result.arbitrated = true;
            result.accentF = scored.front().first;
            result.accentMargin = scored.size() > 1 && scored[1].first > 0.0
                                      ? scored.front().first / scored[1].first : 0.0;
            reason << "; accents chose " << meter << " (F=" << result.accentF
                   << (result.accentF >= kMeterAccentWeakF ? ", strong" : ", weak") << ")";
        }
    }

    result.meterPreSnap = meter;
    result.meter = snapMeter(meter);
    if (result.meter != meter) reason << "; snapped " << meter << "->" << result.meter;
    result.reason = reason.str();

    // Bar spread: the grid's own confidence. Bars are laid from the first beat, which is
    // enough to measure evenness -- the downbeat PHASE is a separate question this does
    // not answer (Phase 7 task 3).
    if (result.meter >= 2)
    {
        std::vector<double> barLengths;
        for (size_t i = 0; i + result.meter < beats.size(); i += static_cast<size_t>(result.meter))
            barLengths.push_back(beats[i + result.meter] - beats[i]);
        if (barLengths.size() >= 2)
        {
            const auto [lo, hi] = std::minmax_element(barLengths.begin(), barLengths.end());
            result.barCount = static_cast<int>(barLengths.size());
            if (*lo > 0.0) result.barSpread = *hi / *lo;
        }
    }

    result.valid = true;
    return result;
}

// --- choose_phase / bar_lines_from, ported ---------------------------------------
//
// Line for line from the reference. Nothing added: no BPM, no fitted grid, no prior.

PhaseResult choosePhase(const std::vector<double>& beats,
                        const std::vector<double>& downbeats,
                        const std::vector<float>& onsetEnvelope,
                        double hopSeconds,
                        int meter)
{
    PhaseResult result;
    if (meter < 2 || static_cast<int>(beats.size()) < meter * 2) return result;

    // downbeat_residues: which position within the bar each tracker downbeat lands on.
    std::map<int, int> counts;
    int downbeatCount = 0;
    if (downbeats.size() >= 3 && beats.size() >= 3) {
        for (double d : downbeats) {
            auto nearest = std::min_element(beats.begin(), beats.end(),
                [d](double a, double b) { return std::abs(a - d) < std::abs(b - d); });
            int index = static_cast<int>(std::distance(beats.begin(), nearest));
            ++counts[index % meter];
            ++downbeatCount;
        }
    }

    // accent_phase: the bar position whose beats are loudest on average.
    const auto accents = accentsAtBeats(onsetEnvelope, beats, hopSeconds);
    auto accentPhase = [&](const std::vector<int>& candidates) {
        int best = candidates.empty() ? 0 : candidates.front();
        double bestMean = -1.0;
        for (int p : candidates) {
            double sum = 0.0; int n = 0;
            for (size_t i = static_cast<size_t>(p); i < accents.size(); i += static_cast<size_t>(meter)) {
                sum += accents[i]; ++n;
            }
            double mean = n > 0 ? sum / n : -1.0;
            if (mean > bestMean) { bestMean = mean; best = p; }
        }
        return best;
    };

    double agreement = 0.0;
    if (downbeatCount > 0) {
        int top = std::max_element(counts.begin(), counts.end(),
            [](auto& a, auto& b) { return a.second < b.second; })->second;
        agreement = static_cast<double>(top) / downbeatCount;
    }
    result.agreement = agreement;

    std::vector<int> candidates;
    for (auto& [position, count] : counts)
        if (static_cast<double>(count) / std::max(downbeatCount, 1) >= kPhaseCandCoverage)
            candidates.push_back(position);

    if (downbeatCount > 0 && candidates.size() > 1) {
        result.phase = accentPhase(candidates);
        result.source = "accent among downbeat residues";
    } else if (downbeatCount > 0 && candidates.size() == 1 && agreement >= kPhaseMinAgreement) {
        result.phase = candidates.front();
        result.source = "downbeats";
    } else {
        std::vector<int> all(static_cast<size_t>(meter));
        std::iota(all.begin(), all.end(), 0);
        result.phase = accentPhase(all);
        result.source = "accent fold (downbeats unusable)";
    }

    // bar_lines_from: every meter-th DETECTED beat, starting at `phase`.
    for (size_t i = static_cast<size_t>(result.phase); i < beats.size(); i += static_cast<size_t>(meter))
        result.barLines.push_back(beats[i]);
    if (result.barLines.size() >= 2) {
        std::vector<double> lengths;
        for (size_t i = 1; i < result.barLines.size(); ++i)
            lengths.push_back(result.barLines[i] - result.barLines[i - 1]);
        std::vector<double> sorted = lengths;
        std::sort(sorted.begin(), sorted.end());
        result.barMedianSeconds = sorted[sorted.size() / 2];
        if (sorted.front() > 0.0) result.barSpread = sorted.back() / sorted.front();
    }
    result.valid = true;
    return result;
}

} // namespace mira
