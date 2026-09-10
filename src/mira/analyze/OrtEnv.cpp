#include "OrtEnv.h"

namespace mira {

Ort::Env& sharedOrtEnv() {
    // Function-local static: thread-safe initialization (C++11 [stmt.dcl]/4), constructed
    // once on first use, destroyed once at process exit — never mid-run.
    static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "mira");
    return env;
}

} // namespace mira
