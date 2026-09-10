#include "ActiveRegions.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mira {

namespace {
constexpr int kFrameSize = 2048;
constexpr int kHopSize = 1024;
constexpr double kSilenceThresholdDb = -60.0;
constexpr double kMaxGapSeconds = 0.3;
constexpr double kFiveMinutesSeconds = 300.0;

double frameRmsDb(const std::vector<float>& audio, size_t start, size_t frameSize) {
    double sumSquares = 0.0;
    size_t end = std::min(start + frameSize, audio.size());
    size_t n = end - start;
    if (n == 0) return -std::numeric_limits<double>::infinity();

    for (size_t i = start; i < end; ++i) sumSquares += static_cast<double>(audio[i]) * audio[i];

    double rms = std::sqrt(sumSquares / n);
    if (rms <= 0.0) return -std::numeric_limits<double>::infinity();
    return 20.0 * std::log10(rms);
}
} // namespace

ActiveRegionResult detectActiveRegions(const std::vector<float>& audio, int sampleRate) {
    ActiveRegionResult result;
    if (audio.empty() || sampleRate <= 0) return result;

    double totalDuration = static_cast<double>(audio.size()) / sampleRate;

    // Pass 1: per-frame active/inactive.
    std::vector<bool> active;
    for (size_t start = 0; start < audio.size(); start += kHopSize) {
        active.push_back(frameRmsDb(audio, start, kFrameSize) > kSilenceThresholdDb);
    }

    // Pass 2: contiguous active frames -> raw spans, in seconds.
    std::vector<ActiveSpan> rawSpans;
    bool inSpan = false;
    size_t spanStartFrame = 0;
    for (size_t i = 0; i < active.size(); ++i) {
        if (active[i] && !inSpan) {
            inSpan = true;
            spanStartFrame = i;
        } else if (!active[i] && inSpan) {
            inSpan = false;
            double startS = static_cast<double>(spanStartFrame) * kHopSize / sampleRate;
            double endS = static_cast<double>(i) * kHopSize / sampleRate;
            rawSpans.push_back({startS, endS});
        }
    }
    if (inSpan) {
        double startS = static_cast<double>(spanStartFrame) * kHopSize / sampleRate;
        rawSpans.push_back({startS, totalDuration});
    }

    // Pass 3: bridge gaps shorter than kMaxGapSeconds so natural micro-pauses within a
    // phrase don't fragment one span into many.
    for (const auto& span : rawSpans) {
        if (!result.spans.empty() && span.startSeconds - result.spans.back().endSeconds < kMaxGapSeconds) {
            result.spans.back().endSeconds = span.endSeconds;
        } else {
            result.spans.push_back(span);
        }
    }

    double activeSeconds = 0.0;
    for (const auto& span : result.spans) activeSeconds += span.endSeconds - span.startSeconds;
    result.activeRatio = totalDuration > 0.0 ? activeSeconds / totalDuration : 0.0;

    return result;
}

bool shouldRunActiveRegionDetection(const std::string& contentType, double durationSeconds) {
    if (contentType == "stem") return true;
    return durationSeconds > kFiveMinutesSeconds;
}

} // namespace mira
