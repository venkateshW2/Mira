#include "EssentiaEngine.h"

#include <essentia/algorithmfactory.h>

namespace mira {

EssentiaEngine::EssentiaEngine() { essentia::init(); }
EssentiaEngine::~EssentiaEngine() { essentia::shutdown(); }

} // namespace mira
