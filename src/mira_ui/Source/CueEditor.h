#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "GroupActivityView.h"
#include "MiraLookAndFeel.h"

// The full-window cue workspace (review round 6, second pass: "this is too small of a
// window... the cue tracking needs a full window").
//
// Its own window rather than a tab, for two reasons. The tab strip is scoped to *folders* —
// every tab is "show me this folder's files" — and a cue workspace isn't a folder, so
// putting it there muddies what a tab means. And cue work wants the whole screen, often on
// a second display beside the DAW the reel came out of, which a tab inside the main window
// can never give. The Log window already set this precedent.
//
// The bottom panel keeps its compact matrix: that one answers "what is this file doing
// inside the set" while you browse. This one is for doing the cue pass itself.
class CueEditorComponent : public juce::Component
{
public:
    explicit CueEditorComponent(const MiraLookAndFeel& lafIn) : laf(lafIn)
    {
        matrix.setRowHeight(20); // readable stem names and a real click target, unlike the 11px lane
        matrix.setShowRuler(true);
        matrix.setViewCallback([this](double startFrac, double zoom) {
            matrix.setView(startFrac, zoom);
            updateZoomLabel();
        });
        matrix.onRangeSelected = [this](double, double) { updateButtons(); };
        addAndMakeVisible(matrix);

        for (auto* b : { &detectButton, &markButton, &clearButton, &zoomOutButton, &zoomInButton, &zoomFitButton })
        {
            b->setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
            b->setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
            addAndMakeVisible(*b);
        }
        detectButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::accent);
        detectButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff1a1204));

        detectButton.onClick = [this] { if (onDetectCues) onDetectCues(); };
        markButton.onClick = [this] {
            // "there should be a cue range marker which we can mark the cue" — sweep a range
            // across the matrix, then this turns it into a real cue covering every stem.
            if (auto range = matrix.getSelectedRange())
                if (onMarkCue) onMarkCue(range->first, range->second);
            matrix.clearSelectedRange();
            updateButtons();
        };
        clearButton.onClick = [this] { if (onClearUntagged) onClearUntagged(); };
        zoomOutButton.onClick = [this] { setZoom(zoom / 2.0); };
        zoomInButton.onClick = [this] { setZoom(zoom * 2.0); };
        zoomFitButton.onClick = [this] { viewStart = 0.0; setZoom(1.0); };

        cueList.setColour(juce::ListBox::backgroundColourId, MiraLookAndFeel::surface2);
        cueList.setRowHeight(22);
        addAndMakeVisible(cueList);

        summary.setFont(laf.monoRegular(11.5f));
        summary.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        addAndMakeVisible(summary);
        updateButtons();
    }

    std::function<void()> onDetectCues, onClearUntagged;
    std::function<void(double, double)> onMarkCue;
    std::function<void(int64_t)> onCueSelected, onCueRightClicked;
    std::function<void(int64_t, double)> onCueBoundaryMoved;

    GroupActivityView& getMatrix() { return matrix; }

    void setContent(std::vector<GroupActivityView::StemRow> stems,
                     std::vector<GroupActivityView::CueMark> cues, double reelSeconds,
                     const juce::String& groupName)
    {
        reel = reelSeconds;
        stemCount = static_cast<int>(stems.size());
        cueMarks = cues;
        matrix.setReelLength(reelSeconds);
        matrix.setStems(std::move(stems));
        matrix.setCues(std::move(cues));
        cueList.updateContent();
        cueList.repaint();
        resized(); // row height depends on how many stems there are

        int tagged = 0;
        for (const auto& cue : cueMarks)
            if (cue.edited) ++tagged;
        summary.setText(groupName + "  " + juce::String(juce::CharPointer_UTF8("\xc2\xb7")) + "  "
                             + juce::String(cueMarks.size()) + " cues, " + juce::String(tagged)
                             + " edited/tagged",
                         juce::dontSendNotification);
        updateButtons();
    }

    void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface); }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto toolbar = bounds.removeFromTop(36).reduced(10, 6);
        detectButton.setBounds(toolbar.removeFromLeft(110));
        toolbar.removeFromLeft(6);
        markButton.setBounds(toolbar.removeFromLeft(140));
        toolbar.removeFromLeft(6);
        clearButton.setBounds(toolbar.removeFromLeft(140));
        zoomInButton.setBounds(toolbar.removeFromRight(30));
        toolbar.removeFromRight(4);
        zoomFitButton.setBounds(toolbar.removeFromRight(54));
        toolbar.removeFromRight(4);
        zoomOutButton.setBounds(toolbar.removeFromRight(30));
        toolbar.removeFromRight(10);
        summary.setBounds(toolbar);

        // The cue list is a fixed column so the matrix gets every remaining pixel — the
        // matrix is the instrument here, the list is the index to it.
        auto list = bounds.removeFromRight(260);
        cueList.setBounds(list.reduced(8));

        // Rows GROW to fill the window rather than staying the 20px they are in the bottom
        // panel's lane. The whole reason this window exists is that the lane was too small,
        // so leaving two thirds of it empty (which a fixed row height does) would miss the
        // point entirely. Capped at 40px: past that a fifteen-stem set starts looking like a
        // bar chart rather than a timeline.
        auto area = bounds.reduced(8);
        if (stemCount > 0)
        {
            int available = area.getHeight() - kRulerAllowance - kDensityAllowance;
            matrix.setRowHeight(juce::jlimit(14, 40, available / stemCount));
        }
        matrix.setBounds(area);
    }

