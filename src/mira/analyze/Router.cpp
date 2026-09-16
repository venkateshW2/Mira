#include "Router.h"
#include "AudioLoader.h"
#include "Onsets.h"

#include <essentia/algorithmfactory.h>
#include <memory>

namespace mira {

RoutingResult routeContentType(const std::vector<float>& audio, int sampleRate) {
    RoutingResult result;
    result.durationSeconds = static_cast<double>(audio.size()) / sampleRate;

    // Onsets come from mira's own spectral flux (Onsets.h), NOT from Essentia's
    // OnsetRate, which used to run here. That is not a performance change, it is a
    // correctness one: measured against six tracks whose tempo the user knows, the
    // OnsetRate onsets phase-locked to the true tempo at R = 0.001-0.024 -- zero -- and
    // instead peaked near 159 BPM on five unrelated songs. Onsets.h explains it at
    // length. Everything downstream (groove, swing, pocket, the fitted grid) was
    // measuring that artefact.
    //
    // Wrapped defensively — the Phase 0 lesson (spike/README.md) was that Essentia can
    // throw well outside just "file not found"; a single pathologically short/quiet
    // clip must not take down the whole analyze batch.
    try {
        auto onsets = detectOnsets(audio, sampleRate);
        if (onsets.ok) {
            result.onsetRate = onsets.rate;
            result.onsetCount = static_cast<int>(onsets.times.size());
            result.onsetTimes = std::move(onsets.times);
        }
        // Not ok: onsetRate/onsetCount stay at 0 — never a substituted value (CLAUDE.md
        // §6), and duration-based routing below still applies.
    } catch (const essentia::EssentiaException&) {
        // onsetRate/onsetCount stay at 0 — duration-based routing below still applies.
    }

    if (result.durationSeconds <= kOneShotMaxSeconds) {
        result.contentType = "one_shot";
    } else if (result.durationSeconds <= kLoopMaxSeconds) {
        result.contentType = "loop";
    } else {
        result.contentType = "track";
    }

    return result;
}

} // namespace mira
