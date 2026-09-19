#include "VideoWindow.h"

namespace mira::canvas {

// The thing inside the window: black, and the picture letterboxed into it. Black rather
// than mira's surface colour because everything around a frame changes how you read it,
// and a grader would not thank us for a grey surround.
struct VideoWindow::Screen : juce::Component
{
    Screen() : video(false)   // false = NO native transport controls
    {
        addAndMakeVisible(video);
        setOpaque(true);
    }
    void paint(juce::Graphics& g) override { g.fillAll(juce::Colours::black); }
    void resized() override { video.setBounds(getLocalBounds()); }
    juce::VideoComponent video;
};

VideoWindow::VideoWindow()
    : juce::DocumentWindow("Picture", juce::Colours::black, juce::DocumentWindow::closeButton)
{
    screen = std::make_unique<Screen>();
    video = &screen->video;
    setUsingNativeTitleBar(true);
    setContentNonOwned(screen.get(), false);
    setResizable(true, false);
    centreWithSize(640, 380);
    // Above mira, as MIRA-VIDEO.md §2 decided: you look at the picture and reach for the
    // canvas, so the canvas must not be able to cover it.
    setAlwaysOnTop(true);
    setVisible(true);
    startTimerHz(25);
}

VideoWindow::~VideoWindow()
{
    stopTimer();
    if (video != nullptr) video->closeVideo();
}

void VideoWindow::setPlacement(double startOnTimeline, double offsetIntoFilm)
{
    clipStart = startOnTimeline;
    sourceOffset = offsetIntoFilm;
}

void VideoWindow::unload()
{
    phase = Phase::Idle;
    picturePlaying = false;
    parkedAt = -1.0;
    startLatency = -1.0;
    lengthSeconds = fps = 0.0;
    videoFile = juce::File();
    if (video != nullptr) video->closeVideo();
}

juce::Result VideoWindow::loadVideo(const juce::File& file)
{
    if (!file.existsAsFile())
        return juce::Result::fail("no such file: " + file.getFullPathName());

    unload();
    const auto err = video->load(file);
    if (err.failed())
        return juce::Result::fail("AVPlayer could not open " + file.getFileName()
                                   + " - " + err.getErrorMessage());

    videoFile = file;
    setName(file.getFileName());

    // 1.4b -- the length from AVFoundation, not from getVideoDuration().
    juce::String probeError;
    if (video_native::probe(file, lengthSeconds, fps, probeError))
    {
        if (onNote)
            onNote("picture: " + juce::String(lengthSeconds, 2) + " s at "
                    + juce::String(fps, 3) + " fps (AVAsset)");
    }
    else
    {
        // Not fatal -- the picture plays regardless -- but it has to be SAID, because a
        // clip whose length is unknown cannot be drawn on the timeline and the timecode
        // ruler has no frame rate to count in. A partial answer is kept: probe() fills in
        // whatever it managed before it gave up, and a length with no frame rate is still
        // a length.
        if (onNote)
            onNote("picture loaded, " + probeError
                    + (lengthSeconds > 0.0 ? " (length " + juce::String(lengthSeconds, 2) + " s)"
                                           : juce::String()));
    }

    // The film's own audio never sounds. One audio clock in the system: mira's mixer
    // reads the film's audio as a block like any other file (Phase 2).
    video->setAudioVolume(0.0f);

    // 1.4 -- measure THIS machine's start latency, now, rather than shipping the -290 ms
    // one laptop measured.
    phase = Phase::Calibrating;
    calibrateFrom = lengthSeconds > 4.0 ? 1.0 : 0.0;
    video->setPlayPosition(calibrateFrom);
    video->setPlaySpeed(1.0);
    video->play();
    picturePlaying = true;
    calibrateStartMs = juce::Time::getMillisecondCounterHiRes();
    return juce::Result::ok();
}

void VideoWindow::seekPicture(double filePosition)
{
    const double lag = startLatency > 0.0 ? startLatency : 0.0;
    const double to = juce::jmax(0.0, filePosition + (picturePlaying ? lag : 0.0));
    video->setPlayPosition(lengthSeconds > 0.0 ? juce::jmin(to, lengthSeconds - 0.001) : to);
}

void VideoWindow::calibrateTick(double nowMs)
{
    const double elapsed = (nowMs - calibrateStartMs) / 1000.0;
    const double advanced = video->getPlayPosition() - calibrateFrom;

    if (elapsed >= 1.0 && advanced > 0.05)
    {
        // Wall clock over one second is a fine stand-in for the audio clock here: Phase
        // 0.2 measured 8 ms of divergence over 700 seconds, so one second of it is noise
        // three orders of magnitude below what is being measured.
        startLatency = juce::jlimit(0.0, 1.0, elapsed - advanced);
        video->stop();
        picturePlaying = false;
        phase = Phase::Ready;
        parkedAt = -1.0;
        if (onNote)
            onNote("picture start latency " + juce::String(startLatency * 1000.0, 0)
                    + " ms - compensated on every locate");
        return;
    }

    if (elapsed > 3.0)
    {
        // The picture never moved. Saying nothing here would leave sync silently
        // uncompensated and blame the file later (convention 6).
        startLatency = 0.0;
        video->stop();
        picturePlaying = false;
        phase = Phase::Ready;
        parkedAt = -1.0;
        if (onNote) onNote("the picture never started playing - sync is uncompensated");
    }
}

void VideoWindow::timerCallback()
{
    if (video == nullptr || videoFile.getFullPathName().isEmpty()) return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();

    if (phase == Phase::Calibrating) { calibrateTick(nowMs); return; }
    if (phase != Phase::Ready) return;

    const double t = transportPosition ? transportPosition() : 0.0;
    const bool playing = transportPlaying && transportPlaying();
    const double target = t - clipStart + sourceOffset;   // where in the FILM we should be

    // Off the end of the clip, or before its start: there is nothing to show, so stop
    // rather than run the picture past its own material.
    const bool inside = target >= 0.0 && (lengthSeconds <= 0.0 || target <= lengthSeconds);
    if (!inside)
    {
        if (picturePlaying) { video->stop(); picturePlaying = false; }
        return;
    }

    if (playing != picturePlaying)
    {
        if (playing)
        {
            picturePlaying = true;          // set first: seekPicture adds the lag only when playing
            seekPicture(target);
            video->setPlaySpeed(1.0);
            speedNudged = false;
            video->play();
            lastCheckMs = nowMs;
        }
        else
        {
            video->stop();
            picturePlaying = false;
            // 1.5 -- stop parks the picture AT the transport position, with no lag
            // compensation: nothing is moving, so there is nothing to be late for.
            seekPicture(target);
            parkedAt = target;
        }
        return;
    }

    if (!playing)
    {
        // Scrubbing. The playhead moved with the transport stopped, so follow it.
        if (parkedAt < 0.0 || std::abs(target - parkedAt) > 0.02)
        {
            seekPicture(target);
            parkedAt = target;
        }
        return;
    }

    // Playing, and in sync until proven otherwise. Half a second between checks: Phase
    // 0.2 says nothing accumulates, so checking faster would only measure AVPlayer's own
    // position quantisation.
    if (nowMs - lastCheckMs < 500.0) return;
    lastCheckMs = nowMs;

    const double error = video->getPlayPosition() - (target + juce::jmax(0.0, startLatency));
    const double frame = fps > 0.0 ? 1.0 / fps : 1.0 / 25.0;

    if (std::abs(error) >= 1.0)
    {
        // A second out is not drift. It is a seek we missed, a stall, or the file
        // running out -- take the position rather than easing towards it.
        seekPicture(target);
        video->setPlaySpeed(1.0);
        speedNudged = false;
    }
    else if (std::abs(error) >= frame * 0.5)
    {
        // The safety net. Ease back rather than seeking, because a seek during playback
        // is a visible jump and this error is not.
        video->setPlaySpeed(juce::jlimit(0.95, 1.05, 1.0 - 0.5 * error));
        speedNudged = true;
    }
    else if (speedNudged)
    {
        video->setPlaySpeed(1.0);
        speedNudged = false;
    }
}

} // namespace mira::canvas
