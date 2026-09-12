#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "MiraLookAndFeel.h"
#include "GroupActivityView.h"
#include "WaveformView.h"

// TASKS.md Phase 5 — skeleton layout pass: every region the reference mockup has, laid
// out with real (FlexBox, resizable) proportions, honestly marked where the real feature
// isn't built yet rather than faked. Each of these gets replaced by its real
// implementation as its own build-order step (folder tree, Add Folder/progress,
// waveform, detail panel) — nothing here is meant to survive long-term as-is.
namespace mira_ui_placeholder {

inline void paintPlaceholder(juce::Graphics& g, juce::Rectangle<int> bounds, const MiraLookAndFeel& laf,
                              const juce::String& text)
{
    MiraLookAndFeel::paintGlassPanel(g, bounds, 0.0f, MiraLookAndFeel::surface2);
    g.setColour(MiraLookAndFeel::textFaint);
    g.setFont(laf.sansRegular(13.0f));
    g.drawFittedText(text, bounds.reduced(8), juce::Justification::centred, 3);
}

} // namespace mira_ui_placeholder

// The filter bar lives in FilterBar.h now -- split into key / BPM range / genre /
// instrument / mood fields plus free text in review round 2, replacing the single
// free-text FilterBarComponent that used to be here.

// SoundBrowser.h's DragBar pattern (a thin draggable divider reporting screen-space
// delta on drag) — same shape, mira's own palette. isVert==true resizes left/right.
// Declared before BottomPanelPlaceholder (moved up from its original spot further down
// this file) since that class now owns one to resize its waveform area.
class DragBar : public juce::Component
{
public:
    explicit DragBar(bool isVertIn) : isVert(isVertIn) {}

    std::function<void(int)> onDelta;

    void paint(juce::Graphics& g) override
    {
        g.setColour(MiraLookAndFeel::surface2);
        g.fillAll();
        g.setColour(MiraLookAndFeel::border);
        if (isVert) g.fillRect(getWidth() / 2 - 1, 4, 2, getHeight() - 8);
        else g.fillRect(4, getHeight() / 2 - 1, getWidth() - 8, 2);
    }

    void mouseEnter(const juce::MouseEvent&) override
    {
        setMouseCursor(isVert ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::UpDownResizeCursor);
    }

    void mouseDown(const juce::MouseEvent& e) override { lastScreen = isVert ? e.getScreenX() : e.getScreenY(); }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        int cur = isVert ? e.getScreenX() : e.getScreenY();
        if (onDelta) onDelta(cur - lastScreen);
        lastScreen = cur;
    }

private:
    bool isVert;
    int lastScreen = 0;
};

// Hand-sketched layout (TASKS.md Phase 5): waveform + details/markers/segments/tags +
// actions, ONE combined region spanning the full window width (under where the sidebar
// sits too, not indented alongside it) — not two separate always-visible fixed strips
// like the first skeleton pass had. MainComponent only gives this region real height
// once a file is actually selected; at zero height nothing here needs to render at all.
//
// The detail row along its bottom is real as of TASKS.md Phase 5's leftovers pass: the
// analysis summary it already showed, plus the segment-marker controls that were the
// "markers · segments · tags · more (not wired yet)" placeholder. Those controls only
// appear for files mira actually has a marker workflow for -- Score Stems (see
// MainComponent::isScoreStemPath) -- since that category split was created precisely
// because long-form score stems need markers and music stems don't.
class BottomPanel : public juce::Component, private juce::Timer
{
public:
    explicit BottomPanel(const MiraLookAndFeel& lafIn) : laf(lafIn)
    {
        addAndMakeVisible(waveform);
        // Hidden until the selected file turns out to be part of a synced stem set --
        // there is nothing to compare a lone file against.
        addChildComponent(matrix);
        waveform.onViewChanged = [this](double startFrac, double zoom) { matrix.setView(startFrac, zoom); };

        addSegmentButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
        addSegmentButton.setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
        addSegmentButton.onClick = [this] {
            auto selection = waveform.getSelectionSeconds();
            if (selection && onAddSegment) onAddSegment(selection->first, selection->second);
        };
        addChildComponent(addSegmentButton);

        segmentsButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
        segmentsButton.setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
        segmentsButton.onClick = [this] { if (onSegmentsMenu) onSegmentsMenu(); };
        addChildComponent(segmentsButton);

        for (auto* b : { &tagsMenuButton, &segmentsMenuButton, &viewMenuButton })
        {
            b->setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface2);
            b->setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
            addAndMakeVisible(*b);
        }
        tagsMenuButton.onClick = [this] { showHeaderMenu(buildTagsMenu, tagsMenuButton); };
        segmentsMenuButton.onClick = [this] { showHeaderMenu(buildSegmentsMenu, segmentsMenuButton); };
        viewMenuButton.onClick = [this] {
            showHeaderMenu([this](juce::PopupMenu& menu) { waveform.buildViewMenu(menu); }, viewMenuButton);
        };

