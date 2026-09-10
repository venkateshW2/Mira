#pragma once

#include <string>
#include <vector>

namespace mira {

// mtg_jamendo_instrument-discogs-effnet-1.onnx (PRD §2c) — 40-class instrument-presence
// sigmoid over the discogs-effnet embedding (Embedding.h). Caller is responsible for
// gating this on ContentGate's is_music signal, same as MoodTheme.
struct InstrumentResult {
    bool ok = false;
    std::vector<double> scores; // 40 entries, same order as kInstrumentClassNames (InstrumentLabels.h)
};

// `embedding` must be the 1280-dim vector from computeEmbedding (Embedding.h). `modelPath`
// is models/classification-heads/mtg_jamendo_instrument/mtg_jamendo_instrument-discogs-effnet-1.onnx.
InstrumentResult classifyInstrument(const std::vector<double>& embedding, const std::string& modelPath);

std::string toJson(const InstrumentResult& i);

} // namespace mira
