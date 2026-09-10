// nii-yamagishilab predominant-instrument recognizer (TASKS.md Phase 2 real-world
// finding). Input contract reverse-engineered from the published checkpoint + source
// (no model code ships with the checkpoint) -- see lab/export_irmas_instrument_onnx.py's
// docstring for the full derivation, verified there to 1e-5 graph parity against the
// original PyTorch model.

#include "StemInstrument.h"

#include <essentia/algorithmfactory.h>
#include <onnxruntime_cxx_api.h>

#include <cmath>
#include <memory>
#include <sstream>

namespace mira {

namespace {
constexpr int kSampleRate = 16000;
constexpr int kWindowSamples = 16000; // 1 second -- the exact clip length training used
constexpr int kClassCount = 11;
constexpr double kTargetLufs = -12.0;
constexpr const char* kClassNames[kClassCount] = {
    "cel", "cla", "flu", "gac", "gel", "org", "pia", "sax", "tru", "vio", "voi"};

std::vector<float> resampleTo16k(const std::vector<float>& mono, int sampleRate) {
    if (sampleRate == kSampleRate) return mono;

    auto& factory = essentia::standard::AlgorithmFactory::instance();
    std::unique_ptr<essentia::standard::Algorithm> resample(factory.create(
        "Resample", "inputSampleRate", static_cast<essentia::Real>(sampleRate),
        "outputSampleRate", static_cast<essentia::Real>(kSampleRate)));

    std::vector<essentia::Real> input(mono.begin(), mono.end());
    std::vector<essentia::Real> output;
    resample->input("signal").set(input);
    resample->output("signal").set(output);
    resample->compute();

    return std::vector<float>(output.begin(), output.end());
}

// Matches IRMASDataset.loudness_normalize (pyloudnorm, ITU-R BS.1770, ref training code)
// as closely as Essentia's own primitives allow: LoudnessEBUR128 only measures stereo,
// so a mono signal is duplicated to L=R first. That measures ~10*log10(2) ≈ 3.01 dB
// *louder* than a true single-channel BS.1770 reading (both channels' power sums
// instead of one), corrected for below. Approximate, not bit-exact against pyloudnorm --
// documented rather than assumed exact; the model's own robustness to modest gain
// differences (it's normalizing to reduce dynamic-range variance, not tuned to a precise
// dB) makes this an acceptable approximation rather than a from-scratch BS.1770
// K-weighting implementation for one preprocessing step.
std::vector<float> loudnessNormalize(const std::vector<float>& mono) {
    if (mono.empty()) return mono;

    auto& factory = essentia::standard::AlgorithmFactory::instance();
    std::unique_ptr<essentia::standard::Algorithm> loudness(
        factory.create("LoudnessEBUR128", "sampleRate", static_cast<essentia::Real>(kSampleRate)));

    std::vector<essentia::StereoSample> stereo(mono.size());
    for (size_t i = 0; i < mono.size(); ++i) {
        stereo[i].left() = mono[i];
        stereo[i].right() = mono[i];
    }

    std::vector<essentia::Real> momentary, shortTerm;
    essentia::Real integrated = 0, range = 0;
    loudness->input("signal").set(stereo);
    loudness->output("momentaryLoudness").set(momentary);
    loudness->output("shortTermLoudness").set(shortTerm);
    loudness->output("integratedLoudness").set(integrated);
    loudness->output("loudnessRange").set(range);
    loudness->compute();

    if (!std::isfinite(integrated)) return mono; // e.g. near-silent window -- leave as-is

    constexpr double kMonoDuplicationCorrectionDb = 10.0 * 0.30102999566; // 10*log10(2)
    double monoLufs = integrated - kMonoDuplicationCorrectionDb;
    double gainDb = kTargetLufs - monoLufs;
    double gainLinear = std::pow(10.0, gainDb / 20.0);

    std::vector<float> out(mono.size());
    for (size_t i = 0; i < mono.size(); ++i) out[i] = static_cast<float>(mono[i] * gainLinear);
    return out;
}

double sigmoid(double x) { return 1.0 / (1.0 + std::exp(-x)); }
} // namespace

StemInstrumentResult classifyStemInstrument(const std::vector<float>& mono, int sampleRate,
                                             const std::string& modelPath) {
    StemInstrumentResult result;
    if (mono.empty() || sampleRate <= 0) return result;

    try {
        auto audio16k = resampleTo16k(mono, sampleRate);
        if (static_cast<int>(audio16k.size()) < kWindowSamples) return result;

        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "mira_stem_instrument");
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        Ort::Session session(env, modelPath.c_str(), sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputNameAlloc = session.GetInputNameAllocated(0, allocator);
        std::string inputName = inputNameAlloc.get();
        auto outputNameAlloc = session.GetOutputNameAllocated(0, allocator);
        std::string outputName = outputNameAlloc.get();
        const char* inputNameC = inputName.c_str();
        const char* outputNameC = outputName.c_str();

        std::vector<double> scoreSum(kClassCount, 0.0);
        int windowCount = 0;
        int numSamples = static_cast<int>(audio16k.size());

        for (int start = 0; start + kWindowSamples <= numSamples; start += kWindowSamples) {
            std::vector<float> window(audio16k.begin() + start, audio16k.begin() + start + kWindowSamples);
            window = loudnessNormalize(window);

            std::vector<int64_t> inputShape = {1, kWindowSamples};
            Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, window.data(), window.size(), inputShape.data(), inputShape.size());

            auto outputs = session.Run(Ort::RunOptions{nullptr}, &inputNameC, &inputTensor, 1,
                                        &outputNameC, 1);

            const float* logits = outputs[0].GetTensorData<float>();
            size_t count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (static_cast<int>(count) != kClassCount) continue;

            for (int i = 0; i < kClassCount; ++i) scoreSum[i] += sigmoid(logits[i]);
            ++windowCount;
        }

        if (windowCount == 0) return result;

        result.scores.resize(kClassCount);
        for (int i = 0; i < kClassCount; ++i) result.scores[i] = scoreSum[i] / windowCount;
        result.windowCount = windowCount;
        result.ok = true;
    } catch (const std::exception&) {
        // ok stays false -- caller skips this file's stem-instrument signal, same
        // discipline as every other analyzer here (Router, ActiveRegions, Key, Chords).
    }

    return result;
}

std::string toJson(const StemInstrumentResult& r) {
    std::ostringstream oss;
    oss << "{\"window_count\":" << r.windowCount << ",\"scores\":{";
    for (int i = 0; i < static_cast<int>(r.scores.size()); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << kClassNames[i] << "\":" << r.scores[i];
    }
    oss << "}}";
    return oss.str();
}

} // namespace mira
