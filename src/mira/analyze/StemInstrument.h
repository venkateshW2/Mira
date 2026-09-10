#pragma once

#include <string>
#include <vector>

namespace mira {

// nii-yamagishilab/predominant-instrument-recognition (real-world finding, TASKS.md
// Phase 2) -- NSynth-pretrained, IRMAS-fine-tuned predominant-instrument recognizer,
// built for exactly the isolated/predominant-instrument case mtg_jamendo_instrument
// (Instrument.h) fails on: that head's embedding was trained entirely on full mixes, so
// an isolated stem's acoustic signature (huge dynamic range, long silence, none of a
// mix's spectral density) is out-of-distribution for it. A complementary signal for
// stems, not a replacement -- mira inspect shows both, since they can and do disagree.
//
// Real-file testing before wiring this in: flamenco.wav (a short demo loop) got "voice"
// wrongly; a real 37-minute "STRINGS LOW" delivery stem got "cello" at 0.52 mean
// confidence, correctly and far more decisively than mtg_jamendo_instrument's 0.27/0.24
// violin/cello split on the same file. Kept as a second opinion given that mixed result,
// not trusted as a sole answer -- store the disagreement, same as tempo (PRD §14.1).
struct StemInstrumentResult {
    bool ok = false;
    // 11 entries, order: cel,cla,flu,gac,gel,org,pia,sax,tru,vio,voi (IRMAS's own label
    // order, NOT alphabetical -- see write_metadata_irmas.py's label_dict, and
    // lab/export_irmas_instrument_onnx.py's docstring for the full derivation).
    std::vector<double> scores;
    int windowCount = 0; // number of 1-second windows the scores were mean-pooled from
};

// `mono`/`sampleRate` should already be restricted to active regions (ActiveRegions.h) --
// this doesn't re-detect silence itself, it just slides 1-second non-overlapping windows
// (matching how the model was trained, PRD §12.6-style "match training preprocessing
// exactly") over whatever it's handed. `modelPath` is
// models/classification-heads/irmas-predominant-instrument/irmas-predominant-instrument-1.onnx.
StemInstrumentResult classifyStemInstrument(const std::vector<float>& mono, int sampleRate,
                                             const std::string& modelPath);

std::string toJson(const StemInstrumentResult& r);

} // namespace mira
