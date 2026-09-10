// CED-small content gate (PRD §2c). Unlike every other neural stage in mira, CED-small's
// feature frontend is kaldi-style fbank, not Essentia's TensorflowInputMusiCNN — so this
// vendors kaldi-native-fbank (Apache-2.0, csukuangfj/kaldi-native-fbank, the exact library
// sherpa-onnx itself uses) rather than trying to hand-replicate it in Essentia. The exact
// FbankOptions below are read directly from sherpa-onnx's own
// sherpa-onnx/csrc/offline-stream.cc (CEDTag constructor) — not guessed.

#include "ContentGate.h"
#include "AudioSetLabels.h"

#include <essentia/algorithmfactory.h>
#include <kaldi-native-fbank/csrc/online-feature.h>
#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <memory>
#include <sstream>

namespace mira {

namespace {
constexpr int kSampleRate = 16000;
constexpr int kFeatDim = 64;
constexpr int kMusicClassIndex = 137; // AudioSet ontology index of "Music" (AudioSetLabels.h)
// First-pass threshold (PRD §12.6: "store everything, tune thresholds later") — set from
// real measurements, not a labeled dataset: flamenco.wav (real music, max-pooled across
// its two chunks) scores 0.52; synthesized white noise (smoke_test.sh's standard
// non-music fixture, single chunk) scores 0.39 — CED-small is genuinely uncertain on
// pure digital noise (its own "White noise" class scores 0.20, a real second-place
// contender, not a wiring bug), not something a hard 0.5 cutoff would misread as
// confidently musical. 0.45 sits between the two measured points; revisit with a wider
// set of real files before trusting it further. musicScore is stored regardless, so a
// caller can re-threshold without re-running inference.
constexpr double kContentGateMusicThreshold = 0.45;

std::vector<float> resampleTo16k(const std::vector<float>& mono, int sampleRate) {
    if (sampleRate == kSampleRate) return mono;

    auto& factory = essentia::standard::AlgorithmFactory::instance();
    std::unique_ptr<essentia::standard::Algorithm> resample(factory.create(
        "Resample", "inputSampleRate", static_cast<essentia::Real>(sampleRate),
        "outputSampleRate", static_cast<essentia::Real>(kSampleRate)));

    std::vector<essentia::Real> input(mono.begin(), mono.end());
    std::vector<essentia::Real> output;
    resample->input("signal").set(input);
    resample->output("signal").set(output);
    resample->compute();

    return std::vector<float>(output.begin(), output.end());
}

knf::FbankOptions cedFbankOptions() {
    // Verbatim from sherpa-onnx/csrc/offline-stream.cc's CEDTag constructor —
    // https://github.com/RicherMans/CED/blob/main/onnx_inference_with_kaldi.py.
    knf::FbankOptions opts;
    opts.frame_opts.frame_length_ms = 32;
    opts.frame_opts.dither = 0;
    opts.frame_opts.preemph_coeff = 0;
    opts.frame_opts.remove_dc_offset = false;
    opts.frame_opts.window_type = "hann";
    opts.frame_opts.snip_edges = false;
    opts.frame_opts.samp_freq = static_cast<float>(kSampleRate);
    opts.mel_opts.num_bins = kFeatDim;
    opts.mel_opts.low_freq = 0;
    opts.mel_opts.high_freq = 8000;
    opts.use_log_fbank = false;
    return opts;
}

// CED-small breaks internally on long inputs — a real 2:06 file threw an ONNX Runtime
// broadcast error ("axis == 1 || axis == largest was false... 187 by 787") that a 14.2s
// file and a 5s clip didn't. sherpa-onnx's own CLI doesn't chunk (github.com/k2-fsa/
// sherpa-onnx/csrc/sherpa-onnx-offline-audio-tagging.cc feeds the whole file at once),
// but its own test_wavs are all short — this limit likely never got exercised upstream.
// Fixed the same way Embedding.cpp already handles long audio: chunk and pool, not feed
// unbounded length into a model whose internals weren't verified past its own test
// fixtures' length.
constexpr double kChunkSeconds = 10.0;
constexpr int kFrameShiftMs = 10; // matches FbankOptions default (not overridden above)
constexpr int kChunkFrames = static_cast<int>(kChunkSeconds * 1000.0 / kFrameShiftMs);
constexpr int kMinPartialChunkFrames = 100; // ~1s — drop a trailing sliver shorter than this

// Runs one already-framed (T,64) chunk through the CED-small graph, returns its 527-d
// probs (empty on failure). `frames` holds `numFrames` pointers into the fbank's
// internal storage (valid only while the OnlineFbank that produced them is alive).
std::vector<float> runCedOnChunk(Ort::Session& session, const std::string& inputName,
                                  const std::string& outputName,
                                  const std::vector<const float*>& frames, int numFrames) {
    // Transpose (T,64) -> (1,64,T), matching sherpa-onnx's OfflineCEDModel::Forward.
    std::vector<float> featsCT(static_cast<size_t>(kFeatDim) * numFrames);
    for (int t = 0; t < numFrames; ++t) {
        for (int c = 0; c < kFeatDim; ++c) {
            featsCT[static_cast<size_t>(c) * numFrames + t] = frames[t][c];
        }
    }

    std::vector<int64_t> inputShape = {1, kFeatDim, numFrames};
    Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memInfo, featsCT.data(), featsCT.size(), inputShape.data(), inputShape.size());

