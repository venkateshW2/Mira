#include "WaveformView.h"

#include "ThumbnailStore.h"

#include <limits>

using namespace mira_ui;

namespace {
juce::String formatTime(double seconds)
{
    if (seconds < 0.0 || !std::isfinite(seconds)) seconds = 0.0;
    int total = static_cast<int>(seconds + 0.5);
    int m = total / 60;
    int s = total % 60;
    return juce::String::formatted("%d:%02d", m, s);
}
} // namespace

void TransportPlayButton::paintButton(juce::Graphics& g, bool isMouseOverButton, bool /*isButtonDown*/)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour(isMouseOverButton ? MiraLookAndFeel::accent.brighter(0.1f) : MiraLookAndFeel::accent);
    g.fillEllipse(bounds);

    auto iconBounds = bounds.reduced(bounds.getWidth() * 0.32f);
    g.setColour(juce::Colour(0xff1a1204)); // same dark-on-accent pairing the sidebar's own "+" button uses
    if (playing)
    {
        auto barW = iconBounds.getWidth() * 0.32f;
        g.fillRoundedRectangle(iconBounds.getX(), iconBounds.getY(), barW, iconBounds.getHeight(), 1.0f);
        g.fillRoundedRectangle(iconBounds.getRight() - barW, iconBounds.getY(), barW, iconBounds.getHeight(), 1.0f);
    }
    else
    {
        // Drawn a touch right-shifted -- a triangle's visual centroid sits left of its
        // bounding box' centre, so this keeps it looking centred in the circle.
        juce::Path tri;
        auto b = iconBounds.withX(iconBounds.getX() + iconBounds.getWidth() * 0.08f);
        tri.addTriangle(b.getX(), b.getY(), b.getX(), b.getBottom(), b.getRight(), b.getCentreY());
        g.fillPath(tri);
    }
}

void MuteButton::paintButton(juce::Graphics& g, bool isMouseOverButton, bool /*isButtonDown*/)
{
    // Simplified -- the earlier version's sound-wave arcs didn't render cleanly at this
    // size ("looks odd"). Just the speaker body + horn now, plus a strike-through when
    // muted; the slider sitting right next to it already makes "this is volume" obvious,
    // so the glyph doesn't need to carry that on its own too.
    auto bounds = getLocalBounds().toFloat().reduced(5.0f);
    g.setColour(isMouseOverButton ? MiraLookAndFeel::text : MiraLookAndFeel::textDim);

    auto bodyW = bounds.getWidth() * 0.4f;
    juce::Rectangle<float> body(bounds.getX(), bounds.getCentreY() - bounds.getHeight() * 0.2f, bodyW,
                                 bounds.getHeight() * 0.4f);
    g.fillRect(body);

    juce::Path horn;
    horn.startNewSubPath(body.getRight(), body.getY());
    horn.lineTo(bounds.getRight(), bounds.getY());
    horn.lineTo(bounds.getRight(), bounds.getBottom());
    horn.lineTo(body.getRight(), body.getBottom());
    horn.closeSubPath();
    g.fillPath(horn);

    if (muted)
        g.drawLine(bounds.getX() - 1.0f, bounds.getBottom() + 1.0f, bounds.getRight() + 1.0f, bounds.getY() - 1.0f,
                   1.6f);
}

WaveformView::WaveformView() : thumbnail(512, formatManager, thumbnailCache)
{
    formatManager.registerBasicFormats();
    thumbnail.addChangeListener(this);

    // 0 inputs, 2 outputs -- playback only, never records, so no microphone permission
    // prompt (macOS would otherwise ask the first time an input-capable device opens).
    auto err = deviceManager.initialiseWithDefaultDevices(0, 2);
    juce::ignoreUnused(err); // if this fails, playback silently won't work rather than
                             // crashing the app over it -- browsing/scanning still do.
    audioSourcePlayer.setSource(&transportSource);
    deviceManager.addAudioCallback(&audioSourcePlayer);

    playButton.onClick = [this] { togglePlayPause(); };
    addAndMakeVisible(playButton);

    muteButton.onClick = [this] {
        muted = !muted;
        muteButton.setMuted(muted);
        transportSource.setGain(static_cast<float>(muted ? 0.0 : volumeSlider.getValue()));
    };
    addAndMakeVisible(muteButton);

    volumeSlider.setSliderStyle(juce::Slider::LinearHorizontal);
    volumeSlider.setRange(0.0, 1.25, 0.01);
    volumeSlider.setValue(1.0, juce::dontSendNotification);
    volumeSlider.setTextBoxStyle(juce::Slider::NoTextBox, true, 0, 0);
    volumeSlider.setColour(juce::Slider::trackColourId, MiraLookAndFeel::accent);
    volumeSlider.setColour(juce::Slider::backgroundColourId, MiraLookAndFeel::surface3);
    volumeSlider.setColour(juce::Slider::thumbColourId, MiraLookAndFeel::text);
    volumeSlider.onValueChange = [this] {
        // Dragging the slider while muted un-mutes -- the same convention most audio
        // apps use, so "why isn't the volume I just set doing anything" can't happen.
        if (muted)
        {
            muted = false;
            muteButton.setMuted(false);
        }
        transportSource.setGain(static_cast<float>(volumeSlider.getValue()));
        updateVolumeLabel();
    };
    addAndMakeVisible(volumeSlider);

    volumePercentLabel.setFont(juce::Font(juce::FontOptions(11.5f)));
    volumePercentLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
    volumePercentLabel.setJustificationType(juce::Justification::centred);
    addAndMakeVisible(volumePercentLabel);
    updateVolumeLabel();

    timeLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
    timeLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
    timeLabel.setJustificationType(juce::Justification::centredRight);
    timeLabel.setText("0:00 / 0:00", juce::dontSendNotification);
    addAndMakeVisible(timeLabel);

    // "zoom in zoom out and reset" -- text buttons rather than custom-drawn glyphs like
    // play/mute above; a plain "+"/minus/"Fit" reads clearly at this size and doesn't
    // need the same visual weight as the primary transport controls.
    for (auto* b : { &zoomOutButton, &zoomInButton, &zoomResetButton })
    {
        b->setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
        b->setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
        addAndMakeVisible(*b);
    }
    // Lane visibility. A menu rather than three separate toggles: the lanes are mostly
    // absent anyway (32 of 108 analyzed files have chords, 29 have notes), so three
    // permanently-visible buttons would cost transport-row width for something rarely
    // touched -- and an item greys out when the current file has no data for that lane,
    // which is the honest answer to "why is nothing showing".
    lanesButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
    lanesButton.setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
    lanesButton.onClick = [this] { showLanesMenu(); };
    addAndMakeVisible(lanesButton);

    zoomOutButton.onClick = [this] { setZoom(zoomFactor / 2.0); };
    zoomInButton.onClick = [this] { setZoom(zoomFactor * 2.0); };
    zoomResetButton.onClick = [this] {
        zoomFactor = 1.0;
        viewStartFrac = 0.0;
        if (onViewChanged) onViewChanged(viewStartFrac, zoomFactor);
        repaint();
    };

    setWantsKeyboardFocus(true);
}

WaveformView::~WaveformView()
{
    thumbnail.removeChangeListener(this);
    transportSource.setSource(nullptr);
    deviceManager.removeAudioCallback(&audioSourcePlayer);
    audioSourcePlayer.setSource(nullptr);
}

void WaveformView::updateVolumeLabel()
{
    volumePercentLabel.setText(juce::String(juce::roundToInt(volumeSlider.getValue() * 100.0)) + "%",
                                juce::dontSendNotification);
}

