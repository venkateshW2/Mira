#include "Descriptors.h"

#include <essentia/algorithmfactory.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>

namespace mira {

namespace {
constexpr int kFrameSize = 2048;
constexpr int kHopSize = 1024;

using Algo = essentia::standard::Algorithm;
using AlgoPtr = std::unique_ptr<Algo>;

AlgoPtr create(const std::string& name) {
    return AlgoPtr(essentia::standard::AlgorithmFactory::instance().create(name));
}

// Frame-average of a scalar spectral descriptor (Centroid or Flatness), computed over
// Windowing(hann) -> Spectrum -> `algoName`, skipping the trailing partial frame.
double averageSpectralDescriptor(const std::vector<float>& mono, const std::string& algoName,
                                  const std::string& outputName,
                                  const std::vector<std::pair<std::string, essentia::Parameter>>& params = {}) {
    if (static_cast<int>(mono.size()) < kFrameSize) return 0.0;

    AlgoPtr windowing = create("Windowing");
    windowing->configure("type", "hann");
    AlgoPtr spectrum = create("Spectrum");
    spectrum->configure("size", kFrameSize);

    AlgoPtr descriptor = create(algoName);
    for (auto& [key, value] : params) descriptor->configure(key, value);

    std::vector<essentia::Real> frame(kFrameSize), windowed, spec;
    windowing->input("frame").set(frame);
    windowing->output("frame").set(windowed);
    spectrum->input("frame").set(windowed);
    spectrum->output("spectrum").set(spec);

    essentia::Real value = 0;
    descriptor->input("array").set(spec);
    descriptor->output(outputName).set(value);

    double sum = 0.0;
    int count = 0;
    for (size_t start = 0; start + kFrameSize <= mono.size(); start += kHopSize) {
        std::copy(mono.begin() + start, mono.begin() + start + kFrameSize, frame.begin());
        windowing->compute();
        spectrum->compute();
        descriptor->compute();
        sum += value;
        ++count;
    }
    return count > 0 ? sum / count : 0.0;
}
} // namespace

DspDescriptors computeDspDescriptors(const std::vector<float>& left,
                                      const std::vector<float>& right, int sampleRate) {
    DspDescriptors d;
    if (left.empty() || left.size() != right.size() || sampleRate <= 0) return d;

    // --- Loudness (LoudnessEBUR128 wants stereo) ---
    std::vector<essentia::StereoSample> stereo(left.size());
    for (size_t i = 0; i < left.size(); ++i) {
        stereo[i].left() = left[i];
        stereo[i].right() = right[i];
    }
    {
        auto loudness = create("LoudnessEBUR128");
        loudness->configure("sampleRate", static_cast<essentia::Real>(sampleRate));
        std::vector<essentia::Real> momentary, shortTerm;
        essentia::Real integrated = 0, range = 0;
        loudness->input("signal").set(stereo);
        loudness->output("momentaryLoudness").set(momentary);
        loudness->output("shortTermLoudness").set(shortTerm);
        loudness->output("integratedLoudness").set(integrated);
        loudness->output("loudnessRange").set(range);
        loudness->compute();
        d.integratedLoudnessLufs = integrated;
        d.loudnessRangeLu = range;
    }

    std::vector<float> mono(left.size());
    for (size_t i = 0; i < left.size(); ++i) mono[i] = 0.5f * (left[i] + right[i]);
    std::vector<essentia::Real> monoReal(mono.begin(), mono.end());

    // --- True peak ---
    {
        auto truePeak = create("TruePeakDetector");
        truePeak->configure("sampleRate", static_cast<essentia::Real>(sampleRate));
        std::vector<essentia::Real> output;
        std::vector<essentia::Real> peakLocations;
        truePeak->input("signal").set(monoReal);
        truePeak->output("output").set(output);
        truePeak->output("peakLocations").set(peakLocations);
        truePeak->compute();
        essentia::Real peak = 0;
        for (auto v : output) peak = std::max(peak, std::abs(v));
        d.truePeakDb = peak > 0 ? 20.0 * std::log10(peak) : -std::numeric_limits<double>::infinity();
    }

    // --- Crest factor (peak / mean of |signal|) ---
    {
        std::vector<essentia::Real> absSignal(monoReal.size());
        for (size_t i = 0; i < monoReal.size(); ++i) absSignal[i] = std::abs(monoReal[i]);
        auto crest = create("Crest");
        essentia::Real crestValue = 0;
        crest->input("array").set(absSignal);
        crest->output("crest").set(crestValue);
        crest->compute();
        d.crestFactor = crestValue;
    }

    // --- Spectral centroid (Hz) and flatness, frame-averaged ---
    d.spectralCentroidHz = averageSpectralDescriptor(
        mono, "Centroid", "centroid", {{"range", essentia::Parameter(sampleRate / 2.0)}});
    d.spectralFlatness = averageSpectralDescriptor(mono, "Flatness", "flatness");

    // --- Attack time (whole-file envelope; most meaningful on one-shots) ---
    {
        auto envelope = create("Envelope");
        envelope->configure("sampleRate", static_cast<essentia::Real>(sampleRate));
        std::vector<essentia::Real> env;
        envelope->input("signal").set(monoReal);
        envelope->output("signal").set(env);
        envelope->compute();

        auto attack = create("LogAttackTime");
        attack->configure("sampleRate", static_cast<essentia::Real>(sampleRate));
        essentia::Real logAttackTime = 0, attackStart = 0, attackStop = 0;
        attack->input("signal").set(env);
        attack->output("logAttackTime").set(logAttackTime);
        attack->output("attackStart").set(attackStart);
        attack->output("attackStop").set(attackStop);
        try {
            attack->compute();
            d.attackTimeSeconds = std::pow(10.0, logAttackTime);
        } catch (const essentia::EssentiaException&) {
            d.attackTimeSeconds = 0.0; // e.g. a silent or pathologically short envelope
        }
    }

    return d;
}

std::string toJson(const DspDescriptors& d) {
    std::ostringstream oss;
    oss << "{\"integrated_loudness_lufs\":" << d.integratedLoudnessLufs
        << ",\"loudness_range_lu\":" << d.loudnessRangeLu
        << ",\"true_peak_db\":" << d.truePeakDb
        << ",\"crest_factor\":" << d.crestFactor
        << ",\"spectral_centroid_hz\":" << d.spectralCentroidHz
        << ",\"spectral_flatness\":" << d.spectralFlatness
        << ",\"attack_time_seconds\":" << d.attackTimeSeconds
        << "}";
    return oss.str();
}

} // namespace mira
