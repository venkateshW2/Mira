#pragma once

#include <string>
#include <vector>

namespace mira {

// discogs-effnet-bsdynamic-1.onnx embedding (PRD §2c) — mandatory input to every
// classification head below it, and the similarity vector itself. Runs on *every*
// content type, unlike rhythm/key: embedding-based similarity is exactly the point of
// comparing one-shots (Sononym-style "find similar samples"), so this is not
// content-type-gated the way tempo/key are.
struct EmbeddingResult {
    bool ok = false;
    std::vector<double> vector; // mean-pooled 1280-dim embedding; empty if unmeasurable
    int patchCount = 0;         // number of 128-frame mel patches pooled into `vector`;
                                 // 0 means the file was too short for even one patch
                                 // (~2s at the model's 16kHz/256-hop mel rate), not a
                                 // failure — same "unmeasured, not zero" discipline as
                                 // harmonicity_frame_count (Descriptors.h)
};

// Requires an EssentiaEngine to already exist. `modelPath` is
// models/feature-extractors/discogs-effnet/discogs-effnet-bsdynamic-1.onnx (PRD §16.3,
// bitwise-parity-verified against the Python reference in spike/02_onnx_parity).
EmbeddingResult computeEmbedding(const std::vector<float>& mono, int sampleRate,
                                  const std::string& modelPath);

std::string toJson(const EmbeddingResult& e);

} // namespace mira
