#pragma once

#include <string>
#include <vector>

namespace mira {

// CLAP text tower -- the other half of the DCLAP model mira already runs.
//
// DCLAP is two encoders trained together so that audio and the words describing it land
// in the SAME 512-dim space. mira has only ever run the audio one (DclapEmbedding.h),
// which is why `mira similar` could find audio-like-this-audio but never
// audio-like-these-words. This is the text one: a sentence in, a 512-dim vector out,
// directly comparable against every vector already stored in `vec_embeddings_dclap` --
// no re-analysis, no new index, the same KNN search.
//
// Two things measured before this was built, both on the real library (172 indexed files,
// ground truth taken from stem filenames the way TASKS.md Phase 4 does):
//
//   1. The towers really are aligned despite the audio one being a *distilled* student --
//      that was the risk worth checking first, since distillation can drift the space and
//      would have made text-vs-audio comparison meaningless. It didn't.
//   2. The prompt template matters more than anything else. As a bare keyword, "strings"
//      didn't reach a real strings stem until rank 16. Wrapped in the caption-style
//      sentence CLAP was trained on, the first hit for every query tested -- snare drum,
//      singing voice, brass, strings, bass guitar -- landed at rank 1 or 2. So mira wraps
//      the query itself (kPromptTemplate) rather than leaving that to whoever is typing:
//      the difference is too large, and too unguessable, to make it the user's problem.
struct ClapTextResult {
    bool ok = false;
    std::vector<float> vector; // 512-dim, L2-normalized -- same space as DclapEmbedding
    std::string error;         // why it's not ok, for a CLI message rather than a silent empty
};

// What a raw query is wrapped in before tokenizing. See the note above -- this is not
// cosmetic, it is the difference between rank 1 and rank 16.
extern const char* const kPromptTemplate; // "This is a sound of %s."

// `modelPath` is models/similarity-embeddings/dclap/clap_text_model.onnx, and the
// tokenizer tables sit beside it (roberta-vocab.tsv / roberta-merges.txt, written by
// lab/export_roberta_tokenizer.py). Loading is per call: this runs once per search, never
// in the analyze loop, and the text tower is ~500MB on disk -- holding it resident for a
// pipeline that never uses it would be the wrong trade.
ClapTextResult computeTextEmbedding(const std::string& query, const std::string& modelPath,
                                     const std::string& vocabTsvPath, const std::string& mergesPath);

} // namespace mira
