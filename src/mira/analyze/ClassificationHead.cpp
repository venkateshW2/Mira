#include "ClassificationHead.h"
#include "OrtEnv.h"

#include <onnxruntime_cxx_api.h>

namespace mira {

namespace {
constexpr int kEmbeddingDim = 1280;
} // namespace

std::vector<double> runClassificationHead(const std::vector<double>& embedding,
                                           const std::string& modelPath, int expectedClassCount) {
    if (static_cast<int>(embedding.size()) != kEmbeddingDim) return {};

    try {
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        Ort::Session session(sharedOrtEnv(), modelPath.c_str(), sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputNameAlloc = session.GetInputNameAllocated(0, allocator);
        std::string inputName = inputNameAlloc.get();
        auto outputNameAlloc = session.GetOutputNameAllocated(0, allocator);
        std::string outputName = outputNameAlloc.get();

        std::vector<float> embeddingF(embedding.begin(), embedding.end());
        std::vector<int64_t> inputShape = {1, kEmbeddingDim};
        Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
            memInfo, embeddingF.data(), embeddingF.size(), inputShape.data(), inputShape.size());

        const char* inputNameC = inputName.c_str();
        const char* outputNameC = outputName.c_str();
        auto outputs = session.Run(Ort::RunOptions{nullptr}, &inputNameC, &inputTensor, 1,
                                    &outputNameC, 1);

        const float* data = outputs[0].GetTensorData<float>();
        size_t count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
        if (static_cast<int>(count) != expectedClassCount) return {};

        return std::vector<double>(data, data + count);
    } catch (const std::exception&) {
        return {};
    }
}

} // namespace mira
