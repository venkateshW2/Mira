#include "VoiceInstrumental.h"
#include "ClassificationHead.h"

#include <sstream>

namespace mira {

namespace {
constexpr int kVoiceInstrumentalClassCount = 2; // [instrumental, voice] — model's own class order
} // namespace

VoiceInstrumentalResult classifyVoiceInstrumental(const std::vector<double>& embedding,
                                                   const std::string& modelPath) {
    VoiceInstrumentalResult result;
    auto scores = runClassificationHead(embedding, modelPath, kVoiceInstrumentalClassCount);
    if (scores.empty()) return result;
    result.voiceProbability = scores[1];
    result.ok = true;
    return result;
}

std::string toJson(const VoiceInstrumentalResult& v) {
    std::ostringstream oss;
    oss << "{\"voice_probability\":" << v.voiceProbability << "}";
    return oss.str();
}

} // namespace mira
