#include "AudioLoader.h"

#include <essentia/algorithmfactory.h>
#include <memory>

namespace mira {

std::optional<LoadedAudio> loadAudio(const std::string& path) {
    try {
        auto& factory = essentia::standard::AlgorithmFactory::instance();
        std::unique_ptr<essentia::standard::Algorithm> loader(
            factory.create("AudioLoader", "filename", path));

        std::vector<essentia::StereoSample> stereo;
        essentia::Real sampleRate = 0;
        int numChannels = 0;
        std::string md5, codec;
        int bitRate = 0;

        loader->output("audio").set(stereo);
        loader->output("sampleRate").set(sampleRate);
        loader->output("numberChannels").set(numChannels);
        loader->output("md5").set(md5);
        loader->output("bit_rate").set(bitRate);
        loader->output("codec").set(codec);
        loader->compute();

        if (stereo.empty() || sampleRate <= 0) return std::nullopt;

        LoadedAudio result;
        result.sampleRate = static_cast<int>(sampleRate);
        result.numChannels = numChannels;
        result.left.reserve(stereo.size());
        result.right.reserve(stereo.size());
        result.mono.reserve(stereo.size());

        for (auto& s : stereo) {
            float l = s.left();
            float r = (numChannels >= 2) ? s.right() : l; // AudioLoader leaves right() unset for mono
            result.left.push_back(l);
            result.right.push_back(r);
            result.mono.push_back(0.5f * (l + r));
        }
        result.durationSeconds = static_cast<double>(stereo.size()) / result.sampleRate;

        return result;
    } catch (const essentia::EssentiaException&) {
        return std::nullopt;
    }
}

} // namespace mira