        // The waveform's own right-click asks for the same Tags/Segments items, so the
        // gesture and the header reach identical menus rather than drifting apart.
        waveform.buildOwnerMenuSections = [this](juce::PopupMenu& menu) {
            juce::PopupMenu tags, segmentsMenu;
            if (buildTagsMenu) buildTagsMenu(tags);
            if (buildSegmentsMenu) buildSegmentsMenu(segmentsMenu);
            if (tags.getNumItems() > 0) menu.addSubMenu("Tags", tags);
            if (segmentsMenu.getNumItems() > 0) menu.addSubMenu("Segments", segmentsMenu);
            if (tags.getNumItems() > 0 || segmentsMenu.getNumItems() > 0) menu.addSeparator();
        };
        waveform.onOwnerMenuAction = [this](int actionId) { if (onMenuAction) onMenuAction(actionId); };

        // The Add button is only meaningful while a range is actually selected, so it
        // tracks the waveform's own selection rather than being permanently clickable
        // and failing with a dialog.
        waveform.onSelectionChanged = [this] { updateSegmentControls(); };
        waveform.onSegmentRightClicked = [this](int64_t id) { if (onSegmentClicked) onSegmentClicked(id); };
        // Promoting a detected span goes through the very same callback the +Segment
        // button uses, so a promoted span gets the group/file scope question and the
        // reload that follows without a second code path.
        waveform.onAddSegmentRequested = [this](double start, double end) {
            if (onAddSegment) onAddSegment(start, end);
        };
    }

    void setSelectedFileName(const juce::String& name) { selectedFileName = name; repaint(); }

    // Review round 5: "in the title bar i need status dots like BLue dot when analysing is
    // on or there is some process going on so we know whats going on."
    //
    // A dot plus a short label, in the one strip that is always on screen while a file is
    // selected. The dot pulses while busy rather than sitting static -- a static dot says
    // "a thing is true", a pulsing one says "a thing is happening", and telling those
    // apart is the entire point here. Teal is MiraLookAndFeel::active, which the palette
    // already designates for in-progress state.
    enum class Activity { idle, scanning, analyzing };

    void setActivity(Activity newActivity, const juce::String& detail)
    {
        bool wasBusy = activity != Activity::idle;
        activity = newActivity;
        activityDetail = detail;
        bool busy = activity != Activity::idle;
        if (busy != wasBusy)
        {
            // 2 Hz, not the 30 Hz the waveform uses while playing: this is a slow breath,
            // and repainting a header strip ten times a second for it would be waste.
            if (busy) startTimer(120);
            else stopTimer();
        }
        repaint();
    }

    // Turns the whole marker workflow on for this file. Off means the controls aren't
    // rendered at all (not just disabled) -- a Samples or Music folder has no use for
    // time-ranged captions, and a permanently dead button is worse than no button.
    void setSegmentWorkflowEnabled(bool enabled)
    {
        segmentWorkflowEnabled = enabled;
        updateSegmentControls();
    }

    void setSegments(std::vector<WaveformView::SegmentSpan> segments)
    {
        segmentCount = static_cast<int>(segments.size());
        waveform.setSegments(std::move(segments));
        updateSegmentControls();
    }

    // Timeline lanes (TASKS.md Phase 5). Pure pass-throughs, same shape as setSegments
    // above -- MainComponent owns every DB read, this panel owns none.
    void setActiveSpans(std::vector<std::pair<double, double>> spans) { waveform.setActiveSpans(std::move(spans)); }
    void setChords(std::vector<WaveformView::ChordMark> chords) { waveform.setChords(std::move(chords)); }
    void setNotes(std::vector<WaveformView::NoteBlock> notes) { waveform.setNotes(std::move(notes)); }
    void setBeats(std::vector<double> beats, std::vector<double> downbeats)
    {
        waveform.setBeats(std::move(beats), std::move(downbeats));
    }

    // For the macOS menu bar, which needs the same View items this header's button shows.
    void buildViewMenu(juce::PopupMenu& menu) const { waveform.buildViewMenu(menu); }
    void performMenuAction(int actionId) { dispatchMenuAction(actionId); }

    bool hasWaveformSelection() const { return waveform.getSelectionSeconds().has_value(); }
    std::optional<std::pair<double, double>> getWaveformSelection() const { return waveform.getSelectionSeconds(); }
    int getSegmentCount() const { return segmentCount; }

    // The stem activity matrix (review round 6). Empty stems hides it entirely.
    void setGroupActivity(std::vector<GroupActivityView::StemRow> rows, double reelSeconds)
    {
        matrix.setReelLength(reelSeconds);
        matrix.setStems(std::move(rows));
        updateActivityVisibility();
    }
    void setCues(std::vector<GroupActivityView::CueMark> cues) { matrix.setCues(std::move(cues)); }
    GroupActivityView& getActivityView() { return matrix; }
    bool isActivityShown() const { return showActivity; }
    void setActivityShown(bool shown)
    {
        showActivity = shown;
        updateActivityVisibility();
    }

    void clearWaveformSelection() { waveform.clearSelection(); }
    void selectWaveformRange(double startSeconds, double endSeconds) { waveform.selectRange(startSeconds, endSeconds); }

    std::function<void(double, double)> onAddSegment; // start/end seconds of the waveform selection
    std::function<void()> onSegmentsMenu;             // the "Segments (N)" drop-down
    std::function<void(int64_t)> onSegmentClicked;    // a band clicked directly in the waveform

    // Review round 5: "the title bar of the waveform, can we use that section for menu -
    // along with the right click - so edit tags, cut segments, zoom, lanes and also into
    // proper menu so it available."
    //
    // Three menus, one per kind of thing: Tags (what this file/segment is called),
    // Segments (cutting it up), View (zoom and lanes). MainComponent builds Tags and
    // Segments because it owns the database; View is built by WaveformView, which owns the
    // zoom and lane state. All three are reachable from this header, from the waveform's
    // right-click menu, and from the macOS menu bar -- same builders behind each, so there
    // is exactly one definition of what "the Segments menu" contains.
    std::function<void(juce::PopupMenu&)> buildTagsMenu, buildSegmentsMenu;
    std::function<void(int)> onMenuAction;

    // "can we get the analysis file details in the bottom" -- a compact one-line real
    // summary (bpm/key/genre/instrument/mood), computed by MainComponent from the same
    // Database reads FileTable.cpp's own buildRow uses. Empty means nothing analyzed yet
    // for the current selection -- falls back to the placeholder text below.
    void setAnalysisSummary(const juce::String& text) { analysisSummary = text; repaint(); }

    // Real audio path, not just the display name above -- WaveformView opens the file
    // itself (juce::AudioFormatManager) to generate real peaks, same file the row's
    // Status/BPM/Key columns already read from disk.
    void setSelectedFile(const juce::File& file) { waveform.setFile(file); }

    // "play stop with space bar" -- MainComponent's Space-bar key handler reaches through
    // here rather than duplicating WaveformView's playback state.
    void togglePlayback() { waveform.togglePlayPause(); }

    // "the osx toolbar should have setting for audio" -- MiraMenuBarModel's Audio
    // Settings... item reaches through here to the one AudioDeviceManager WaveformView
    // actually owns, rather than mira keeping a second, disconnected one.
    juce::AudioDeviceManager& getAudioDeviceManager() { return waveform.getAudioDeviceManager(); }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds();
        MiraLookAndFeel::paintGlassPanel(g, bounds, 0.0f, MiraLookAndFeel::surface);
        auto header = bounds.removeFromTop(kHeaderHeight).reduced(12, 0);
        header.removeFromRight(kHeaderMenuWidth); // the three menu buttons, placed in resized()

        // Activity dot first, so the filename starts at a fixed x whether or not anything
        // is running -- a name that shifts sideways every time a batch starts reads as a
        // glitch.
        auto dotZone = header.removeFromLeft(kActivityDotWidth);
        if (activity != Activity::idle)
        {
            auto colour = activity == Activity::analyzing ? MiraLookAndFeel::active : MiraLookAndFeel::accent;
            // Sine rather than a hard blink: a blinking dot reads as an alarm, a breathing
            // one reads as work in progress.
            float pulse = 0.55f + 0.45f * std::sin(static_cast<float>(pulsePhase) * 0.35f);
            g.setColour(colour.withAlpha(pulse));
            g.fillEllipse(static_cast<float>(dotZone.getX()), static_cast<float>(dotZone.getCentreY() - 4), 8.0f, 8.0f);
        }
        else
        {
            header = header.withX(dotZone.getX()).withWidth(header.getRight() - dotZone.getX());
        }

        g.setColour(MiraLookAndFeel::text);
        g.setFont(laf.sansMedium(14.5f));
        auto nameWidth = juce::jmin(header.getWidth(),
                                     juce::GlyphArrangement::getStringWidthInt(laf.sansMedium(14.5f), selectedFileName) + 8);
        g.drawText(selectedFileName, header.removeFromLeft(nameWidth), juce::Justification::centredLeft, 1);

        if (activity != Activity::idle && activityDetail.isNotEmpty() && header.getWidth() > 80)
        {
            g.setColour(MiraLookAndFeel::textFaint);
            g.setFont(laf.sansRegular(11.5f));
            g.drawText((activity == Activity::analyzing ? "analyzing " : "scanning ") + activityDetail,
                        header.reduced(8, 0), juce::Justification::centredLeft, true);
        }

        // One-line detail row ("marker/segment section is big -- should be one line
        // container, we will use icons and menu to open the action"): real segment
        // controls on the right (laid out in resized()), the analysis summary filling
        // whatever's left on the left.
        auto detailArea = bounds.removeFromBottom(kDetailBarHeight).reduced(12, 0);
        if (segmentWorkflowEnabled)
            detailArea = detailArea.withTrimmedRight(kSegmentControlsWidth);

        g.setFont(laf.sansRegular(12.0f));
        if (analysisSummary.isNotEmpty())
        {
            g.setColour(MiraLookAndFeel::textDim);
            g.drawText(analysisSummary, detailArea, juce::Justification::centredLeft, true);
        }
        else
        {
            // juce::String(const char*) is plain ASCII, not UTF-8 -- CharPointer_UTF8
            // needed for the middle dots to render correctly rather than as mojibake
            // (see Main.cpp's updateScanStatusText comment for the full explanation).
            g.setColour(MiraLookAndFeel::textFaint);
            g.drawText(juce::String(juce::CharPointer_UTF8("no analysis yet \xc2\xb7 right-click the file to analyze")),
                        detailArea, juce::Justification::centredLeft, true);
        }
    }

    // Fills all the room between the header and the one-line detail bar -- "the waveform
    // section also needs to be resizable" is handled one level up (MainComponent's own
    // bottomSplitter drag handle resizes this whole panel's height), so the waveform
    // itself just always takes whatever's left in between, no separate inner splitter.
    void resized() override
    {
        auto bounds = getLocalBounds();
        auto header = bounds.removeFromTop(kHeaderHeight).reduced(12, 3);
        auto menus = header.removeFromRight(kHeaderMenuWidth);
        viewMenuButton.setBounds(menus.removeFromRight(64));
        menus.removeFromRight(4);
        segmentsMenuButton.setBounds(menus.removeFromRight(84));
        menus.removeFromRight(4);
        tagsMenuButton.setBounds(menus.removeFromRight(62));
        auto detailArea = bounds.removeFromBottom(kDetailBarHeight).reduced(12, 0);

        // The matrix takes what it needs from the BOTTOM of the waveform's area, capped at
        // 40% of it: the waveform is still the thing being listened to, and a fifteen-stem
        // set would otherwise squeeze it out entirely.
        if (matrix.isVisible())
        {
            int wanted = juce::jmin(matrix.preferredHeight(), juce::roundToInt(bounds.getHeight() * 0.4));
            matrix.setBounds(bounds.removeFromBottom(wanted).reduced(12, 2));
        }
        waveform.setBounds(bounds.reduced(12, 4));

        auto controls = detailArea.removeFromRight(kSegmentControlsWidth);
        segmentsButton.setBounds(controls.removeFromRight(110).reduced(0, 1));
        controls.removeFromRight(6);
        addSegmentButton.setBounds(controls.removeFromRight(110).reduced(0, 1));
    }

