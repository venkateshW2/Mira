#include "MoodTheme.h"
#include "ClassificationHead.h"
#include "MoodThemeLabels.h"

#include <sstream>

namespace mira {

MoodThemeResult classifyMoodTheme(const std::vector<double>& embedding, const std::string& modelPath) {
    MoodThemeResult result;
    auto scores = runClassificationHead(embedding, modelPath, kMoodThemeClassCount);
    if (scores.empty()) return result;
    result.scores = std::move(scores);
    result.ok = true;
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
