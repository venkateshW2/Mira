#pragma once

#include <string>
#include <vector>

namespace mira {

// mtg_jamendo_moodtheme-discogs-effnet-1.onnx (PRD §2c) — 56-class mood/theme sigmoid
// over the discogs-effnet embedding (Embedding.h). Caller is responsible for gating this
// on ContentGate's is_music signal — "Music heads must only run on music" (PRD §2c), the
// same honesty principle already applied to key/chords via the harmonicity gate.
//
// Simplification, not yet resolved: this feeds the single whole-file mean-pooled
// embedding through the head once, rather than running the head per-patch and averaging
// *sigmoid outputs* (which is what MTG's own reference pipeline does — averaging before
// vs. after a nonlinearity are not equivalent). Documented here rather than hidden;
// revisit if scores look systematically off against real files.
struct MoodThemeResult {
    bool ok = false;
    std::vector<double> scores; // 56 entries, same order as kMoodThemeClassNames (MoodThemeLabels.h)
};

// `embedding` must be the 1280-dim vector from computeEmbedding (Embedding.h). `modelPath`
// is models/classification-heads/mtg_jamendo_moodtheme/mtg_jamendo_moodtheme-discogs-effnet-1.onnx.
MoodThemeResult classifyMoodTheme(const std::vector<double>& embedding, const std::string& modelPath);

std::string toJson(const MoodThemeResult& m);

} // namespace mira