void WaveformView::setFile(const juce::File& file)
{
    if (file == currentFile) return;

    transportSource.stop();
    transportSource.setSource(nullptr);
    readerSource.reset();
    playButton.setPlaying(false);
    stopTimer();
    hasSelection = false;
    isDraggingSelection = false;
    zoomFactor = 1.0;
    viewStartFrac = 0.0;
    // Segments and lane data belong to the file that was showing; the caller re-supplies
    // them for the new one (MainComponent's selection handler) rather than them
    // lingering. Nothing here is cleared on a *repaint* -- only on a file change, so a
    // lane never briefly shows the previous file's chords over the new file's peaks.
    segments.clear();
    activeSpans.clear();
    chords.clear();
    notes.clear();
    beats.clear();
    downbeats.clear();
    onsets.clear();
    groove = {};
    noteLowPitch = noteHighPitch = 0;
    dragMode = DragMode::none;
    if (onSelectionChanged) onSelectionChanged();

    currentFile = file;
    loadFailed = false;

    if (!file.existsAsFile())
    {
        thumbnail.clear();
        timeLabel.setText("0:00 / 0:00", juce::dontSendNotification);
        repaint();
        return;
    }

    // Persisted peaks first (ThumbnailStore) -- "a 40-minute file takes a few seconds to
    // show its waveform the first time" is exactly the several-seconds-long wait this
    // skips, and a hit means the waveform is complete before the first paint rather than
    // filling in over the following seconds.
    //
    // A miss is not an error: it falls through to setSource, which is the original lazy
    // path unchanged. That matters because the precache pass is best-effort (a file
    // added between scans, a file too new for the pass to have reached yet) and must
    // never be something the click path depends on.
    if (thumbnails::loadInto(thumbnail, file))
    {
        loadFailed = false;
    }
    else
    {
        // setSource returns false immediately for a format it can't even open (corrupt
        // header, unsupported codec) -- everything else (peak generation for a genuinely
        // huge file) happens on AudioThumbnail's own background thread and arrives via
        // changeListenerCallback below, same as spike/03_dragout's thumbnail already relied on.
        loadFailed = !thumbnail.setSource(new juce::FileInputSource(file));
    }

    if (!loadFailed)
    {
        if (auto* reader = formatManager.createReaderFor(file))
        {
            readerSource = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
            transportSource.setSource(readerSource.get(), 0, nullptr, reader->sampleRate);
            transportSource.setGain(static_cast<float>(muted ? 0.0 : volumeSlider.getValue()));
        }
        else
        {
            loadFailed = true;
        }
    }

    timeLabel.setText("0:00 / " + formatTime(thumbnail.getTotalLength()), juce::dontSendNotification);
    repaint();
}

void WaveformView::togglePlayPause()
{
    if (currentFile == juce::File() || loadFailed || readerSource == nullptr) return;
    playStopAtSeconds = 0.0; // pressing Play yourself means "play on", not "play that cue"
    if (transportSource.isPlaying())
    {
        transportSource.stop();
        stopTimer();
    }
    else
    {
        // Past-the-end restarts from zero -- otherwise Play after a track finished would
        // silently do nothing, reading as broken rather than "already at the end".
        if (transportSource.getCurrentPosition() >= transportSource.getLengthInSeconds() - 0.05)
            transportSource.setPosition(0.0);
        transportSource.start();
        startTimerHz(30);
    }
    playButton.setPlaying(transportSource.isPlaying());
}

juce::Rectangle<int> WaveformView::getWaveformBounds() const
{
    return getLocalBounds().withTrimmedBottom(kTransportRowHeight);
}

WaveformView::LaneLayout WaveformView::computeLanes() const
{
    // Bottom-up, in the order they are allowed to claim height: the segment band first
    // (the one that is actually clicked, and the oldest), then chords, then spans. Each
    // lane is only taken if what's left for the peaks stays above kMinPeaksHeight, so
    // dragging the bottom panel small drops lanes one at a time from the top of this
    // list instead of crushing the waveform itself to a few pixels.
    LaneLayout layout;
    auto remaining = getWaveformBounds();

    auto claim = [&remaining](int height, bool wanted) -> juce::Rectangle<int> {
        if (!wanted || remaining.getHeight() - height < kMinPeaksHeight) return {};
        return remaining.removeFromBottom(height);
    };

    // The ruler is the one lane taken off the TOP, and the first to be claimed: it is
    // the time axis every other lane is read against, so it is the last thing that should
    // disappear when the panel gets short.
    if (lanes.ruler && thumbnail.getTotalLength() > 0.0
        && remaining.getHeight() - kRulerHeight >= kMinPeaksHeight)
        layout.ruler = remaining.removeFromTop(kRulerHeight);

    layout.segmentBand = claim(kSegmentBandHeight, !segments.empty());
    layout.chordLane = claim(kChordLaneHeight, lanes.chords && !chords.empty());
    layout.spanLane = claim(kSpanLaneHeight, lanes.spans && !activeSpans.empty());
    // Claimed last of the bottom lanes, so it ends up closest to the peaks: an onset is a
    // claim about a transient in the audio directly above it, and every pixel of distance
    // between the tick and the peak it marks makes that harder to check by eye.
    layout.onsetLane = claim(kOnsetLaneHeight, lanes.onsets && !onsets.empty());
    layout.peaks = remaining;
    return layout;
}

juce::Rectangle<int> WaveformView::getSegmentBandBounds() const { return computeLanes().segmentBand; }

int WaveformView::secondsToX(double seconds, juce::Rectangle<int> area) const
{
    auto total = thumbnail.getTotalLength();
    if (total <= 0.0 || area.getWidth() <= 0) return area.getX();
    double windowFrac = 1.0 / zoomFactor;
    return area.getX() + static_cast<int>((seconds / total - viewStartFrac) / windowFrac * area.getWidth());
}

juce::Colour WaveformView::chordColour(int rootPitchClass)
{
    // Root pitch class around the hue wheel by fifths, not by semitone: neighbouring
    // chords in real progressions are usually a fifth apart, so a fifths ordering makes
    // a I-IV-V sit in neighbouring hues and a genuine key change read as a colour jump.
    // A no-chord ("N") region stays neutral rather than being given a hue it doesn't have.
    if (rootPitchClass < 0 || rootPitchClass > 11) return MiraLookAndFeel::textFaint;
    int fifths = (rootPitchClass * 7) % 12;
    return juce::Colour::fromHSV(static_cast<float>(fifths) / 12.0f, 0.45f, 0.78f, 1.0f);
}

void WaveformView::setSegments(std::vector<SegmentSpan> newSegments)
{
    segments = std::move(newSegments);
    repaint();
}

void WaveformView::setActiveSpans(std::vector<std::pair<double, double>> spans)
{
    activeSpans = std::move(spans);
    repaint();
}

void WaveformView::setChords(std::vector<ChordMark> newChords)
{
    chords = std::move(newChords);
    repaint();
}

void WaveformView::setNotes(std::vector<NoteBlock> newNotes)
{
    notes = std::move(newNotes);

    // The overlay maps pitch to y across the range this file actually uses, not across
    // all 128 MIDI notes: a bass stem living in one octave would otherwise be a flat
    // line across the bottom eighth of the waveform. A pad of at least an octave keeps a
    // single-note drone from being stretched over the full height as if it were a melody.
    noteLowPitch = noteHighPitch = 0;
    if (!notes.empty())
    {
        noteLowPitch = noteHighPitch = notes.front().pitch;
        for (const auto& n : notes)
        {
            noteLowPitch = juce::jmin(noteLowPitch, n.pitch);
            noteHighPitch = juce::jmax(noteHighPitch, n.pitch);
        }
        if (noteHighPitch - noteLowPitch < 12)
        {
            int centre = (noteHighPitch + noteLowPitch) / 2;
            noteLowPitch = centre - 6;
            noteHighPitch = centre + 6;
        }
    }
    repaint();
}

void WaveformView::setBeats(std::vector<double> newBeats, std::vector<double> newDownbeats)
{
    beats = std::move(newBeats);
    downbeats = std::move(newDownbeats);
    repaint();
}

void WaveformView::setOnsets(std::vector<double> newOnsets)
{
    onsets = std::move(newOnsets);
    repaint();
}

