#include "Router.h"
#include "AudioLoader.h"

#include <essentia/algorithmfactory.h>
#include <memory>

namespace mira {

RoutingResult routeContentType(const std::vector<float>& audio, int sampleRate) {
    RoutingResult result;
    result.durationSeconds = static_cast<double>(audio.size()) / sampleRate;

    std::vector<essentia::Real> essentiaAudio(audio.begin(), audio.end());

    // Wrapped defensively — the Phase 0 lesson (spike/README.md) was that Essentia can
    // throw well outside just "file not found"; a single pathologically short/quiet
    // clip must not take down the whole analyze batch.
    try {
        auto& factory = essentia::standard::AlgorithmFactory::instance();
        std::unique_ptr<essentia::standard::Algorithm> onsetRateAlgo(factory.create("OnsetRate"));
        std::vector<essentia::Real> onsetTimes;
        essentia::Real onsetRate = 0;
        onsetRateAlgo->input("signal").set(essentiaAudio);
        onsetRateAlgo->output("onsets").set(onsetTimes);
        onsetRateAlgo->output("onsetRate").set(onsetRate);
        onsetRateAlgo->compute();

        result.onsetRate = onsetRate;
        result.onsetCount = static_cast<int>(onsetTimes.size());
        result.onsetTimes.assign(onsetTimes.begin(), onsetTimes.end());
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
