// Phase 0, day 1-2 spike (PRD §9): prove Essentia C++ builds arm64 no-TF and
// links from an *external* CMake project — not the waf build tree itself.
// essentia::init() -> MonoLoader -> RhythmExtractor2013 + LoudnessEBUR128.

#include <iostream>
#include <essentia/algorithmfactory.h>
#include <essentia/essentiamath.h>

using namespace std;
using namespace essentia;
using namespace essentia::standard;

int main(int argc, char* argv[]) {
  if (argc != 2) {
    cerr << "Usage: " << argv[0] << " <audio-file>" << endl;
    return 1;
  }

  essentia::init();

  AlgorithmFactory& factory = AlgorithmFactory::instance();

  Algorithm* loader = factory.create("MonoLoader",
                                      "filename", argv[1],
                                      "sampleRate", 44100);

  Algorithm* rhythm = factory.create("RhythmExtractor2013",
                                      "method", "multifeature");

  Algorithm* loudness = factory.create("LoudnessEBUR128",
                                        "sampleRate", 44100);

  vector<Real> audio;
  loader->output("audio").set(audio);
  loader->compute();

  cout << "loaded " << audio.size() << " samples ("
       << (double)audio.size() / 44100.0 << " s)" << endl;

  Real bpm, confidence;
  vector<Real> ticks, estimates, bpmIntervals;
  rhythm->input("signal").set(audio);
  rhythm->output("bpm").set(bpm);
  rhythm->output("ticks").set(ticks);
  rhythm->output("confidence").set(confidence);
  rhythm->output("estimates").set(estimates);
  rhythm->output("bpmIntervals").set(bpmIntervals);
  rhythm->compute();

  cout << "bpm: " << bpm << "  confidence: " << confidence << endl;

  // LoudnessEBUR128 wants stereo input; duplicate the mono signal.
  vector<StereoSample> stereo(audio.size());
  for (size_t i = 0; i < audio.size(); ++i) {
    stereo[i].left() = audio[i];
    stereo[i].right() = audio[i];
  }

  Real integratedLoudness, loudnessRange;
  vector<Real> momentaryLoudness, shortTermLoudness;
  loudness->input("signal").set(stereo);
  loudness->output("momentaryLoudness").set(momentaryLoudness);
  loudness->output("shortTermLoudness").set(shortTermLoudness);
  loudness->output("integratedLoudness").set(integratedLoudness);
  loudness->output("loudnessRange").set(loudnessRange);
  loudness->compute();

  cout << "integrated loudness: " << integratedLoudness << " LUFS" << endl;
  cout << "loudness range: " << loudnessRange << " LU" << endl;

  delete loader;
  delete rhythm;
  delete loudness;
  essentia::shutdown();

  cout << "SPIKE OK — Essentia links and runs from an external CMake project." << endl;
  return 0;
}
