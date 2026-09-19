#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_video/juce_video.h>

#include <vector>

// ---- MIRA-VIDEO.md Phases 1 and 4: the picture, slaved to the transport ---------------
//
// A floating window holding the picture, with NO native controls. mira's transport is the
// only transport -- a play button on the picture would be a second one, and two transports
// that can disagree is exactly the fault this whole feature exists to avoid.
//
// It is a separate WINDOW rather than a pane in the canvas because `VideoComponent` is a
// native `AVPlayerView`: it sits above JUCE's rendering and nothing can be drawn over it.
// A pane inside the arrangement would punch a hole through the blocks and the playhead.
//
// ---- how the chase works, and why it is this simple ----------------------------------
//
// MIRA-VIDEO.md Phase 0.2 ran a muted VideoComponent against a free-running audio device
// for 700 seconds, comparing AVPlayer's position to samples the device had actually
// consumed. The result:
//
//     start latency        -290.3 ms      constant
//     worst drift from it     8.1 ms      0.20 frames at 25
//
// The error is an OFFSET, not a drift. It settles within the first seconds -- `play()`
// takes that long to put a frame up -- and then never grows: 8.0 ms at 30 s, 8.1 ms at
// 692 s. So the mechanism is a one-time seek that compensates the offset, and the
// rate-nudge is a SAFETY NET for what the spike could not produce (a system under load, a
// drive stalling), not the thing that holds sync.
//
// The offset is measured per machine at load, because -290 ms is one laptop with one
// device at 48 kHz, not a constant of nature. This machine measured 261, 276, 289, 307 and
// 362 ms across five runs -- which is also why it is measured at every load rather than
// remembered from the last one.
//
// ---- two players, because a reel change must not be a black frame --------------------
//
// Phase 4 puts several clips on the one video track, and swapping an AVPlayerItem at a
// boundary is visible. So there are TWO VideoComponents: the one you are watching, and one
// holding the next clip parked on its first frame. Crossing a boundary swaps which is
// visible, which costs nothing.
namespace mira::canvas {

class VideoWindow : public juce::DocumentWindow,
                    private juce::Timer
{
public:
    VideoWindow();
    ~VideoWindow() override;

    // One clip of picture as the window needs to know it. The canvas owns the real
    // VideoClip; this is only what it takes to put the right frame up at the right time.
    struct Clip
    {
        juce::File file;
        double start = 0.0;          // on the canvas timeline
        double length = 0.0;
        double sourceOffset = 0.0;   // where in the film `start` corresponds to
        double end() const { return start + length; }
    };

    // The whole video track, in order.
    void setClips(std::vector<Clip> newClips);
    bool hasClips() const { return !clips.empty(); }
    void unload();

    // Positive seconds: how far behind the transport the picture starts. -1 until
    // measured; 0 with a note if it could not be measured.
    double getStartLatency() const { return startLatency; }

    // The transport is the clock. Polled rather than pushed: the canvas already runs a
    // timer and a second notification path would be a second thing to keep in step.
    std::function<double()> transportPosition;
    std::function<bool()>   transportPlaying;
    // Anything worth saying out loud -- what the latency measured, a clip that would not
    // open, a reel change that had to load rather than swap.
    std::function<void(const juce::String&)> onNote;
    std::function<void()> onClosed;

    void closeButtonPressed() override { if (onClosed) onClosed(); }

    // Phase 1.6 -- geometry survives a relaunch, per project.
    juce::String geometryString() { return getWindowStateAsString(); }
    void restoreGeometry(const juce::String& s) { if (s.isNotEmpty()) restoreWindowStateFromString(s); }

private:
    void timerCallback() override;
    // One tick of the load-time latency measurement: play muted from a known position, and
    // a second later ask how far the picture actually got.
    void calibrateTick(double nowMs);
    int clipAt(double seconds) const;        // which clip covers this position, or -1
    int clipAfter(double seconds) const;     // the next clip to start, or -1
    juce::VideoComponent& shown();
    juce::VideoComponent& spare();
    bool prepare(int index);                 // load `index` into the spare, parked
    void swapPlayers();
    void seekPicture(double filePosition);
    void showNothing();

    struct Screen;
    std::unique_ptr<Screen> screen;

    std::vector<Clip> clips;
    int shownClip = -1;        // which clip the VISIBLE player holds, or -1
    int readyClip = -1;        // which clip the SPARE player holds, or -1

    double startLatency = -1.0;
    enum class Phase { Idle, Calibrating, Ready };
    Phase phase = Phase::Idle;
    double calibrateStartMs = 0.0, calibrateFrom = 0.0;

    bool picturePlaying = false;
    double lastCheckMs = 0.0, parkedAt = -1.0;
    bool speedNudged = false;
    // How far ahead of a boundary the next clip is loaded. Six seconds is comfortably more
    // than a load takes off a local disk, and short enough that scrubbing about does not
    // thrash the spare player.
    static constexpr double kPreloadLead = 6.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoWindow)
};

// AVFoundation, asked directly. `VideoComponent::getVideoDuration()` reported 0.00 for an
// entire 700-second run in Phase 0.2, so a clip's length cannot come from it.
// Implemented in VideoNative.mm.
namespace video_native {
bool probe(const juce::File& file, double& seconds, double& framesPerSecond, juce::String& error);

// MIRA-VIDEO.md Phase 2.1, the fallback route. JUCE's CoreAudioFormat reads the audio
// track of an `.mp4` and, measured in Phase 0.1, does NOT read a `.mov` -- 4 of 4 against
// 0 of 4, despite `.mov` being in its own advertised extension list and `afinfo` opening
// every one of them. So when `createReaderFor` returns nothing, Core Audio is asked
// directly and the result written beside the project as a wav.
//
// The source rate is kept (48 kHz stays 48 kHz): the canvas resamples per voice on the way
// to the device, so converting here would be a second resample nobody asked for.
//
// `progress` is called from the calling thread with 0..1; returning false cancels.
bool extractAudio(const juce::File& source, const juce::File& destinationWav,
                  juce::String& error, const std::function<bool(double)>& progress);
}

} // namespace mira::canvas
