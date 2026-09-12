#include "CueDetection.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace mira {

namespace {

using Span = std::pair<double, double>;

// A candidate mix must cover at least this much of the union of the stems to be believed.
// Not 1.0: the union legitimately runs slightly longer than the mix where a stem's reverb
// tail was printed into the stem but faded in the mix.
constexpr double kMixCoverageFloor = 0.8;

// Merge overlapping/adjacent spans, bridging gaps up to `bridge`.
std::vector<Span> mergeSpans(std::vector<Span> spans, double bridge) {
    if (spans.empty()) return {};
    std::sort(spans.begin(), spans.end());
    std::vector<Span> merged;
    for (const auto& [start, end] : spans) {
        if (end <= start) continue;
        if (!merged.empty() && start <= merged.back().second + bridge)
            merged.back().second = std::max(merged.back().second, end);
        else
            merged.emplace_back(start, end);
    }
    return merged;
}

double coveredSeconds(const std::vector<Span>& spans) {
    double total = 0.0;
    for (const auto& [start, end] : spans) total += std::max(0.0, end - start);
    return total;
}

bool activeAt(const std::vector<Span>& spans, double t) {
    // Spans are sorted and non-overlapping, so the first one that could contain t is the
    // last one starting at or before it.
    auto it = std::upper_bound(spans.begin(), spans.end(), t,
                                [](double value, const Span& s) { return value < s.first; });
    if (it == spans.begin()) return false;
    --it;
    return t < it->second;
}

} // namespace

