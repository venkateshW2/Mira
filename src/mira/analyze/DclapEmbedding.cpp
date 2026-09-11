// DCLAP audio-tower embedding (PRD §4, §14.5; TASKS.md Phase 4 "Embedding A/B"). The
// mel frontend + segmentation + ONNX inference below is a straight port of
// spike/05_dclap_parity/main.cpp, which proved this exact pipeline agrees with the
// Python/librosa reference (lab/dclap_parity_reference.py) to cosine similarity
// >0.999 on the real flamenco.wav fixture — not a fresh implementation. See that
// spike's main.cpp for the full derivation of the mel-filterbank/resampler choices;
// comments here cover only what differs from it: takes already-decoded native-rate
// audio instead of reloading the file, uses the shared process-lifetime Ort::Env
// (OrtEnv.h) instead of a local one, and degrades gracefully (try/catch, ok=false)
// instead of exiting on error, matching Embedding.cpp's own convention.

#include "DclapEmbedding.h"
#include "OrtEnv.h"

#include <essentia/algorithmfactory.h>
#include <onnxruntime_cxx_api.h>
#include <soxr.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <memory>
#include <sstream>

namespace mira {

namespace {
constexpr double kTargetSr = 48000.0;
constexpr int kSegmentLength = 480000; // 10s @ 48kHz
constexpr int kHopLength = 240000;     // 50% overlap
constexpr int kNumMels = 128;
constexpr int kNFft = 2048;
constexpr int kHopLengthMels = 480;
constexpr int kNumFreqBins = kNFft / 2 + 1; // 1025
constexpr double kFmin = 0.0;
constexpr double kFmax = 14000.0;
constexpr int kEmbeddingDim = 512;

// Slaney mel scale (librosa/core/convert.py hz_to_mel/mel_to_hz, htk=False) —
// spike/05_dclap_parity/main.cpp verified this reproduces librosa's mel filterbank
// closely enough for the model to agree with the Python reference to cosine >0.999.
double hzToMelSlaney(double f) {
    constexpr double fSp = 200.0 / 3.0;
    constexpr double minLogHz = 1000.0;
    constexpr double minLogMel = minLogHz / fSp; // 15.0
    const double logstep = std::log(6.4) / 27.0;
    if (f >= minLogHz) return minLogMel + std::log(f / minLogHz) / logstep;
    return f / fSp;
}

double melToHzSlaney(double mel) {
    constexpr double fSp = 200.0 / 3.0;
    constexpr double minLogHz = 1000.0;
    constexpr double minLogMel = minLogHz / fSp;
    const double logstep = std::log(6.4) / 27.0;
    if (mel >= minLogMel) return minLogHz * std::exp(logstep * (mel - minLogMel));
    return fSp * mel;
}

std::vector<std::vector<double>> buildMelFilterbank() {
    std::vector<double> fftFreqs(kNumFreqBins);
    for (int k = 0; k < kNumFreqBins; ++k) fftFreqs[k] = k * kTargetSr / kNFft;

    int nPoints = kNumMels + 2;
    double melMin = hzToMelSlaney(kFmin);
    double melMax = hzToMelSlaney(kFmax);
    std::vector<double> melF(nPoints);
    for (int i = 0; i < nPoints; ++i) {
        double mel = melMin + (melMax - melMin) * i / (nPoints - 1);
        melF[i] = melToHzSlaney(mel);
    }

    std::vector<std::vector<double>> weights(kNumMels, std::vector<double>(kNumFreqBins, 0.0));
    for (int i = 0; i < kNumMels; ++i) {
        double fdiffLower = melF[i + 1] - melF[i];
        double fdiffUpper = melF[i + 2] - melF[i + 1];
        for (int k = 0; k < kNumFreqBins; ++k) {
            double lower = (fftFreqs[k] - melF[i]) / fdiffLower;
            double upper = (melF[i + 2] - fftFreqs[k]) / fdiffUpper;
            weights[i][k] = std::max(0.0, std::min(lower, upper));
        }
        double enorm = 2.0 / (melF[i + 2] - melF[i]); // norm='slaney'
        for (int k = 0; k < kNumFreqBins; ++k) weights[i][k] *= enorm;
    }
    return weights;
}

// numpy 'reflect' padding: whole-sample symmetric, edge value not repeated.
int reflectIndex(int j, int n) {
    if (n == 1) return 0;
    int period = 2 * (n - 1);
    int m = j % period;
    if (m < 0) m += period;
    if (m >= n) m = period - m;
    return m;
}

std::vector<float> resampleToSoxrHQ(const std::vector<float>& input, double inRate, double outRate) {
    if (inRate == outRate) return input;
    size_t outCapacity = static_cast<size_t>(std::ceil(input.size() * outRate / inRate)) + 16;
    std::vector<float> output(outCapacity);
    size_t idone = 0, odone = 0;
    soxr_error_t err = soxr_oneshot(inRate, outRate, 1, input.data(), input.size(), &idone,
                                     output.data(), output.size(), &odone, nullptr, nullptr,
                                     nullptr); // NULL => float32, SOXR_HQ, default runtime
    if (err) return {};
    output.resize(odone);
    return output;
}

// DCLAP README segmentation: pad short files to one segment; otherwise 50%-overlap hop,
// plus a final tail segment ending exactly at the file's end if the hop grid doesn't
// already land there.
std::vector<std::vector<float>> segmentAudio(const std::vector<float>& y) {
    std::vector<std::vector<float>> segments;
    int total = static_cast<int>(y.size());
    if (total == 0) return segments;
    if (total <= kSegmentLength) {
        std::vector<float> seg(kSegmentLength, 0.0f);
        std::copy(y.begin(), y.end(), seg.begin());
        segments.push_back(std::move(seg));
        return segments;
    }
    int start = 0;
    while (start + kSegmentLength <= total) {
        segments.emplace_back(y.begin() + start, y.begin() + start + kSegmentLength);
        start += kHopLength;
    }
    int lastStart = static_cast<int>(segments.size()) * kHopLength;
    if (lastStart < total) {
        segments.emplace_back(y.end() - kSegmentLength, y.end());
    }
    return segments;
}

// One segment -> [128, T] log-mel, matching librosa.feature.melspectrogram(n_fft=2048,
// hop=480, win_length=2048, window='hann', center=True, pad_mode='reflect', power=2.0)
// followed by power_to_db(ref=1.0, amin=1e-10, top_db=None).
std::vector<float> computeLogMel(const std::vector<float>& segment,
                                  const std::vector<std::vector<double>>& melFB,
                                  essentia::standard::AlgorithmFactory& factory) {
    int n = static_cast<int>(segment.size());
    int padAmt = kNFft / 2;
    int paddedLen = n + 2 * padAmt;
    std::vector<float> padded(paddedLen);
    for (int j = -padAmt; j < n + padAmt; ++j) {
        padded[j + padAmt] = segment[reflectIndex(j, n)];
    }

    std::vector<double> window(kNFft);
    for (int k = 0; k < kNFft; ++k) window[k] = 0.5 - 0.5 * std::cos(2.0 * M_PI * k / kNFft);

    int nFrames = 1 + (paddedLen - kNFft) / kHopLengthMels;

    std::unique_ptr<essentia::standard::Algorithm> fft(factory.create("FFT", "size", kNFft));
    std::vector<essentia::Real> frameBuf(kNFft);
    std::vector<std::complex<essentia::Real>> fftOut;
    fft->input("frame").set(frameBuf);
    fft->output("fft").set(fftOut);

    std::vector<float> logMel(static_cast<size_t>(kNumMels) * nFrames);
    std::vector<double> power(kNumFreqBins);

    for (int t = 0; t < nFrames; ++t) {
        int frameStart = t * kHopLengthMels;
        for (int k = 0; k < kNFft; ++k) {
            frameBuf[k] = static_cast<essentia::Real>(padded[frameStart + k] * window[k]);
        }
        fft->compute();
        for (int k = 0; k < kNumFreqBins; ++k) {
            double re = fftOut[k].real(), im = fftOut[k].imag();
            power[k] = re * re + im * im;
        }
        for (int m = 0; m < kNumMels; ++m) {
            double acc = 0.0;
            for (int k = 0; k < kNumFreqBins; ++k) acc += melFB[m][k] * power[k];
            double db = 10.0 * std::log10(std::max(1e-10, acc)); // ref=1.0 -> subtract 0
            logMel[static_cast<size_t>(m) * nFrames + t] = static_cast<float>(db);
        }
    }
    return logMel; // row-major [128, nFrames]
}
} // namespace

DclapEmbeddingResult computeDclapEmbedding(const std::vector<float>& mono, int sampleRate,
                                            const std::string& audioModelPath) {
    DclapEmbeddingResult result;
    if (mono.empty() || sampleRate <= 0) return result;

    try {
        auto audio48k = resampleToSoxrHQ(mono, sampleRate, kTargetSr);
        if (audio48k.empty()) return result;

        // int16 quantize round-trip — matches the PyTorch CLAP preprocessing the
        // student was distilled against (DCLAP README is explicit about this step).
        for (float& s : audio48k) {
            float c = std::max(-1.0f, std::min(1.0f, s));
            int16_t q = static_cast<int16_t>(c * 32767.0f);
            s = static_cast<float>(q) / 32767.0f;
        }

        auto segments = segmentAudio(audio48k);
        if (segments.empty()) return result;

        auto melFB = buildMelFilterbank();
        auto& factory = essentia::standard::AlgorithmFactory::instance();

        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        Ort::Session session(sharedOrtEnv(), audioModelPath.c_str(), sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputNameAlloc = session.GetInputNameAllocated(0, allocator);
        std::string inputName = inputNameAlloc.get();
        auto outputNameAlloc = session.GetOutputNameAllocated(0, allocator);
        std::string outputName = outputNameAlloc.get();
        const char* inputNameC = inputName.c_str();
        const char* outputNameC = outputName.c_str();

        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::vector<double> avgEmb(kEmbeddingDim, 0.0);
        for (const auto& segment : segments) {
            std::vector<float> logMel = computeLogMel(segment, melFB, factory);
            int nFrames = static_cast<int>(logMel.size() / kNumMels);

            std::vector<int64_t> inputShape = {1, 1, kNumMels, nFrames};
            Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
                memInfo, logMel.data(), logMel.size(), inputShape.data(), inputShape.size());

            auto outputs = session.Run(Ort::RunOptions{nullptr}, &inputNameC, &inputTensor, 1,
                                        &outputNameC, 1);
            const float* embData = outputs[0].GetTensorData<float>();
            size_t embCount = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
            if (static_cast<int>(embCount) != kEmbeddingDim) return result;

            for (int d = 0; d < kEmbeddingDim; ++d) avgEmb[d] += embData[d];
        }

        for (double& v : avgEmb) v /= static_cast<double>(segments.size());
        double norm = 0.0;
        for (double v : avgEmb) norm += v * v;
        norm = std::sqrt(norm) + 1e-9;
        for (double& v : avgEmb) v /= norm;

        result.vector = std::move(avgEmb);
        result.segmentCount = static_cast<int>(segments.size());
        result.ok = true;
    } catch (const std::exception&) {
        // ok stays false — caller skips this file's DCLAP embedding, same discipline
        // as every other analyzer here (Embedding, Router, ActiveRegions, Key, Chords).
    }

    return result;
}

std::string toJson(const DclapEmbeddingResult& e) {
    std::ostringstream oss;
    oss << "{\"segment_count\":" << e.segmentCount << ",\"vector\":[";
    for (size_t i = 0; i < e.vector.size(); ++i) {
        if (i > 0) oss << ",";
        oss << e.vector[i];
    }
    oss << "]}";
    return oss.str();
}

} // namespace mira
