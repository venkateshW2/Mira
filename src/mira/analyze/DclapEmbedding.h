#pragma once

#include <string>
#include <vector>

namespace mira {

// AudioMuse-AI-DCLAP audio-tower embedding (TASKS.md Phase 4 "Embedding A/B", PRD §4,
// §14.5) — a distilled LAION-CLAP audio encoder, 512-dim, a second similarity space
// alongside discogs-effnet's 1280-dim one (Embedding.h). Where discogs-effnet is trained
// on Discogs music-genre labels, DCLAP is trained for general audio/text similarity —
// the PRD's stated hypothesis (§4) is that this generalizes better to one-shots and
// non-music-genre content than a genre classifier's embedding does; A/B'd empirically,
// not assumed.
struct DclapEmbeddingResult {
    bool ok = false;
    std::vector<double> vector; // 512-dim, L2-normalized; empty if unmeasurable
    int segmentCount = 0;       // number of 10s/50%-overlap segments averaged, per the
                                 // model author's own README; 0 means unmeasurable, not
                                 // a failure — same "unmeasured, not zero" discipline as
                                 // EmbeddingResult::patchCount
};

// Requires the DCLAP student audio encoder (models/similarity-embeddings/dclap/
// model_epoch_36.onnx, with model_epoch_36.onnx.data alongside it — ONNX Runtime
// resolves the external-data file by its path recorded inside the .onnx graph, relative
// to modelPath's own directory). `mono` is the file's full-length downmix at its native
// sampleRate (AudioLoader.h) — this function does its own resample to 48kHz internally
// (via libsoxr, matching the reference pipeline's librosa.load(sr=48000) default
// res_type='soxr_hq' — NOT Essentia's own Resample, a different algorithm that would
// silently produce a different embedding, spike/05_dclap_parity's whole point).
DclapEmbeddingResult computeDclapEmbedding(const std::vector<float>& mono, int sampleRate,
                                            const std::string& audioModelPath);

std::string toJson(const DclapEmbeddingResult& e);

} // namespace mira
