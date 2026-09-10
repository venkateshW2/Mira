#include "Key.h"

#include <keyfinder.h>
#include <sstream>
#include <unordered_map>

namespace mira {

namespace {
constexpr double kHarmonicityThreshold = 0.5;

struct KeyInfo {
    const char* name;
    const char* camelot;
    const char* openKey;
};

// Two independent direct lookups from libKeyFinder's key_t (PRD §12b: "sources disagree
// on the exact Camelot<->Open Key numeric offset" — that ambiguity only arises when
// *converting* one system to the other via a formula. Deriving both straight from the 24
// keys via the circle of fifths, as done here, sidesteps it entirely: Camelot's major
// side runs 8B(C)->9B(G)->10B(D)->... in fifths, Open Key's major side runs
// 1d(C)->2d(G)->3d(D)->... in the same order, each relative minor sharing its major's
// number on the 'A'/'m' side. Verify against openkeyscan-analyzer before shipping, per
// the PRD's own caveat.
const std::unordered_map<int, KeyInfo>& keyTable() {
    static const std::unordered_map<int, KeyInfo> table = {
        {KeyFinder::A_MAJOR,       {"A major",       "11B", "4d"}},
        {KeyFinder::A_MINOR,       {"A minor",       "8A",  "1m"}},
        {KeyFinder::B_FLAT_MAJOR,  {"B flat major",  "6B",  "11d"}},
        {KeyFinder::B_FLAT_MINOR,  {"B flat minor",  "3A",  "8m"}},
        {KeyFinder::B_MAJOR,       {"B major",       "1B",  "6d"}},
        {KeyFinder::B_MINOR,       {"B minor",       "10A", "3m"}},
        {KeyFinder::C_MAJOR,       {"C major",       "8B",  "1d"}},
        {KeyFinder::C_MINOR,       {"C minor",       "5A",  "10m"}},
        {KeyFinder::D_FLAT_MAJOR,  {"D flat major",  "3B",  "8d"}},
        {KeyFinder::D_FLAT_MINOR,  {"D flat minor",  "12A", "5m"}},
        {KeyFinder::D_MAJOR,       {"D major",       "10B", "3d"}},
        {KeyFinder::D_MINOR,       {"D minor",       "7A",  "12m"}},
        {KeyFinder::E_FLAT_MAJOR,  {"E flat major",  "5B",  "10d"}},
        {KeyFinder::E_FLAT_MINOR,  {"E flat minor",  "2A",  "7m"}},
        {KeyFinder::E_MAJOR,       {"E major",       "12B", "5d"}},
        {KeyFinder::E_MINOR,       {"E minor",       "9A",  "2m"}},
        {KeyFinder::F_MAJOR,       {"F major",       "7B",  "12d"}},
        {KeyFinder::F_MINOR,       {"F minor",       "4A",  "9m"}},
        {KeyFinder::G_FLAT_MAJOR,  {"G flat major",  "2B",  "7d"}},
        {KeyFinder::G_FLAT_MINOR,  {"G flat minor",  "11A", "4m"}},
        {KeyFinder::G_MAJOR,       {"G major",       "9B",  "2d"}},
        {KeyFinder::G_MINOR,       {"G minor",       "6A",  "11m"}},
        {KeyFinder::A_FLAT_MAJOR,  {"A flat major",  "4B",  "9d"}},
        {KeyFinder::A_FLAT_MINOR,  {"A flat minor",  "1A",  "6m"}},
        {KeyFinder::SILENCE,       {"silence",       "",    ""}},
    };
    return table;
}
} // namespace

KeyResult detectKey(const std::vector<float>& mono, int sampleRate) {
    KeyResult result;
    if (mono.empty() || sampleRate <= 0) return result;

    KeyFinder::AudioData audio;
    audio.setChannels(1);
    audio.setFrameRate(static_cast<unsigned int>(sampleRate));
    audio.addToSampleCount(static_cast<unsigned int>(mono.size()));
    for (size_t i = 0; i < mono.size(); ++i) {
        audio.setSample(static_cast<unsigned int>(i), mono[i]);
    }

    KeyFinder::KeyFinder keyFinder;
    KeyFinder::key_t key = keyFinder.keyOfAudio(audio);

    auto it = keyTable().find(static_cast<int>(key));
    if (it != keyTable().end()) {
        result.key = it->second.name;
        result.camelot = it->second.camelot;
        result.openKey = it->second.openKey;
    }
    return result;
}

bool shouldRunKeyDetection(double harmonicity, int harmonicityFrameCount) {
    return harmonicityFrameCount > 0 && harmonicity >= kHarmonicityThreshold;
}

std::string toJson(const KeyResult& k) {
    std::ostringstream oss;
    oss << "{\"key\":\"" << k.key << "\""
        << ",\"camelot\":\"" << k.camelot << "\""
        << ",\"open_key\":\"" << k.openKey << "\""
        << "}";
    return oss.str();
}

} // namespace mira
