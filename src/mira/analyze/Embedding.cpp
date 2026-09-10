// discogs-effnet embedding (PRD §2c, §16.3). The mel frontend + patching + ONNX
// inference below is adapted from spike/02_onnx_parity/main.cpp, which proved this exact
// sequence bitwise-identical to the Python reference on flamenco.wav (PRD §9 day 3 exit
// criterion) — not a fresh implementation, a straight port of already-verified code into
// the app. Differences from the spike: takes already-decoded/resampled audio instead of
// reloading the file, returns a mean-pooled vector instead of writing .bin files, and
// degrades gracefully (try/catch, ok=false) instead of exiting on error.

#include "Embedding.h"
#include "OrtEnv.h"

#include <essentia/algorithmfactory.h>
#include <onnxruntime_cxx_api.h>

#include <memory>
#include <numeric>
#include <sstream>

namespace mira {

namespace {
constexpr int kFrameSize = 512;
constexpr int kHopSize = 256;
constexpr int kSampleRate = 16000;
constexpr int kNumberBands = 96;
constexpr int kPatchSize = 128;
constexpr int kPatchHopSize = 62;
constexpr int kEmbeddingDim = 1280;

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
} // namespace

EmbeddingResult computeEmbedding(const std::vector<float>& mono, int sampleRate,
                                  const std::string& modelPath) {
    EmbeddingResult result;
    if (mono.empty() || sampleRate <= 0) return result;

    try {
        auto audio16k = resampleTo16k(mono, sampleRate);
        if (audio16k.empty()) return result;

        // --- Mel frontend: FrameCutter -> TensorflowInputMusiCNN (no-TF-safe, PRD §16.2) ---
        auto& factory = essentia::standard::AlgorithmFactory::instance();
        std::unique_ptr<essentia::standard::Algorithm> frameCutter(
            factory.create("FrameCutter", "frameSize", kFrameSize, "hopSize", kHopSize));
        std::unique_ptr<essentia::standard::Algorithm> tfInput(
            factory.create("TensorflowInputMusiCNN"));

        std::vector<essentia::Real> signalBuf(audio16k.begin(), audio16k.end());
        frameCutter->input("signal").set(signalBuf);

        std::vector<essentia::Real> frame;
        frameCutter->output("frame").set(frame);

        std::vector<essentia::Real> bandsOut;
        tfInput->input("frame").set(frame);
        tfInput->output("bands").set(bandsOut);

        std::vector<std::vector<float>> bands; // [n_frames][96]
        while (true) {
            frameCutter->compute();
            if (frame.empty()) break;
            tfInput->compute();
            bands.emplace_back(bandsOut.begin(), bandsOut.end());
        }

        int nFrames = static_cast<int>(bands.size());
        // Real bug, found via ASan (container-overflow reading `bands` out of bounds):
        // C++ integer division truncates toward zero, not floor. When nFrames is just
        // under kPatchSize, (nFrames - kPatchSize) is negative, and e.g. -4/62 == 0 (not
        // -1) — so the old `1 + (nFrames-kPatchSize)/kPatchHopSize` formula could compute
        // nPatches==1 even when there wasn't a full 128-frame patch available, silently
        // reading past the end of `bands` below. Guarding nFrames < kPatchSize directly,
        // before the division, avoids relying on truncation behavior to get this right.
        if (nFrames < kPatchSize) return result; // too short for even one patch — unmeasured, not a failure
        int nPatches = 1 + (nFrames - kPatchSize) / kPatchHopSize;

        // --- Patch into [n, 128, 96], overlap 62, discard last partial (PRD §16.3) ---
        std::vector<float> patches(static_cast<size_t>(nPatches) * kPatchSize * kNumberBands);
        for (int p = 0; p < nPatches; ++p) {
            int startFrame = p * kPatchHopSize;
            for (int t = 0; t < kPatchSize; ++t) {
                const std::vector<float>& srcFrame = bands[startFrame + t];
                float* dst = &patches[(static_cast<size_t>(p) * kPatchSize + t) * kNumberBands];
                std::copy(srcFrame.begin(), srcFrame.end(), dst);
            }
        }

        // --- ONNX Runtime inference ---
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        Ort::Session session(sharedOrtEnv(), modelPath.c_str(), sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputNameAlloc = session.GetInputNameAllocated(0, allocator);
        std::string inputName = inputNameAlloc.get();

        size_t numOutputs = session.GetOutputCount();
        std::vector<std::string> outputNames;
        std::vector<const char*> outputNamesC;
        for (size_t i = 0; i < numOutputs; ++i) {
            auto nameAlloc = session.GetOutputNameAllocated(i, allocator);
            outputNames.push_back(nameAlloc.get());
        }
        for (auto& n : outputNames) outputNamesC.push_back(n.c_str());

        std::vector<int64_t> inputShape = {nPatches, kPatchSize, kNumberBands};
        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, patches.data(), patches.size(), inputShape.data(), inputShape.size());

        const char* inputNameC = inputName.c_str();
        auto outputs = session.Run(Ort::RunOptions{nullptr}, &inputNameC, &inputTensor, 1,
                                    outputNamesC.data(), outputNamesC.size());

        // Find the 1280-dim embedding output (PartitionedCall:1 per the model's own .json).
        for (size_t i = 0; i < outputs.size(); ++i) {
            auto shape = outputs[i].GetTensorTypeAndShapeInfo().GetShape();
            if (shape.empty() || shape.back() != kEmbeddingDim) continue;

            const float* data = outputs[i].GetTensorData<float>();
            std::vector<double> pooled(kEmbeddingDim, 0.0);
            for (int p = 0; p < nPatches; ++p) {
                for (int d = 0; d < kEmbeddingDim; ++d) {
                    pooled[d] += data[static_cast<size_t>(p) * kEmbeddingDim + d];
                }
            }
            for (double& v : pooled) v /= nPatches;

            result.vector = std::move(pooled);
            result.patchCount = nPatches;
            result.ok = true;
            break;
        }
    } catch (const std::exception&) {
        // ok stays false — caller skips this file's embedding/classification, same
        // discipline as every other analyzer here (Router, ActiveRegions, Key, Chords).
    }

    return result;
}

std::string toJson(const EmbeddingResult& e) {
    std::ostringstream oss;
    oss << "{\"patch_count\":" << e.patchCount << ",\"vector\":[";
    for (size_t i = 0; i < e.vector.size(); ++i) {
        if (i > 0) oss << ",";
        oss << e.vector[i];
    }
    oss << "]}";
    return oss.str();
}

} // namespace mira