CueResult detectCues(const std::vector<CueStem>& stems, double reelSeconds, const CueOptions& options) {
    CueResult result;
    if (stems.empty() || reelSeconds <= 0.0) return result;

    // --- Signal 1: where is ANY of this set playing? --------------------------------
    std::vector<Span> unionSpans;
    const CueStem* mix = nullptr;
    for (const auto& stem : stems) {
        if (stem.isMix) {
            // First mix wins if a delivery somehow ships two -- picking arbitrarily is
            // better than silently combining two different mixes of the same reel.
            if (mix == nullptr) mix = &stem;
            continue; // the mix is not one of the parts; it must not vote in the union
        }
        unionSpans.insert(unionSpans.end(), stem.activeSpans.begin(), stem.activeSpans.end());
    }
    unionSpans = mergeSpans(std::move(unionSpans), options.bridgeSeconds);

    // The mix is the better region map when present (a stem's reverb tail can run past the
    // end of a cue and smear the union; the mix just stops), but the union is kept as a
    // cross-check rather than being thrown away.
    std::vector<Span> regions = unionSpans;
    if (mix != nullptr) {
        auto mixSpans = mergeSpans(mix->activeSpans, options.bridgeSeconds);
        double mixCovered = coveredSeconds(mixSpans);
        double unionCovered = coveredSeconds(unionSpans);
        result.mixDisagreementSeconds = std::abs(mixCovered - unionCovered);

        // Confirmation before trust: a real mix plays wherever ANY stem plays, so it must
        // cover essentially all of the union. A candidate that covers far less is not the
        // mix, whatever its name says -- and taking it anyway is catastrophic rather than
        // merely wrong, because the region map is what everything downstream is built on.
        // Found on real data: EP9's "FX MASTER.wav" passed the filename test (since
        // fixed), covers 10s against the union's 1490s, and produced zero cues.
        if (!mixSpans.empty() && unionCovered > 0.0 && mixCovered >= unionCovered * kMixCoverageFloor) {
            result.usedMix = true;
            regions = std::move(mixSpans);
        }
    }
    if (regions.empty()) return result;

    // Each region's edges are boundaries on their own -- that is signal 1's whole output.
    std::vector<std::pair<double, std::string>> boundaries;
    boundaries.emplace_back(regions.front().first, "reel");
    for (size_t i = 1; i < regions.size(); ++i) boundaries.emplace_back(regions[i].first, "silence");
    result.silenceBoundaries = static_cast<int>(regions.size()) - 1;

    // --- Signal 2: where does the instrumentation change sharply? --------------------
    // Sampled on a fixed grid rather than from span edges directly, because what matters
    // is how many stems changed *together*, and span edges a second apart are the same
    // event. Only the parts vote here, never the mix -- the mix is active whenever
    // anything is, so it would contribute no information and dilute the count.
    std::vector<const CueStem*> parts;
    for (const auto& stem : stems)
        if (!stem.isMix) parts.push_back(&stem);

    struct Candidate {
        double t;
        int changed;
    };
    std::vector<Candidate> candidates;
    if (!parts.empty() && options.gridSeconds > 0.0) {
        std::set<size_t> previous;
        bool havePrevious = false;
        for (double t = 0.0; t < reelSeconds; t += options.gridSeconds) {
            std::set<size_t> current;
            for (size_t i = 0; i < parts.size(); ++i)
                if (activeAt(parts[i]->activeSpans, t)) current.insert(i);

            if (havePrevious && !(previous.empty() && current.empty())) {
                int changed = 0;
                for (size_t i = 0; i < parts.size(); ++i)
                    if (previous.count(i) != current.count(i)) ++changed;
                if (changed >= options.minStemsChanged) candidates.push_back({ t, changed });
            }
            previous = std::move(current);
            havePrevious = true;
        }
    }

    // Non-maximum suppression: strongest change first, and a candidate is only accepted if
    // it is at least minCueSeconds from every boundary already accepted. Without this the
    // four separate 3-stem changes EP9 has inside ten seconds around 20:50 would become
    // four cues, when they are one phrase.
    std::sort(candidates.begin(), candidates.end(),
               [](const Candidate& a, const Candidate& b) { return a.changed > b.changed; });
    for (const auto& candidate : candidates) {
        // Only subdivide inside a region -- a "change" in the middle of silence is an
        // artefact of the grid, not a cue starting.
        bool insideRegion = false;
        for (const auto& [start, end] : regions)
            if (candidate.t > start && candidate.t < end) { insideRegion = true; break; }
        if (!insideRegion) continue;

        bool tooClose = false;
        for (const auto& [t, reason] : boundaries)
            if (std::abs(t - candidate.t) < options.minCueSeconds) { tooClose = true; break; }
        // Also keep clear of the end of the region it falls in, or the last cue is a sliver.
        if (!tooClose)
            for (const auto& [start, end] : regions)
                if (candidate.t > start && candidate.t < end
                    && end - candidate.t < options.minCueSeconds) { tooClose = true; break; }
        if (tooClose) continue;

        boundaries.emplace_back(candidate.t, "instrumentation");
        ++result.instrumentationBoundaries;
    }

    std::sort(boundaries.begin(), boundaries.end(),
               [](const auto& a, const auto& b) { return a.first < b.first; });

    // --- Turn boundaries into cues ---------------------------------------------------
    // A cue runs from its boundary to whichever comes first: the next boundary, or the end
    // of the region it lives in. The second case is what keeps a cue from spanning a
    // silence it has no business covering.
    for (size_t i = 0; i < boundaries.size(); ++i) {
        double start = boundaries[i].first;
        double end = i + 1 < boundaries.size() ? boundaries[i + 1].first : reelSeconds;
        for (const auto& [regionStart, regionEnd] : regions)
            if (start >= regionStart && start < regionEnd) { end = std::min(end, regionEnd); break; }
        if (end - start < options.minCueSeconds) continue;

        Cue cue;
        cue.startSeconds = start;
        cue.endSeconds = end;
        cue.startReason = boundaries[i].second;
        for (const auto* part : parts) {
            for (const auto& [spanStart, spanEnd] : part->activeSpans)
                if (spanStart < end && spanEnd > start) { ++cue.stemsPlaying; break; }
        }
        result.cues.push_back(cue);
    }
    return result;
}

} // namespace mira
