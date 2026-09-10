#include "Genre.h"
#include "ClassificationHead.h"
#include "GenreLabels.h"

#include <sstream>

namespace mira {

GenreResult classifyGenre(const std::vector<double>& embedding, const std::string& modelPath) {
    GenreResult result;
    auto scores = runClassificationHead(embedding, modelPath, kGenreClassCount);
    if (scores.empty()) return result;
    result.scores = std::move(scores);
    result.ok = true;
    return result;
}

std::string toJson(const GenreResult& g) {
    std::ostringstream oss;
    oss << "{";
    for (size_t i = 0; i < g.scores.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << kGenreClassNames[i] << "\":" << g.scores[i];
    }
    oss << "}";
    return oss.str();
}

} // namespace mira