private:
    void updateSegmentControls()
    {
        addSegmentButton.setVisible(segmentWorkflowEnabled);
        segmentsButton.setVisible(segmentWorkflowEnabled);
        addSegmentButton.setEnabled(waveform.getSelectionSeconds().has_value());
        // Says why it's dead rather than just being dead: a disabled button with no
        // explanation is the thing that makes a feature look broken instead of unused.
        addSegmentButton.setButtonText(waveform.getSelectionSeconds() ? "+ Segment" : "Drag to select");
        segmentsButton.setButtonText("Segments (" + juce::String(segmentCount) + ")");
        segmentsButton.setEnabled(segmentCount > 0);
        resized();
        repaint();
    }

    void timerCallback() override
    {
        ++pulsePhase;
        repaint();
    }

    void updateActivityVisibility()
    {
        matrix.setVisible(showActivity && matrix.preferredHeight() > 0);
        resized();
        repaint();
    }

    // A menu built on demand, every time: the items' enabled/ticked states depend on what
    // is selected right now (is there a segment? a selection? does this file have chords?),
    // and a menu cached at construction would answer last week's question.
    void showHeaderMenu(const std::function<void(juce::PopupMenu&)>& builder, juce::Component& target)
    {
        if (!builder) return;
        juce::PopupMenu menu;
        builder(menu);
        if (menu.getNumItems() == 0) return;
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(target),
                            [this](int result) { dispatchMenuAction(result); });
    }

    // View ids belong to WaveformView (it owns zoom and lane state); everything else is
    // MainComponent's. One dispatcher so every surface routes the same way.
    void dispatchMenuAction(int result)
    {
        if (result == 0) return;
        if (result >= WaveformView::kViewActionFirst && result <= WaveformView::kViewActionLast)
            waveform.performViewAction(result);
        else if (onMenuAction)
            onMenuAction(result);
    }

    static constexpr int kHeaderHeight = 28;
    static constexpr int kHeaderMenuWidth = 218; // three buttons + gaps, reserved in paint() too
    static constexpr int kActivityDotWidth = 14;
    static constexpr int kDetailBarHeight = 22;
    static constexpr int kSegmentControlsWidth = 226; // two 110px buttons + a 6px gap
    const MiraLookAndFeel& laf;
    juce::String selectedFileName;
    juce::String analysisSummary;
    WaveformView waveform;
    GroupActivityView matrix;
    bool showActivity = true;
    juce::TextButton addSegmentButton { "Drag to select" }, segmentsButton { "Segments (0)" };
    juce::TextButton tagsMenuButton { "Tags" }, segmentsMenuButton { "Segments" }, viewMenuButton { "View" };
    Activity activity = Activity::idle;
    juce::String activityDetail;
    int pulsePhase = 0;
    bool segmentWorkflowEnabled = false;
    int segmentCount = 0;
};

