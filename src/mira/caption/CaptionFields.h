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

    // Two more shape fields (CAPTION-TAGGING.md, 2026-09-14), added for the same reason
    // the three above were: the existing fields describe a *sound* but not the two things
    // that most separate one score corpus from another.
    //
    // `palette` is what the music is MADE OF. Measured across five film scores it is the
    // single strongest discriminator in the library: Lord of the Rings is 56/56 acoustic
    // (mean electronic share 0.047) while Dune is 0.389. genre and instruments already
    // carry this implicitly, spread across a list; this states it as one word a prompt
    // can set.
    //
    // `timing` is HOW TIGHT the pulse is -- the rubato/quantised axis. It is the field
    // that makes beat-based material trainable next to orchestral score: an orchestral
    // cue reads `loose`, a quantised beat reads `tight`, and a producer who plays behind
    // the grid reads `human`. Correlation with onset_rate (the existing `rhythm` field)
    // is only -0.19 over 317 files, so it is a genuinely separate axis and not `rhythm`
    // measured twice.
    //
    // Deliberately NOT included, for the same "fewer fields that discriminate" reason the
    // block above rejected spectral_centroid and crest_factor:
    //   - tempo drift (|mean(first half) - mean(second half)| / mean of the inter-beat
    //     intervals): r=0.79 with the jitter that `timing` already uses. Same axis twice.
    //   - swing (alternating inter-beat ratio): every decile in this library sits between
    //     1.000 and 1.023, i.e. there is no swung material here to calibrate a threshold
    //     against, and the tail is corrupt (max 128.07 from failed beat tracking). The
    //     measurement is implemented and sound; the corpus cannot yet say where the line
    //     goes. Revisit when a real beat corpus lands -- and note that `beat_this_beats`
    //     holds BEATS, so 8th/16th-note swing is invisible to it regardless.
    std::optional<std::string> palette;   // acoustic | hybrid | electronic   (instrument head)
    std::optional<std::string> timing;    // tight | human | loose            (beat-tick jitter)

    // Groove fields (2026-09-15), measured over the 94-file Amon Tobin / Two Fingers
    // corpus -- the first beat-driven material in this library, and the corpus this
    // header's own `swing` note was waiting for ("Revisit when a real beat corpus
    // lands"). That note also explains why these could not be built earlier: they are
    // derived from `$.onset_times` via mira::analyzeGroove, not from `beat_this_beats`,
    // which holds BEATS and so cannot see an 8th-note swing at all.
    //
    // Both are omitted on a file whose onsets never lock to a grid (Groove.h's
    // kGrooveMinGridStrength). That omission is the point: the flat-histogram bug these
    // replaced reported swing 0.54-0.57 for EVERY file in the library, orchestral cues
    // included, which is what hid it for so long. 18 of the 94 are omitted today, and
    // they are the ambient/sound-design pieces.
    //
    // Distributions over the 94 (the tertiles the thresholds below come from):
    //   grid strength  min 1.06  p33 1.73  median 2.07  p66 2.67  max 7.45
    //   swing          min 41.9% p33 50.9% median 53.2% p66 55.2% max 65.2%
    //
    // Deliberately NOT included, on the same "fewer fields that discriminate" rule the
    // blocks above use:
    //   - pocket (mean signed offset from the nearest 16th): the middle two thirds of
    //     the corpus spans -2.4 to +2.0 ms, while the onset detector quantises at
    //     ~11.6 ms (Essentia OnsetRate, hop 512 @ 44.1 kHz). Two thirds of the files sit
    //     inside ONE quantisation step, so the field would be measuring rounding, not
    //     feel. Only the +/-30 ms tails are real. Revisit if onsets ever get finer.
    //   - syncopation: r=-0.46 with `groove`, i.e. partly the same axis, and its middle
    //     band is narrow (62-71%). Worth adding only if `groove` alone turns out not to
    //     respond at generation time.
    std::optional<std::string> groove;    // organic | steady | programmed    (onset grid strength)
    std::optional<std::string> swing;     // straight | light swing | swung   (off-beat 8th position)

    // Two sound-design fields over DSP added for this corpus (Descriptors.cpp's
    // sub_ratio / flux_mean). Same selection rule as everything above -- measured first,
    // then kept or dropped on whether they split the corpus and whether they duplicate an
    // axis already captioned:
    //
    //   low_end   sub_ratio (20-80 Hz share of spectral energy)
    //             min 0.006  p33 0.202  median 0.256  p66 0.335  max 0.588
    //             r=+0.12 with spectral_flatness (`texture`), -0.28 with crest_factor,
    //             +0.31 with flux_mean. Correlates with nothing already captioned, which
    //             is exactly what a new field has to prove.
    //
    //   motion    flux_mean (mean frame-to-frame spectral change) -- how much the timbre
    //             itself moves, which is the closest single number to what "sound design"
    //             means on this material.
    //             min 0.019  p33 0.141  median 0.167  p66 0.198  max 0.352
    //             r=+0.42 with spectral_flatness, -0.55 with crest_factor. The highest
    //             correlation of any field kept here, and the reason it is one field
    //             rather than two: flux_stddev is r=+0.71 with flux_mean, the same axis
    //             measured twice, so it is stored by the analyzer but never captioned.
    std::optional<std::string> lowEnd;    // light | balanced | heavy         (sub_ratio)
    std::optional<std::string> motion;    // static | shifting | morphing     (flux_mean)
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

