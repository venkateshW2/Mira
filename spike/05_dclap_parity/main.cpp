// Phase 4 spike (TASKS.md Phase 4 "Embedding A/B"): DCLAP mel frontend + ONNX
// Runtime C++ inference, reimplementing AudioMuse-AI-DCLAP's README preprocessing
// exactly (https://github.com/NeptuneHub/AudioMuse-AI-DCLAP) so it can be diffed
// against lab/dclap_parity_reference.py's librosa-based output — the same
// two-sided-parity discipline spike/02_onnx_parity used for discogs-effnet
// (PRD §9 day 3, ~1e-4 exit criterion).
//
// Pipeline: AudioLoader(native rate) -> MonoMixer(0.5*(L+R), matches librosa's
// to_mono) -> libsoxr resample to 48kHz (SOXR_HQ, matching librosa.load's default
// res_type='soxr_hq' — NOT Essentia's own libsamplerate-backed Resample, which
// uses a different algorithm and would not agree numerically) -> int16
// quantize round-trip (matches the PyTorch CLAP preprocessing the student was
// distilled against) -> 10s/50%-overlap segmentation -> per segment: reflect-padded
// framing (n_fft=2048, hop=480), periodic Hann window, real FFT via Essentia's
// FFTW-backed "FFT" algorithm (same unnormalized-forward-transform convention as
// numpy's rfft, so no extra scaling needed), power spectrum, a hand-built
// librosa-formula slaney mel filterbank (128 bands, fmin=0, fmax=14000 — Essentia
// ships no algorithm that reproduces librosa's specific mel-filter normalization,
// so this is generated from the same closed-form formulas as
// librosa/filters.py's mel()), power_to_db (amin=1e-10, ref=1.0, no top_db clip)
// -> model_epoch_36.onnx (fixed batch=1, one segment at a time) -> average the
// per-segment embeddings -> L2-normalize.

#include <iostream>
#include <fstream>
#include <vector>
#include <cmath>
#include <complex>
#include <essentia/algorithmfactory.h>
#include <onnxruntime_cxx_api.h>
#include <soxr.h>

using namespace std;
using namespace essentia;
using namespace essentia::standard;

static constexpr double TARGET_SR = 48000.0;
static constexpr int SEGMENT_LENGTH = 480000; // 10s @ 48kHz
static constexpr int HOP_LENGTH = 240000;     // 50% overlap
static constexpr int N_MELS = 128;
static constexpr int N_FFT = 2048;
static constexpr int HOP_LENGTH_MELS = 480;
static constexpr int N_FREQ_BINS = N_FFT / 2 + 1; // 1025
static constexpr double FMIN = 0.0;
static constexpr double FMAX = 14000.0;
static constexpr int EMBED_DIM = 512;

static void writeBin(const string& path, const vector<float>& data) {
  ofstream f(path, ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(float));
}

// --- Slaney mel scale (librosa/core/convert.py hz_to_mel/mel_to_hz, htk=False) ---
static double hzToMelSlaney(double f) {
  constexpr double fSp = 200.0 / 3.0;
  constexpr double minLogHz = 1000.0;
  constexpr double minLogMel = minLogHz / fSp; // 15.0
  const double logstep = std::log(6.4) / 27.0;
  if (f >= minLogHz) return minLogMel + std::log(f / minLogHz) / logstep;
  return f / fSp;
}

static double melToHzSlaney(double mel) {
  constexpr double fSp = 200.0 / 3.0;
  constexpr double minLogHz = 1000.0;
  constexpr double minLogMel = minLogHz / fSp;
  const double logstep = std::log(6.4) / 27.0;
  if (mel >= minLogMel) return minLogHz * std::exp(logstep * (mel - minLogMel));
  return fSp * mel;
}

