#pragma once

#include <string>
#include <vector>

namespace mira {

// Rhythm/MIR (PRD §5B, §14.1). beat_this_cpp (neural beat/downbeat tracker) is the
// primary and default tempo estimator — measured more accurate (it's the only one that
// gives downbeats, and handles syncopated/complex material better) and, on a real 5:08
// song, ~3.4x cheaper to skip than to run relative to the alternative (9.2s vs 2.7s).
// Essentia's RhythmExtractor2013 is now an opt-in recheck: pass runRecheck=true to also
// run it (plus BeatsLoudness at its beats) purely for comparison against beat_this_cpp —
// bpmRatio is that comparison signal, not resolved by picking a winner.
// Caller is responsible for gating this to loops/tracks/stems, not one-shots (§5).
struct RhythmResult {
    bool ok = false;

    // beat_this_cpp — the default, primary tempo/beat/downbeat estimator
    double beatThisBpm = 0.0;              // MEDIAN beat interval (Mir.cpp says why, at
                                           // length); 0 if <2 beats found
    std::vector<double> beatThisBeats;      // seconds
    std::vector<double> beatThisDownbeats;  // seconds

    // How even the beat spacing is -- the share of beat intervals within 25% of the
    // median (see gridStability in Mir.cpp, and the six ground-truth tracks tabulated
    // there). 1.0 is a perfectly regular grid; below ~0.90 the tracker changed pulse
    // level partway through the file and NOTHING derived from the grid should be
    // believed, tempo included. This is the confidence signal whose absence let a
    // confidently-wrong tempo stand for a week. 0 means unmeasured (<3 beats), which is
    // not the same as "unstable" -- check beatThisBeats.size() before reading it.
    double beatGridStability = 0.0;

    // Which postprocessor's grid the fields above actually came from: "dbn" (the
    // default) or "minimal" (the peak-picker, chosen because the DBN's grid came out
    // uneven and this one was steadier). Empty when beat tracking did not run.
    std::string beatGridSource;
    // Both stabilities, kept so the choice is auditable rather than asserted.
    // beatGridStabilityMinimal is -1 when the cross-check never ran, which is the common
    // case -- the DBN grid was steady and the minimal grid was never decoded.
    double beatGridStabilityDbn = 0.0;
    double beatGridStabilityMinimal = -1.0;

    // The two cruder tempo numbers beatThisBpm is derived from, kept so the derivation
    // can be checked rather than believed:
    //   beatThisMedianBpm   60 / median beat interval. What the collaborator's tool
    //                       reports, and what mira reported until the fit landed. Cannot
    //                       express a tempo between two 50 fps bins -- 87 BPM is one.
    //   beatFittedBpmRaw    the least-squares fit, before any octave choice. Right up to
    //                       a factor of two; beatThisBpm is this folded by a prior.
    //   beatFitUsedFraction share of beats that actually landed on the fitted grid. Near
    //                       1.0 means the fit describes the file; well under means the
    //                       grid is not one grid, whatever tempo is printed.
    double beatThisMedianBpm = 0.0;
    double beatFittedBpmRaw = 0.0;
    double beatFitUsedFraction = 0.0;

    // Tempo stability — a single whole-file BPM (beatThisBpm above) is a defensible
    // average but a bad *summary* for through-composed material with real tempo changes
    // (a long score mix going 60 BPM -> 175 BPM is not "~105 BPM"). Computed from
    // beatThisBeats by taking local BPM in fixed windows and measuring how much those
    // windows disagree with each other — the same "store the disagreement, don't resolve
    // it" spirit as bpmRatio above, just within one estimator instead of across two.
    double tempoStabilityBpmStddev = 0.0; // stddev of per-window local BPM; 0 if unmeasured
    double tempoRangeBpm = 0.0;           // max window BPM - min window BPM; 0 if unmeasured
    int tempoWindowCount = 0;             // number of windows with a measurable local BPM;
                                           // stays 0 below 3 windows (~3 min of material) —
                                           // 0 means "not enough beats/duration to judge
                                           // stability," not "perfectly stable"; check this
                                           // before trusting stddev==0
    // First-pass, unmeasured threshold (PRD §12.6: "store everything, tune thresholds
    // later") — stddev > kTempoUnstableStddevBpm (Mir.cpp). True means "don't treat
    // beatThisBpm as representing the whole file," not "the estimate is wrong."
    bool tempoUnstable = false;

    // RhythmExtractor2013 (multifeature) — only populated when runRecheck=true
    double essentiaBpm = 0.0;
    double essentiaConfidence = 0.0;
    std::vector<double> essentiaBeatTicks; // seconds — the full array, not just the BPM scalar (§6)
    double beatsLoudnessMean = 0.0;        // only populated when runRecheck=true (needs essentia's beats)

    // essentiaBpm / beatThisBpm — near 1.0 is agreement; near 2.0 or 0.5 is the classic
    // octave (double/half-time) error PRD §10's risk table flags. Only meaningful when
    // runRecheck=true (stays 0 otherwise). A first-pass raw signal, not a resolved
    // verdict — rendering/thresholding is a later phase's job (§12.6: "store everything,
    // tune thresholds later").
    double bpmRatio = 0.0;

    double danceability = 0.0;
};

// Requires an EssentiaEngine already constructed. `beatThisModelPath` is
// vendor/beat_this_cpp/onnx/beat_this.onnx — not vendored into mira's own repo (PRD §7:
// "79 MB ONNX committed in-tree" refers to beat_this_cpp's own repo, which
// scripts/fetch-vendor.sh clones). `runRecheck` opts into also running Essentia's
// RhythmExtractor2013 for comparison (off by default — it's the slower, less accurate
// estimator of the two; see struct comment above).
RhythmResult analyzeRhythm(const std::vector<float>& mono, int sampleRate,
                            const std::string& beatThisModelPath, bool runRecheck = false);

std::string toJson(const RhythmResult& r);

} // namespace mira
