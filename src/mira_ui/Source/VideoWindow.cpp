#include "VideoWindow.h"

namespace mira::canvas {

// The thing inside the window: black, with two players stacked in it and one of them
// visible. Black rather than mira's surface colour because everything around a frame
// changes how you read it, and a grader would not thank us for a grey surround.
//
// Black is also what Phase 4.4 asks for in the GAP between clips: both players hidden and
// this showing through, rather than the last frame held, which reads as "the picture has
// stopped following" at exactly the moment it has not.
struct VideoWindow::Screen : juce::Component
{
    Screen() : a(false), b(false)   // false = NO native transport controls
    {
        addChildComponent(a);
        addChildComponent(b);
        setOpaque(true);
    }
    void paint(juce::Graphics& g) override { g.fillAll(juce::Colours::black); }
    void resized() override { a.setBounds(getLocalBounds()); b.setBounds(getLocalBounds()); }
    juce::VideoComponent a, b;
    int front = 0;   // 0 = a is the one you are watching, 1 = b
};

VideoWindow::VideoWindow()
    : juce::DocumentWindow("Picture", juce::Colours::black, juce::DocumentWindow::closeButton)
{
    screen = std::make_unique<Screen>();
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
    screen->a.closeVideo();
    screen->b.closeVideo();
}

juce::VideoComponent& VideoWindow::shown() { return screen->front == 0 ? screen->a : screen->b; }
juce::VideoComponent& VideoWindow::spare() { return screen->front == 0 ? screen->b : screen->a; }

void VideoWindow::showNothing()
{
    if (picturePlaying) { shown().stop(); picturePlaying = false; }
    screen->a.setVisible(false);
    screen->b.setVisible(false);
}

void VideoWindow::unload()
{
    phase = Phase::Idle;
    picturePlaying = false;
    parkedAt = -1.0;
    startLatency = -1.0;
    shownClip = readyClip = -1;
    clips.clear();
    screen->a.closeVideo();
    screen->b.closeVideo();
    screen->a.setVisible(false);
    screen->b.setVisible(false);
    setName("Picture");
}

void VideoWindow::setClips(std::vector<Clip> newClips)
{
    // Only the LIST changes here. WHICH clip is loaded is decided by the playhead in
    // timerCallback, so adding a clip to the end of the track never disturbs the one you
    // are watching.
    const auto wasShowing = juce::isPositiveAndBelow(shownClip, (int) clips.size())
                                ? clips[(size_t) shownClip].file : juce::File();
    clips = std::move(newClips);

    if (clips.empty()) { unload(); return; }

    // Keep watching the same film if it is still on the track, even if its index moved.
    shownClip = -1;
    for (int i = 0; i < (int) clips.size(); ++i)
        if (clips[(size_t) i].file.getFullPathName() == wasShowing.getFullPathName()
            && wasShowing.getFullPathName().isNotEmpty())
        { shownClip = i; break; }
    readyClip = -1;
}

bool VideoWindow::prepare(int index)
{
    if (!juce::isPositiveAndBelow(index, (int) clips.size())) return false;
    const auto& c = clips[(size_t) index];
    if (!c.file.existsAsFile())
    {
        if (onNote) onNote("the film is not where the project left it: " + c.file.getFullPathName());
        return false;
    }

    auto& player = spare();
    const auto err = player.load(c.file);
    if (err.failed())
    {
        if (onNote) onNote("AVPlayer could not open " + c.file.getFileName() + " - " + err.getErrorMessage());
        return false;
    }
    // The film's own audio never sounds: one audio clock in the system, and mira's mixer
    // reads the film's audio as a block like any other file (Phase 2).
    player.setAudioVolume(0.0f);
    player.setPlaySpeed(1.0);
    player.setPlayPosition(juce::jmax(0.0, c.sourceOffset));
    readyClip = index;
    return true;
}

void VideoWindow::swapPlayers()
{
    auto& goingAway = shown();
    goingAway.stop();
    goingAway.setVisible(false);
    screen->front = screen->front == 0 ? 1 : 0;
    shown().setVisible(true);
    picturePlaying = false;
    parkedAt = -1.0;
    speedNudged = false;
    shownClip = readyClip;
    readyClip = -1;
}

int VideoWindow::clipAt(double seconds) const
{
    for (int i = 0; i < (int) clips.size(); ++i)
    {
        const auto& c = clips[(size_t) i];
        if (seconds >= c.start && (c.length <= 0.0 || seconds < c.end())) return i;
    }
    return -1;
}

int VideoWindow::clipAfter(double seconds) const
{
    int best = -1;
    for (int i = 0; i < (int) clips.size(); ++i)
        if (clips[(size_t) i].start > seconds
            && (best < 0 || clips[(size_t) i].start < clips[(size_t) best].start))
            best = i;
    return best;
}

void VideoWindow::seekPicture(double filePosition)
{
    const double lag = startLatency > 0.0 ? startLatency : 0.0;
    const double to = juce::jmax(0.0, filePosition + (picturePlaying ? lag : 0.0));
    shown().setPlayPosition(to);
}

void VideoWindow::calibrateTick(double nowMs)
{
    const double elapsed = (nowMs - calibrateStartMs) / 1000.0;
    const double advanced = shown().getPlayPosition() - calibrateFrom;

    if (elapsed >= 1.0 && advanced > 0.05)
    {
        // Wall clock over one second is a fine stand-in for the audio clock here: Phase
        // 0.2 measured 8 ms of divergence over 700 seconds, so one second of it is noise
        // three orders of magnitude below what is being measured.
        startLatency = juce::jlimit(0.0, 1.0, elapsed - advanced);
        shown().stop();
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
        shown().stop();
        picturePlaying = false;
        phase = Phase::Ready;
        parkedAt = -1.0;
        if (onNote) onNote("the picture never started playing - sync is uncompensated");
    }
}

void VideoWindow::timerCallback()
{
    if (clips.empty()) return;

    const double nowMs = juce::Time::getMillisecondCounterHiRes();
    const double t = transportPosition ? transportPosition() : 0.0;
    const bool playing = transportPlaying && transportPlaying();
    const int want = clipAt(t);

    // --- the gap between clips is BLACK (4.4), not the last frame held. A held frame
    // reads as "the picture has stopped following" at exactly the moment it has not.
    if (want < 0)
    {
        if (shownClip >= 0 || screen->a.isVisible() || screen->b.isVisible()) showNothing();
        shownClip = -1;
        // Still worth having the next reel ready: a gap is usually the run-up to the clip
        // that follows it.
        if (const int next = clipAfter(t); next >= 0 && next != readyClip
            && clips[(size_t) next].start - t < kPreloadLead)
            prepare(next);
        return;
    }

    // --- a reel change
    if (want != shownClip)
    {
        if (want == readyClip)
        {
            swapPlayers();                      // free: already decoded and parked
        }
        else
        {
            // Not pre-loaded -- a seek straight into the middle of another reel. Load it
            // now and SAY so: this is the case the pre-load exists to avoid, and knowing
            // when it did not happen is how the lead time gets tuned rather than guessed.
            if (!prepare(want)) { showNothing(); shownClip = -1; return; }
            swapPlayers();
            if (onNote && clips.size() > 1)
                onNote("loaded " + clips[(size_t) want].file.getFileName() + " on the jump");
        }
        shown().setVisible(true);
        setName(clips[(size_t) want].file.getFileName());

        // The latency is a property of the player and the device, not of the film, so it
        // is measured once for the window and reused across reels.
        if (startLatency < 0.0 && phase != Phase::Calibrating)
        {
            phase = Phase::Calibrating;
            calibrateFrom = juce::jmax(0.0, clips[(size_t) want].sourceOffset);
            shown().setPlayPosition(calibrateFrom);
            shown().play();
            picturePlaying = true;
            calibrateStartMs = nowMs;
            return;
        }
        phase = Phase::Ready;
    }

    if (phase == Phase::Calibrating) { calibrateTick(nowMs); return; }
    if (startLatency < 0.0 || !juce::isPositiveAndBelow(shownClip, (int) clips.size())) return;

    const auto& c = clips[(size_t) shownClip];
    const double target = t - c.start + c.sourceOffset;   // where in THIS film we should be

    // --- pre-load the next reel as the boundary approaches (4.2)
    if (const int next = clipAfter(t); next >= 0 && next != readyClip
        && clips[(size_t) next].start - t < kPreloadLead)
        prepare(next);

    if (playing != picturePlaying)
    {
        if (playing)
        {
            picturePlaying = true;          // set first: seekPicture adds the lag only when playing
            seekPicture(target);
            shown().setPlaySpeed(1.0);
            speedNudged = false;
            shown().play();
            lastCheckMs = nowMs;
        }
        else
        {
            shown().stop();
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

    // Playing, and in sync until proven otherwise. Half a second between checks: Phase 0.2
    // says nothing accumulates, so checking faster would only measure AVPlayer's own
    // position quantisation.
    if (nowMs - lastCheckMs < 500.0) return;
    lastCheckMs = nowMs;

    const double error = shown().getPlayPosition() - (target + juce::jmax(0.0, startLatency));
    const double frame = 1.0 / 25.0;

    if (std::abs(error) >= 1.0)
    {
        // A second out is not drift. It is a seek we missed, a stall, or the file running
        // out -- take the position rather than easing towards it.
        seekPicture(target);
        shown().setPlaySpeed(1.0);
        speedNudged = false;
    }
    else if (std::abs(error) >= frame * 0.5)
    {
        // The safety net. Ease back rather than seeking, because a seek during playback is
        // a visible jump and this error is not.
        shown().setPlaySpeed(juce::jlimit(0.95, 1.05, 1.0 - 0.5 * error));
        speedNudged = true;
    }
    else if (speedNudged)
    {
        shown().setPlaySpeed(1.0);
        speedNudged = false;
    }
}

} // namespace mira::canvas
