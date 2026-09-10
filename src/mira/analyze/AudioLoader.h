#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mira {

inline constexpr int kAnalysisSampleRate = 44100;

// Loads `path` as mono audio at kAnalysisSampleRate via Essentia's MonoLoader. Requires
// an EssentiaEngine to already exist (essentia::init() called). Returns nullopt rather
// than throwing if the file can't be opened or decoded — callers (an analyze run over a
// large, imperfect batch) must be able to skip one bad file without dying, the same
// lesson the Router crash fix already applied.
std::optional<std::vector<float>> loadMonoAudio(const std::string& path);

} // namespace mira
