// MIRA-VIDEO.md Phase 0 -- the three questions, measured rather than assumed.
//
// Usage:  spike_video_sync <video file> [seconds to run 0.2 for]
//
// Prints its answers to stderr and exits. Nothing here is meant to survive into mira; it
// exists to decide what mira should be built to do.

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_video/juce_video.h>

namespace {

void line (const juce::String& s) { std::cerr << s << std::endl; }
void head (const juce::String& s) { std::cerr << "\n=== " << s << std::endl; }

// ---- 0.1 -----------------------------------------------------------------------------
// Does registerBasicFormats() give us something that can open an .mp4? If it does, the
// film's audio is just a file mira already knows how to read, and MIRA-VIDEO.md Phase 2.1
// has no demux step in it at all.
bool question01 (const juce::File& video)
{
    head ("0.1  can mira read the audio out of the video?");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    static bool listedFormats = false;
    if (!listedFormats)
    {
        for (auto* f : formats)
            line ("   format: " + f->getFormatName() + "   " + f->getFileExtensions().joinIntoString(" "));
        listedFormats = true;
    }

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (video));
    if (reader == nullptr)
    {
        line ("   -> NO reader for " + video.getFileName());
        line ("   => Phase 2.1 needs an AVAssetReader pass that writes a wav beside the project.");
        return false;
    }

    const double seconds = reader->lengthInSamples / reader->sampleRate;
    line ("   -> reader: " + juce::String (reader->sampleRate, 0) + " Hz, "
          + juce::String ((int) reader->numChannels) + " ch, "
          + juce::String (seconds, 2) + " s, " + juce::String ((int) reader->bitsPerSample) + " bits");

    // Opening it is not reading it. A format that reports a length and then returns
    // silence would pass a header check and fail in the only way that matters.
    juce::AudioBuffer<float> buffer ((int) juce::jmax (1u, reader->numChannels), 65536);
    const auto from = (juce::int64) (reader->lengthInSamples / 3);   // a third in, past any leader
    buffer.clear();
    const bool ok = reader->read (&buffer, 0, buffer.getNumSamples(), from, true, true);
    float peak = 0.0f;
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        peak = juce::jmax (peak, buffer.getMagnitude (c, 0, buffer.getNumSamples()));
    line (juce::String ("   -> read ok: ") + (ok ? "yes" : "NO") + ", peak over 1.5 s at 1/3 in: "
          + juce::String (juce::Decibels::gainToDecibels (juce::jmax (peak, 1.0e-6f)), 1) + " dBFS");

    if (ok && peak > 0.0001f)
        line ("   => YES. The reference track is a block pointing at the video file.");
    else
        line ("   => opens but reads silence -- treat as NO.");
    return ok && peak > 0.0001f;
}

// ---- 0.3 -----------------------------------------------------------------------------
// The first thumbnail is the only part of loading a long film that is O(length). If it is
// minutes, Phase 1 needs progress and a cache before it needs anything else.
void question03 (const juce::File& video)
{
    head ("0.3  how long does the first waveform take?");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    juce::AudioThumbnailCache cache (1);
    juce::AudioThumbnail thumb (512, formats, cache);

    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    thumb.setSource (new juce::FileInputSource (video));
    // setSource is asynchronous: it hands the work to a background thread and returns.
    // Polling getProportionComplete is what actually measures the read.
    // 0.999, NOT 1.0. getProportionComplete never quite reaches 1.0 -- it sat at 0.9999
    // for the full 120 s timeout on the first run while the actual read had finished in
    // under two seconds. Waiting for exactly 1.0 measures the timeout, not the work.
    double lastReport = 0.0;
    while (thumb.getProportionComplete() < 0.999)
    {
        const auto elapsed = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        if (elapsed - lastReport >= 0.25)
        {
            line ("   " + juce::String (elapsed, 2) + "s  "
                  + juce::String (thumb.getProportionComplete() * 100.0, 1) + "%");
            lastReport = elapsed;
        }
        if (elapsed > 120.0) { line ("   giving up at 120 s"); break; }
        juce::Thread::sleep (10);
    }
    const auto took = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
    line ("   -> " + juce::String (took, 2) + " s for " + juce::String (thumb.getTotalLength(), 1)
          + " s of audio  (" + juce::String (thumb.getTotalLength() / juce::jmax (took, 0.001), 0) + "x realtime)");
    line ("   -> extrapolated to 40 min: " + juce::String (2400.0 / juce::jmax (thumb.getTotalLength() / juce::jmax (took, 0.001), 0.001), 1) + " s");
}

