#include "ClapText.h"

#include "OrtEnv.h"
#include "RobertaTokenizer.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

namespace mira {

const char* const kPromptTemplate = "This is a sound of %s.";

ClapTextResult computeTextEmbedding(const std::string& query, const std::string& modelPath,
                                     const std::string& vocabTsvPath, const std::string& mergesPath) {
    ClapTextResult result;

    RobertaTokenizer tokenizer(vocabTsvPath, mergesPath);
    if (!tokenizer.ok()) {
        result.error = "tokenizer unavailable: " + tokenizer.error();
        return result;
    }

    // Wrap the query the way the model was trained to read it (see kPromptTemplate).
    std::vector<char> prompt(query.size() + 64);
    std::snprintf(prompt.data(), prompt.size(), kPromptTemplate, query.c_str());
    auto ids = tokenizer.encode(prompt.data());

    // RoBERTa's own limit. A search query nowhere near approaches it, but a truncation
    // here is silent in the vector rather than an error, so it is clamped explicitly.
    constexpr size_t kMaxTokens = 512;
    if (ids.size() > kMaxTokens) {
        ids.resize(kMaxTokens);
        ids.back() = ids.front() == 0 ? 2 : ids.back(); // keep a closing </s>
    }

    try {
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        Ort::Session session(sharedOrtEnv(), modelPath.c_str(), sessionOptions);

        // input_ids + attention_mask, both int64 [batch, seq]; one output, [batch, 512].
        std::vector<int64_t> mask(ids.size(), 1);
        std::array<int64_t, 2> shape { 1, static_cast<int64_t>(ids.size()) };
        auto memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::array<Ort::Value, 2> inputs {
            Ort::Value::CreateTensor<int64_t>(memInfo, ids.data(), ids.size(), shape.data(), shape.size()),
            Ort::Value::CreateTensor<int64_t>(memInfo, mask.data(), mask.size(), shape.data(), shape.size())
        };
        const char* inputNames[] = { "input_ids", "attention_mask" };
        const char* outputNames[] = { "text_embedding" };

        auto outputs = session.Run(Ort::RunOptions { nullptr }, inputNames, inputs.data(), inputs.size(),
                                    outputNames, 1);
        const float* raw = outputs[0].GetTensorData<float>();
        auto count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();

        // L2-normalize, matching how the stored audio vectors are normalized -- cosine
        // similarity between the two spaces is only meaningful if both are unit length.
        double norm = 0.0;
        for (size_t i = 0; i < count; ++i) norm += static_cast<double>(raw[i]) * raw[i];
        norm = std::sqrt(norm);
        if (!(norm > 0.0)) {
            result.error = "text model returned a zero vector";
            return result;
        }
        result.vector.resize(count);
        for (size_t i = 0; i < count; ++i) result.vector[i] = static_cast<float>(raw[i] / norm);
        result.ok = true;
    } catch (const Ort::Exception& e) {
        result.error = std::string("text model failed: ") + e.what();
    }
    return result;
}

} // namespace mira
