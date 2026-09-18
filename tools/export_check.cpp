// Headless check for the Phase 6 renderer. Not a unit-test framework: it writes real
// files and reads them back, because the two things most likely to be wrong here --
// the sample rate on disk and the fade arithmetic -- are only observable in the output.
#include "../src/mira_ui/Source/Export.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <cstdio>

static int failures = 0;
static void check(bool ok, const juce::String& what) {
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.toRawUTF8());
    if (!ok) ++failures;
}

int main(int argc, char** argv) {
    juce::ignoreUnused(argc, argv);
    using namespace mira::ui;

    // ---- naming (MIRA-GENERATE.md §3.7) --------------------------------------------
    check(slugify("Short Film") == "shortfilm", "slugify drops spaces and case");
    check(keySlug("A minor") == "Amin", "A minor -> Amin");
    check(keySlug("F# major") == "F#maj", "F# major -> F#maj");
    check(keySlug("") == "", "no key -> empty, never invented");
    check(deliveryName("Short Film", "cue01", 120.4, "A minor", 1)
              == "shortfilm_cue01_120bpm_Amin_v1.wav", "full name, bpm rounded");
    check(deliveryName("Short Film", "cue01", std::nullopt, "A minor", 1)
              == "shortfilm_cue01_Amin_v1.wav", "missing bpm drops its token");
    check(deliveryName("Short Film", "cue01", std::nullopt, "", 0)
              == "shortfilm_cue01.wav", "no key, no version -> both tokens gone");
    check(versionFromWorkingName("bangeraction_cue01_v4.wav") == 4, "version read from name");
    check(versionFromWorkingName("lgr-s29991-20260918.wav") == 0, "no version -> 0");

    // ---- render --------------------------------------------------------------------
    auto tmp = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("mira-export-check");
    tmp.deleteRecursively();
    tmp.createDirectory();

    // A 4 s 44,100 Hz stereo source at exactly 0.5 amplitude, so every number below is
    // one a human can check by hand.
    const double sr = 44100.0;
    const int len = static_cast<int>(sr * 4.0);
    juce::AudioBuffer<float> tone (2, len);
    for (int ch = 0; ch < 2; ++ch)
        for (int i = 0; i < len; ++i)
            tone.setSample(ch, i, 0.5f * std::sin(juce::MathConstants<float>::twoPi * 440.0f * i / 44100.0f));

    auto src = tmp.getChildFile("source.wav");
    {
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::FileOutputStream> os (src.createOutputStream());
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor(os.get(), sr, 2, 24, {}, 0));
        os.release();
        w->writeFromAudioSampleBuffer(tone, 0, len);
    }

    auto readBack = [](const juce::File& f, juce::AudioBuffer<float>& into, double& rate, int& bits) {
        juce::AudioFormatManager fm; fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor(f));
        if (r == nullptr) return false;
        rate = r->sampleRate; bits = static_cast<int>(r->bitsPerSample);
        into.setSize(static_cast<int>(r->numChannels), static_cast<int>(r->lengthInSamples));
        r->read(&into, 0, static_cast<int>(r->lengthInSamples), 0, true, true);
        return true;
    };

    // 1. whole file, no edit
    {
        auto dst = tmp.getChildFile("whole.wav");
        auto res = renderTake(src, dst, {});
        juce::AudioBuffer<float> b; double rate = 0; int bits = 0;
        check(res.ok && readBack(dst, b, rate, bits), "renders with no edit");
        check(rate == 44100.0, "sample rate preserved at 44100, never resampled");
        check(bits == 24, "bit depth follows the source (24)");
        check(std::abs(b.getNumSamples() - len) <= 1, "length preserved");
    }

    // 2. trim 1.0 - 3.0 s
    {
        auto dst = tmp.getChildFile("trim.wav");
        TakeEdit e; e.startSeconds = 1.0; e.endSeconds = 3.0;
        auto res = renderTake(src, dst, e);
        juce::AudioBuffer<float> b; double rate = 0; int bits = 0;
        check(res.ok && readBack(dst, b, rate, bits), "renders a trim");
        check(std::abs(b.getNumSamples() - static_cast<int>(sr * 2.0)) <= 1,
              "trim length is exactly 2.0 s");
    }

    // 3. gain -6 dB halves the amplitude
    {
        auto dst = tmp.getChildFile("gain.wav");
        TakeEdit e; e.gainDb = -6.0206;
        auto res = renderTake(src, dst, e);
        juce::AudioBuffer<float> b; double rate = 0; int bits = 0;
        check(res.ok && readBack(dst, b, rate, bits), "renders a gain change");
        check(std::abs(b.getMagnitude(0, 0, b.getNumSamples()) - 0.25f) < 0.005f,
              "-6 dB takes 0.5 peak to 0.25");
    }

    // 4. fades: silent at the very start and end, full in the middle
    {
        auto dst = tmp.getChildFile("fades.wav");
        TakeEdit e; e.fadeInSeconds = 1.0; e.fadeOutSeconds = 1.0;
        auto res = renderTake(src, dst, e);
        juce::AudioBuffer<float> b; double rate = 0; int bits = 0;
        check(res.ok && readBack(dst, b, rate, bits), "renders fades");
        const int n = b.getNumSamples();
        check(std::abs(b.getSample(0, 0)) < 0.001f, "fade in starts at silence");
        check(std::abs(b.getSample(0, n - 1)) < 0.01f, "fade out ends at silence");
        // Half way through a 1 s linear fade the envelope is 0.5, so the peak over the
        // window around 0.5 s is half the source's 0.5.
        const float halfway = b.getMagnitude(0, static_cast<int>(sr * 0.48), static_cast<int>(sr * 0.04));
        check(std::abs(halfway - 0.25f) < 0.02f, "linear fade is at 0.5 halfway through");
        check(std::abs(b.getMagnitude(0, static_cast<int>(sr * 2.0), 1000) - 0.5f) < 0.01f,
              "untouched in the middle");
    }

    // 5. two fades longer than the take are squeezed, not multiplied into a dip
    {
        auto dst = tmp.getChildFile("squeeze.wav");
        TakeEdit e; e.startSeconds = 0.0; e.endSeconds = 2.0;
        e.fadeInSeconds = 3.0; e.fadeOutSeconds = 3.0;
        auto res = renderTake(src, dst, e);
        juce::AudioBuffer<float> b; double rate = 0; int bits = 0;
        check(res.ok && readBack(dst, b, rate, bits), "renders over-long fades");
        check(b.getMagnitude(0, 0, b.getNumSamples()) > 0.4f,
              "the peak still reaches the crossover, not a dip");
    }

    // 6. re-export overwrites, never _1
    {
        auto dst = tmp.getChildFile("once.wav");
        renderTake(src, dst, {});
        const auto first = dst.getSize();
        TakeEdit e; e.endSeconds = 1.0;
        renderTake(src, dst, e);
        check(dst.getSize() < first, "re-export replaced the file in place");
        check(!tmp.getChildFile("once_1.wav").exists() && !tmp.getChildFile("once(1).wav").exists(),
              "no _1 sibling was created");
    }

    // 7. a missing source reports rather than writing something
    {
        auto res = renderTake(tmp.getChildFile("nope.wav"), tmp.getChildFile("out.wav"), {});
        check(!res.ok && res.message.contains("gone"), "missing source reports, never silently no-ops");
    }

    tmp.deleteRecursively();
    std::printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES ABOVE");
    return failures == 0 ? 0 : 1;
}
