#pragma once

// MIRA-BLOCKS.md step 3 -- time-stretching, as one function over raw buffers.
//
// DELIBERATELY JUCE-FREE, so `spike/08_stretch_latency` can test THIS code rather than a
// sketch of it. A stretcher verified by reading it is not verified (convention 11), and the
// only thing worth verifying here -- that the output lands where the arithmetic says -- is
// exactly what a standalone test can ask and a running app cannot.
//
// WHY A FILE AND NOT AN EFFECT: the canvas mixer renders through `renderRange(from, n)`,
// which is random-access -- the transport seeks, the loop rewinds, export walks arbitrary
// ranges. A stretcher is sequential with internal state and cannot be jumped into. So a
// stretch renders to disk and becomes another take in the block's folder.
// (ARCHITECTURE.md §6.3, MIRA-BLOCKS.md §6.)

#include "signalsmith-stretch.h"

#include <cmath>
#include <string>
#include <vector>

namespace mira::stretch {

// How many output frames `inFrames` becomes at this length ratio.
inline int outputFramesFor(int inFrames, double lengthRatio)
{
    return (int) std::llround((double) inFrames * lengthRatio);
}

// The stretcher's own delay, which is the whole reason spike/08 exists: at ratio 1.0 it is
// 120 ms, about a third of a beat at 143 bpm, and it would otherwise ship invisibly as
// "the stretched take feels slightly late".
//
// Measured to be EXACTLY `inputLatency*ratio + outputLatency` over five ratios from 0.90 to
// 1.10, agreeing with the prediction to within 0.5 ms. So this is arithmetic, not a search.
inline int outputDelayFrames(int inputLatency, int outputLatency, double lengthRatio)
{
    return (int) std::llround((double) inputLatency * lengthRatio + (double) outputLatency);
}

// Stretch `in` to `outFrames`, writing latency-compensated audio so that input frame 0 is
// output frame 0. Returns false and sets `error` rather than producing something plausible
// (convention 6).
//
// `lengthRatio` is OUT over IN: 1.10 makes the audio longer and therefore slower. To take
// audio at `sourceBpm` and have it play at `targetBpm`, the ratio is sourceBpm/targetBpm --
// a faster target means a shorter file.
inline bool render(const float* const* in, int inFrames, int channels, double sampleRate,
                   double lengthRatio, float* const* out, int outFrames, std::string& error)
{
    if (inFrames <= 0 || channels <= 0 || outFrames <= 0 || sampleRate <= 0.0)
    { error = "nothing to stretch"; return false; }
    if (lengthRatio <= 0.05 || lengthRatio >= 20.0)
    { error = "that stretch ratio is not a musical operation"; return false; }

    signalsmith::stretch::SignalsmithStretch<float> st;
    st.presetDefault(channels, (float) sampleRate);

    const int skip = outputDelayFrames(st.inputLatency(), st.outputLatency(), lengthRatio);

    // PAD THE INPUT, don't shorten the output. The library's ratio for a call IS
    // outputSamples/inputSamples, so asking for the extra `skip` frames out of the same
    // input would quietly change the stretch amount -- the take would come back at the
    // wrong tempo and the ratio printed on screen would be a lie. Padding both sides keeps
    // the ratio exactly what was asked for and buys the run-in and run-out separately.
    const int padIn = st.inputLatency() + (int) std::ceil(st.outputLatency() / lengthRatio) + 64;
    const int paddedIn = inFrames + padIn;
    const int paddedOut = outputFramesFor(paddedIn, lengthRatio);
    if (paddedOut < skip + outFrames)
    { error = "internal: not enough run-out to compensate the stretcher's latency"; return false; }

    std::vector<std::vector<float>> inBuf((size_t) channels), outBuf((size_t) channels);
    std::vector<float*> inPtr((size_t) channels), outPtr((size_t) channels);
    for (int c = 0; c < channels; ++c)
    {
        inBuf[(size_t) c].assign((size_t) paddedIn, 0.0f);
        // Silence past the end, not a repeat of the tail: a stretcher run into looped audio
        // invents a transient at the join that was never in the take.
        std::copy(in[c], in[c] + inFrames, inBuf[(size_t) c].begin());
        outBuf[(size_t) c].assign((size_t) paddedOut, 0.0f);
        inPtr[(size_t) c] = inBuf[(size_t) c].data();
        outPtr[(size_t) c] = outBuf[(size_t) c].data();
    }

    st.process(inPtr.data(), paddedIn, outPtr.data(), paddedOut);

    for (int c = 0; c < channels; ++c)
        std::copy(outBuf[(size_t) c].begin() + skip,
                  outBuf[(size_t) c].begin() + skip + outFrames, out[c]);
    return true;
}

} // namespace mira::stretch