// librosa.filters.mel(sr, n_fft, n_mels, fmin, fmax, htk=False, norm='slaney')
static vector<vector<double>> buildMelFilterbank() {
  vector<double> fftFreqs(N_FREQ_BINS);
  for (int k = 0; k < N_FREQ_BINS; ++k) fftFreqs[k] = k * TARGET_SR / N_FFT;

  int nPoints = N_MELS + 2;
  double melMin = hzToMelSlaney(FMIN);
  double melMax = hzToMelSlaney(FMAX);
  vector<double> melF(nPoints);
  for (int i = 0; i < nPoints; ++i) {
    double mel = melMin + (melMax - melMin) * i / (nPoints - 1);
    melF[i] = melToHzSlaney(mel);
  }

  vector<vector<double>> weights(N_MELS, vector<double>(N_FREQ_BINS, 0.0));
  for (int i = 0; i < N_MELS; ++i) {
    double fdiffLower = melF[i + 1] - melF[i];
    double fdiffUpper = melF[i + 2] - melF[i + 1];
    for (int k = 0; k < N_FREQ_BINS; ++k) {
      double lower = (fftFreqs[k] - melF[i]) / fdiffLower;
      double upper = (melF[i + 2] - fftFreqs[k]) / fdiffUpper;
      weights[i][k] = std::max(0.0, std::min(lower, upper));
    }
    double enorm = 2.0 / (melF[i + 2] - melF[i]); // norm='slaney'
    for (int k = 0; k < N_FREQ_BINS; ++k) weights[i][k] *= enorm;
  }
  return weights;
}

// numpy 'reflect' padding: whole-sample symmetric, edge value not repeated.
static int reflectIndex(int j, int n) {
  if (n == 1) return 0;
  int period = 2 * (n - 1);
  int m = j % period;
  if (m < 0) m += period;
  if (m >= n) m = period - m;
  return m;
}

static vector<float> resampleToSoxrHQ(const vector<float>& input, double inRate, double outRate) {
  size_t outCapacity = static_cast<size_t>(std::ceil(input.size() * outRate / inRate)) + 16;
  vector<float> output(outCapacity);
  size_t idone = 0, odone = 0;
  soxr_error_t err = soxr_oneshot(inRate, outRate, 1,
                                   input.data(), input.size(), &idone,
                                   output.data(), output.size(), &odone,
                                   nullptr, nullptr, nullptr); // NULL => float32, SOXR_HQ, default runtime
  if (err) {
    cerr << "soxr_oneshot failed: " << soxr_strerror(err) << endl;
    exit(1);
  }
  output.resize(odone);
  return output;
}

// Segment per the DCLAP README: pad short files to one segment; otherwise 50%-overlap
// hop, plus a final tail segment ending exactly at the file's end if the hop grid
// doesn't already land there.
static vector<vector<float>> segmentAudio(const vector<float>& y) {
  vector<vector<float>> segments;
  int total = static_cast<int>(y.size());
  if (total <= SEGMENT_LENGTH) {
    vector<float> seg(SEGMENT_LENGTH, 0.0f);
    std::copy(y.begin(), y.end(), seg.begin());
    segments.push_back(std::move(seg));
    return segments;
  }
  int start = 0;
  while (start + SEGMENT_LENGTH <= total) {
    segments.emplace_back(y.begin() + start, y.begin() + start + SEGMENT_LENGTH);
    start += HOP_LENGTH;
  }
  int lastStart = static_cast<int>(segments.size()) * HOP_LENGTH;
  if (lastStart < total) {
    segments.emplace_back(y.end() - SEGMENT_LENGTH, y.end());
  }
  return segments;
}

