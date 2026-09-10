#include "Mir.h"

#include <essentia/algorithmfactory.h>
#include <beat_this_api.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>
#include <sstream>

namespace mira {

namespace {
double meanBeatIntervalBpm(const std::vector<float>& beats) {
    if (beats.size() < 2) return 0.0;
    std::vector<double> intervals;
    intervals.reserve(beats.size() - 1);
    for (size_t i = 1; i < beats.size(); ++i) intervals.push_back(beats[i] - beats[i - 1]);
    double meanInterval = std::accumulate(intervals.begin(), intervals.end(), 0.0) / intervals.size();
    return meanInterval > 0.0 ? 60.0 / meanInterval : 0.0;
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
// those windows — a whole-file mean BPM (meanBeatIntervalBpm above) is silent about
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
    try {
        BeatThis::BeatThis beatThis(beatThisModelPath, /*use_dbn=*/true);
        auto beatResult = beatThis.process_audio(mono, sampleRate, /*channels=*/1);
        result.beatThisBeats.assign(beatResult.beats.begin(), beatResult.beats.end());
        result.beatThisDownbeats.assign(beatResult.downbeats.begin(), beatResult.downbeats.end());
        result.beatThisBpm = meanBeatIntervalBpm(beatResult.beats);

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
