#pragma once

#include <optional>
#include <string>
#include <vector>

namespace mira {

struct NoteEvent {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    int midiPitch = 0;
    float amplitude = 0.0f;
    std::optional<std::vector<int>> pitchBends;
};

struct TranscriptionResult {
    bool ok = false;
    std::vector<NoteEvent> notes;
};

// Basic Pitch (PRD §5, §12b) — Spotify, Apache-2.0. "A first-class feature, not a MIR
// afterthought": audio-to-MIDI on stems and loops is a real deliverable in its own
// right, not only an internal signal. `nmp.onnx` bakes the CQT frontend into the graph,
// so this needs only ONNX Runtime — no separate DSP frontend to get right, unlike
// beat_this_cpp's hand-written mel frontend.
//
// The inference (ort_inference) and note-decoding (peak-picking, the "melodia trick" for
// monophonic melody continuation, Gaussian-windowed pitch-bend estimation) are adapted
// from github.com/sevagh/basicpitch.cpp (MIT), which already implements Basic Pitch's
// published post-processing algorithm in C++ — reimplementing that decoding logic from
// scratch here would risk getting the thresholding/melodia-trick details subtly wrong
// with no easy way to verify against ground truth. Trimmed to note-event output only;
// the upstream project's MIDI-file serialization (which needs libremidi) is not used —
// mira stores note events directly in `machine`, not MIDI files.
TranscriptionResult transcribe(const std::vector<float>& mono, int sampleRate,
                                const std::string& modelPath);

std::string toJson(const TranscriptionResult& t);

} // namespace mira
