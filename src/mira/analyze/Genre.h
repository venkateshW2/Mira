#pragma once

#include <string>
#include <vector>

namespace mira {

// genre_discogs400-discogs-effnet-1.onnx (PRD §2c, §16.3) — 400-class Discogs genre/style
// sigmoid over the discogs-effnet embedding (Embedding.h). No published ONNX existed
// (PRD §16.3); converted via tf2onnx from the .pb graph, verified to 2.06e-6 max abs diff
// against TensorFlow directly (see TASKS.md Phase 2). Caller is responsible for gating
// this on ContentGate's is_music signal, same as MoodTheme/Instrument.
struct GenreResult {
    bool ok = false;
    std::vector<double> scores; // 400 entries, same order as kGenreClassNames (GenreLabels.h)
};

// `embedding` must be the 1280-dim vector from computeEmbedding (Embedding.h). `modelPath`
// is models/classification-heads/genre_discogs400/genre_discogs400-discogs-effnet-1.onnx.
GenreResult classifyGenre(const std::vector<double>& embedding, const std::string& modelPath);

std::string toJson(const GenreResult& g);

} // namespace mira
