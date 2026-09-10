// Phase 0, day 3 spike (PRD §9): ONNX end-to-end + numerical parity.
//
// MonoLoader(16kHz) -> FrameCutter(512,256) -> TensorflowInputMusiCNN -> [n,128,96]
// patches (patchSize=128, patchHopSize=62, discard last partial — the exact defaults
// read from vendor/essentia/src/algorithms/machinelearning/tensorflowpredicteffnetdiscogs.h,
// since that composite algorithm itself needs TensorflowPredict/libtensorflow, which the
// no-TF build does not have) -> discogs-effnet-bsdynamic-1.onnx via ONNX Runtime -> 1280-d
// embeddings.
//
// Writes patches + embeddings as raw float32 so lab/compare_parity.py can diff them
// against lab/parity_reference.py's Python output to ~1e-4 (PRD §9 day 3 exit criterion).

#include <iostream>
#include <fstream>
#include <vector>
#include <essentia/algorithmfactory.h>
#include <onnxruntime_cxx_api.h>

using namespace std;
using namespace essentia;
using namespace essentia::standard;

static constexpr int FRAME_SIZE = 512;
static constexpr int HOP_SIZE = 256;
static constexpr int SAMPLE_RATE = 16000;
static constexpr int NUMBER_BANDS = 96;
static constexpr int PATCH_SIZE = 128;
static constexpr int PATCH_HOP_SIZE = 62;

static void writeBin(const string& path, const vector<float>& data) {
  ofstream f(path, ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()), data.size() * sizeof(float));
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

  // --- Load + mel frontend -------------------------------------------------
  Algorithm* loader = factory.create("MonoLoader",
                                      "filename", audioPath,
                                      "sampleRate", SAMPLE_RATE);
  vector<Real> audio;
  loader->output("audio").set(audio);
  loader->compute();
  cout << "loaded " << audio.size() << " samples @ " << SAMPLE_RATE << " Hz" << endl;

  Algorithm* frameCutter = factory.create("FrameCutter",
                                           "frameSize", FRAME_SIZE,
                                           "hopSize", HOP_SIZE);
  Algorithm* tfInput = factory.create("TensorflowInputMusiCNN");

  vector<Real> signalBuf = audio;
  frameCutter->input("signal").set(signalBuf);

  vector<Real> frame;
  frameCutter->output("frame").set(frame);

  vector<Real> bandsOut;
  tfInput->input("frame").set(frame);
  tfInput->output("bands").set(bandsOut);

  vector<vector<float>> bands; // [n_frames][96]
  while (true) {
    frameCutter->compute();
    if (frame.empty()) break;
    tfInput->compute();
    bands.emplace_back(bandsOut.begin(), bandsOut.end());
  }
  cout << "mel frames: " << bands.size() << " x " << NUMBER_BANDS << endl;

  // --- Patch into [n, 128, 96], overlap patchHopSize=62, discard last partial ---
  int nFrames = (int)bands.size();
  int nPatches = 1 + (nFrames - PATCH_SIZE) / PATCH_HOP_SIZE;
  if (nPatches < 1) {
    cerr << "file too short: only " << nFrames << " mel frames, need >= " << PATCH_SIZE << endl;
    return 1;
  }
  cout << "patches: " << nPatches << " x " << PATCH_SIZE << " x " << NUMBER_BANDS << endl;

  vector<float> patches(static_cast<size_t>(nPatches) * PATCH_SIZE * NUMBER_BANDS);
  for (int p = 0; p < nPatches; ++p) {
    int startFrame = p * PATCH_HOP_SIZE;
    for (int t = 0; t < PATCH_SIZE; ++t) {
      const vector<float>& srcFrame = bands[startFrame + t];
      float* dst = &patches[(static_cast<size_t>(p) * PATCH_SIZE + t) * NUMBER_BANDS];
      copy(srcFrame.begin(), srcFrame.end(), dst);
    }
  }
  writeBin(outPrefix + "_patches.bin", patches);

  // --- ONNX Runtime inference ----------------------------------------------
  Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "mira_spike");
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetIntraOpNumThreads(1);
  Ort::Session session(env, modelPath.c_str(), sessionOptions);

  Ort::AllocatorWithDefaultOptions allocator;
  auto inputNameAlloc = session.GetInputNameAllocated(0, allocator);
  string inputName = inputNameAlloc.get();

  size_t numOutputs = session.GetOutputCount();
  vector<string> outputNames;
  vector<const char*> outputNamesC;
  for (size_t i = 0; i < numOutputs; ++i) {
    auto nameAlloc = session.GetOutputNameAllocated(i, allocator);
    outputNames.push_back(nameAlloc.get());
  }
  for (auto& n : outputNames) outputNamesC.push_back(n.c_str());

  vector<int64_t> inputShape = {nPatches, PATCH_SIZE, NUMBER_BANDS};
  Ort::MemoryInfo memInfo = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  Ort::Value inputTensor = Ort::Value::CreateTensor<float>(
      memInfo, patches.data(), patches.size(), inputShape.data(), inputShape.size());

  const char* inputNameC = inputName.c_str();
  auto outputs = session.Run(Ort::RunOptions{nullptr}, &inputNameC, &inputTensor, 1,
                              outputNamesC.data(), outputNamesC.size());

  // Find the 1280-dim embedding output (PartitionedCall:1 per the model's own .json).
  for (size_t i = 0; i < outputs.size(); ++i) {
    auto shape = outputs[i].GetTensorTypeAndShapeInfo().GetShape();
    if (!shape.empty() && shape.back() == 1280) {
      const float* data = outputs[i].GetTensorData<float>();
      size_t count = outputs[i].GetTensorTypeAndShapeInfo().GetElementCount();
      vector<float> embeddings(data, data + count);
      writeBin(outPrefix + "_embeddings.bin", embeddings);
      cout << "embeddings: " << shape[0] << " x " << shape[1] << endl;
      cout << "wrote " << outPrefix << "_patches.bin, " << outPrefix << "_embeddings.bin" << endl;
      break;
    }
  }

  delete loader;
  delete frameCutter;
  delete tfInput;
  essentia::shutdown();

  cout << "SPIKE OK — mel frontend + ONNX Runtime C++ inference completed." << endl;
  return 0;
}