// ---- 0.2 -----------------------------------------------------------------------------
// THE ONE THAT DECIDES THE DESIGN. Two clocks -- the audio device and AVPlayer -- and the
// plan says the device is master and the picture chases. This measures how far apart they
// get with NO correction at all, which is the number every threshold in MIRA-VIDEO.md §3
// has to be chosen against.
class DriftTest : public juce::Component, private juce::Timer
{
public:
    DriftTest (const juce::File& videoFile, double runSeconds)
        : video (false), runFor (runSeconds)
    {
        addAndMakeVisible (video);
        setSize (640, 360);
        setVisible (true);
        addToDesktop (juce::ComponentPeer::windowHasTitleBar);

        // The audio device is the clock. Nothing is played through it -- an empty callback
        // still runs at the device's real rate, which is the only thing being compared.
        device.initialiseWithDefaultDevices (0, 2);
        device.addAudioCallback (&clock);

        const auto err = video.load (videoFile);
        if (err.failed()) { line ("   -> could not load video: " + err.getErrorMessage()); done = true; return; }

        // getVideoDuration() returned 0.00 on the first run: load() is asynchronous on
        // macOS and the asset's duration is not known the instant it returns. Polled, with
        // a bound -- a player that never reports a duration is a real failure, not a wait.
        // Thread::sleep, not a nested dispatch loop: JUCE_MODAL_LOOPS_PERMITTED is off in
        // this build, and AVPlayer's duration is filled in by its own KVO on another
        // thread -- it does not need our message loop to be pumped.
        for (int i = 0; i < 100 && video.getVideoDuration() <= 0.0; ++i)
            juce::Thread::sleep (50);
        line ("   video duration " + juce::String (video.getVideoDuration(), 2) + " s"
              + (video.getVideoDuration() <= 0.0 ? "   (never reported!)" : ""));
        line ("   device rate    " + juce::String (device.getCurrentAudioDevice() != nullptr
                                                    ? device.getCurrentAudioDevice()->getCurrentSampleRate() : 0.0, 0) + " Hz");
        video.setAudioVolume (0.0f);     // muted, as the plan requires: one audio clock
        video.setPlayPosition (0.0);
        video.play();
        clock.reset();
        startTimerHz (5);
    }

    ~DriftTest() override { device.removeAudioCallback (&clock); }

    bool finished() const { return done; }

    void resized() override { video.setBounds (getLocalBounds()); }

private:
    // Counts samples the device has actually asked for. That IS the audio clock -- not
    // wall time, which is what a drift test must not use, and not the transport, which
    // does not exist in this spike.
    struct DeviceClock : juce::AudioIODeviceCallback
    {
        std::atomic<juce::int64> samples { 0 };
        std::atomic<double> rate { 0.0 };
        void reset() { samples = 0; }
        double seconds() const { const double r = rate.load(); return r > 0.0 ? samples.load() / r : 0.0; }

        void audioDeviceIOCallbackWithContext (const float* const*, int, float* const* out, int numOut,
                                               int numSamples, const juce::AudioIODeviceCallbackContext&) override
        {
            for (int c = 0; c < numOut; ++c)
                if (out[c] != nullptr) juce::FloatVectorOperations::clear (out[c], numSamples);
            samples += numSamples;
        }
        void audioDeviceAboutToStart (juce::AudioIODevice* d) override { rate = d->getCurrentSampleRate(); }
        void audioDeviceStopped() override {}
    };

