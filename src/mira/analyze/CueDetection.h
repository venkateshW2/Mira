#pragma once

#include <string>
#include <vector>

namespace mira {

// Cue detection across a synced stem set (review round 6 discussion).
//
// A "cue" is one piece of music inside a long score reel -- EP9 is a single 41:27 file per
// stem holding roughly twenty cues end to end. Cues are what a score library is actually
// searched by, and mira's schema already has the right shape for one: a segment scoped to
// a `group_id` covers every stem in the synced set at identical timestamps, and
// `segment_analysis` is keyed on (segment_id, file_id) so the same cue can be analyzed
// separately as heard in each stem.
//
// What was missing is deriving the boundaries. Two facts shaped this:
//
//   * It needs NO AUDIO. Everything here is arithmetic over `files.active_spans`, which
//     analysis already stored. That is why it lives in mira_core and runs in the UI in
//     milliseconds rather than being another CLI child process.
//   * It will never be fully right, so it must be easy to correct. Most scoring is
//     "carpeted" -- wall-to-wall music with no silence between cues -- so no detector can
//     find every boundary. These are proposals a human edits, not answers.
//
// Two independent signals, both free from the stored spans:
//
//   1. SILENCE. Where every stem in the set is quiet, a cue has ended. Works on gapped
//      deliveries; on EP9 it collapsed 506 raw spans into 21 clean regions separated by
//      silences of up to 4:36.
//   2. INSTRUMENTATION CHANGE. A cue change is an instrumentation change even when
//      nothing goes silent -- six stems stop and four others start. Measured on EP9: 38
//      moments where three or more stems changed state at once, several of them *inside*
//      a silence-derived region and invisible to signal 1 (the 2:27 region at 7:34 has
//      internal changes at 8:51, 9:40 and 9:57).
//
// Neither is reliable alone. Silence proposes the outer regions, instrumentation change
// subdivides them.

struct CueStem {
    std::string path;
    std::vector<std::pair<double, double>> activeSpans;
    // The full mix delivered alongside the stems, when there is one. It plays wherever any
    // cue plays, so its own spans are a better region map than the union of the stems --
    // the union can be fooled by a stem whose reverb tail runs past the end of a cue,
    // while the mix simply stops. Used as the primary map when present, with the union
    // kept as confirmation (see CueResult::mixDisagreementSeconds).
    bool isMix = false;
};

struct Cue {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    // Why this cue starts where it does -- carried into the UI so an edit is an informed
    // one rather than moving a line someone can't account for.
    // "silence"       : every stem was quiet before this point
    // "instrumentation": the set of playing stems changed sharply here
    // "reel"          : the start or end of the file itself
    std::string startReason = "reel";
    int stemsPlaying = 0; // how many stems are active anywhere inside this cue
};

struct CueResult {
    std::vector<Cue> cues;
    int silenceBoundaries = 0;
    int instrumentationBoundaries = 0;
    // Seconds the mix says are active but no stem does (or the reverse). Large values mean
    // the set is incomplete or the wrong file was taken for the mix -- worth surfacing
    // rather than silently trusting either side.
    double mixDisagreementSeconds = 0.0;
    bool usedMix = false;
};

struct CueOptions {
    // Gaps shorter than this don't end a cue. Above the 300ms ActiveRegions already
    // bridges, because a reverb tail or a breath between phrases is not a cue boundary.
    double bridgeSeconds = 1.5;
    // No cue shorter than this. The floor exists because raw instrumentation changes are
    // noisy -- EP9 has four separate 3-stem changes inside ten seconds around 20:50, which
    // is one phrase, not four cues.
    //
    // 25s chosen from a parameter sweep on EP9 (41:27, 15 stems), against the user's own
    // estimate of "around 21" cues for that reel:
    //   minStems=3 minCue=15 -> 36 cues, shortest 16s   (over-split; slivers at the floor)
    //   minStems=3 minCue=25 -> 24 cues, shortest 27s, median 54s   <-- chosen
    //   minStems=5 minCue=40 -> 13 cues, longest 264s   (instrumentation signal dead;
    //                                                    a whole 4:24 region left unsplit)
    // The middle setting keeps 8 instrumentation boundaries doing real work while
    // producing nothing under half a minute.
    double minCueSeconds = 25.0;
    // How many stems must start or stop together to count as an instrumentation change.
    int minStemsChanged = 3;
    // Sampling grid for the instrumentation signal. 1s is well below the shortest cue and
    // costs nothing at this scale (2500 samples x 15 stems).
    double gridSeconds = 1.0;
};

// `reelSeconds` is the common length of the synced set (every stem in a set is the same
// length by definition -- that is what makes it a set). Returns cues in time order.
CueResult detectCues(const std::vector<CueStem>& stems, double reelSeconds,
                      const CueOptions& options = {});

} // namespace mira
