#pragma once

#include <string>
#include <vector>

namespace mira {

// Shared inference path for every discogs-effnet classification head (PRD §2c) — each
// head (moodtheme, instrument, danceability, and later genre/voice once tf2onnx lands) is
// a trivial second Ort::Session taking the same 1280-dim embedding (Embedding.h) as
// input. Returns the raw output vector — sigmoid or softmax, whichever the head's own
// graph applies; mira doesn't second-guess that — empty on failure or if the output's
// element count doesn't match `expectedClassCount`.
std::vector<double> runClassificationHead(const std::vector<double>& embedding,
                                           const std::string& modelPath, int expectedClassCount);

} // namespace mira
