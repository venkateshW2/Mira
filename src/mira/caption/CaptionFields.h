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

    // Shape fields (sa3-studio/PACKAGING.md's caption work, 2026-09-13). Three worded
    // buckets over DSP mira already measures but never captioned. They exist because the
    // existing fields are all *song* descriptors -- genre, mood, instruments, BPM -- and
    // none of them separates score-like music from song-like music, which is what a film
    // score LoRA is actually trained to learn.
    //
    // Measured on this library's 171 analysed track+stem files before being picked (see
    // the threshold block below). Deliberately NOT included: harmonicity (Dune 0.939 vs
    // other tracks 0.944 -- no separation at all, so the tag would be pure prompt noise),
    // spectral_centroid (r=0.91 with spectral_flatness, same axis twice) and crest_factor
    // (r=0.44 with loudness_range -- partly redundant with `dynamics`). Fewer fields that
    // genuinely discriminate beat more fields that correlate; that is the same reasoning
    // the per-head thresholds above use, applied to DSP.
    std::optional<std::string> rhythm;    // sparse | moderate | driving      (onset_rate)
    std::optional<std::string> dynamics;  // wide | moderate | compressed     (loudness_range_lu)
    std::optional<std::string> texture;   // tonal | mixed | noisy            (spectral_flatness)
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
// Raised from 4 in review round 4: "the caption should take all the instruments,
// anything and everything detected". A full mix genuinely has many instruments at once,
// and the old cap is what made a whole arrangement caption as just "voice stem". Still a
// cap rather than unbounded -- Sa3Renderer's 45-word budget has to fit a sentence too.
constexpr int kCaptionMaxInstruments = 8;
constexpr int kCaptionMaxMood = 3;
constexpr int kCaptionMaxKeywords = 8; // safety cap on human.keywords -- see CaptionFields.cpp

// Shape-field bucket boundaries, calibrated the same way Phase 4 calibrated the heads:
// by measuring, not by guessing. Tertiles over the 171 analysed track+stem files in a
// real library, then rounded to interpretable numbers and re-checked for balance --
// onset_rate landed 61/56/54 across the three buckets, near-perfectly even.
//
// The check that matters is that a tag VARIES WITHIN a corpus, not just between corpora:
// a field that reads the same on every training file teaches the model nothing (that is
// what makes "epic" useless on a set where everything is epic). onset_rate spans
// 0.1-3.04 across the 38 Dune tracks alone -- a 30x spread, landing 20 sparse /
// 15 moderate / 3 driving -- so it is real signal a LoRA can attach to a word.
//
// Measured tertiles: onset p33=0.75 p66=1.93 | lra p33=3.6 p66=13.2 | flat p33=0.031
// p66=0.062. These are first-pass numbers from ONE library, unlike the head thresholds
// which were checked against ground truth -- they will need re-checking on a second
// score corpus (Mad Max is the intended test: it should sit far above Dune's 0.95 mean
// onset_rate, and if it does not, this field is not measuring what it claims to).
constexpr double kCaptionRhythmSparseMax = 0.8;      // onset_rate
constexpr double kCaptionRhythmDrivingMin = 2.0;
constexpr double kCaptionDynamicsCompressedMax = 4.0; // loudness_range_lu
constexpr double kCaptionDynamicsWideMin = 13.0;
constexpr double kCaptionTextureTonalMax = 0.03;      // spectral_flatness
constexpr double kCaptionTextureNoisyMin = 0.06;

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