// One segment -> [128, T] log-mel, matching librosa.feature.melspectrogram(n_fft=2048,
// hop=480, win_length=2048, window='hann', center=True, pad_mode='reflect', power=2.0)
// followed by power_to_db(ref=1.0, amin=1e-10, top_db=None).
static vector<float> computeLogMel(const vector<float>& segment,
                                    const vector<vector<double>>& melFB,
                                    AlgorithmFactory& factory) {
  int n = static_cast<int>(segment.size());
  int padAmt = N_FFT / 2;
  int paddedLen = n + 2 * padAmt;
  vector<float> padded(paddedLen);
  for (int j = -padAmt; j < n + padAmt; ++j) {
    padded[j + padAmt] = segment[reflectIndex(j, n)];
  }

  // Periodic Hann window (fftbins=True): w[k] = 0.5 - 0.5*cos(2*pi*k/N), k=0..N-1.
  vector<double> window(N_FFT);
  for (int k = 0; k < N_FFT; ++k) window[k] = 0.5 - 0.5 * std::cos(2.0 * M_PI * k / N_FFT);

  int nFrames = 1 + (paddedLen - N_FFT) / HOP_LENGTH_MELS;

  unique_ptr<Algorithm> fft(factory.create("FFT", "size", N_FFT));
  vector<Real> frameBuf(N_FFT);
  vector<complex<Real>> fftOut;
  fft->input("frame").set(frameBuf);
  fft->output("fft").set(fftOut);

  vector<float> logMel(static_cast<size_t>(N_MELS) * nFrames);
  vector<double> power(N_FREQ_BINS);

  for (int t = 0; t < nFrames; ++t) {
    int frameStart = t * HOP_LENGTH_MELS;
    for (int k = 0; k < N_FFT; ++k) {
      frameBuf[k] = static_cast<Real>(padded[frameStart + k] * window[k]);
    }
    fft->compute();
    for (int k = 0; k < N_FREQ_BINS; ++k) {
      double re = fftOut[k].real(), im = fftOut[k].imag();
      power[k] = re * re + im * im;
    }
    for (int m = 0; m < N_MELS; ++m) {
      double acc = 0.0;
      for (int k = 0; k < N_FREQ_BINS; ++k) acc += melFB[m][k] * power[k];
      double db = 10.0 * std::log10(std::max(1e-10, acc)); // ref=1.0 -> subtract 0
      logMel[static_cast<size_t>(m) * nFrames + t] = static_cast<float>(db);
    }
  }
  return logMel; // row-major [128, nFrames], matches Python's log_mel[np.newaxis,np.newaxis,:,:]
}