void WaveformView::setGroove(GrooveOverlay newGroove)
{
    groove = std::move(newGroove);
    repaint();
}

void WaveformView::setLaneVisibility(LaneVisibility newVisibility)
{
    lanes = newVisibility;
    repaint();
}

namespace {
// One id per View action. Explicit values rather than an enum class so they can be
// compared against the reserved range without casting at every call site.
enum ViewAction {
    kZoomIn = WaveformView::kViewActionFirst,
    kZoomOut,
    kZoomFit,
    kLaneRuler,
    kLaneBarGrid,
    kLaneSpans,
    kLaneChords,
    kLaneNotes,
    kLaneOnsets,
    kLaneGrooveGrid,
    kLaneGrooveHistogram,
    kClearSelection,
};
} // namespace

void WaveformView::addLaneItemsTo(juce::PopupMenu& menu) const
{
    // isTicked only means "switched on"; isEnabled means "this file has the data".
    // Separating them is the point -- a ticked-but-greyed item says "on, but this file
    // has no chords", which is a different thing from "you turned it off".
    menu.addItem(kLaneRuler, "Time ruler", thumbnail.getTotalLength() > 0.0, lanes.ruler);
    menu.addItem(kLaneBarGrid, "Bar grid", !downbeats.empty(), lanes.barGrid);
    menu.addItem(kLaneSpans, "Active spans", !activeSpans.empty(), lanes.spans);
    menu.addItem(kLaneChords, "Chords", !chords.empty(), lanes.chords);
    menu.addItem(kLaneNotes, "Notes", !notes.empty(), lanes.notes);
    menu.addItem(kLaneOnsets, "Onsets", !onsets.empty(), lanes.onsets);
    menu.addItem(kLaneGrooveGrid, "Groove grid", groove.valid, lanes.grooveGrid);
    menu.addItem(kLaneGrooveHistogram, "Groove histogram", groove.valid, lanes.grooveHistogram);
}

void WaveformView::buildViewMenu(juce::PopupMenu& menu) const
{
    bool haveFile = thumbnail.getTotalLength() > 0.0;
    menu.addItem(kZoomIn, "Zoom In", haveFile && zoomFactor < kMaxZoom);
    menu.addItem(kZoomOut, "Zoom Out", haveFile && zoomFactor > 1.0);
    menu.addItem(kZoomFit, "Fit Whole File", haveFile && zoomFactor > 1.0);
    menu.addSeparator();
    menu.addSectionHeader("Lanes");
    addLaneItemsTo(menu);
    menu.addSeparator();
    menu.addItem(kClearSelection, "Clear Selection", hasSelection);
}

void WaveformView::performViewAction(int actionId)
{
    switch (actionId)
    {
        case kZoomIn:  setZoom(zoomFactor * 2.0); return;
        case kZoomOut: setZoom(zoomFactor / 2.0); return;
        case kZoomFit:
            zoomFactor = 1.0;
            viewStartFrac = 0.0;
            if (onViewChanged) onViewChanged(viewStartFrac, zoomFactor);
            repaint();
            return;
        case kClearSelection: clearSelection(); return;
        default: applyLaneMenuResult(actionId); return;
    }
}

void WaveformView::applyLaneMenuResult(int result)
{
    switch (result)
    {
        case kLaneRuler:   lanes.ruler = !lanes.ruler; break;
        case kLaneBarGrid: lanes.barGrid = !lanes.barGrid; break;
        case kLaneSpans:   lanes.spans = !lanes.spans; break;
        case kLaneChords:  lanes.chords = !lanes.chords; break;
        case kLaneNotes:   lanes.notes = !lanes.notes; break;
        case kLaneOnsets:  lanes.onsets = !lanes.onsets; break;
        case kLaneGrooveGrid: lanes.grooveGrid = !lanes.grooveGrid; break;
        case kLaneGrooveHistogram: lanes.grooveHistogram = !lanes.grooveHistogram; break;
        default: return;
    }
    repaint();
}

void WaveformView::showLanesMenu()
{
    juce::PopupMenu menu;
    addLaneItemsTo(menu);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(lanesButton),
                        [this](int result) { applyLaneMenuResult(result); });
}

// Right-click anywhere in the waveform. Everything reachable by gesture is also reachable
// here by name -- the gestures below are worth learning but shouldn't be the only way in,
// and a menu is also where "what can this thing even do" gets answered.
void WaveformView::showContextMenu()
{
    juce::PopupMenu menu;
    // Owner sections first (Tags, Segments): they act on what's selected, which is what a
    // right-click is usually asking about. View controls are further down because they are
    // the same for every file and are also two clicks away in the header.
    if (buildOwnerMenuSections) buildOwnerMenuSections(menu);

    juce::PopupMenu viewMenu;
    buildViewMenu(viewMenu);
    menu.addSubMenu("View", viewMenu);

    menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
        if (result == 0) return;
        if (result >= kViewActionFirst && result <= kViewActionLast) performViewAction(result);
        else if (onOwnerMenuAction) onOwnerMenuAction(result);
    });
}

void WaveformView::panByFraction(double deltaFrac)
{
    double windowFrac = 1.0 / zoomFactor;
    viewStartFrac = juce::jlimit(0.0, juce::jmax(0.0, 1.0 - windowFrac), viewStartFrac + deltaFrac);
    if (onViewChanged) onViewChanged(viewStartFrac, zoomFactor);
    repaint();
}

void WaveformView::scrubTo(int x)
{
    if (readerSource == nullptr) return;
    transportSource.setPosition(xToFraction(x) * thumbnail.getTotalLength());
    repaint();
}

double WaveformView::chooseTickSeconds(double visibleSeconds, int widthPixels)
{
    // Fixed ladder rather than a computed "round number": these are the intervals that
    // read as musical/clock time (quarter seconds, seconds, 15s, half a minute, minutes),
    // and a generic nice-number algorithm would happily pick 20s or 2.5 minutes.
    static const double ladder[] = { 0.1,  0.25, 0.5, 1.0,  2.0,   5.0,   10.0,
                                      15.0, 30.0, 60.0, 120.0, 300.0, 600.0, 1800.0 };
    if (widthPixels <= 0 || visibleSeconds <= 0.0) return 1.0;
    for (double candidate : ladder)
        if (candidate / visibleSeconds * widthPixels >= kMinTickSpacing) return candidate;
    return ladder[std::size(ladder) - 1];
}

std::optional<std::pair<double, double>> WaveformView::getSelectionSeconds() const
{
    auto total = thumbnail.getTotalLength();
    if (!hasSelection || total <= 0.0) return std::nullopt;
    auto s = juce::jmin(selectionStartFrac, selectionEndFrac) * total;
    auto e = juce::jmax(selectionStartFrac, selectionEndFrac) * total;
    return std::make_pair(s, e);
}

void WaveformView::clearSelection()
{
    if (!hasSelection) return;
    hasSelection = false;
    if (onSelectionChanged) onSelectionChanged();
    repaint();
}

void WaveformView::selectRange(double startSeconds, double endSeconds)
{
    // The transport's length, not the thumbnail's: selecting a segment row loads its file
    // in the same click, and for an uncached file the peaks (and so the thumbnail's
    // length) arrive later, while the reader's length is known as soon as setFile returns.
    auto total = transportSource.getLengthInSeconds();
    if (total <= 0.0) total = thumbnail.getTotalLength();
    if (total <= 0.0) return;

    selectionStartFrac = juce::jlimit(0.0, 1.0, startSeconds / total);
    selectionEndFrac = juce::jlimit(0.0, 1.0, endSeconds / total);
    hasSelection = selectionEndFrac > selectionStartFrac;
    if (readerSource != nullptr) transportSource.setPosition(startSeconds);

    double windowFrac = 1.0 / zoomFactor;
    if (selectionStartFrac < viewStartFrac || selectionEndFrac > viewStartFrac + windowFrac)
        viewStartFrac = juce::jlimit(0.0, juce::jmax(0.0, 1.0 - windowFrac), selectionStartFrac);

    if (onSelectionChanged) onSelectionChanged();
    repaint();
}

