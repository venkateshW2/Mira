#include "AudioLoader.h"

#include <essentia/algorithmfactory.h>
#include <memory>

namespace mira {

std::optional<std::vector<float>> loadMonoAudio(const std::string& path) {
    try {
        auto& factory = essentia::standard::AlgorithmFactory::instance();
        std::unique_ptr<essentia::standard::Algorithm> loader(
            factory.create("MonoLoader", "filename", path, "sampleRate", kAnalysisSampleRate));

        std::vector<essentia::Real> audio;
        loader->output("audio").set(audio);
        loader->compute();

        if (audio.empty()) return std::nullopt;
        return std::vector<float>(audio.begin(), audio.end());
    } catch (const essentia::EssentiaException&) {
        return std::nullopt;
    }
}

} // namespace mira
