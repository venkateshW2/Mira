#pragma once

#include <onnxruntime_cxx_api.h>

namespace mira {

// One shared, process-lifetime Ort::Env, not a fresh one per call. Real bug found via a
// crash investigation: every ONNX-backed analyzer (Embedding, ContentGate,
// ClassificationHead, StemInstrument) used to construct its own local Ort::Env per
// function call — harmless-looking, but Ort::Env owns process-global state (thread
// pools, logging), and repeatedly constructing/destroying it within one process is an
// ONNX Runtime anti-pattern, not a supported pattern. With up to 8 ONNX calls per file
// (embedding, content gate, 5 classification heads, stem instrument), this reliably
// corrupted memory — intermittently, so it passed most runs and then segfaulted
// mid-memmove somewhere unrelated a few files later. Reproduced with lldb (crash inside
// libsystem_platform.dylib's memmove, corrupted pointer, unwind failed — a classic heap
// corruption signature) before tracing it back to the Env churn rather than anything in
// the newly-added genre/taxonomy code that happened to be the last thing added before it
// surfaced.
Ort::Env& sharedOrtEnv();

} // namespace mira