// Left: real library stats (moved here from the old top-of-window status label — the
// mockup keeps stats in a custom titlebar, which a native-chrome DocumentWindow can't
// host). Right: reserved for "selected: <file> · id <n>" once row-selection callbacks
// exist.
class StatusBarComponent : public juce::Component
{
public:
    explicit StatusBarComponent(const MiraLookAndFeel& lafIn) : laf(lafIn)
    {
        leftLabel.setFont(laf.monoRegular(12.5f));
        leftLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        addAndMakeVisible(leftLabel);
        rightLabel.setFont(laf.monoRegular(12.5f));
        rightLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        rightLabel.setJustificationType(juce::Justification::centredRight);
        rightLabel.setText("no file selected", juce::dontSendNotification);
        addAndMakeVisible(rightLabel);
    }

    void setLeftText(const juce::String& text) { leftLabel.setText(text, juce::dontSendNotification); }
    void setRightText(const juce::String& text) { rightLabel.setText(text, juce::dontSendNotification); }

    // "can that be bolder and colored so we know its scanning" — an active background
    // scan is easy to miss as a thin dim status line; bold + accent colour makes it read
    // as "something is happening" at a glance, reverting to the normal dim readout once
    // idle again.
    void setLeftTextScanning(bool scanning)
    {
        leftLabel.setFont(scanning ? laf.monoMedium(12.5f) : laf.monoRegular(12.5f));
        leftLabel.setColour(juce::Label::textColourId, scanning ? MiraLookAndFeel::accent : MiraLookAndFeel::textFaint);
    }

    void paint(juce::Graphics& g) override
    {
        MiraLookAndFeel::paintGlassPanel(g, getLocalBounds(), 0.0f, MiraLookAndFeel::surface2);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(10, 0);
        leftLabel.setBounds(bounds.removeFromLeft(bounds.getWidth() / 2));
        rightLabel.setBounds(bounds);
    }

private:
    const MiraLookAndFeel& laf;
    juce::Label leftLabel, rightLabel;
};