    const char* inputNameC = inputName.c_str();
    const char* outputNameC = outputName.c_str();
    auto outputs = session.Run(Ort::RunOptions{nullptr}, &inputNameC, &inputTensor, 1,
                                &outputNameC, 1);

    const float* probs = outputs[0].GetTensorData<float>();
    size_t count = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (static_cast<int>(count) != kAudioSetClassCount) return {};
    return std::vector<float>(probs, probs + count);
}
} // namespace

ContentGateResult runContentGate(const std::vector<float>& mono, int sampleRate,
                                  const std::string& modelPath) {
    ContentGateResult result;
    if (mono.empty() || sampleRate <= 0) return result;

    try {
        auto audio16k = resampleTo16k(mono, sampleRate);
        if (audio16k.empty()) return result;

        // --- kaldi-style fbank frontend (64 mel bins, linear not log) ---
        knf::OnlineFbank fbank(cedFbankOptions());
        fbank.AcceptWaveform(static_cast<float>(kSampleRate), audio16k.data(),
                              static_cast<int32_t>(audio16k.size()));
        fbank.InputFinished();

        int numFrames = fbank.NumFramesReady();
        if (numFrames < 1) return result;

        std::vector<const float*> allFrames(numFrames);
        for (int t = 0; t < numFrames; ++t) allFrames[t] = fbank.GetFrame(t);

        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "mira_content_gate");
        Ort::SessionOptions sessionOptions;
        sessionOptions.SetIntraOpNumThreads(1);
        Ort::Session session(env, modelPath.c_str(), sessionOptions);

        Ort::AllocatorWithDefaultOptions allocator;
        auto inputNameAlloc = session.GetInputNameAllocated(0, allocator);
        std::string inputName = inputNameAlloc.get();
        auto outputNameAlloc = session.GetOutputNameAllocated(0, allocator);
        std::string outputName = outputNameAlloc.get();

        // Chunk into <=10s windows (kChunkFrames) and MAX-pool probs across chunks — see
        // the comment above kChunkSeconds for why chunking exists at all (CED-small
        // breaks on long single-shot inputs). Max, not mean: this gate answers "is there
        // music *anywhere* in this file," not "is the file's average musical" — measured
        // on flamenco.wav (a 14.2s loop split into a 10s + 4.2s chunk), the trailing
        // chunk scored 0.21 on "Music" against the first chunk's 0.52, and mean-pooling
        // dragged the whole file down to 0.37 (below any reasonable threshold) purely
        // because one quiet/sparse section existed alongside an obviously musical one.
        // Max-pooling reports 0.52 instead — a quiet tail shouldn't veto an otherwise
        // clearly musical file, the same reasoning active-region detection already
        // applies elsewhere (silence in a stem doesn't make the whole stem "not audio").
        // A trailing partial chunk under ~1s is dropped, not padded.
        std::vector<double> probMax(kAudioSetClassCount, 0.0);
        int chunkCount = 0;
        for (int start = 0; start < numFrames; start += kChunkFrames) {
            int chunkLen = std::min(kChunkFrames, numFrames - start);
            if (chunkLen < kMinPartialChunkFrames && chunkCount > 0) break; // trailing sliver
            std::vector<const float*> chunkFrames(allFrames.begin() + start,
                                                   allFrames.begin() + start + chunkLen);
            auto chunkProbs = runCedOnChunk(session, inputName, outputName, chunkFrames, chunkLen);
            if (chunkProbs.empty()) continue;
            for (int i = 0; i < kAudioSetClassCount; ++i)
                probMax[i] = std::max(probMax[i], static_cast<double>(chunkProbs[i]));
            ++chunkCount;
        }
        if (chunkCount == 0) return result;

        const std::vector<double>& probs = probMax;

        result.musicScore = probs[kMusicClassIndex];
        result.isMusic = result.musicScore > kContentGateMusicThreshold;

        std::vector<int> indices(kAudioSetClassCount);
        for (int i = 0; i < kAudioSetClassCount; ++i) indices[i] = i;
        std::partial_sort(indices.begin(), indices.begin() + 5, indices.end(),
                           [&](int a, int b) { return probs[a] > probs[b]; });
        for (int i = 0; i < 5; ++i) {
            result.topLabels.push_back({kAudioSetClassNames[indices[i]], probs[indices[i]]});
        }

        result.ok = true;
    } catch (const std::exception&) {
        // ok stays false — caller skips this file's gate/moodtheme, same discipline as
        // every other analyzer here (Router, ActiveRegions, Key, Chords).
    }

    return result;
}

std::string toJson(const ContentGateResult& g) {
    std::ostringstream oss;
    oss << "{\"is_music\":" << (g.isMusic ? "true" : "false")
        << ",\"music_score\":" << g.musicScore << ",\"top_labels\":[";
    for (size_t i = 0; i < g.topLabels.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"name\":\"" << g.topLabels[i].name << "\",\"score\":" << g.topLabels[i].score << "}";
    }
    oss << "]}";
    return oss.str();
}

} // namespace mira