// `palette` and `timing` boundaries, calibrated the same way: tertiles over every
// analysed file in a real library (341 with usable instrument scores, 317 with usable
// beat tracks), then rounded to interpretable numbers and re-checked for balance.
//
//   palette 0.15 / 0.35 -> 108 acoustic / 114 hybrid / 119 electronic
//   timing  0.08 / 0.20 -> 103 tight    / 108 human  / 106 loose
//
// Both land near-perfectly even, and palette sorts the five known corpora the way a
// listener would: LOTR 0.047 acoustic, BvS 0.166 / DarkKnight 0.226 / MadMax 0.266
// hybrid, Dune 0.389 electronic.
//
// Measured p33/p66 before rounding: palette 0.155/0.363, timing 0.084/0.204.
constexpr double kCaptionPaletteAcousticMax = 0.15;   // electronic share of instrument mass
constexpr double kCaptionPaletteElectronicMin = 0.35;
constexpr double kCaptionTimingTightMax = 0.08;       // stdev(inter-beat) / mean(inter-beat)
constexpr double kCaptionTimingLooseMin = 0.20;

// Above this, the beat track is not rubato -- it is broken. Measured max in this library
// is 11.67, which is not a tempo, it is a failed detection; dropping those is what makes
// the tertiles above meaningful. `timing` is omitted rather than guessed for such a file,
// the same rule bpm and keyScale follow.
constexpr double kCaptionTimingJitterSane = 2.0;
// A beat track shorter than this cannot support a jitter estimate worth captioning.
constexpr int kCaptionTimingMinBeats = 16;
// Instrument mass below this means the head found essentially nothing -- palette would be
// a ratio of noise to noise, so it is omitted instead.
constexpr double kCaptionPaletteMinMass = 0.05;

// Groove and sound-design tertiles, measured on the 94-file Amon Tobin / Two Fingers
// corpus (see the field comments above for the full distributions). Rounded off the
// measured p33/p66 rather than picked: groove 1.73/2.67, swing 0.509/0.552,
// low_end 0.202/0.335, motion 0.141/0.198.
constexpr double kCaptionGrooveOrganicMax = 1.75;      // onset phase histogram peak/uniform
constexpr double kCaptionGrooveProgrammedMin = 2.65;
constexpr double kCaptionSwingStraightMax = 0.51;      // position of the off-beat 8th in the beat
constexpr double kCaptionSwingSwungMin = 0.55;
constexpr double kCaptionLowEndLightMax = 0.20;        // 20-80 Hz share of spectral energy
constexpr double kCaptionLowEndHeavyMin = 0.34;
constexpr double kCaptionMotionStaticMax = 0.14;       // mean frame-to-frame spectral flux
constexpr double kCaptionMotionMorphingMin = 0.20;

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
