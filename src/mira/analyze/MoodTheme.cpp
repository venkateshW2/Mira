#include "MoodTheme.h"
#include "MoodThemeLabels.h"

#include <onnxruntime_cxx_api.h>

#include <sstream>

namespace mira {

namespace {
constexpr int kEmbeddingDim = 1280;
} // namespace

MoodThemeResult classifyMoodTheme(const std::vector<double>& embedding, const std::string& modelPath) {
    MoodThemeResult result;
    if (static_cast<int>(embedding.size()) != kEmbeddingDim) return result;

    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "mira_moodtheme");
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        Ort::Session session(env, modelPath.c_str(), sessionOptions);

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
        if (static_cast<int>(count) != kMoodThemeClassCount) return result;

        result.scores.assign(data, data + count);
        result.ok = true;
    } catch (const std::exception&) {
        // ok stays false — caller skips this file's moodtheme output
    }

    return result;
}

std::string toJson(const MoodThemeResult& m) {
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < m.scores.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << kMoodThemeClassNames[i] << "\":" << m.scores[i];
    }
    oss << "}";
    return oss.str();
}

} // namespace mira