std::optional<int64_t> WaveformView::segmentIdAt(juce::Point<int> position) const
{
    auto band = getSegmentBandBounds();
    auto total = thumbnail.getTotalLength();
    if (band.isEmpty() || total <= 0.0 || !band.contains(position)) return std::nullopt;

    double windowFrac = 1.0 / zoomFactor;
    // Last match wins: bands are drawn in order, so a later one is painted on top of an
    // overlapping earlier one and is what the click visibly landed on.
    std::optional<int64_t> hit;
    for (const auto& seg : segments)
    {
        auto s = juce::jmax(seg.startSeconds / total, viewStartFrac);
        auto e = juce::jmin(seg.endSeconds / total, viewStartFrac + windowFrac);
        if (e <= s) continue;
        int x0 = band.getX() + static_cast<int>((s - viewStartFrac) / windowFrac * band.getWidth());
        int x1 = band.getX() + static_cast<int>((e - viewStartFrac) / windowFrac * band.getWidth());
        if (position.x >= x0 && position.x <= x1) hit = seg.id;
    }
    return hit;
}

std::optional<std::pair<double, double>> WaveformView::spanAt(juce::Point<int> position) const
{
    auto lane = computeLanes().spanLane;
    if (lane.isEmpty() || !lane.contains(position)) return std::nullopt;

    // A 3px slop either side: several real spans are under a second long on a
    // 37-minute file, which is a sub-pixel block -- without this they would be drawn
    // (the paint path floors span widths at 1px) but impossible to actually hit.
    for (const auto& span : activeSpans)
    {
        int x0 = secondsToX(span.first, lane);
        int x1 = secondsToX(span.second, lane);
        if (position.x >= x0 - 3 && position.x <= juce::jmax(x1, x0 + 1) + 3) return span;
    }
    return std::nullopt;
}

double WaveformView::xToFraction(int x) const
{
    auto bounds = getWaveformBounds();
    if (bounds.getWidth() <= 0) return 0.0;
    double windowFrac = 1.0 / zoomFactor;
    double inView = juce::jlimit(0.0, 1.0, (x - bounds.getX()) / static_cast<double>(bounds.getWidth()));
    return juce::jlimit(0.0, 1.0, viewStartFrac + inView * windowFrac);
}

void WaveformView::setZoom(double newZoomFactor, std::optional<double> anchorFrac)
{
    if (thumbnail.getTotalLength() <= 0.0) return;
    auto oldWindowFrac = 1.0 / zoomFactor;
    zoomFactor = juce::jlimit(1.0, kMaxZoom, newZoomFactor);
    auto windowFrac = 1.0 / zoomFactor;

    if (anchorFrac)
    {
        // Zoom about a point that must not move -- the mouse, for wheel and pinch. Keeping
        // the audio under the cursor pinned is what makes "zoom into this hit" one gesture
        // instead of zoom-then-hunt-then-pan.
        double anchorOffset = juce::jlimit(0.0, 1.0, (*anchorFrac - viewStartFrac) / oldWindowFrac);
        viewStartFrac = juce::jlimit(0.0, juce::jmax(0.0, 1.0 - windowFrac),
                                      *anchorFrac - anchorOffset * windowFrac);
    }
    else
    {
        // Button zoom has no cursor to anchor to: re-centre on the playhead (falling back
        // to the old view's centre when nothing's positioned) so it doesn't jump somewhere
        // unrelated to what's on screen.
        auto total = thumbnail.getTotalLength();
        double centreFrac = total > 0.0 ? transportSource.getCurrentPosition() / total
                                         : viewStartFrac + oldWindowFrac * 0.5;
        viewStartFrac = juce::jlimit(0.0, juce::jmax(0.0, 1.0 - windowFrac), centreFrac - windowFrac * 0.5);
    }
    if (onViewChanged) onViewChanged(viewStartFrac, zoomFactor);
    repaint();
}

void WaveformView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus(); // so Space toggles playback right after clicking a waveform, not just via MainComponent's fallback
    if (currentFile == juce::File() || loadFailed) return;
    if (!getWaveformBounds().contains(e.getPosition())) return;

    dragMode = DragMode::none;
    auto layout = computeLanes();

    // Right-click is the menu everywhere except on a span or segment band, which have
    // their own (handled further down) -- those are specific objects, and their own
    // actions matter more there than the generic view controls.
    bool overSpan = spanAt(e.getPosition()).has_value();
    bool overSegment = segmentIdAt(e.getPosition()).has_value();
    if (e.mods.isPopupMenu() && !overSpan && !overSegment)
    {
        showContextMenu();
        return;
    }

    // Ruler: press and drag scrubs. This is the one place a drag means "move the
    // playhead" rather than "select a range", which is why the ruler is worth having as
    // its own strip and not just painted decoration -- scrubbing and selecting both want
    // a horizontal drag, and they need somewhere unambiguous to live.
    if (!layout.ruler.isEmpty() && layout.ruler.contains(e.getPosition()))
    {
        dragMode = DragMode::scrubbing;
        // Playback keeps running while scrubbing if it was running -- setPosition on a
        // playing transport is how you hear where you are landing.
        wasPlayingBeforeScrub = transportSource.isPlaying();
        scrubTo(e.x);
        return;
    }

    // Alt-drag (or the middle button) pans the view. Chosen over hijacking plain drag
    // because plain drag already means "select a range", and a selection is how segments
    // get declared -- the older, more load-bearing gesture keeps the unmodified button.
    if (e.mods.isAltDown() || e.mods.isMiddleButtonDown())
    {
        dragMode = DragMode::panning;
        dragLastX = e.x;
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
        return;
    }

    // A click in the segment band belongs to that segment, not to seek/selection -- it's
    // the only way to reach one boundary's own edit/delete menu.
    if (auto segmentId = segmentIdAt(e.getPosition()))
    {
        if (onSegmentRightClicked) onSegmentRightClicked(*segmentId);
        return;
    }

    // A click in the span lane offers to promote that detected span into a caption
    // segment (TASKS.md: "a detected span should be promotable to a caption segment").
    // A menu rather than doing it straight away: creating a segment is a write, and this
    // lane sits one row above the segment band, so a mis-aimed click would otherwise
    // silently add a row. A left click still selects the span's range, which is useful
    // on its own (it seeks there and arms the +Segment button with the same range).
    if (auto span = spanAt(e.getPosition()))
    {
        selectRange(span->first, span->second);
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addSectionHeader("Detected span");
            menu.addItem("+ Segment from this span", [this, range = *span] {
                if (onAddSegmentRequested) onAddSegmentRequested(range.first, range.second);
            });
            menu.showMenuAsync(juce::PopupMenu::Options());
        }
        return;
    }

    isDraggingSelection = true;
    dragMode = DragMode::selecting;
    selectionStartFrac = selectionEndFrac = xToFraction(e.x);
    hasSelection = false; // becomes true in mouseDrag once it's actually a drag, not just a click

    if (readerSource != nullptr)
        transportSource.setPosition(selectionStartFrac * thumbnail.getTotalLength());
    repaint();
}

void WaveformView::mouseDrag(const juce::MouseEvent& e)
{
    if (dragMode == DragMode::scrubbing)
    {
        scrubTo(e.x);
        return;
    }

    if (dragMode == DragMode::panning)
    {
        auto bounds = getWaveformBounds();
        if (bounds.getWidth() > 0)
        {
            // Drag the content, not the viewport: moving the mouse right moves the audio
            // right, which is what "grab and pull" means everywhere else.
            double windowFrac = 1.0 / zoomFactor;
            panByFraction(-(e.x - dragLastX) / static_cast<double>(bounds.getWidth()) * windowFrac);
        }
        dragLastX = e.x;
        return;
    }

    if (!isDraggingSelection) return;
    bool hadSelection = hasSelection;
    selectionEndFrac = xToFraction(e.x);
    hasSelection = std::abs(selectionEndFrac - selectionStartFrac) > 0.002; // ignore a near-zero jitter as a plain click
    if (hasSelection != hadSelection && onSelectionChanged) onSelectionChanged();
    repaint();
}

