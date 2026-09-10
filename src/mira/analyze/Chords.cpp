#include "Chords.h"

// _VAMP_PLUGIN_IN_HOST_NAMESPACE is defined for this whole translation unit (and for
// nnls-chroma itself) via nnls_chroma's PUBLIC compile definition in CMakeLists.txt —
// see the comment there for why it has to be consistent across every TU that touches
// these Vamp types, not just this one.
#include <vamp-hostsdk/PluginBufferingAdapter.h>
#include <vamp-hostsdk/PluginInputDomainAdapter.h>

#include "Chordino.h"

#include <algorithm>
#include <memory>
#include <sstream>

using namespace Vamp;
using namespace Vamp::HostExt;

namespace mira {

ChordResult detectChords(const std::vector<float>& mono, int sampleRate) {
    ChordResult result;
    if (mono.empty() || sampleRate <= 0) return result;

    // Each Vamp wrapper layer's destructor deletes the plugin it wraps (PluginWrapper::
    // ~PluginWrapper does `delete m_plugin`), so only the outermost `adapter` is an
    // owning pointer — wrapping the inner layers in their own unique_ptrs too would
    // double-free them. Raw `new` here matches nnls-chroma's own chordextract.cpp.
    Chordino* chordino = new Chordino(static_cast<float>(sampleRate));
    PluginInputDomainAdapter* inputDomainAdapter = new PluginInputDomainAdapter(chordino);
    inputDomainAdapter->setProcessTimestampMethod(PluginInputDomainAdapter::ShiftData);
    auto adapter = std::make_unique<PluginBufferingAdapter>(inputDomainAdapter);

    int blockSize = adapter->getPreferredBlockSize();
    if (blockSize <= 0) return result;

    // Chordino requires 1 channel; `mono` already is.
    if (!adapter->initialise(1, blockSize, blockSize)) return result;

    int chordOutputIndex = -1;
    Plugin::OutputList outputs = adapter->getOutputDescriptors();
    for (size_t i = 0; i < outputs.size(); ++i) {
        if (outputs[i].identifier == "simplechord") {
            chordOutputIndex = static_cast<int>(i);
            break;
        }
    }
    if (chordOutputIndex < 0) return result;

    std::vector<float> block(blockSize);
    const float* blockPtr = block.data();

    size_t frame = 0;
    while (frame < mono.size()) {
        size_t count = std::min(static_cast<size_t>(blockSize), mono.size() - frame);
        std::copy(mono.begin() + frame, mono.begin() + frame + count, block.begin());
        if (count < static_cast<size_t>(blockSize)) {
            std::fill(block.begin() + count, block.end(), 0.0f);
        }

        RealTime timestamp = RealTime::frame2RealTime(static_cast<long>(frame), sampleRate);
        Plugin::FeatureSet fs = adapter->process(&blockPtr, timestamp);
        for (const auto& feature : fs[chordOutputIndex]) {
            result.chords.push_back({feature.timestamp.sec + feature.timestamp.nsec / 1e9,
                                      feature.label});
        }

        frame += count;
    }

    // Chordino does the bulk of its work here, per nnls-chroma's own chordextract.cpp.
    Plugin::FeatureSet fs = adapter->getRemainingFeatures();
    for (const auto& feature : fs[chordOutputIndex]) {
        result.chords.push_back({feature.timestamp.sec + feature.timestamp.nsec / 1e9,
                                  feature.label});
    }

    result.ok = true;
    return result;
}

std::string toJson(const ChordResult& c) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < c.chords.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"t\":" << c.chords[i].timeSeconds << ",\"chord\":\"" << c.chords[i].label << "\"}";
    }
    oss << "]";
    return oss.str();
}

} // namespace mira
