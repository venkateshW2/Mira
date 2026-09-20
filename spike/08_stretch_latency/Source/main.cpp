// MIRA-BLOCKS.md step 3.1 -- signalsmith-stretch, measured before anything depends on it.
//
// THE QUESTION THAT MATTERED: a stretcher has latency, and a stretched take that comes back
// 120 ms late is a take that lands a third of a beat behind the grid it was stretched TO --
// which is the one failure step 3.4 exists to prevent ("right tempo with the wrong phase is
// the failure nobody predicts"). So the point of this spike is not "does it stretch", it is
// "by exactly how much does its output move, and is that predictable".
//
// ANSWER: the output is delayed by `inputLatency()*ratio + outputLatency()`, exactly, and
// both are 2646 samples at 44.1 kHz with presetDefault. So the compensation is arithmetic,
// not a search. See the table in MIRA-BLOCKS.md §3.
#include "signalsmith-stretch.h"
// mira's own wrapper, tested HERE rather than by reading it (convention 11).
#include "Stretch.h"
#include <cmath>
#include <cstdio>
#include <vector>

int main()
{
    const double sr = 44100.0;
    {
        signalsmith::stretch::SignalsmithStretch<float> st;
        st.presetDefault(1, (float) sr);
        std::printf("presetDefault(1ch, 44100): inputLatency %d, outputLatency %d\n\n",
                    st.inputLatency(), st.outputLatency());
    }

    std::printf("%-7s %10s %10s %10s %10s\n", "ratio", "expected", "measured", "offset", "predicted");
    for (double ratio : { 0.90, 0.95, 1.00, 1.05, 1.10 })
    {
        signalsmith::stretch::SignalsmithStretch<float> st;
        st.presetDefault(1, (float) sr);
        const int inN = (int) (sr * 3), outN = (int) (inN * ratio);
        std::vector<float> in((size_t) inN, 0.0f), out((size_t) outN, 0.0f);
        // ONE click, at exactly one second. A click is the only test signal whose position
        // in the output is not a matter of opinion.
        for (int i = 0; i < 20; ++i) in[(size_t) (sr + i)] = 1.0f;

        float* ip[1] = { in.data() };
        float* op[1] = { out.data() };
        st.process(ip, inN, op, outN);

        int peak = 0; float best = 0.0f;
        for (int i = 0; i < outN; ++i)
            if (std::abs(out[(size_t) i]) > best) { best = std::abs(out[(size_t) i]); peak = i; }

        const double expect = sr * ratio;
        const double predicted = st.inputLatency() * ratio + st.outputLatency();
        std::printf("%-7.2f %10.0f %10d %10.0f %10.0f\n",
                    ratio, expect, peak, peak - expect, predicted);
    }
    std::printf("\nThe last two columns agreeing is the whole result: the delay is\n"
                "inputLatency*ratio + outputLatency, so mira can compensate exactly.\n");

    // ---- and now the SAME test through mira::stretch::render ------------------------
    // The point of this half: the compensation above is only worth anything if the code
    // that ships actually applies it. So this asks the shipping function the same question
    // -- one click at exactly one second, where does it come out -- and the answer has to
    // be "where it should be", not "where it should be plus 120 ms".
    std::printf("\n=== mira::stretch::render (the shipping code) ===\n");
    std::printf("%-7s %10s %10s %10s %8s\n", "ratio", "expected", "measured", "error", "verdict");
    int failures = 0;
    for (double ratio : { 0.80, 0.90, 0.95, 1.00, 1.05, 1.10, 1.25 })
    {
        const int inN = (int) (sr * 3);
        const int outN = mira::stretch::outputFramesFor(inN, ratio);
        std::vector<float> in((size_t) inN, 0.0f), out((size_t) outN, 0.0f);
        for (int i = 0; i < 20; ++i) in[(size_t) (sr + i)] = 1.0f;

        const float* ip[1] = { in.data() };
        float* op[1] = { out.data() };
        std::string err;
        if (!mira::stretch::render(ip, inN, 1, sr, ratio, op, outN, err))
        { std::printf("%-7.2f  FAILED: %s\n", ratio, err.c_str()); ++failures; continue; }

        int peak = 0; float best = 0.0f;
        for (int i = 0; i < outN; ++i)
            if (std::abs(out[(size_t) i]) > best) { best = std::abs(out[(size_t) i]); peak = i; }

        const double expect = sr * ratio;
        const double errMs = (peak - expect) / sr * 1000.0;
        // 5 ms. The click is 20 samples wide, the stretcher smears a transient by design,
        // and a millisecond at 143 bpm is 1/300th of a beat -- so this is tight enough to
        // catch a missing compensation (which would be 120 ms) and loose enough not to fail
        // on the thing a phase vocoder is FOR.
        const bool ok = std::abs(errMs) <= 5.0;
        if (!ok) ++failures;
        std::printf("%-7.2f %10.0f %10d %9.1fms %8s\n", ratio, expect, peak, errMs, ok ? "ok" : "FAIL");
    }
    std::printf("\n%s\n", failures == 0
        ? "PASS -- the shipping function puts the audio where the arithmetic says."
        : "FAIL -- mira::stretch::render is not compensating its latency.");
    return failures == 0 ? 0 : 1;
}
