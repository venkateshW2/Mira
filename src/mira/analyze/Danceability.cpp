#include "Danceability.h"
#include "ClassificationHead.h"

#include <sstream>

namespace mira {

namespace {
constexpr int kDanceabilityClassCount = 2; // [danceable, not_danceable] — model's own class order
} // namespace

DanceabilityResult classifyDanceability(const std::vector<double>& embedding, const std::string& modelPath) {
    DanceabilityResult result;
    auto scores = runClassificationHead(embedding, modelPath, kDanceabilityClassCount);
    if (scores.empty()) return result;
    result.danceableProbability = scores[0];
    result.ok = true;
    return result;
}

std::string toJson(const DanceabilityResult& d) {
    std::ostringstream oss;
    oss << "{\"danceable_probability\":" << d.danceableProbability << "}";
    return oss.str();
}

} // namespace mira
