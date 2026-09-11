#pragma once

#include <optional>
#include <string>
#include <vector>

#include "../db/Database.h"

namespace mira {

struct ScoredLabel {
    std::string label;
    double score = 0.0;
};

// PRD §11/§15 (Phase 3): "captioning becomes a renderer over the same analysis document
// -- no re-analysis, no changes to Part 1." CaptionFields is that document. It is built
// once per file, by confidence-gated extraction out of `machine`/`human` JSON (PRD
// §12.6: the store keeps everything unfiltered; the *field dict* is where a value below
// threshold gets left out entirely, never asserted). Every renderer (Sa3Renderer, and
// any future one) reads only this struct, never `machine` JSON directly, so every
// renderer sees identical gating decisions and mira never has to teach a second renderer
// the same JSON paths.
struct CaptionFields {
    std::string contentType;    // one_shot | loop | track | stem | unknown -- record.contentType, verbatim
    double durationSeconds = 0.0;

    std::vector<ScoredLabel> genre;        // taxonomy-normalized where available, top-k, gated
    std::vector<ScoredLabel> instruments;  // instrument or stem_instrument, depending on contentType
    std::vector<ScoredLabel> moods;        // moodtheme, top-k, gated

    // Free-form descriptive tags -- "funny", "quirky", "action", "tense drama scene".
    // Unlike genre/instruments/moods, this has NO machine-derived source at all: mira has
    // no analyzer that measures narrative/vibe qualities like this (moodtheme's 56-word
    // MTG-Jamendo vocabulary is the closest thing mira computes, and it's a different,
    // much narrower kind of word). `keywords` exists purely so a person can hand-label
    // what genre/instrument/mood classifiers can't -- it only ever comes from `human`.
    std::vector<std::string> keywords;

    // Omitted (nullopt) rather than asserted when not confidently measured -- see the
    // per-field gating notes in CaptionFields.cpp for exactly what "confident" means for
    // each one.
    std::optional<double> bpm;
    std::optional<std::string> keyScale;   // e.g. "F minor" -- already gated by the analyzer's
                                            // own harmonicity check (Key.cpp), nothing further here
    std::optional<bool> isInstrumental;    // from voice_instrumental.voice_probability, thresholded
};

// Confidence-gate thresholds and top-k caps. Originally first-pass/unmeasured; TASKS.md
// Phase 4 "per-head confidence calibration" ran all four heads across a real 294-file
// library and checked the actual score distributions (and, for voice_instrumental,
// real vocal-vs-instrumental ground truth from filenames) against these numbers. Result:
// no number changed. instrument's raw scores split cleanly (86% of all label/file score
// pairs sit below 0.05, a clear tail above 0.10) and voice_instrumental's 0.5 correctly
// separated 5/6 vocal-named files from 49 non-vocal ones (the one miss was a heavily
// processed vocal *one-shot* at 0.39 -- a genuine hard case, not a threshold problem, and
// moving the threshold either direction traded that miss for new ones on the other side).
// moodtheme's ceiling is inherently low on loop content (max top-1 score seen: 0.28,
// against 0.10's threshold) -- it's a full-song-trained head applied to short
// instrumental loops, a domain mismatch no threshold number fixes, and raising the bar
// would have gutted the field almost entirely (only 7 of 3080 raw scores clear 0.20).
// genre is the one head flagged, not fixed: its score distribution has no clear
// noise/signal gap the way instrument's does, and at least one spot-checked label
// ("Progressive Metal" on a soft electric guitar loop) looked like a real miss -- but
// with no ground-truth genre labels for isolated loops to test against, picking a new
// number here would be guessing, not calibrating. Left as-is and documented rather than
// silently "fixed" with an unjustified value.
constexpr double kCaptionGenreThreshold = 0.10;
constexpr double kCaptionInstrumentThreshold = 0.10;
constexpr double kCaptionMoodThreshold = 0.10;
constexpr double kCaptionVoiceThreshold = 0.5;
constexpr int kCaptionMaxGenre = 3;
constexpr int kCaptionMaxInstruments = 4;
constexpr int kCaptionMaxMood = 3;
constexpr int kCaptionMaxKeywords = 8; // safety cap on human.keywords -- see CaptionFields.cpp

// Reads `record.machine` into a CaptionFields document, then layers `record.human`
// overrides on top (PRD §11: "human field always wins on conflict"). See
// CaptionFields.cpp for the human-override JSON convention this reads -- there is no
// general human-field schema defined elsewhere in the project yet (TASKS.md's
// "folder-level human defaults" is separate, still unimplemented); this is
// CaptionFields' own minimal convention, documented where it's read.
CaptionFields extractCaptionFields(Database& db, const FileRecord& record);

// Same as extractCaptionFields(), but for one time-ranged segment (TASKS.md Phase 3
// addition, see Database.h's SegmentRecord / Database.cpp's `segments` table comment for
// why this exists at all: SA3 has no per-clip timeline conditioning, so a long file or
// synced stem group whose character changes partway through can only be captioned by
// cutting it into per-segment clips). `durationSeconds` becomes the segment's own length;
// `segment.human` overrides are applied on top of the file's own (both machine- and
// human-derived) fields -- see CaptionFields.cpp for exactly what is and isn't
// re-derived per segment today.
CaptionFields extractCaptionFieldsForSegment(Database& db, const FileRecord& record,
                                              const SegmentRecord& segment);

} // namespace mira
