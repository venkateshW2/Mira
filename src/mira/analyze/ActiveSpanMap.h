#pragma once

#include <algorithm>
#include <utility>
#include <vector>

namespace mira {

// Converting between the two time bases a mira analysis actually has.
//
// PRD §5 makes active-region detection mandatory: "descriptors, MIR and the embedding
// then see only those spans." `extractActiveAudio` splices the active spans together and
// every analyzer downstream runs on that spliced buffer -- so everything time-stamped by
// one of them (chord changes, transcribed notes, beats, downbeats) is stamped in
// *spliced-active time*, which is NOT a position in the original file. On a real library
// file (id 551, BASS_1.wav) the difference is not subtle: a 30.0 s file with one active
// span [10.496, 25.387] has its last chord at 14.72 s -- a timestamp that, read as a file
// position, points at the wrong bar entirely, and drew the chord lane compressed into the
// left half of a waveform whose audio sits between 35% and 85%.
//
// Header-only and dependency-free on purpose: the analyzer (the heavy `mira` target) and
// mira_ui (which links only mira_core and has no decoder at all) both need exactly this
// arithmetic, and neither should be made to depend on the other to get it.
//
// `spans` must be sorted and non-overlapping, which is what detectActiveRegions produces
// and what files.active_spans stores. An EMPTY span list means active-region detection
// never ran for this file (shouldRunActiveRegionDetection: only stems, or anything past
// five minutes) -- in that case the analyzers saw the whole file and the two time bases
// are already the same thing, so every function here is an identity.

// One instant in spliced-active time -> its position in the original file.
// A time past the end of the active audio clamps to the end of the last span rather than
// running off into silence that was never analyzed.
inline double activeTimeToFileTime(const std::vector<std::pair<double, double>>& spans,
                                    double activeSeconds) {
    if (spans.empty()) return activeSeconds;
    double consumed = 0.0;
    for (const auto& [spanStart, spanEnd] : spans) {
        double length = spanEnd - spanStart;
        if (length <= 0.0) continue;
        if (activeSeconds < consumed + length) return spanStart + (activeSeconds - consumed);
        consumed += length;
    }
    return spans.back().second;
}

// One interval in spliced-active time -> the file-time intervals it actually occupies.
//
// Returns a LIST, not a single interval, because an interval that straddles a splice
// point covers two stretches of the file with silence in between -- and that silence is
// audio no analyzer ever saw. Mapping the two endpoints independently would instead
// produce one long interval spanning material that was never analyzed, which is exactly
// the kind of confident-looking wrong answer this codebase avoids elsewhere. Splitting
// keeps a chord block or a note block sitting only over audio that genuinely produced it.
inline std::vector<std::pair<double, double>> activeIntervalToFileIntervals(
    const std::vector<std::pair<double, double>>& spans, double activeStart, double activeEnd) {
    if (activeEnd <= activeStart) return {};
    if (spans.empty()) return { { activeStart, activeEnd } };

    std::vector<std::pair<double, double>> result;
    double consumed = 0.0;
    for (const auto& [spanStart, spanEnd] : spans) {
        double length = spanEnd - spanStart;
        if (length <= 0.0) continue;
        double overlapStart = std::max(activeStart, consumed);
        double overlapEnd = std::min(activeEnd, consumed + length);
        if (overlapEnd > overlapStart)
            result.emplace_back(spanStart + (overlapStart - consumed), spanStart + (overlapEnd - consumed));
        consumed += length;
        if (consumed >= activeEnd) break;
    }
    return result;
}

// The same split for an interval ALREADY in file time -- what a row analyzed after the
// analyzer started writing file-time timestamps needs. Keeps the drawn result identical
// between a freshly analyzed row and a legacy one: a chord block never covers silence
// either way, so the lane lines up with the peaks in both cases.
inline std::vector<std::pair<double, double>> clipFileIntervalToSpans(
    const std::vector<std::pair<double, double>>& spans, double fileStart, double fileEnd) {
    if (fileEnd <= fileStart) return {};
    if (spans.empty()) return { { fileStart, fileEnd } };

    std::vector<std::pair<double, double>> result;
    for (const auto& [spanStart, spanEnd] : spans) {
        double overlapStart = std::max(fileStart, spanStart);
        double overlapEnd = std::min(fileEnd, spanEnd);
        if (overlapEnd > overlapStart) result.emplace_back(overlapStart, overlapEnd);
        if (spanStart > fileEnd) break;
    }
    return result;
}

// Total spliced-active duration -- the length of the buffer the analyzers actually saw,
// and so the end of the last chord in active time.
inline double totalActiveSeconds(const std::vector<std::pair<double, double>>& spans) {
    double total = 0.0;
    for (const auto& [spanStart, spanEnd] : spans) total += std::max(0.0, spanEnd - spanStart);
    return total;
}

} // namespace mira
