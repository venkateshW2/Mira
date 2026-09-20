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
    return 0;
}
