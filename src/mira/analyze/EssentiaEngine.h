#pragma once

namespace mira {

// RAII around essentia::init()/shutdown(). One instance for the process lifetime of
// any command that touches Essentia (analyze today; MIR/neural commands later).
class EssentiaEngine {
public:
    EssentiaEngine();
    ~EssentiaEngine();

    EssentiaEngine(const EssentiaEngine&) = delete;
    EssentiaEngine& operator=(const EssentiaEngine&) = delete;
};

} // namespace mira
