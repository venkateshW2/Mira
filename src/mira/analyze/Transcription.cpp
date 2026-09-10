#include "Transcription.h"
#include "BasicPitchInternal.h"

#include <essentia/algorithmfactory.h>
#include <memory>
#include <sstream>

namespace mira {

namespace {
// Basic Pitch's ONNX graph expects 22050 Hz (PRD §16.5-style frontend-baked-into-graph
// design) — our AudioLoader preserves the file's native rate, so resample here. Reuses
// Essentia's Resample (libsamplerate-backed) rather than pulling in the Oboe resampler
// basicpitch.cpp's own CLI uses, since Essentia is already linked for everything else.
std::vector<float> resampleTo22050(const std::vector<float>& mono, int sampleRate) {
    if (sampleRate == basic_pitch::constants::SAMPLE_RATE) return mono;

    auto& factory = essentia::standard::AlgorithmFactory::instance();
    std::unique_ptr<essentia::standard::Algorithm> resample(factory.create(
        "Resample", "inputSampleRate", static_cast<essentia::Real>(sampleRate),
        "outputSampleRate", static_cast<essentia::Real>(basic_pitch::constants::SAMPLE_RATE)));

    std::vector<essentia::Real> input(mono.begin(), mono.end());
    std::vector<essentia::Real> output;
    resample->input("signal").set(input);
    resample->output("signal").set(output);
    resample->compute();

    return std::vector<float>(output.begin(), output.end());
}
} // namespace

TranscriptionResult transcribe(const std::vector<float>& mono, int sampleRate,
                                const std::string& modelPath) {
    TranscriptionResult result;
    if (mono.empty() || sampleRate <= 0) return result;

    try {
        auto resampled = resampleTo22050(mono, sampleRate);
        if (resampled.empty()) return result;

        auto inference = basic_pitch::ort_inference(resampled, basic_pitch::constants::SAMPLE_RATE,
                                                      modelPath);
        auto noteEvents = basic_pitch::getNoteEvents(inference);

        int nFrames = static_cast<int>(inference.onsets.dimension(0));
        auto frameTimes = basic_pitch::model_frames_to_time(nFrames);

        for (const auto& note : noteEvents) {
            NoteEvent event;
            event.startSeconds = (note.start_idx < static_cast<int>(frameTimes.size()))
                                      ? frameTimes[note.start_idx] : 0.0;
            event.endSeconds = (note.end_idx < static_cast<int>(frameTimes.size()))
                                    ? frameTimes[note.end_idx] : event.startSeconds;
            event.midiPitch = note.pitch;
            event.amplitude = note.amplitude;
            event.pitchBends = note.pitch_bends;
            result.notes.push_back(std::move(event));
        }

        result.ok = true;
    } catch (const std::exception&) {
        // ok stays false — caller skips this file's transcription, same discipline as
        // every other analyzer here (Router, ActiveRegions, Key, Chords).
    }

    return result;
}

std::string toJson(const TranscriptionResult& t) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < t.notes.size(); ++i) {
        if (i > 0) oss << ",";
        const auto& n = t.notes[i];
        oss << "{\"start\":" << n.startSeconds << ",\"end\":" << n.endSeconds
            << ",\"pitch\":" << n.midiPitch << ",\"amplitude\":" << n.amplitude << "}";
    }
    oss << "]";
    return oss.str();
}

} // namespace mira