void WaveformView::mouseUp(const juce::MouseEvent&)
{
    if (dragMode == DragMode::panning || dragMode == DragMode::scrubbing)
    {
        dragMode = DragMode::none;
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }
    dragMode = DragMode::none;
    isDraggingSelection = false;
    if (onSelectionChanged) onSelectionChanged(); // final range -- the Add Segment control reads it now
    if (!hasSelection) return;
    // Seek to the start of what was just selected, same as a click -- selection marks a
    // range for later use (loop/export region, not built yet) without changing today's
    // "click seeks" behaviour.
    auto start = juce::jmin(selectionStartFrac, selectionEndFrac);
    if (readerSource != nullptr) transportSource.setPosition(start * thumbnail.getTotalLength());
}

void WaveformView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (thumbnail.getTotalLength() <= 0.0) return;

    // Horizontal (trackpad two-finger swipe) pans; vertical zooms about the cursor.
    // Whichever axis dominates wins, so a slightly-off swipe doesn't zoom by accident.
    // Panning only means anything once zoomed in -- at zoomFactor==1 the whole file is
    // already visible and there is nowhere to pan to.
    if (std::abs(wheel.deltaX) > std::abs(wheel.deltaY))
    {
        if (zoomFactor <= 1.0) return;
        panByFraction(-static_cast<double>(wheel.deltaX) / zoomFactor * 0.6);
        return;
    }

    if (std::abs(wheel.deltaY) < 1.0e-4) return;
    // Exponential, so each notch is the same proportional change whether you are at 1x or
    // 40x -- a linear step would crawl when zoomed out and jump wildly when zoomed in.
    setZoom(zoomFactor * std::pow(2.0, static_cast<double>(wheel.deltaY) * 1.5), xToFraction(e.x));
}

void WaveformView::mouseMagnify(const juce::MouseEvent& e, float scaleFactor)
{
    if (thumbnail.getTotalLength() <= 0.0 || scaleFactor <= 0.0f) return;
    setZoom(zoomFactor * scaleFactor, xToFraction(e.x));
}

void WaveformView::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Double-click to fit, the same convention as a double-click on a window edge. Not in
    // the ruler, where a double-click is just two scrubs and jumping the view under a
    // gesture that was aiming the playhead would be actively wrong.
    if (!layoutRulerContains(e.getPosition()))
    {
        zoomFactor = 1.0;
        viewStartFrac = 0.0;
        repaint();
    }
}

void WaveformView::mouseMove(const juce::MouseEvent& e)
{
    // Cursor as the only discoverability hint the gestures get: an I-beam over the ruler
    // says "this scrubs", a grab hand under Alt says "this pans".
    if (currentFile == juce::File() || loadFailed)
    {
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }
    if (layoutRulerContains(e.getPosition()))
        setMouseCursor(juce::MouseCursor::IBeamCursor);
    else if (e.mods.isAltDown())
        setMouseCursor(juce::MouseCursor::DraggingHandCursor);
    else
        setMouseCursor(juce::MouseCursor::NormalCursor);
}

bool WaveformView::layoutRulerContains(juce::Point<int> position) const
{
    auto ruler = computeLanes().ruler;
    return !ruler.isEmpty() && ruler.contains(position);
}

void WaveformView::changeListenerCallback(juce::ChangeBroadcaster*) { repaint(); }

void WaveformView::playRange(double startSeconds, double endSeconds)
{
    if (currentFile == juce::File() || loadFailed || readerSource == nullptr) return;
    selectRange(startSeconds, endSeconds);
    playStopAtSeconds = endSeconds;
    transportSource.setPosition(startSeconds);
    transportSource.start();
    startTimerHz(30);
    playButton.setPlaying(true);
}

void WaveformView::stopPlayback()
{
    transportSource.stop();
    playStopAtSeconds = 0.0;
    stopTimer();
    playButton.setPlaying(false);
    repaint();
    if (onPlaybackStopped) onPlaybackStopped();
}

void WaveformView::timerCallback()
{
    // End of a ranged audition. Cleared first so the ordinary transport is unaffected
    // afterwards -- the next plain Play must run to the end of the file, not to this cue's.
    if (playStopAtSeconds > 0.0 && transportSource.getCurrentPosition() >= playStopAtSeconds)
    {
        stopPlayback();
        return;
    }

    // Auto-follow the playhead when zoomed in, so playback doesn't silently run off the
    // edge of the visible window -- keeps it a bit ahead of the window's left edge
    // (10%) rather than dead-centring it, so there's still visible lead-in context.
    if (zoomFactor > 1.0 && thumbnail.getTotalLength() > 0.0)
    {
        double windowFrac = 1.0 / zoomFactor;
        double posFrac = transportSource.getCurrentPosition() / thumbnail.getTotalLength();
        if (posFrac < viewStartFrac || posFrac > viewStartFrac + windowFrac)
        {
            viewStartFrac = juce::jlimit(0.0, juce::jmax(0.0, 1.0 - windowFrac), posFrac - windowFrac * 0.1);
            if (onViewChanged) onViewChanged(viewStartFrac, zoomFactor);
        }
    }
    repaint();
}

// The whole file folded onto one beat: bin i is the share of onsets landing in the i-th
// sixteenth of a 16th-resolution beat, normalised so 1.0 is "perfectly even". So the
// tallest bar IS the `strength` number, and a flat picture is literally the null result
// this whole measurement was built to stop shipping -- on the corpus that exposed the bug,
// every one of 94 files drew flat against `beat_this`'s grid and 76 drew peaked against
// the fitted one.
void WaveformView::paintGrooveHistogram(juce::Graphics& g, juce::Rectangle<int> peaks) const
{
    if (!groove.valid || groove.phaseHistogram.empty()) return;
    // Never at the cost of the waveform: on a short panel the peaks matter more than the
    // picture of them, which is the same rule computeLanes() applies to every other lane.
    if (peaks.getWidth() < kGrooveHistogramWidth + 24
        || peaks.getHeight() < kGrooveHistogramHeight + 16)
        return;

    auto panel = juce::Rectangle<int>(peaks.getRight() - kGrooveHistogramWidth - 8,
                                      peaks.getY() + 6, kGrooveHistogramWidth,
                                      kGrooveHistogramHeight);

    g.setColour(MiraLookAndFeel::surface.withAlpha(0.88f));
    g.fillRoundedRectangle(panel.toFloat(), 4.0f);
    g.setColour(MiraLookAndFeel::border);
    g.drawRoundedRectangle(panel.toFloat(), 4.0f, 1.0f);

    auto inner = panel.reduced(6, 4);
    auto header = inner.removeFromTop(12);
    g.setFont(juce::Font(juce::FontOptions(9.5f)));
    g.setColour(MiraLookAndFeel::textDim);
    g.drawText(juce::String(groove.bpm, 1) + " BPM  " + groove.octaveSource, header,
               juce::Justification::centredLeft, false);
    // A locked grid earns the accent colour; an unlocked one stays dim, because the number
    // beside it is then a description of noise and should not look like a result.
    g.setColour(groove.locked ? MiraLookAndFeel::accent : MiraLookAndFeel::textFaint);
    g.drawText(juce::String(groove.strength, 2) + juce::String(juce::CharPointer_UTF8("\xc3\x97")), header,
               juce::Justification::centredRight, false);

    auto footer = inner.removeFromBottom(11);
    g.setFont(juce::Font(juce::FontOptions(9.0f)));
    g.setColour(MiraLookAndFeel::textFaint);
    g.drawText(groove.summary, footer, juce::Justification::centredLeft, false);

    auto plot = inner.reduced(0, 2);
    if (plot.getHeight() < 8) return;

    // Fixed ceiling rather than auto-scaling to this file's own peak: the point of the
    // picture is comparing one track against another, and a y-axis that rescales per file
    // would make a flat 1.1x look exactly like a locked 6.9x.
    constexpr double kHistogramCeiling = 4.0;
    const int bins = static_cast<int>(groove.phaseHistogram.size());
    const float binWidth = static_cast<float>(plot.getWidth()) / static_cast<float>(bins);

    // The uniform line -- where every bar would sit if onsets fell anywhere at all.
    int uniformY = plot.getBottom()
                   - static_cast<int>(1.0 / kHistogramCeiling * plot.getHeight());
    g.setColour(MiraLookAndFeel::textFaint.withAlpha(0.45f));
    g.drawHorizontalLine(uniformY, static_cast<float>(plot.getX()),
                          static_cast<float>(plot.getRight()));

    for (int i = 0; i < bins; ++i)
    {
        double value = juce::jlimit(0.0, kHistogramCeiling, groove.phaseHistogram[static_cast<size_t>(i)]);
        auto height = static_cast<float>(value / kHistogramCeiling * plot.getHeight());
        juce::Rectangle<float> bar(plot.getX() + i * binWidth, plot.getBottom() - height,
                                   juce::jmax(1.0f, binWidth - 1.0f), height);
        // Beats (every fourth bin) are the metrical anchors; the rest are the off-beat
        // sixteenths. Colouring them apart is what turns the picture into a groove
        // reading instead of a bar chart -- swing shows up as the off-beat bar leaning
        // late, syncopation as the off-beat bars out-growing the beats.
        const bool onBeat = (i % 4) == 0;
        g.setColour(onBeat ? MiraLookAndFeel::accent.withAlpha(groove.locked ? 0.95f : 0.45f)
                           : MiraLookAndFeel::active.withAlpha(groove.locked ? 0.80f : 0.35f));
        g.fillRect(bar);
    }
}

