#pragma once

#include <string>
#include <vector>

namespace mira {

// Key detection (PRD §12b): deliberately NOT Essentia's KeyExtractor — libKeyFinder
// instead (GPL-3.0, fine under mira's AGPL-3.0), chosen for measured accuracy not
// meaningfully behind Essentia's while needing no model. Gated on harmonic content by
// the caller; a rhythm stem or noise should never reach this (PRD §5, §12b).
struct KeyResult {
    std::string key;      // e.g. "C minor", or "silence"
    std::string camelot;  // e.g. "5A"
    std::string openKey;  // e.g. "10m"
};

KeyResult detectKey(const std::vector<float>& mono, int sampleRate);

// PRD §5/§12b: key is gated on harmonic content, never run blindly — using the real
// harmonicity descriptor (Descriptors.h's PitchYinFFT->HarmonicPeaks->Inharmonicity
// pipeline), not a proxy. `frameCount==0` means no frame was confidently pitched enough
// to measure at all (verified on white noise: harmonicity 0, frameCount 0) — gated out
// on that alone, before even looking at the harmonicity score. Threshold on the score
// itself is a documented first-pass guess, not measured on real material — same caveat
// as the router's and active-region's thresholds.
bool shouldRunKeyDetection(double harmonicity, int harmonicityFrameCount);

std::string toJson(const KeyResult& k);

} // namespace mira
