#include "Instrument.h"
#include "ClassificationHead.h"
#include "InstrumentLabels.h"

#include <sstream>

namespace mira {

InstrumentResult classifyInstrument(const std::vector<double>& embedding, const std::string& modelPath) {
    InstrumentResult result;
    auto scores = runClassificationHead(embedding, modelPath, kInstrumentClassCount);
    if (scores.empty()) return result;
    result.scores = std::move(scores);
    result.ok = true;
    return result;
}

std::string toJson(const InstrumentResult& i) {
    std::ostringstream oss;
    oss << "{";
    for (size_t j = 0; j < i.scores.size(); ++j) {
        if (j > 0) oss << ",";
        oss << "\"" << kInstrumentClassNames[j] << "\":" << i.scores[j];
    }
    oss << "}";
    return oss.str();
}

} // namespace mira
