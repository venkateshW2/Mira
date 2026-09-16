#include "Onsets.h"

#include <essentia/algorithmfactory.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numeric>

namespace mira {

namespace {

using AlgoPtr = std::unique_ptr<essentia::standard::Algorithm>;

AlgoPtr create(const char* name) {
    return AlgoPtr(essentia::standard::AlgorithmFactory::instance().create(name));
}

constexpr int kMelBands = 128;

// --- peak picking -----------------------------------------------------------------
//
// The standard adaptive-threshold picker (Bello et al. 2005, and what librosa's
// peak_pick implements). A frame is an onset when it is the largest in a short
// neighbourhood AND stands clear of a longer-window local mean by kOnsetDelta, with a
// refractory gap afterwards so one transient cannot fire twice.
//
// Every window here is in FRAMES and so scales with the hop; they are given in
// milliseconds below and converted, rather than hard-coded, so that changing
// kOnsetHopSize cannot silently change what counts as an onset.
constexpr double kPreMaxMs = 30.0;   // must be the local maximum over this much before
constexpr double kPostMaxMs = 30.0;  // ...and this much after
constexpr double kPreAvgMs = 100.0;  // mean is taken over this much before
constexpr double kPostAvgMs = 100.0; // ...and this much after
constexpr double kWaitMs = 30.0;     // refractory gap between accepted onsets
// How far above the local mean a peak must sit, as a fraction of the envelope's own
// standard deviation. Expressed relative to the signal rather than as an absolute
// because the envelope's scale depends on the material -- a fixed number would find
// everything in a dense mix and nothing in a sparse one.
constexpr double kOnsetDelta = 0.30;

std::vector<int> pickPeaks(const std::vector<float>& env, double hopSeconds) {
    std::vector<int> peaks;
    if (env.size() < 3 || hopSeconds <= 0.0) return peaks;

    auto frames = [&](double ms) {
        return std::max(1, static_cast<int>(std::lround(ms / 1000.0 / hopSeconds)));
    };
    const int preMax = frames(kPreMaxMs), postMax = frames(kPostMaxMs);
    const int preAvg = frames(kPreAvgMs), postAvg = frames(kPostAvgMs);
    const int wait = frames(kWaitMs);
    const int n = static_cast<int>(env.size());

    double mean = std::accumulate(env.begin(), env.end(), 0.0) / n;
    double variance = 0.0;
    for (float v : env) variance += (v - mean) * (v - mean);
    const double delta = kOnsetDelta * std::sqrt(variance / n);

    // Prefix sums, so the moving average is O(1) per frame rather than O(window).
    std::vector<double> prefix(n + 1, 0.0);
    for (int i = 0; i < n; ++i) prefix[i + 1] = prefix[i] + env[i];

    int lastAccepted = -wait - 1;
    for (int i = 0; i < n; ++i) {
        int maxLo = std::max(0, i - preMax), maxHi = std::min(n, i + postMax + 1);
        bool isLocalMax = true;
        for (int j = maxLo; j < maxHi; ++j) {
            if (env[j] > env[i]) { isLocalMax = false; break; }
        }
        if (!isLocalMax) continue;

        int avgLo = std::max(0, i - preAvg), avgHi = std::min(n, i + postAvg + 1);
        double localMean = (prefix[avgHi] - prefix[avgLo]) / (avgHi - avgLo);
        if (env[i] < localMean + delta) continue;

        if (i - lastAccepted <= wait) continue;
        peaks.push_back(i);
        lastAccepted = i;
    }
    return peaks;
}

} // namespace

OnsetResult detectOnsets(const std::vector<float>& mono, int sampleRate) {
    OnsetResult result;
    if (sampleRate <= 0 || static_cast<int>(mono.size()) < kOnsetFrameSize) return result;

    const double hopSeconds = static_cast<double>(kOnsetHopSize) / sampleRate;
    result.hopSeconds = hopSeconds;

    AlgoPtr windowing = create("Windowing");
    windowing->configure("type", "hann", "size", kOnsetFrameSize);
    AlgoPtr spectrum = create("Spectrum");
    spectrum->configure("size", kOnsetFrameSize);
    AlgoPtr melBands = create("MelBands");
    melBands->configure("inputSize", kOnsetFrameSize / 2 + 1, "numberBands", kMelBands,
                        "sampleRate", static_cast<essentia::Real>(sampleRate),
                        "highFrequencyBound",
                        static_cast<essentia::Real>(std::min(11000.0, sampleRate / 2.0 - 1.0)));

    std::vector<essentia::Real> frame(kOnsetFrameSize), windowed, spec, bands;
    windowing->input("frame").set(frame);
    windowing->output("frame").set(windowed);
    spectrum->input("frame").set(windowed);
    spectrum->output("spectrum").set(spec);
    melBands->input("spectrum").set(spec);
    melBands->output("bands").set(bands);

    // The onset detection function: positive first difference of the log-mel
    // spectrogram, averaged across bands. Positive-only ("half-wave rectified") on
    // purpose -- energy LEAVING a band is a note ending, and a note ending is not an
    // onset. Taking the magnitude instead is the classic way to get two onsets per hit.
    // CENTRED frames, via a reflect pad of half a frame at each end -- librosa's
    // convention, and the collaborator's tool's (`np.pad(y, N_FFT // 2, mode="reflect")`).
    // Without it, frame i covers [i*hop, i*hop + 2048] and its energy therefore belongs
    // to a moment ~23 ms after the frame's nominal time, while the flux PEAK arrives
    // later still -- the frame has to fill with the transient before the difference is
    // largest. Measured: onsets landed a consistent 1/16 of a beat (~43 ms at 86 BPM)
    // behind the beat grid, which is the frame length, not the music. With the pad,
    // frame i is centred on sample i*hop.
    const size_t pad = kOnsetFrameSize / 2;
    std::vector<float> padded(mono.size() + 2 * pad);
    for (size_t i = 0; i < pad; ++i) {
        padded[i] = mono[std::min(pad - i, mono.size() - 1)];
        padded[padded.size() - 1 - i] = mono[mono.size() - 1 - std::min(pad - i, mono.size() - 1)];
    }
    std::copy(mono.begin(), mono.end(), padded.begin() + static_cast<long>(pad));

    std::vector<float> envelope;
    envelope.reserve(mono.size() / kOnsetHopSize + 1);
    std::vector<float> previousDb;

    for (size_t start = 0; start + kOnsetFrameSize <= padded.size(); start += kOnsetHopSize) {
        std::copy(padded.begin() + static_cast<long>(start),
                  padded.begin() + static_cast<long>(start + kOnsetFrameSize), frame.begin());
        try {
            windowing->compute();
            spectrum->compute();
            melBands->compute();
        } catch (const essentia::EssentiaException&) {
            return result; // never substitute a value for a failed transform (CLAUDE.md §6)
        }

        std::vector<float> db(bands.size());
        for (size_t i = 0; i < bands.size(); ++i)
            db[i] = 10.0f * std::log10(std::max(static_cast<float>(bands[i]), 1e-10f));

        if (previousDb.size() == db.size()) {
            double sum = 0.0;
            for (size_t i = 0; i < db.size(); ++i) sum += std::max(0.0f, db[i] - previousDb[i]);
            envelope.push_back(static_cast<float>(sum / db.size()));
        } else {
            envelope.push_back(0.0f); // the first frame has nothing to difference against
        }
        previousDb = std::move(db);
    }

    if (envelope.size() < 3) return result;

    auto peaks = pickPeaks(envelope, hopSeconds);
    result.times.reserve(peaks.size());
    for (int frameIndex : peaks) result.times.push_back(frameIndex * hopSeconds);

    // Frames are centred, so frame i sits at i*hop in file time. The envelope value at
    // i is the difference between the centres of frames i-1 and i, so the change it
    // reports happened between them: half a hop back is that interval's midpoint.
    for (double& t : result.times) t = std::max(0.0, t - hopSeconds * 0.5);

    result.envelope = std::move(envelope);
    double duration = static_cast<double>(mono.size()) / sampleRate;
    result.rate = duration > 0.0 ? result.times.size() / duration : 0.0;
    result.ok = true;
    return result;
}

} // namespace mira