    void timerCallback() override
    {
        const double audio = clock.seconds();
        const double picture = video.getPlayPosition();
        const double error = picture - audio;

        // OFFSET AND DRIFT ARE DIFFERENT PROBLEMS, and the first run conflated them. The
        // error settled at a constant -292 ms and stayed there, which is START LATENCY --
        // play() takes time to actually produce a frame -- not a diverging clock. A fixed
        // offset is corrected once; drift has to be chased forever. Reporting the second
        // as though it were the first is how a 5 ms problem gets called a 12-second one.
        if (offset == 0.0 && audio >= 5.0) { offset = error; line ("   settled offset "
            + juce::String (offset * 1000.0, 1) + " ms  (start latency, corrected once)"); }

        if (offset != 0.0)
        {
            const double drift = error - offset;
            worst = juce::jmax (worst, std::abs (drift));
            sum += drift; ++count;
            if (audio - lastReport >= 30.0)
            {
                line ("   audio " + juce::String (audio, 1) + " s   drift from offset "
                      + juce::String (drift * 1000.0, 1) + " ms   ("
                      + juce::String (drift / (1.0 / 25.0), 2) + " frames at 25)   worst so far "
                      + juce::String (worst * 1000.0, 1) + " ms");
                lastReport = audio;
            }
        }

        if (audio >= runFor || (video.getVideoDuration() > 0.0 && picture >= video.getVideoDuration() - 0.3))
        {
            video.stop();
            stopTimer();
            const double frame = 1.0 / 25.0;
            line ("   -> ran " + juce::String (audio, 1) + " s");
            line ("   -> start latency        " + juce::String (offset * 1000.0, 1) + " ms");
            line ("   -> worst drift from it  " + juce::String (worst * 1000.0, 1) + " ms  ("
                  + juce::String (worst / frame, 2) + " frames at 25)");
            line ("   -> mean drift           " + juce::String (count > 0 ? sum / count * 1000.0 : 0.0, 1) + " ms");
            line (worst < frame * 0.5
                      ? "   => within half a frame for the whole run: a one-time offset is enough."
                      : "   => exceeds half a frame: the chase loop of MIRA-VIDEO.md §3 is needed.");
            done = true;
        }
    }

    juce::VideoComponent video;
    juce::AudioDeviceManager device;
    DeviceClock clock;
    double runFor, lastReport = 0.0, worst = 0.0, offset = 0.0, sum = 0.0;
    int count = 0;
    bool done = false;
};

} // namespace

class SpikeApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "mira video sync spike"; }
    const juce::String getApplicationVersion() override { return "0.1"; }

    void initialise (const juce::String& commandLine) override
    {
        auto args = juce::ArgumentList ("spike", commandLine).arguments;
        if (args.isEmpty())
        {
            line ("usage: spike_video_sync <video file> [seconds for the drift test]");
            quit();
            return;
        }
        const juce::File video (args[0].resolveAsFile());
        const double runFor = args.size() > 1 ? args[1].text.getDoubleValue() : 60.0;

        line ("file: " + video.getFullPathName());
        if (!video.existsAsFile()) { line ("   not a file"); quit(); return; }

        const bool onlyQ1 = args.size() > 1 && args[1].text == "q1";
        question01 (video);
        if (onlyQ1) { quit(); return; }
        question03 (video);

        head ("0.2  how far does the picture drift from the audio clock, uncorrected?");
        test = std::make_unique<DriftTest> (video, runFor);
        startWatching();
    }

    void shutdown() override { test.reset(); }

private:
    void startWatching()
    {
        // Poll rather than callback: the spike's whole life is these three answers.
        juce::Timer::callAfterDelay (500, [this] {
            if (test == nullptr || test->finished()) { line (""); quit(); }
            else startWatching();
        });
    }
    std::unique_ptr<DriftTest> test;
};

START_JUCE_APPLICATION (SpikeApp)