private:
    void setZoom(double newZoom)
    {
        zoom = juce::jlimit(1.0, 64.0, newZoom);
        double window = 1.0 / zoom;
        viewStart = juce::jlimit(0.0, juce::jmax(0.0, 1.0 - window), viewStart);
        matrix.setView(viewStart, zoom);
        updateZoomLabel();
    }

    void updateZoomLabel() { zoomFitButton.setButtonText(zoom <= 1.0 ? "Fit" : juce::String(juce::roundToInt(zoom)) + "x"); }

    void updateButtons()
    {
        bool haveRange = matrix.getSelectedRange().has_value();
        markButton.setEnabled(haveRange);
        markButton.setButtonText(haveRange ? "+ Cue from range" : "Drag to mark a cue");
        clearButton.setEnabled(!cueMarks.empty());
    }

    // The cue list: one row per cue, click to select, double-click for its menu. Deliberately
    // plain — the numbers that matter when checking a cue pass are start, length and name.
    struct CueListModel : juce::ListBoxModel
    {
        CueEditorComponent& owner;
        explicit CueListModel(CueEditorComponent& o) : owner(o) {}

        int getNumRows() override { return static_cast<int>(owner.cueMarks.size()); }

        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            if (row < 0 || row >= static_cast<int>(owner.cueMarks.size())) return;
            const auto& cue = owner.cueMarks[static_cast<size_t>(row)];
            if (selected) g.fillAll(MiraLookAndFeel::surface3);
            auto time = [](double s) {
                return juce::String(static_cast<int>(s) / 60) + ":"
                        + juce::String(static_cast<int>(s) % 60).paddedLeft('0', 2);
            };
            g.setColour(cue.edited ? MiraLookAndFeel::good : MiraLookAndFeel::textDim);
            g.setFont(owner.laf.monoRegular(11.0f));
            g.drawText(juce::String(row + 1).paddedLeft(' ', 2) + "  " + time(cue.startSeconds), 6, 0, 96,
                        height, juce::Justification::centredLeft, false);
            g.setColour(MiraLookAndFeel::textFaint);
            g.drawText(time(cue.endSeconds - cue.startSeconds), 104, 0, 46, height,
                        juce::Justification::centredLeft, false);
            if (cue.label.isNotEmpty())
            {
                g.setColour(MiraLookAndFeel::text);
                g.setFont(owner.laf.sansRegular(11.0f));
                g.drawText(cue.label, 154, 0, width - 160, height, juce::Justification::centredLeft, true);
            }
        }

        void listBoxItemClicked(int row, const juce::MouseEvent& e) override
        {
            if (row < 0 || row >= static_cast<int>(owner.cueMarks.size())) return;
            auto id = owner.cueMarks[static_cast<size_t>(row)].id;
            if (e.mods.isPopupMenu())
            {
                if (owner.onCueRightClicked) owner.onCueRightClicked(id);
            }
            else if (owner.onCueSelected)
            {
                owner.onCueSelected(id);
            }
        }

        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
        {
            if (row < 0 || row >= static_cast<int>(owner.cueMarks.size())) return;
            if (owner.onCueRightClicked) owner.onCueRightClicked(owner.cueMarks[static_cast<size_t>(row)].id);
        }
    };

    const MiraLookAndFeel& laf;
    GroupActivityView matrix;
    std::vector<GroupActivityView::CueMark> cueMarks;
    double reel = 0.0, viewStart = 0.0, zoom = 1.0;
    int stemCount = 0;
    static constexpr int kRulerAllowance = 16;   // must match GroupActivityView's own ruler
    static constexpr int kDensityAllowance = 20;

    juce::TextButton detectButton { "Detect Cues" }, markButton { "Drag to mark a cue" },
        clearButton { "Clear Untagged Cues" };
    juce::TextButton zoomOutButton { juce::CharPointer_UTF8("\xe2\x88\x92") }, zoomInButton { "+" },
        zoomFitButton { "Fit" };
    juce::Label summary;
    CueListModel listModel { *this };
    juce::ListBox cueList { "cues", &listModel };
};

class CueEditorWindow : public juce::DocumentWindow
{
public:
    explicit CueEditorWindow(const MiraLookAndFeel& laf)
        : juce::DocumentWindow("MIRA Cues", MiraLookAndFeel::surface, juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        content = new CueEditorComponent(laf);
        setContentOwned(content, false);
        setResizable(true, false);
        // Deliberately large: this exists precisely because the bottom panel was too small
        // to do a cue pass in. Capped to the display so it can't open off-screen.
        auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
        centreWithSize(juce::jmin(1500, area.getWidth() - 80), juce::jmin(820, area.getHeight() - 80));
        setVisible(true);
        toFront(true); // same reason LogWindow needs it -- a fresh DocumentWindow can open behind
    }

    CueEditorComponent& getContent() { return *content; }
    std::function<void()> onClosed;
    void closeButtonPressed() override { if (onClosed) onClosed(); }

private:
    CueEditorComponent* content = nullptr;
};
