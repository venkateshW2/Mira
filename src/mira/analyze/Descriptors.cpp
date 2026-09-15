#include "Descriptors.h"

#include <essentia/algorithmfactory.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <sstream>

namespace mira {

namespace {
constexpr int kFrameSize = 2048;
constexpr int kHopSize = 1024;

using Algo = essentia::standard::Algorithm;
using AlgoPtr = std::unique_ptr<Algo>;

AlgoPtr create(const std::string& name) {
    return AlgoPtr(essentia::standard::AlgorithmFactory::instance().create(name));
}

constexpr essentia::Real kHarmonicityMinPitchConfidence = 0.5f;
constexpr int kNumMfccCoefficients = 13;
constexpr int kChromaSize = 12; // HPCP bins, one per pitch class

struct SpectralAverages {
    double centroidHz = 0.0;
    double flatness = 0.0;
    // Share of frame energy below 80 Hz. `spectral_centroid` is a mean dominated by the
    // mids and cannot see the sub at all, so a track built on a 40 Hz fundamental and one
    // with no bottom end score alike. 20-80 rather than 20-60 on purpose: at kFrameSize
    // 2048 / 44.1 kHz a bin is 21.5 Hz wide, so 20-60 is barely two bins -- the wider
    // band buys a third bin and a usable ratio. If this turns out to discriminate, the
    // upgrade is a second 8192-point pass (5.4 Hz bins) for this measure alone.
    double subRatio = 0.0;
    // How fast the spectrum MOVES, frame to frame -- sum of squared bin differences.
    // flatness describes what the spectrum looks like on average; flux describes whether
    // it is going anywhere. A sustained drone and a continuously morphing texture can
    // have identical flatness and completely different flux, and that difference is what
    // "sound design" means as a measurement.
    double fluxMean = 0.0;
    // Steady motion vs bursty. Continuous granular movement and a static bed cut by
    // sudden edits both raise fluxMean; only the second raises this.
    double fluxStddev = 0.0;
    double harmonicity = 0.0;
    int harmonicityFrameCount = 0;
    std::vector<double> mfcc;    // size kNumMfccCoefficients, empty if unmeasurable
    std::vector<double> chroma;  // size kChromaSize, empty if unmeasurable
};

// ONE Windowing(hann) -> Spectrum pass per frame, shared by every spectral descriptor
// below (Centroid, Flatness, MFCC, the SpectralPeaks -> HPCP chroma chain, and the
// PitchYinFFT -> SpectralPeaks -> HarmonicPeaks -> Inharmonicity chain for harmonicity)
// — these used to be three separate frame loops, each redoing Windowing+Spectrum from
// scratch over the same audio. Found by comparing against a hand-written analyser
// (single-STFT-pass design) that does this correctly; this was silently tripling mira's
// own spectral analysis cost for no reason.
SpectralAverages computeSpectralAverages(const std::vector<float>& mono, int sampleRate,
                                          SpectralFrames* framesOut) {
    SpectralAverages result;
    if (static_cast<int>(mono.size()) < kFrameSize) return result;

    AlgoPtr windowing = create("Windowing");
    windowing->configure("type", "hann");
    AlgoPtr spectrum = create("Spectrum");
    spectrum->configure("size", kFrameSize);
    AlgoPtr centroidAlgo = create("Centroid");
    centroidAlgo->configure("range", essentia::Parameter(sampleRate / 2.0));
    AlgoPtr flatnessAlgo = create("Flatness");
    AlgoPtr subBandAlgo = create("EnergyBandRatio");
    subBandAlgo->configure("sampleRate", static_cast<essentia::Real>(sampleRate),
                            "startFrequency", 20.0f, "stopFrequency", 80.0f);
    // Stateful: Flux keeps the previous frame's spectrum itself, so it must see every
    // frame in order -- which the shared pass below already guarantees.
    AlgoPtr fluxAlgo = create("Flux");

    AlgoPtr mfccAlgo = create("MFCC");
    // highFrequencyBound must stay below Nyquist — matters for low-sample-rate stems.
    double mfccHighFreq = std::min(11000.0, sampleRate / 2.0 - 1.0);
    mfccAlgo->configure("sampleRate", static_cast<essentia::Real>(sampleRate),
                         "inputSize", kFrameSize / 2 + 1, "numberCoefficients", kNumMfccCoefficients,
                         "highFrequencyBound", mfccHighFreq);

    AlgoPtr pitchYin = create("PitchYinFFT");
    pitchYin->configure("frameSize", kFrameSize, "sampleRate", static_cast<essentia::Real>(sampleRate));
    AlgoPtr spectralPeaks = create("SpectralPeaks");
    spectralPeaks->configure("sampleRate", static_cast<essentia::Real>(sampleRate));
    AlgoPtr harmonicPeaks = create("HarmonicPeaks");
    AlgoPtr inharmonicity = create("Inharmonicity");

    AlgoPtr hpcp = create("HPCP");
    hpcp->configure("sampleRate", static_cast<essentia::Real>(sampleRate), "size", kChromaSize);

    std::vector<essentia::Real> frame(kFrameSize), windowed, spec;
    windowing->input("frame").set(frame);
    windowing->output("frame").set(windowed);
    spectrum->input("frame").set(windowed);
    spectrum->output("spectrum").set(spec);

    essentia::Real centroidValue = 0;
    centroidAlgo->input("array").set(spec);
    centroidAlgo->output("centroid").set(centroidValue);

    essentia::Real flatnessValue = 0;
    flatnessAlgo->input("array").set(spec);
    flatnessAlgo->output("flatness").set(flatnessValue);

    essentia::Real subValue = 0;
    subBandAlgo->input("spectrum").set(spec);
    subBandAlgo->output("energyBandRatio").set(subValue);

    essentia::Real fluxValue = 0;
    fluxAlgo->input("spectrum").set(spec);
    fluxAlgo->output("flux").set(fluxValue);

    std::vector<essentia::Real> mfccBands, mfccCoeffs;
    mfccAlgo->input("spectrum").set(spec);
    mfccAlgo->output("bands").set(mfccBands);
    mfccAlgo->output("mfcc").set(mfccCoeffs);

    essentia::Real pitch = 0, pitchConfidence = 0;
    pitchYin->input("spectrum").set(spec);
    pitchYin->output("pitch").set(pitch);
    pitchYin->output("pitchConfidence").set(pitchConfidence);

    std::vector<essentia::Real> peakFreqs, peakMags;
    spectralPeaks->input("spectrum").set(spec);
    spectralPeaks->output("frequencies").set(peakFreqs);
    spectralPeaks->output("magnitudes").set(peakMags);

    std::vector<essentia::Real> chromaValues;
    hpcp->input("frequencies").set(peakFreqs);
    hpcp->input("magnitudes").set(peakMags);
    hpcp->output("hpcp").set(chromaValues);

    std::vector<essentia::Real> harmFreqs, harmMags;
    harmonicPeaks->input("frequencies").set(peakFreqs);
    harmonicPeaks->input("magnitudes").set(peakMags);
    harmonicPeaks->input("pitch").set(pitch);
    harmonicPeaks->output("harmonicFrequencies").set(harmFreqs);
    harmonicPeaks->output("harmonicMagnitudes").set(harmMags);

    essentia::Real inharmonicityValue = 0;
    inharmonicity->input("frequencies").set(harmFreqs);
    inharmonicity->input("magnitudes").set(harmMags);
    inharmonicity->output("inharmonicity").set(inharmonicityValue);

    double centroidSum = 0.0, flatnessSum = 0.0, harmonicitySum = 0.0;
    // Sum and sum-of-squares, so the stddev needs no second pass over the frames.
    double subSum = 0.0, fluxSum = 0.0, fluxSumSq = 0.0;
    int frameCount = 0, harmonicityCount = 0;
    std::vector<double> mfccSum(kNumMfccCoefficients, 0.0);
    std::vector<double> chromaSum(kChromaSize, 0.0);
    int mfccCount = 0, chromaCount = 0;

    // Keeping the frames, when the caller asked. Reserving on a rough frame estimate
    // keeps this from repeatedly reallocating a multi-megabyte buffer mid-analysis.
    if (framesOut != nullptr) {
        const size_t estimate = mono.size() / kHopSize + 1;
        framesOut->hopSeconds = static_cast<double>(kHopSize) / sampleRate;
        framesOut->mfcc.reserve(estimate * kNumMfccCoefficients);
        framesOut->chroma.reserve(estimate * kChromaSize);
        framesOut->flux.reserve(estimate);
    }

    for (size_t start = 0; start + kFrameSize <= mono.size(); start += kHopSize) {
        std::copy(mono.begin() + start, mono.begin() + start + kFrameSize, frame.begin());
        windowing->compute();
        spectrum->compute();

        centroidAlgo->compute();
        centroidSum += centroidValue;
        flatnessAlgo->compute();
        flatnessSum += flatnessValue;
        // Both are a handful of sums over bins on a spectrum that already exists --
        // measured as noise against the MFCC/HPCP chains in the same loop.
        subBandAlgo->compute();
        subSum += subValue;
        fluxAlgo->compute();
        fluxSum += fluxValue;
        fluxSumSq += static_cast<double>(fluxValue) * fluxValue;
        ++frameCount;

        // Frames are appended per-frame and transposed to row-major at the end, so a
        // frame that throws below still leaves every kept row the same length.
        if (framesOut != nullptr) framesOut->flux.push_back(static_cast<float>(fluxValue));

        bool mfccOk = false;
        try {
            mfccAlgo->compute();
            if (static_cast<int>(mfccCoeffs.size()) == kNumMfccCoefficients) {
                for (int i = 0; i < kNumMfccCoefficients; ++i) mfccSum[i] += mfccCoeffs[i];
                ++mfccCount;
                mfccOk = true;
            }
        } catch (const essentia::EssentiaException&) {
            // e.g. a near-silent frame producing an unstable log — skip it, not the file
        }
        // A frame that threw still gets a row, zero-filled: the beat-synchronous
        // averaging in Meter.cpp indexes frames by time, so a missing row would silently
        // shift every later frame against the beat grid.
        if (framesOut != nullptr) {
            for (int i = 0; i < kNumMfccCoefficients; ++i)
                framesOut->mfcc.push_back(mfccOk ? static_cast<float>(mfccCoeffs[i]) : 0.0f);
            if (framesOut->melBands == 0 && mfccOk)
                framesOut->melBands = static_cast<int>(mfccBands.size());
            for (int i = 0; i < framesOut->melBands; ++i)
                framesOut->mel.push_back(mfccOk && i < static_cast<int>(mfccBands.size())
                                              ? static_cast<float>(mfccBands[i]) : 0.0f);
        }

        // SpectralPeaks -> HPCP: unconditional per frame (chroma is meaningful on
        // polyphonic/noisy material too, unlike the monophonic-pitch harmonicity chain
        // below), and shared with that chain so peaks aren't computed twice.
        bool chromaOk = false;
        try {
            spectralPeaks->compute();
            if (!peakFreqs.empty()) {
                hpcp->compute();
                if (static_cast<int>(chromaValues.size()) == kChromaSize) {
                    for (int i = 0; i < kChromaSize; ++i) chromaSum[i] += chromaValues[i];
                    ++chromaCount;
                    chromaOk = true;
                }
            }
        } catch (const essentia::EssentiaException&) {
            // skip this frame's chroma contribution
        }
        if (framesOut != nullptr)
            for (int i = 0; i < kChromaSize; ++i)
                framesOut->chroma.push_back(chromaOk ? static_cast<float>(chromaValues[i]) : 0.0f);

        pitchYin->compute();
        if (pitchConfidence < kHarmonicityMinPitchConfidence) continue;
        if (peakFreqs.empty()) continue; // nothing for HarmonicPeaks to work with
        try {
            harmonicPeaks->compute();
            inharmonicity->compute();
        } catch (const essentia::EssentiaException&) {
            continue; // e.g. too few peaks for this frame — skip it, not the whole file
        }
        harmonicitySum += (1.0 - inharmonicityValue);
        ++harmonicityCount;
    }

    if (frameCount > 0) {
        result.centroidHz = centroidSum / frameCount;
        result.flatness = flatnessSum / frameCount;
        result.subRatio = subSum / frameCount;
        result.fluxMean = fluxSum / frameCount;
        // max(0,...) because catastrophic cancellation can drive the variance a hair
        // below zero when every frame's flux is near-identical (a pure tone, silence).
        const double var = std::max(0.0, fluxSumSq / frameCount - result.fluxMean * result.fluxMean);
        result.fluxStddev = std::sqrt(var);
    }

    // Transpose frame-major -> row-major ([dim][frame]), which is what MeterFeature wants
    // and what makes the beat-synchronous averaging a contiguous walk per dimension.
    if (framesOut != nullptr && frameCount > 0) {
        framesOut->frames = frameCount;
        auto transpose = [frameCount](std::vector<float>& v, int dims) {
            if (dims <= 0 || v.size() != static_cast<size_t>(dims) * frameCount) { v.clear(); return; }
            std::vector<float> out(v.size());
            for (int f = 0; f < frameCount; ++f)
                for (int d = 0; d < dims; ++d)
                    out[static_cast<size_t>(d) * frameCount + f] = v[static_cast<size_t>(f) * dims + d];
            v.swap(out);
        };
        transpose(framesOut->mfcc, kNumMfccCoefficients);
        transpose(framesOut->chroma, kChromaSize);
        transpose(framesOut->mel, framesOut->melBands);
    }
    result.harmonicity = harmonicityCount > 0 ? harmonicitySum / harmonicityCount : 0.0;
    result.harmonicityFrameCount = harmonicityCount;
    if (mfccCount > 0) {
        result.mfcc.resize(kNumMfccCoefficients);
        for (int i = 0; i < kNumMfccCoefficients; ++i) result.mfcc[i] = mfccSum[i] / mfccCount;
    }
    if (chromaCount > 0) {
        result.chroma.resize(kChromaSize);
        for (int i = 0; i < kChromaSize; ++i) result.chroma[i] = chromaSum[i] / chromaCount;
    }
    return result;
}
} // namespace

DspDescriptors computeDspDescriptors(const std::vector<float>& left,
                                      const std::vector<float>& right, int sampleRate,
                                      SpectralFrames* framesOut) {
    DspDescriptors d;
    if (left.empty() || left.size() != right.size() || sampleRate <= 0) return d;

    // --- Loudness (LoudnessEBUR128 wants stereo) ---
    std::vector<essentia::StereoSample> stereo(left.size());
    for (size_t i = 0; i < left.size(); ++i) {
        stereo[i].left() = left[i];
        stereo[i].right() = right[i];
    }
    {
        auto loudness = create("LoudnessEBUR128");
        loudness->configure("sampleRate", static_cast<essentia::Real>(sampleRate));
        std::vector<essentia::Real> momentary, shortTerm;
        essentia::Real integrated = 0, range = 0;
        loudness->input("signal").set(stereo);
        loudness->output("momentaryLoudness").set(momentary);
        loudness->output("shortTermLoudness").set(shortTerm);
        loudness->output("integratedLoudness").set(integrated);
        loudness->output("loudnessRange").set(range);
        loudness->compute();
        d.integratedLoudnessLufs = integrated;
        d.loudnessRangeLu = range;
    }

    std::vector<float> mono(left.size());
    for (size_t i = 0; i < left.size(); ++i) mono[i] = 0.5f * (left[i] + right[i]);
    std::vector<essentia::Real> monoReal(mono.begin(), mono.end());

    // --- True peak ---
    {
        auto truePeak = create("TruePeakDetector");
        truePeak->configure("sampleRate", static_cast<essentia::Real>(sampleRate), "oversamplingFactor", 2);
        std::vector<essentia::Real> output;
        std::vector<essentia::Real> peakLocations;
        truePeak->input("signal").set(monoReal);
        truePeak->output("output").set(output);
        truePeak->output("peakLocations").set(peakLocations);
        truePeak->compute();
        essentia::Real peak = 0;
        for (auto v : output) peak = std::max(peak, std::abs(v));
        d.truePeakDb = peak > 0 ? 20.0 * std::log10(peak) : -std::numeric_limits<double>::infinity();
    }

    // --- Crest factor (peak / mean of |signal|) ---
    {
        std::vector<essentia::Real> absSignal(monoReal.size());
        for (size_t i = 0; i < monoReal.size(); ++i) absSignal[i] = std::abs(monoReal[i]);
        auto crest = create("Crest");
        essentia::Real crestValue = 0;
        crest->input("array").set(absSignal);
        crest->output("crest").set(crestValue);
        crest->compute();
        d.crestFactor = crestValue;
    }

    // --- Spectral centroid, flatness, harmonicity, MFCC, chroma — one shared frame loop ---
    auto spectral = computeSpectralAverages(mono, sampleRate, framesOut);
    d.spectralCentroidHz = spectral.centroidHz;
    d.spectralFlatness = spectral.flatness;
    d.subRatio = spectral.subRatio;
    d.fluxMean = spectral.fluxMean;
    d.fluxStddev = spectral.fluxStddev;
    d.harmonicity = spectral.harmonicity;
    d.harmonicityFrameCount = spectral.harmonicityFrameCount;
    d.mfcc = spectral.mfcc;
    d.chroma = spectral.chroma;

    // --- Attack time (whole-file envelope; most meaningful on one-shots) ---
    {
        auto envelope = create("Envelope");
        envelope->configure("sampleRate", static_cast<essentia::Real>(sampleRate));
        std::vector<essentia::Real> env;
        envelope->input("signal").set(monoReal);
        envelope->output("signal").set(env);
        envelope->compute();

        auto attack = create("LogAttackTime");
        attack->configure("sampleRate", static_cast<essentia::Real>(sampleRate));
        essentia::Real logAttackTime = 0, attackStart = 0, attackStop = 0;
        attack->input("signal").set(env);
        attack->output("logAttackTime").set(logAttackTime);
        attack->output("attackStart").set(attackStart);
        attack->output("attackStop").set(attackStop);
        try {
            attack->compute();
            d.attackTimeSeconds = std::pow(10.0, logAttackTime);
        } catch (const essentia::EssentiaException&) {
            d.attackTimeSeconds = 0.0; // e.g. a silent or pathologically short envelope
        }
    }

    return d;
}

namespace {
std::string arrayJson(const std::vector<double>& values) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) oss << ",";
        oss << values[i];
    }
    oss << "]";
    return oss.str();
}
} // namespace

std::string toJson(const DspDescriptors& d) {
    std::ostringstream oss;
    oss << "{\"integrated_loudness_lufs\":" << d.integratedLoudnessLufs
        << ",\"loudness_range_lu\":" << d.loudnessRangeLu
        << ",\"true_peak_db\":" << d.truePeakDb
        << ",\"crest_factor\":" << d.crestFactor
        << ",\"spectral_centroid_hz\":" << d.spectralCentroidHz
        << ",\"spectral_flatness\":" << d.spectralFlatness
        << ",\"sub_ratio\":" << d.subRatio
        << ",\"flux_mean\":" << d.fluxMean
        << ",\"flux_stddev\":" << d.fluxStddev
        << ",\"attack_time_seconds\":" << d.attackTimeSeconds
        << ",\"harmonicity\":" << d.harmonicity
        << ",\"harmonicity_frame_count\":" << d.harmonicityFrameCount
        << ",\"mfcc\":" << arrayJson(d.mfcc)
        << ",\"chroma\":" << arrayJson(d.chroma)
        << "}";
    return oss.str();
}

} // namespace mira
