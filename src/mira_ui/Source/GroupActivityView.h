#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "MiraLookAndFeel.h"

#include <functional>
#include <optional>
#include <utility>
#include <vector>

// The stem activity matrix (review round 6: "activity matrix is perfect - lets do it like
// that and build on this idea").
//
// The problem it solves: judging whether a cue boundary is right means seeing what every
// stem in the synced set is doing at that moment. The obvious answer is a DAW-style
// multitrack view -- fifteen waveforms stacked on a shared timeline -- and it was
// considered and rejected: fifteen 41-minute waveforms is a lot of decoding and a lot of
// vertical space, for a question that doesn't actually need the waveform.
//
// What the question needs is only WHERE EACH STEM IS PLAYING, which `files.active_spans`
// already answers for free. So each stem is one thin row of blocks, ~11px, fifteen of them
// in about the height of two waveform lanes. A cue boundary is a vertical line through all
// of them, and "six stems stop and four others start here" -- the thing that marks a cue
// change in carpeted score where nothing goes silent -- is visible at a glance.
//
// The density strip along the bottom (how many stems are playing at each x) carries the
// same signal in one row for when the panel is too short for the full matrix.
class GroupActivityView : public juce::Component
{
public:
    struct StemRow
    {
        juce::String label;                               // short name, drawn in the gutter
        std::vector<std::pair<double, double>> spans;      // seconds
        bool isMix = false;                                // drawn apart: it is the sum, not a part
        bool isCurrent = false;                            // the file selected in the table
    };

    struct CueMark
    {
        int64_t id = 0;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        juce::String label;     // the cue's human tag ("action"), empty until someone names it
        bool edited = false;    // human-touched: drawn solid, and never regenerated
    };

    void setReelLength(double seconds);
    void setStems(std::vector<StemRow> newStems);
    void setCues(std::vector<CueMark> newCues);

    // Shares the waveform's zoom/pan so the two read as one timeline rather than two
    // pictures of the same thing at different scales.
    void setView(double newViewStartFrac, double newZoomFactor);

    // Which edge of a cue a drag is moving. Both are draggable: the first version only
    // ever grabbed START edges, which meant a cue's end could not be moved at all unless
    // another cue happened to begin there -- "now i really need to move the cue end to
    // match the end of the segement and i cant do it". An end with silence after it had no
    // handle of any kind.
    enum class Edge { start, end };

    // Dragging a boundary edits the cue before it and the cue after it at once -- cues in a
    // carpeted reel are a partition, not islands, so a boundary belongs to both neighbours
    // (review round 6). Reported once on mouse-up, not continuously, so one drag is one
    // database write and one undo step.
    std::function<void(int64_t cueId, Edge edge, double newSeconds)> onCueBoundaryMoved;
    std::function<void(int64_t cueId)> onCueClicked;
    std::function<void(int64_t cueId)> onCueRightClicked;

    // Clicking a stem's name or row selects that file (review round 7 follow-up: "i cant
    // select a track /file so idont know the segemetn names"). The index is into the rows
    // as drawn, which is the order setStems settled after floating the mix to the bottom --
    // so the owner must map it back through the same order it supplied.
    std::function<void(int stemIndex)> onStemClicked;

    // Double-clicking a cue's bar in the lane renames it in place (review round 7 item 7:
    // "named inline by clicking the label -- not a colour band whose name is only reachable
    // through a right-click menu"). The rectangle is handed back so the owner can put an
    // editor exactly over the label it replaces.
    std::function<void(int64_t cueId, juce::Rectangle<int> labelBounds)> onCueRenameRequested;

    // Marking a cue by hand: drag across empty matrix space to sweep a range, which stays
    // tinted until it is turned into a cue or cleared. This is the answer to "there should
    // be a cue range marker which we can mark the cue" -- detection proposes, but a
    // carpeted reel will always have boundaries only a person can hear.
    std::function<void(double startSeconds, double endSeconds)> onRangeSelected;
    std::optional<std::pair<double, double>> getSelectedRange() const;
    void clearSelectedRange();

    // Bigger rows and a ruler of its own in the full cue window; compact and rulerless when
    // it is a lane inside the bottom panel, where the waveform above already has one.
    void setRowHeight(int height);
    void setShowRuler(bool shouldShow);
    void setViewCallback(std::function<void(double, double)> callback) { onViewChanged = std::move(callback); }

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    // Where a given cue's bar sits right now, in this component's coordinates -- empty if it
    // has no lane or is scrolled out of view.
    juce::Rectangle<int> cueBarBounds(int64_t cueId) const;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    // Height this wants for `stems.size()` rows -- MainComponent asks before deciding how
    // much of the bottom panel to give it.
    int preferredHeight() const;

private:
    juce::Rectangle<int> matrixBounds() const;   // rows only, gutter excluded
    juce::Rectangle<int> densityBounds() const;
    int secondsToX(double seconds, juce::Rectangle<int> area) const;
    double xToSeconds(int x, juce::Rectangle<int> area) const;
    // Which cue edge is within grabbing distance of x, if any. Checks both edges of every
    // cue; where two coincide (one cue ending exactly where the next begins) the START wins,
    // because that is the shared boundary round 6's two-neighbour drag is about.
    struct BoundaryHit { size_t index; Edge edge; };
    std::optional<BoundaryHit> cueBoundaryNear(int x) const;

    std::vector<StemRow> stems;
    std::vector<CueMark> cues;
    double reelSeconds = 0.0;
    double viewStartFrac = 0.0;
    double zoomFactor = 1.0;

    std::optional<BoundaryHit> draggingBoundary;
    double dragSeconds = 0.0;

    // Sweeping a new range, as opposed to moving an existing boundary.
    bool sweeping = false;
    double sweepAnchor = 0.0, sweepEnd = 0.0;
    std::optional<std::pair<double, double>> selectedRange;
    std::function<void(double, double)> onViewChanged;

    int rowHeight = 11;
    bool showRuler = false;
    juce::Rectangle<int> rulerBounds() const;

    // The numbered cue lane, drawn only in the full workspace (where the ruler is). It is
    // the answer to "Can't tell where cues are": a cue is a NAMED NUMBERED BAR spanning its
    // range, and the number is the same number the cue list column shows, so the two
    // finally refer to each other. A first step toward review round 7 item 7's full
    // region-marker treatment, in the one view that has room for it today.
    juce::Rectangle<int> cueLaneBounds() const;
    bool showCueLane() const { return showRuler; }

    static constexpr int kRulerHeight = 16;
    static constexpr int kCueLaneHeight = 17;
    static constexpr int kGutterWidth = 92;   // stem names; fixed so every row's blocks align
    static constexpr int kDensityHeight = 14;
    static constexpr int kBoundaryGrabPixels = 5;
};
