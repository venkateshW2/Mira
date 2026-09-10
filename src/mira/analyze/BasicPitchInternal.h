#pragma once

// Internal types for Basic Pitch inference/decoding, adapted from
// github.com/sevagh/basicpitch.cpp's basicpitch.hpp (MIT) — see Transcription.h (mira's
// public API) for the full attribution and rationale. Not part of mira's public
// analyze API.

#include <Eigen/Dense>
#include <cmath>
#include <optional>
#include <string>
#include <tuple>
#include <unsupported/Eigen/CXX11/Tensor>
#include <vector>

namespace Eigen {
typedef Tensor<float, 3> Tensor3dXf;
typedef Tensor<float, 3, Eigen::RowMajor> Tensor3dRowMajorXf;
typedef Tensor<float, 2> Tensor2dXf;
typedef Tensor<float, 1> Tensor1dXf;
typedef Matrix<float, Dynamic, Dynamic> MatrixXf;
} // namespace Eigen

namespace basic_pitch {
namespace constants {
const int SAMPLE_RATE = 22050;
const int AUDIO_SAMPLE_RATE = SAMPLE_RATE;
const int FFT_HOP = 256;
constexpr int ANNOTATIONS_FPS = SAMPLE_RATE / FFT_HOP;
const float ONSET_THRESHOLD = 0.5f;
const float FRAME_THRESHOLD = 0.3f;
const float ANNOTATIONS_BASE_FREQUENCY = 27.5f; // lowest key on a piano
const float MAGIC_NUMBER = 0.0018f;
const int CONTOURS_BINS_PER_SEMITONE = 3;
const int AUDIO_WINDOW_LENGTH = 2;
const int MIN_NOTE_LEN = 11;
const int MIDI_OFFSET = 21;
const int MAX_FREQ_IDX = 87;
const int ENERGY_TOL = 11;
constexpr float ANNOT_N_FRAMES = ANNOTATIONS_FPS * AUDIO_WINDOW_LENGTH;
constexpr float AUDIO_N_SAMPLES = SAMPLE_RATE * AUDIO_WINDOW_LENGTH - FFT_HOP;
} // namespace constants

struct InferenceResult {
    Eigen::Tensor2dXf notes;
    Eigen::Tensor2dXf onsets;
    Eigen::Tensor2dXf contours;
};

struct NoteEvent {
    int start_idx;
    int end_idx;
    int pitch;
    float amplitude;
    std::optional<std::vector<int>> pitch_bends;

    bool operator<(const NoteEvent& other) const {
        return std::tie(start_idx, end_idx, pitch, amplitude, pitch_bends) <
               std::tie(other.start_idx, other.end_idx, other.pitch, other.amplitude,
                        other.pitch_bends);
    }
};

// BasicPitchOrt.cpp — loads modelPath fresh each call (simpler than upstream's
// WASM-oriented embedded-header approach; mira has a real filesystem and doesn't need to
// bake the model into the binary).
InferenceResult ort_inference(const std::vector<float>& mono_audio, int sampleRate,
                               const std::string& modelPath);

// BasicPitchNotes.cpp
std::vector<float> model_frames_to_time(int n_frames);
std::vector<NoteEvent> getNoteEvents(const InferenceResult& inference_result,
                                      bool use_melodia_trick = true,
                                      bool include_pitch_bends = true);

} // namespace basic_pitch
