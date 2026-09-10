#include "Router.h"

#include <essentia/algorithmfactory.h>
#include <memory>
#include <vector>

namespace mira {

namespace {
constexpr int kSampleRate = 44100;
}

RoutingResult routeContentType(const std::string& path) {
    RoutingResult result;

    // Essentia can throw at algorithm *creation* (MonoLoader validates/opens the file in
    // configure(), not compute()) as well as during compute() — a missing, unreadable,
    // or corrupt file must not take down the whole `analyze` run, which is meant to be
    // resumable across an arbitrarily large batch (PRD §8).
    try {
        auto& factory = essentia::standard::AlgorithmFactory::instance();

        std::unique_ptr<essentia::standard::Algorithm> loader(
            factory.create("MonoLoader", "filename", path, "sampleRate", kSampleRate));

        std::vector<essentia::Real> audio;
        loader->output("audio").set(audio);
        loader->compute();

        if (audio.empty()) return result;

        result.durationSeconds = static_cast<double>(audio.size()) / kSampleRate;

        std::unique_ptr<essentia::standard::Algorithm> onsetRateAlgo(factory.create("OnsetRate"));
        std::vector<essentia::Real> onsetTimes;
        essentia::Real onsetRate = 0;
        onsetRateAlgo->input("signal").set(audio);
        onsetRateAlgo->output("onsets").set(onsetTimes);
        onsetRateAlgo->output("onsetRate").set(onsetRate);
        onsetRateAlgo->compute();

        result.onsetRate = onsetRate;
        result.onsetCount = static_cast<int>(onsetTimes.size());

        if (result.durationSeconds <= kOneShotMaxSeconds) {
            result.contentType = "one_shot";
        } else if (result.durationSeconds <= kLoopMaxSeconds) {
            result.contentType = "loop";
        } else {
            result.contentType = "track";
        }

        result.ok = true;
    } catch (const essentia::EssentiaException&) {
        // ok stays false — caller logs and skips this file.
    }

    return result;
}

} // namespace mira
