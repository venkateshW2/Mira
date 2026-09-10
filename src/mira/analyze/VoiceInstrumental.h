#pragma once

#include <string>
#include <vector>

namespace mira {

// voice_instrumental-discogs-effnet-1.onnx (PRD §2c, §16.3) — binary softmax over the
// discogs-effnet embedding (Embedding.h): [instrumental, voice]. No published ONNX
// existed at all (PRD §16.3); converted via tf2onnx from the .pb graph, verified to
// 1.79e-7 max abs diff against TensorFlow directly (see TASKS.md Phase 2).
struct VoiceInstrumentalResult {
    bool ok = false;
    double voiceProbability = 0.0; // softmax score for "voice" (vs. "instrumental")
};

// `embedding` must be the 1280-dim vector from computeEmbedding (Embedding.h). `modelPath`
// is models/classification-heads/voice_instrumental/voice_instrumental-discogs-effnet-1.onnx.
VoiceInstrumentalResult classifyVoiceInstrumental(const std::vector<double>& embedding,
                                                   const std::string& modelPath);

std::string toJson(const VoiceInstrumentalResult& v);

} // namespace mira
