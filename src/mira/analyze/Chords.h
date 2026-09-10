#pragma once

#include <string>
#include <vector>

namespace mira {

struct ChordSegment {
    double timeSeconds = 0.0;
    std::string label;
};

struct ChordResult {
    bool ok = false;
    std::vector<ChordSegment> chords;
};

// Chordino/NNLS-Chroma (PRD §12b) — Mauch & Dixon, GPL-2.0-or-later, fine under mira's
// AGPL-3.0. Not Essentia's job (PRD §7: its own docs flag the NNLS-Chroma path as
// GPL-encumbered even for paying commercial licensees). Gated on harmonic content by the
// caller, same proxy and caveat as key detection (Key.h) — never run blindly (PRD §5).
//
// Chordino expects FrequencyDomain input (getInputDomain() returns FrequencyDomain, not
// TimeDomain), so this goes through vamp-hostsdk's PluginInputDomainAdapter to get the
// FFT framing exactly right, plus PluginBufferingAdapter for block-size negotiation —
// the same pattern nnls-chroma's own chordextract.cpp example uses, adapted from reading
// a file via libsndfile to an in-memory buffer already decoded by AudioLoader.
ChordResult detectChords(const std::vector<float>& mono, int sampleRate);

std::string toJson(const ChordResult& c);

} // namespace mira
