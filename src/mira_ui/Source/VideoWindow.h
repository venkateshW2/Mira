#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_video/juce_video.h>

// ---- MIRA-VIDEO.md Phase 1: the picture, slaved to the transport ---------------------
//
// A floating window holding one `VideoComponent`, with NO native controls. mira's
// transport is the only transport -- a play button on the picture would be a second one,
// and two transports that can disagree is exactly the fault this whole phase exists to
// avoid.
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
// rate-nudge below is a SAFETY NET for what the spike could not produce (a system under
// load, a drive stalling), not the thing that holds sync.
//
// The offset is measured per machine at load (see `calibrate`), because -290 ms is one
// laptop with one device at 48 kHz, not a constant of nature.
namespace mira::canvas {

class VideoWindow : public juce::DocumentWindow,
                    private juce::Timer
{
public:
    VideoWindow();
    ~VideoWindow() override;

    // Loads the picture and starts the latency calibration. Returns a failed Result with
    // the reason -- never a silent no-op (convention 6).
    juce::Result loadVideo(const juce::File& file);
    void unload();

    juce::File getFile() const { return videoFile; }
    // The clip's length, from an AVAsset query -- NOT from `getVideoDuration()`, which
    // Phase 0.2 watched return 0.00 for 700 seconds of successful playback.
    double getClipLength() const { return lengthSeconds; }
    double getFrameRate() const { return fps; }
    // Positive seconds: how far behind the transport the picture starts. -1 until
    // measured; 0 with a note if it could not be measured.
    double getStartLatency() const { return startLatency; }

    // Where the clip sits on the canvas timeline, and where in the film that point is.
    void setPlacement(double startOnTimeline, double sourceOffset);

    // The transport is the clock. Polled rather than pushed: the canvas already runs a
    // timer and a second notification path would be a second thing to keep in step.
    std::function<double()> transportPosition;
    std::function<bool()>   transportPlaying;
    // Anything worth saying out loud -- which route the length came from, what the
    // latency measured, a picture that never started.
    std::function<void(const juce::String&)> onNote;
    std::function<void()> onClosed;

    void closeButtonPressed() override { if (onClosed) onClosed(); }

    // Phase 1.6 -- geometry survives a relaunch, per project.
    juce::String geometryString() { return getWindowStateAsString(); }
    void restoreGeometry(const juce::String& s) { if (s.isNotEmpty()) restoreWindowStateFromString(s); }

private:
    void timerCallback() override;
    // One tick of the load-time latency measurement. Plays muted from a known position
    // and asks, a second later, how far the picture actually got.
    void calibrateTick(double nowMs);
    void seekPicture(double filePosition);

    struct Screen;
    std::unique_ptr<Screen> screen;
    juce::VideoComponent* video = nullptr;   // owned by screen

    juce::File videoFile;
    double lengthSeconds = 0.0, fps = 0.0;
    double clipStart = 0.0, sourceOffset = 0.0;

    // -1 = not measured yet. Positive seconds of start lag, added to every seek.
    double startLatency = -1.0;
    enum class Phase { Idle, Calibrating, Ready };
    Phase phase = Phase::Idle;
    double calibrateStartMs = 0.0, calibrateFrom = 0.0;

    bool picturePlaying = false;
    double lastCheckMs = 0.0, parkedAt = -1.0;
    bool speedNudged = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(VideoWindow)
};

// AVFoundation, asked directly. `VideoComponent::getVideoDuration()` reported 0.00 for an
// entire 700-second run in Phase 0.2, so the clip's length cannot come from it.
// Implemented in VideoNative.mm.
namespace video_native {
bool probe(const juce::File& file, double& seconds, double& framesPerSecond, juce::String& error);
}

} // namespace mira::canvas