void WaveformView::paint(juce::Graphics& g)
{
    auto waveformBounds = getWaveformBounds();
    g.setColour(MiraLookAndFeel::surface3);
    g.fillRect(waveformBounds);

    if (currentFile == juce::File())
    {
        g.setColour(MiraLookAndFeel::textFaint);
        g.setFont(juce::Font(juce::FontOptions(12.5f)));
        g.drawText("No file selected", waveformBounds, juce::Justification::centred, true);
        return;
    }

    if (loadFailed)
    {
        g.setColour(MiraLookAndFeel::warn);
        g.setFont(juce::Font(juce::FontOptions(12.5f)));
        g.drawText("Couldn't read audio from this file", waveformBounds, juce::Justification::centred, true);
        return;
    }

    if (thumbnail.getTotalLength() <= 0.0)
    {
        // AudioThumbnail is still generating peaks (or hasn't started) -- the
        // changeListenerCallback above repaints as that progresses, so this is a
        // genuinely transient state, not a dead end. A long file (tens of minutes) can
        // sit here for a few seconds; pre-generating previews during Scan so this is
        // instant by the time a file's clicked is a real next step, not done here.
        g.setColour(MiraLookAndFeel::textFaint);
        g.setFont(juce::Font(juce::FontOptions(12.5f)));
        g.drawText(juce::String(juce::CharPointer_UTF8("Loading waveform\xe2\x80\xa6")), waveformBounds,
                   juce::Justification::centred, true);
        return;
    }

    auto total = thumbnail.getTotalLength();
    double windowFrac = 1.0 / zoomFactor;
    double viewStart = viewStartFrac * total;
    double viewEnd = juce::jmin(total, (viewStartFrac + windowFrac) * total);
    auto layout = computeLanes();

    // Selection tint, drawn under everything else -- a neutral bright wash (mira has no
    // separate "selection" colour token yet) rather than borrowing Soundly's magenta,
    // which isn't part of mira's own palette. Full height of the waveform area, lanes
    // included: a selection is a range in time, and the lanes are the same time axis.
    if (hasSelection)
    {
        auto s0 = juce::jmax(juce::jmin(selectionStartFrac, selectionEndFrac), viewStartFrac);
        auto e0 = juce::jmin(juce::jmax(selectionStartFrac, selectionEndFrac), viewStartFrac + windowFrac);
        if (e0 > s0)
        {
            auto selBounds = waveformBounds
                                  .withX(waveformBounds.getX()
                                         + static_cast<int>((s0 - viewStartFrac) / windowFrac * waveformBounds.getWidth()))
                                  .withWidth(static_cast<int>((e0 - s0) / windowFrac * waveformBounds.getWidth()));
            g.setColour(MiraLookAndFeel::text.withAlpha(0.10f));
            g.fillRect(selBounds);
        }
    }

    // --- Ruler ---------------------------------------------------------------------
    // A real scale for the timeline, so a chord can be matched to a hit by eye instead of
    // by guessing. Two scales in one strip: clock time always (m:ss ticks), and bar
    // numbers on top of it whenever the analysis found downbeats. They share a strip
    // rather than stacking into two because vertical space in this panel is already
    // contended by four lanes, and the two scales never overlap horizontally -- bar
    // numbers sit on the tick line, times sit under it.
    double visibleSeconds = (viewEnd - viewStart) > 0.0 ? (viewEnd - viewStart) : total;
    if (!layout.ruler.isEmpty())
    {
        auto ruler = layout.ruler;
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(ruler);
        g.setColour(MiraLookAndFeel::border);
        g.drawHorizontalLine(ruler.getBottom() - 1, static_cast<float>(ruler.getX()),
                              static_cast<float>(ruler.getRight()));

        double tick = chooseTickSeconds(visibleSeconds, ruler.getWidth());
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        for (double t = std::ceil(viewStart / tick) * tick; t <= viewEnd; t += tick)
        {
            int x = secondsToX(t, ruler);
            g.setColour(MiraLookAndFeel::textFaint);
            g.drawVerticalLine(x, static_cast<float>(ruler.getBottom() - 5),
                                static_cast<float>(ruler.getBottom() - 1));
            g.setColour(MiraLookAndFeel::textDim);
            // Sub-second ticks need the fraction, or every label on a zoomed-in view
            // reads as the same second repeated.
            auto label = tick < 1.0 ? formatTime(t) + juce::String(t - std::floor(t), 1).substring(1)
                                     : formatTime(t);
            g.drawText(label, x + 3, ruler.getY(), 46, ruler.getHeight() - 4,
                        juce::Justification::centredLeft, false);
        }
    }

    // --- Bar grid ------------------------------------------------------------------
    // Drawn from the analysis' own detected downbeats, never from a grid laid out off the
    // BPM scalar: a synthetic grid drifts against anything that isn't metronomic, which is
    // exactly the through-composed material this is most needed for. Beat lines only
    // appear once they are far enough apart to mean anything, so a zoomed-out 37-minute
    // file doesn't turn into a solid block of lines.
    auto gridArea = layout.ruler.isEmpty() ? waveformBounds
                                            : waveformBounds.withTop(layout.ruler.getBottom());
    if (lanes.barGrid && !downbeats.empty())
    {
        double beatSpacingPx = beats.size() > 1
            ? (beats[1] - beats[0]) / visibleSeconds * gridArea.getWidth() : 0.0;

        if (beatSpacingPx >= 14.0)
        {
            g.setColour(MiraLookAndFeel::text.withAlpha(0.05f));
            for (double beat : beats)
            {
                if (beat < viewStart || beat > viewEnd) continue;
                g.drawVerticalLine(secondsToX(beat, gridArea), static_cast<float>(gridArea.getY()),
                                    static_cast<float>(gridArea.getBottom()));
            }
        }

        // Bar numbers thin out against the LAST ONE ACTUALLY DRAWN, not against an
        // estimated spacing. The estimate used downbeats[1]-downbeats[0] as representative,
        // which is wrong whenever a stem's downbeats are unevenly spread: STRINGS.wav opens
        // with a long silence, so its first two downbeats are minutes apart, the estimate
        // came out huge, and every one of its downbeats got numbered -- rendering the whole
        // ruler as overlapping digits at Fit zoom. Tracking the last drawn x is correct for
        // any distribution.
        int barNumber = 0;
        int lastNumberX = std::numeric_limits<int>::min();
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        for (double downbeat : downbeats)
        {
            ++barNumber;
            if (downbeat < viewStart || downbeat > viewEnd) continue;
            int x = secondsToX(downbeat, gridArea);
            g.setColour(MiraLookAndFeel::text.withAlpha(0.13f));
            g.drawVerticalLine(x, static_cast<float>(gridArea.getY()),
                                static_cast<float>(gridArea.getBottom()));
            if (!layout.ruler.isEmpty() && x - lastNumberX >= kMinBarNumberSpacing)
            {
                lastNumberX = x;
                g.setColour(MiraLookAndFeel::active);
                g.drawText(juce::String(barNumber), x + 2, layout.ruler.getY() - 1, 30, 11,
                            juce::Justification::centredLeft, false);
            }
        }
    }

    // --- Groove grid ---------------------------------------------------------------
    // The beat grid FITTED TO THE ONSETS (mira::analyzeGroove), which is a different claim
    // from the bar grid above and is drawn in a different colour for exactly that reason:
    // the bar grid is `beat_this`'s detected downbeats, this is the period that actually
    // maximises onset phase concentration. On the 94-file Amon Tobin corpus they disagree
    // on nearly every track, and seeing both at once is what makes the disagreement
    // checkable rather than an argument between two numbers.
    //
    // Unlike the bar grid this one IS laid out from a period -- that is the whole point,
    // it is a hypothesis about a steady pulse -- so it drifts on rubato material. That is
    // a feature here: visible drift is the eye's version of a low `strength`.
    if (lanes.grooveGrid && groove.valid && groove.periodSeconds > 0.0)
    {
        double spacingPx = groove.periodSeconds / visibleSeconds * gridArea.getWidth();
        if (spacingPx >= 5.0)
        {
            // Start at the first grid line at or before the visible window, so panning
            // never shifts the grid's own phase.
            double firstIndex = std::floor((viewStart - groove.phaseSeconds) / groove.periodSeconds);
            // Quarter-beat (16th) subdivisions only once they are legible; they are what
            // the pocket and syncopation numbers are measured against.
            bool showSixteenths = spacingPx >= 48.0;
            for (double i = firstIndex;; i += 1.0)
            {
                double t = groove.phaseSeconds + i * groove.periodSeconds;
                if (t > viewEnd) break;
                if (t >= viewStart)
                {
                    g.setColour(MiraLookAndFeel::accent.withAlpha(0.30f));
                    g.drawVerticalLine(secondsToX(t, gridArea), static_cast<float>(gridArea.getY()),
                                        static_cast<float>(gridArea.getBottom()));
                }
                if (showSixteenths)
                {
                    g.setColour(MiraLookAndFeel::accent.withAlpha(0.10f));
                    for (int sub = 1; sub < 4; ++sub)
                    {
                        double subT = t + groove.periodSeconds * sub / 4.0;
                        if (subT < viewStart || subT > viewEnd) continue;
                        g.drawVerticalLine(secondsToX(subT, gridArea), static_cast<float>(gridArea.getY()),
                                            static_cast<float>(gridArea.getBottom()));
                    }
                }
            }
        }
    }

    // White/near-white waveform -- "the waveform not orange in colour, use white".
    g.setColour(MiraLookAndFeel::text);
    thumbnail.drawChannels(g, layout.peaks.reduced(0, 4), viewStart, viewEnd, 1.0f);

    // --- Onset lane ------------------------------------------------------------------
    // The raw evidence. Ticks are split into two tiers by how far each onset sits from the
    // nearest 16th of the fitted grid: on-grid onsets full height and bright, off-grid ones
    // short and dim. That split IS the syncopation number drawn -- a four-on-the-floor
    // pattern reads as a row of tall ticks, a broken-beat one as a scatter -- and it is
    // also the fastest way to see a grid that has locked onto the wrong period, because
    // then nothing is tall.
    if (!layout.onsetLane.isEmpty())
    {
        auto lane = layout.onsetLane;
        g.setColour(MiraLookAndFeel::surface3);
        g.fillRect(lane);

        const bool haveGrid = groove.valid && groove.periodSeconds > 0.0;
        const double cell = haveGrid ? groove.periodSeconds / 4.0 : 0.0;
        // How close counts as "on the grid". A 16th at 80 BPM is 187 ms, so 18% of a cell
        // is ~34 ms -- tight enough to mean something, loose enough to survive the ~11.6 ms
        // quantisation of the onset detector's own hop.
        constexpr double kOnGridTolerance = 0.18;

        for (double onset : onsets)
        {
            if (onset < viewStart || onset > viewEnd) continue;
            int x = secondsToX(onset, lane);

            bool onGrid = false;
            if (haveGrid)
            {
                double cells = (onset - groove.phaseSeconds) / cell;
                onGrid = std::abs(cells - std::round(cells)) <= kOnGridTolerance;
            }

            if (onGrid)
            {
                g.setColour(MiraLookAndFeel::accent.withAlpha(0.95f));
                g.drawVerticalLine(x, static_cast<float>(lane.getY() + 1),
                                    static_cast<float>(lane.getBottom() - 1));
            }
            else
            {
                g.setColour(MiraLookAndFeel::textDim.withAlpha(0.75f));
                g.drawVerticalLine(x, static_cast<float>(lane.getY() + 5),
                                    static_cast<float>(lane.getBottom() - 1));
            }
        }

        g.setColour(MiraLookAndFeel::border);
        g.drawHorizontalLine(lane.getY(), static_cast<float>(lane.getX()),
                              static_cast<float>(lane.getRight()));
    }

    if (lanes.grooveHistogram)
        paintGrooveHistogram(g, layout.peaks);

    // Note transcription ($.notes), drawn as a piano roll straight over the peaks rather
    // than in a lane of its own (decided in review): a roll reads against the audio it
    // transcribes, and the files that have notes are short melodic loops where the peaks
    // are not carrying much information on their own anyway. Pitch maps to y across this
    // file's own range (see setNotes), amplitude to alpha, so a quiet passing note doesn't
    // shout as loudly as the melody.
    if (lanes.notes && !notes.empty() && noteHighPitch > noteLowPitch)
    {
        auto rollArea = layout.peaks.reduced(0, 4);
        int pitchSpan = noteHighPitch - noteLowPitch;
        // At least 2px tall so a wide-range transcription doesn't render as invisible hairlines.
        int blockH = juce::jmax(2, rollArea.getHeight() / juce::jmax(1, pitchSpan + 1));
        for (const auto& n : notes)
        {
            if (n.endSeconds < viewStart || n.startSeconds > viewEnd) continue;
            int x0 = secondsToX(juce::jmax(n.startSeconds, viewStart), rollArea);
            int x1 = secondsToX(juce::jmin(n.endSeconds, viewEnd), rollArea);
            // Low pitch at the bottom, the way a piano roll and a keyboard both run.
            double rel = static_cast<double>(n.pitch - noteLowPitch) / pitchSpan;
            int y = rollArea.getBottom() - blockH - static_cast<int>(rel * (rollArea.getHeight() - blockH));
            g.setColour(MiraLookAndFeel::accent.withAlpha(juce::jlimit(0.25f, 0.9f, n.amplitude)));
            g.fillRect(x0, y, juce::jmax(2, x1 - x0), blockH);
        }
    }

    // Active spans (files.active_spans): the detected non-silent regions, as their own
    // thin lane directly above the segment band -- a span is the raw material a caption
    // segment gets made out of (right-click promotes one), so they sit adjacent and read
    // as the same kind of object at different stages. Teal, which MiraLookAndFeel already
    // documents as the active-region colour.
    if (!layout.spanLane.isEmpty())
    {
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(layout.spanLane);
        g.setColour(MiraLookAndFeel::active.withAlpha(0.75f));
        for (const auto& [spanStart, spanEnd] : activeSpans)
        {
            if (spanEnd < viewStart || spanStart > viewEnd) continue;
            int x0 = secondsToX(juce::jmax(spanStart, viewStart), layout.spanLane);
            int x1 = secondsToX(juce::jmin(spanEnd, viewEnd), layout.spanLane);
            // Minimum 1px: a 0.08s span on a 37-minute file (the EP6 brass stem has
            // several) would otherwise round away to nothing and read as "not detected".
            g.fillRect(x0, layout.spanLane.getY() + 1, juce::jmax(1, x1 - x0), layout.spanLane.getHeight() - 2);
        }
    }

    // Chord lane ($.chords). Every change gets a block coloured by its root (see
    // chordColour); the label is only drawn when the block is wide enough to hold it, so
    // a 211-change file at Fit zoom is a readable harmonic map rather than a smear of
    // clipped text, and zooming in fills the names back in.
    if (!layout.chordLane.isEmpty())
    {
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(layout.chordLane);
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        for (const auto& chord : chords)
        {
            if (chord.endSeconds < viewStart || chord.startSeconds > viewEnd) continue;
            int x0 = secondsToX(juce::jmax(chord.startSeconds, viewStart), layout.chordLane);
            int x1 = secondsToX(juce::jmin(chord.endSeconds, viewEnd), layout.chordLane);
            juce::Rectangle<int> block(x0, layout.chordLane.getY(), juce::jmax(1, x1 - x0),
                                        layout.chordLane.getHeight());
            auto colour = chordColour(chord.rootPitchClass);
            g.setColour(colour.withAlpha(chord.rootPitchClass < 0 ? 0.12f : 0.55f));
            g.fillRect(block.reduced(0, 1));
            if (block.getWidth() > 26 && chord.rootPitchClass >= 0)
            {
                g.setColour(MiraLookAndFeel::text);
                g.drawText(chord.label, block.reduced(3, 0), juce::Justification::centredLeft, false);
            }
        }
    }

    // Declared segments and cues: a full-height tint over the peaks (so the boundary is
    // readable against the wave itself) plus a solid labelled band along the bottom that is
    // the actual click target.
    //
    // Colour says WHICH KIND: green (`good`) is a cue -- group-scoped, one piece of music
    // across the whole synced set; amber (`accent`) is a segment -- file-scoped, a sample
    // cut out of this one stem.
    //
    // Teal was tried for segments (to free `good` up, since it also means "edited" in the
    // matrix) and was WRONG, on screen, immediately: teal is the active-span lane's colour,
    // and that lane sits directly above this band. A segment is literally made out of a
    // span, so painting both teal made two adjacent lanes of different objects look
    // identical -- the user's "what are these green things in the segment?".
    //
    // The lesson is that hue alone cannot carry three objects here when one of them is
    // already teal. The real fix is structural and is review round 7 item 7: cues move out
    // of this band entirely, onto their own named locator bar along the top of the
    // timeline. Until then this is the pre-existing pairing, which at least does not
    // collide with the lane above it.
    auto band = layout.segmentBand;
    if (!band.isEmpty())
    {
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(band);
        for (const auto& seg : segments)
        {
            auto s0 = juce::jmax(seg.startSeconds / total, viewStartFrac);
            auto e0 = juce::jmin(seg.endSeconds / total, viewStartFrac + windowFrac);
            if (e0 <= s0) continue;
            int x0 = band.getX() + static_cast<int>((s0 - viewStartFrac) / windowFrac * band.getWidth());
            int x1 = band.getX() + static_cast<int>((e0 - viewStartFrac) / windowFrac * band.getWidth());
            auto colour = seg.groupScoped ? MiraLookAndFeel::good : MiraLookAndFeel::accent;

            g.setColour(colour.withAlpha(0.10f));
            g.fillRect(x0, layout.peaks.getY(), juce::jmax(1, x1 - x0), layout.peaks.getHeight());

            auto segBand = juce::Rectangle<int>(x0, band.getY(), juce::jmax(2, x1 - x0), band.getHeight());
            g.setColour(colour.withAlpha(0.45f));
            g.fillRect(segBand);
            g.setColour(colour);
            g.drawLine(static_cast<float>(x0), static_cast<float>(layout.peaks.getY()), static_cast<float>(x0),
                       static_cast<float>(band.getBottom()), 1.0f);

            if (seg.label.isNotEmpty() && segBand.getWidth() > 40)
            {
                g.setColour(MiraLookAndFeel::text);
                g.setFont(juce::Font(juce::FontOptions(10.0f)));
                g.drawText(seg.label, segBand.reduced(4, 0), juce::Justification::centredLeft, true);
            }
        }
    }

    // Playhead, only when it's within the currently visible (possibly zoomed) window.
    // Full height of the waveform area, lanes included -- it's the one element that is
    // about the time axis itself rather than about any single lane's content.
    auto posFrac = transportSource.getCurrentPosition() / total;
    if (posFrac >= viewStartFrac && posFrac <= viewStartFrac + windowFrac)
    {
        auto x = waveformBounds.getX() + static_cast<int>((posFrac - viewStartFrac) / windowFrac * waveformBounds.getWidth());
        g.setColour(MiraLookAndFeel::accent);
        // Through the ruler too, not just the lanes below it: the ruler is where the
        // playhead's position is actually read off, so stopping short of it would make
        // the one thing it is for harder.
        g.drawLine(static_cast<float>(x), static_cast<float>(waveformBounds.getY()), static_cast<float>(x),
                   static_cast<float>(waveformBounds.getBottom()), 1.5f);
    }

    timeLabel.setText(formatTime(transportSource.getCurrentPosition()) + " / " + formatTime(total),
                       juce::dontSendNotification);
    zoomResetButton.setButtonText(zoomFactor <= 1.0 ? "Fit" : juce::String(juce::roundToInt(zoomFactor)) + "x");
}

