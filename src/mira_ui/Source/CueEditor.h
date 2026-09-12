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
        matrix.onStemClicked = [this](int index) { if (onStemClicked) onStemClicked(index); };
        matrix.onCueClicked = [this](int64_t id) {
            selectCueById(id);
            if (onCueSelected) onCueSelected(id);
        };
        matrix.onCueRenameRequested = [this](int64_t id, juce::Rectangle<int> bar) {
            beginRename(id, bar);
        };

        // The inline editor, parked invisible until a bar is double-clicked. One editor
        // reused rather than one per cue: a reel can have fifty, and forty-nine invisible
        // TextEditors is fifty times the work for the same result.
        renameEditor.setFont(laf.sansRegular(11.0f));
        renameEditor.setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::surface3);
        renameEditor.setColour(juce::TextEditor::textColourId, MiraLookAndFeel::text);
        renameEditor.setColour(juce::TextEditor::outlineColourId, MiraLookAndFeel::accent);
        renameEditor.setBorder(juce::BorderSize<int>(1));
        renameEditor.onReturnKey = [this] { commitRename(); };
        renameEditor.onFocusLost = [this] { commitRename(); };
        renameEditor.onEscapeKey = [this] { renameEditor.setVisible(false); renamingCueId = 0; };
        addChildComponent(renameEditor);
        addAndMakeVisible(matrix);

        for (auto* b : { &detectButton, &markButton, &clearButton, &playButton, &zoomOutButton,
                          &zoomInButton, &zoomFitButton })
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
        // "i will need to be able to play the cue to hear it and name it" (review round 7
        // item 6). Scoped to the SELECTED cue, and it stops at that cue's end rather than
        // running on into the next four.
        //
        // What it plays: whichever stem is selected in the table, because that is the file
        // the one transport has loaded. mira cannot sum fifteen stems live, so a "play the
        // set" button would be a lie -- clicking a stem name in the gutter to choose what
        // you audition is the honest version, and is why the gutter became clickable.
        playButton.onClick = [this] {
            if (onPlayCue) onPlayCue(selectedCueId);
        };
        zoomOutButton.onClick = [this] { setZoom(zoom / 2.0); };
        zoomInButton.onClick = [this] { setZoom(zoom * 2.0); };
        zoomFitButton.onClick = [this] { viewStart = 0.0; setZoom(1.0); };

        cueList.setColour(juce::ListBox::backgroundColourId, MiraLookAndFeel::surface2);
        cueList.setRowHeight(22);
        addAndMakeVisible(cueList);

        // "so what the thing on the right side the timimg" -- the column was three unlabelled
        // numbers, and there is no way to guess that the second is a start time and the third
        // a length rather than an end time. It says so now. The "#" column is the same number
        // the matrix draws inside each cue bar, which is what lets one be read against the
        // other.
        // Drawn in paint() rather than set on a Label, so the headings use the SAME x
        // offsets the rows do (kColNumberX and friends). A label with hand-spaced text
        // lines up until the first time someone changes a column, which is the worst kind
        // of alignment.

        summary.setFont(laf.monoRegular(11.5f));
        summary.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        addAndMakeVisible(summary);
        updateButtons();
    }

    std::function<void()> onDetectCues, onClearUntagged;
    std::function<void(double, double)> onMarkCue;
    std::function<void(int64_t)> onCueSelected, onCueRightClicked;
    std::function<void(int)> onStemClicked;
    std::function<void(int64_t cueId)> onPlayCue; // 0 means nothing selected
    // The inline rename commits a TYPE, not a free-form name: the rest of the name is
    // derived (folder + reel position), so the type is the only part a person authors. This
    // is the same value the Type submenu sets -- one field, two ways in.
    std::function<void(int64_t cueId, juce::String type)> onCueRenamed;
    // Told by the owner, because the transport lives in the bottom panel, not here.
    void setPlaying(bool playing)
    {
        playButton.setButtonText(playing ? juce::String(juce::CharPointer_UTF8("\xe2\x96\xa0  Stop"))
                                          : juce::String(juce::CharPointer_UTF8("\xe2\x96\xb6  Play Cue")));
    }
    std::function<void(int64_t, GroupActivityView::Edge, double)> onCueBoundaryMoved;

    GroupActivityView& getMatrix() { return matrix; }

    // Keeps the list's highlight in step with a cue picked in the matrix, so the two halves
    // of the window agree about which cue is being worked on.
    void selectCueById(int64_t id)
    {
        selectedCueId = id;
        updateButtons();
        for (size_t i = 0; i < cueMarks.size(); ++i)
            if (cueMarks[i].id == id) { cueList.selectRow(static_cast<int>(i)); return; }
    }

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
        renameEditor.setVisible(false); // never left floating over content it no longer matches
        renamingCueId = 0;
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

    void beginRename(int64_t cueId, juce::Rectangle<int> bar)
    {
        if (bar.isEmpty()) return;
        renamingCueId = cueId;
        juce::String existing;
        for (const auto& cue : cueMarks)
            if (cue.id == cueId)
            {
                // The bar shows "12  action"; only the authored half is editable, so the
                // editor opens on the type alone rather than on a derived string the user
                // would have to carefully not break.
                existing = cue.label.fromLastOccurrenceOf("_", false, false);
                break;
            }
        // Anchored to the bar but given a usable minimum: a 20px cue is still nameable.
        auto bounds = bar.translated(matrix.getX(), matrix.getY()).withWidth(juce::jmax(120, bar.getWidth()));
        renameEditor.setBounds(bounds);
        renameEditor.setText(existing, juce::dontSendNotification);
        renameEditor.setVisible(true);
        renameEditor.grabKeyboardFocus();
        renameEditor.selectAll();
    }

    void commitRename()
    {
        if (renamingCueId == 0 || !renameEditor.isVisible()) return;
        auto id = renamingCueId;
        auto text = renameEditor.getText().trim();
        renamingCueId = 0;
        renameEditor.setVisible(false);
        if (onCueRenamed) onCueRenamed(id, text);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(MiraLookAndFeel::surface);
        if (cueHeaderBounds.isEmpty()) return;
        g.setColour(MiraLookAndFeel::textFaint);
        g.setFont(laf.monoRegular(9.0f));
        auto heading = [&](const char* text, int x, int width) {
            g.drawText(text, cueHeaderBounds.getX() + x, cueHeaderBounds.getY(), width,
                        cueHeaderBounds.getHeight(), juce::Justification::centredLeft, false);
        };
        heading("#", kColNumberX, 20);
        heading("START", kColStartX, 46);
        heading("LENGTH", kColLengthX, 46);
        heading("NAME", kColNameX, 80);
        g.setColour(MiraLookAndFeel::border);
        g.drawHorizontalLine(cueHeaderBounds.getBottom() - 1,
                              static_cast<float>(cueHeaderBounds.getX()),
                              static_cast<float>(cueHeaderBounds.getRight()));
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto toolbar = bounds.removeFromTop(36).reduced(10, 6);
        detectButton.setBounds(toolbar.removeFromLeft(110));
        toolbar.removeFromLeft(6);
        markButton.setBounds(toolbar.removeFromLeft(140));
        toolbar.removeFromLeft(6);
        clearButton.setBounds(toolbar.removeFromLeft(140));
        toolbar.removeFromLeft(6);
        playButton.setBounds(toolbar.removeFromLeft(110));
        zoomInButton.setBounds(toolbar.removeFromRight(30));
        toolbar.removeFromRight(4);
        zoomFitButton.setBounds(toolbar.removeFromRight(54));
        toolbar.removeFromRight(4);
        zoomOutButton.setBounds(toolbar.removeFromRight(30));
        toolbar.removeFromRight(10);
        summary.setBounds(toolbar);

        // The cue list is a fixed column so the matrix gets every remaining pixel — the
        // matrix is the instrument here, the list is the index to it.
        auto list = bounds.removeFromRight(260).reduced(8);
        cueHeaderBounds = list.removeFromTop(14);
        cueList.setBounds(list);

        // Rows GROW to fill the window rather than staying the 20px they are in the bottom
        // panel's lane. The whole reason this window exists is that the lane was too small,
        // so leaving two thirds of it empty (which a fixed row height does) would miss the
        // point entirely. Capped at 40px: past that a fifteen-stem set starts looking like a
        // bar chart rather than a timeline.
        auto area = bounds.reduced(8);
        if (stemCount > 0)
        {
            int available = area.getHeight() - kRulerAllowance - kCueLaneAllowance - kDensityAllowance;
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
        playButton.setEnabled(selectedCueId != 0);
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
            // Columns at the shared offsets, so they sit under the headings drawn above.
            g.setColour(MiraLookAndFeel::textFaint);
            g.setFont(owner.laf.monoRegular(11.0f));
            g.drawText(juce::String(row + 1), kColNumberX, 0, 20, height,
                        juce::Justification::centredLeft, false);
            g.setColour(cue.edited ? MiraLookAndFeel::good : MiraLookAndFeel::accent);
            g.drawText(time(cue.startSeconds), kColStartX, 0, 46, height,
                        juce::Justification::centredLeft, false);
            g.setColour(MiraLookAndFeel::textFaint);
            g.drawText(time(cue.endSeconds - cue.startSeconds), kColLengthX, 0, 46, height,
                        juce::Justification::centredLeft, false);
            if (cue.label.isNotEmpty())
            {
                g.setColour(MiraLookAndFeel::text);
                g.setFont(owner.laf.sansRegular(11.0f));
                g.drawText(cue.label, kColNameX, 0, width - kColNameX - 6, height,
                            juce::Justification::centredLeft, true);
            }
            else
            {
                // An unnamed cue says so rather than leaving the column blank -- naming is
                // the point of a cue, so a blank there is a to-do, not an absence.
                g.setColour(MiraLookAndFeel::textFaint);
                g.setFont(owner.laf.sansRegular(10.5f));
                g.drawText("unnamed", kColNameX, 0, width - kColNameX - 6, height,
                            juce::Justification::centredLeft, false);
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
            else
            {
                owner.selectedCueId = id;
                owner.updateButtons();
                if (owner.onCueSelected) owner.onCueSelected(id);
            }
        }

        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
        {
            if (row < 0 || row >= static_cast<int>(owner.cueMarks.size())) return;
            // Rename, matching the lane's own double-click, rather than opening the menu --
            // double-click means "let me type here" everywhere else in this window.
            auto id = owner.cueMarks[static_cast<size_t>(row)].id;
            owner.selectCueById(id);
            owner.beginRename(id, owner.matrix.cueBarBounds(id));
        }
    };

    const MiraLookAndFeel& laf;
    GroupActivityView matrix;
    std::vector<GroupActivityView::CueMark> cueMarks;
    double reel = 0.0, viewStart = 0.0, zoom = 1.0;
    int stemCount = 0;
    static constexpr int kRulerAllowance = 16;      // must match GroupActivityView's own ruler
    static constexpr int kCueLaneAllowance = 17;   // and its numbered cue lane
    static constexpr int kDensityAllowance = 20;
    // One definition of where the cue list's columns are, shared by the header and the rows.
    static constexpr int kColNumberX = 6, kColStartX = 30, kColLengthX = 104, kColNameX = 154;

    juce::TextButton detectButton { "Detect Cues" }, markButton { "Drag to mark a cue" },
        clearButton { "Clear Untagged Cues" },
        playButton { juce::CharPointer_UTF8("\xe2\x96\xb6  Play Cue") };
    int64_t selectedCueId = 0;
    int64_t renamingCueId = 0;
    juce::TextEditor renameEditor;
    juce::TextButton zoomOutButton { juce::CharPointer_UTF8("\xe2\x88\x92") }, zoomInButton { "+" },
        zoomFitButton { "Fit" };
    juce::Label summary;
    juce::Rectangle<int> cueHeaderBounds;
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
