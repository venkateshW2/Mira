#pragma once

#include <string>
#include <vector>

namespace mira {

// danceability-discogs-effnet-1.onnx (PRD §2c) — binary softmax over the discogs-effnet
// embedding (Embedding.h): [danceable, not_danceable]. Distinct from Mir.h's
// `danceability` field (Essentia's DSP-based `Danceability` algorithm, a real-valued DFA
// score) — this is a separate, model-based second opinion, not a duplicate.
struct DanceabilityResult {
    bool ok = false;
    double danceableProbability = 0.0; // softmax score for "danceable"
};

// `embedding` must be the 1280-dim vector from computeEmbedding (Embedding.h). `modelPath`
// is models/classification-heads/danceability/danceability-discogs-effnet-1.onnx.
DanceabilityResult classifyDanceability(const std::vector<double>& embedding, const std::string& modelPath);

std::string toJson(const DanceabilityResult& d);

} // namespace mira