void WaveformView::resized()
{
    // Transport row height must stay >= playSize below or the play button's circle
    // overflows the component's own bottom edge and gets clipped by it ("something not
    // rite... getting clipped" -- that overflow was the actual bug: a 30px button in a
    // 26px-tall row).
    constexpr int playSize = 28;

    auto bounds = getLocalBounds();
    auto row = bounds.removeFromBottom(kTransportRowHeight).reduced(0, 3);

    // Left zone: mute + volume slider + percentage -- fixed width so the play button
    // (below) can sit at the true horizontal centre of the whole row, not just the
    // centre of whatever's left over. "volume bar should look professional with a mute
    // button" / Soundly reference screenshot's own left-aligned volume cluster.
    auto leftZone = row.removeFromLeft(140);
    muteButton.setBounds(leftZone.removeFromLeft(24).withSizeKeepingCentre(24, 24));
    leftZone.removeFromLeft(6);
    auto volumeCol = leftZone;
    volumePercentLabel.setBounds(volumeCol.removeFromTop(12));
    volumeSlider.setBounds(volumeCol);

    // Right zone: zoom controls + time readout.
    auto rightZone = row.removeFromRight(256);
    timeLabel.setBounds(rightZone.removeFromRight(76));
    rightZone.removeFromRight(8);
    zoomInButton.setBounds(rightZone.removeFromRight(26));
    rightZone.removeFromRight(4);
    zoomResetButton.setBounds(rightZone.removeFromRight(40));
    rightZone.removeFromRight(4);
    zoomOutButton.setBounds(rightZone.removeFromRight(26));
    rightZone.removeFromRight(8);
    lanesButton.setBounds(rightZone.removeFromRight(52));

    // Play button: true centre of the whole component's width, "play button can be in
    // the centre" -- computed from getWidth(), not from whatever's left of row after the
    // two zones above. Vertically centred within the row itself, not pinned to its top.
    playButton.setBounds(getWidth() / 2 - playSize / 2, row.getY() + (row.getHeight() - playSize) / 2, playSize,
                          playSize);
}