int main(int argc, char* argv[]) {
  if (argc != 4) {
    cerr << "Usage: " << argv[0] << " <audio-file> <onnx-model> <out-prefix>" << endl;
    return 1;
  }
  string audioPath = argv[1];
  string modelPath = argv[2];
  string outPrefix = argv[3];

  essentia::init();
  AlgorithmFactory& factory = AlgorithmFactory::instance();

  // --- Load at native rate, downmix (0.5*(L+R), matches librosa's to_mono) ---
  Algorithm* loader = factory.create("AudioLoader", "filename", audioPath);
  vector<StereoSample> stereoAudio;
  Real nativeSampleRate = 0;
  int numChannels = 0;
  string md5, codec;
  int bitRate = 0;
  loader->output("audio").set(stereoAudio);
  loader->output("sampleRate").set(nativeSampleRate);
  loader->output("numberChannels").set(numChannels);
  loader->output("md5").set(md5);
  loader->output("bit_rate").set(bitRate);
  loader->output("codec").set(codec);
  loader->compute();
  cout << "loaded " << stereoAudio.size() << " samples @ " << nativeSampleRate
       << " Hz, " << numChannels << " ch" << endl;

  Algorithm* monoMixer = factory.create("MonoMixer", "type", "mix");
  vector<Real> monoAudio;
  monoMixer->input("audio").set(stereoAudio);
  monoMixer->input("numberChannels").set(numChannels);
  monoMixer->output("audio").set(monoAudio);
  monoMixer->compute();

  vector<float> monoF(monoAudio.begin(), monoAudio.end());

  // --- Resample to 48kHz via libsoxr (SOXR_HQ), matching librosa.load's default ---
  vector<float> audio48k = resampleToSoxrHQ(monoF, nativeSampleRate, TARGET_SR);
  cout << "resampled to " << audio48k.size() << " samples @ 48000 Hz" << endl;

  // --- int16 quantize round-trip (matches PyTorch CLAP preprocessing) ---
  for (float& s : audio48k) {
    float c = std::max(-1.0f, std::min(1.0f, s));
    int16_t q = static_cast<int16_t>(c * 32767.0f);
    s = static_cast<float>(q) / 32767.0f;
  }

  // --- Segment ---
  auto segments = segmentAudio(audio48k);
  cout << "segments: " << segments.size() << endl;

  auto melFB = buildMelFilterbank();

  // --- ONNX Runtime session (external .onnx.data resolved relative to modelPath) ---
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "mira_spike_dclap");
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetIntraOpNumThreads(1);
  Ort::Session session(env, modelPath.c_str(), sessionOptions);

  Ort::AllocatorWithDefaultOptions allocator;
  auto inputNameAlloc = session.GetInputNameAllocated(0, allocator);
  string inputName = inputNameAlloc.get();
  auto outputNameAlloc = session.GetOutputNameAllocated(0, allocator);
  string outputName = outputNameAlloc.get();
  const char* inputNameC = inputName.c_str();
  const char* outputNameC = outputName.c_str();

  Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

  vector<float> segmentEmbeddings; // [n_segments * 512]
  int firstFrameCount = -1;

  for (size_t s = 0; s < segments.size(); ++s) {
    vector<float> logMel = computeLogMel(segments[s], melFB, factory);
    int nFrames = static_cast<int>(logMel.size() / N_MELS);
    if (s == 0) {
      firstFrameCount = nFrames;
      writeBin(outPrefix + "_seg0_logmel.bin", logMel); // for standalone mel-frontend diffing
    }

    vector<int64_t> inputShape = {1, 1, N_MELS, nFrames};
    Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
        memInfo, logMel.data(), logMel.size(), inputShape.data(), inputShape.size());

    auto outputs = session.Run(Ort::RunOptions{nullptr}, &inputNameC, &inputTensor, 1,
                                &outputNameC, 1);
    const float* embData = outputs[0].GetTensorData<float>();
    size_t embCount = outputs[0].GetTensorTypeAndShapeInfo().GetElementCount();
    if (static_cast<int>(embCount) != EMBED_DIM) {
      cerr << "unexpected embedding dim " << embCount << endl;
      return 1;
    }
    segmentEmbeddings.insert(segmentEmbeddings.end(), embData, embData + embCount);
    cout << "segment " << s << ": " << nFrames << " frames -> " << embCount << "-dim embedding" << endl;
  }

  writeBin(outPrefix + "_segment_embeddings.bin", segmentEmbeddings);

  // --- Average + L2-normalize ---
  size_t nSegments = segments.size();
  vector<float> avgEmb(EMBED_DIM, 0.0f);
  for (size_t s = 0; s < nSegments; ++s)
    for (int d = 0; d < EMBED_DIM; ++d)
      avgEmb[d] += segmentEmbeddings[s * EMBED_DIM + d];
  for (float& v : avgEmb) v /= static_cast<float>(nSegments);

  double norm = 0.0;
  for (float v : avgEmb) norm += static_cast<double>(v) * v;
  norm = std::sqrt(norm) + 1e-9;
  for (float& v : avgEmb) v = static_cast<float>(v / norm);

  writeBin(outPrefix + "_embedding.bin", avgEmb);
  cout << "wrote " << outPrefix << "_embedding.bin (n_segments=" << nSegments
       << ", first segment frames=" << firstFrameCount << ")" << endl;

  delete loader;
  delete monoMixer;
  essentia::shutdown();

  cout << "SPIKE OK — DCLAP mel frontend + ONNX Runtime C++ inference completed." << endl;
  return 0;
}
