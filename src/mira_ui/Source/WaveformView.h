#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "MiraLookAndFeel.h"

// TASKS.md Phase 5 build-order step: the bottom panel's waveform + real playback --
// "play head playing thru and stuff... transport bar volume control". juce::AudioThumbnail
// still does the peak-drawing work (spike/03_dragout's same AudioThumbnail/
// AudioThumbnailCache pairing), but this now also owns a real playback graph
// (AudioDeviceManager -> AudioSourcePlayer -> AudioTransportSource -> AudioFormatReaderSource)
// instead of only ever displaying a static waveform.
//
// Small custom flat-glyph buttons, not system icons or unicode glyphs (the codebase's
// own "flat glyph, drawn with Path, not a system icon" convention -- FolderTreeItem's
// folder icon): play/pause (triangle/bars) and mute (speaker + sound-wave arcs, or a
// crossed-out speaker when muted).
class TransportPlayButton : public juce::Button
{
public:
    TransportPlayButton() : juce::Button("play") {}
    void setPlaying(bool nowPlaying) { playing = nowPlaying; repaint(); }

private:
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;
    bool playing = false;
};

class MuteButton : public juce::Button
{
public:
    MuteButton() : juce::Button("mute") {}
    void setMuted(bool nowMuted) { muted = nowMuted; repaint(); }

private:
    void paintButton(juce::Graphics& g, bool isMouseOverButton, bool isButtonDown) override;
    bool muted = false;
};

class WaveformView : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    WaveformView();
    ~WaveformView() override;

    // Empty file clears everything back to "no file selected" -- same convention
    // FileTableComponent's own empty state uses. Stops playback of whatever was
    // previously loaded first.
    void setFile(const juce::File& file);

    // Declared segment boundaries drawn over the waveform (TASKS.md Phase 5 leftovers,
    // "segment-marker tagging workflow for Score Stems"). Seconds, not fractions: the
    // `segments` table stores seconds, and a group-scoped segment's boundaries are the
    // same seconds across every sibling stem regardless of their individual lengths --
    // converting to per-file fractions here would throw that away.
    struct SegmentSpan
    {
        int64_t id = 0;
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        juce::String label;  // short tag summary, drawn in the band when there's room
        bool groupScoped = false; // covers every sibling stem, not just this file
    };

    void setSegments(std::vector<SegmentSpan> newSegments);

    // Timeline lanes (TASKS.md Phase 5, "data already exists, nothing draws it"). All
    // three come straight out of what `mira analyze` already stored -- this class does no
    // analysis of its own, it only draws what MainComponent read out of the DB. Seconds
    // throughout, for the same reason SegmentSpan uses them.
    //
    // Deliberately plain structs rather than mira::Database's own ChordChange/NoteEvent:
    // WaveformView knows nothing about mira_core (it opens audio files, not databases),
    // and MainComponent already converts DB records into SegmentSpan the same way.
    struct ChordMark
    {
        double startSeconds = 0.0;
        double endSeconds = 0.0; // derived from the next change; the last one runs to the end
        juce::String label;
        int rootPitchClass = -1; // 0..11 for colouring, -1 for "N" (no chord)
    };

    struct NoteBlock
    {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        int pitch = 0;
        float amplitude = 1.0f;
    };

    // Detected non-silent regions (files.active_spans). Drawn as their own thin lane of
    // blocks directly above the segment band, so a span reads as "the thing a segment
    // gets made out of" -- right-clicking one offers exactly that.
    void setActiveSpans(std::vector<std::pair<double, double>> spans);
    void setChords(std::vector<ChordMark> newChords);
    void setNotes(std::vector<NoteBlock> newNotes);

    // Real detected beats and downbeats (`$.rhythm.beat_this_beats`/`_downbeats`), in
    // file time. These drive the ruler's bar numbering and the bar grid drawn down
    // through every lane -- the thing that makes "does this chord line up with that hit"
    // answerable by eye. Empty is normal (a one-shot has no rhythm analysis at all), and
    // the ruler then falls back to time ticks only.
    void setBeats(std::vector<double> newBeats, std::vector<double> newDownbeats);

    // Every detected onset (`$.onset_times`, stored by `mira analyze --groove`), drawn as
    // its own tick lane. This is the raw evidence the groove grid below is fitted to, and
    // it is drawn for the same reason the fit exists at all: the bug that produced a flat
    // groove measurement across 94 files was invisible in the numbers and obvious the
    // moment onsets and grid were put on the same axis.
    void setOnsets(std::vector<double> newOnsets);

    // The fitted groove grid (mira::GrooveResult, converted by the owner). Deliberately a
    // plain struct rather than mira_core's own type, for the same reason ChordMark and
    // SegmentSpan are: this class opens audio files, not databases, and knows nothing
    // about mira_core.
    struct GrooveOverlay
    {
        bool valid = false;
        double periodSeconds = 0.0; // one beat
        double phaseSeconds = 0.0;  // file time of the grid's first line
        double bpm = 0.0;
        double strength = 0.0;      // phase histogram peak/uniform; 1.0 means no grid at all
        bool locked = false;        // strength cleared the threshold, so the metrics below are real
        juce::String octaveSource;  // which estimator picked the octave
        juce::String summary;       // one line: swing / pocket / syncopation, or why they're missing
        std::vector<double> phaseHistogram; // normalised to mean 1.0, so each bar IS a peak/uniform ratio

        // Meter (Meter.h), measured against the DETECTED beats rather than this fitted
        // grid -- see Meter.h on why bar spread is meaningless on a synthetic grid. 0
        // when the file has none.
        int meter = 0;
        // Longest bar / shortest bar on the detected beats. Above kMeterSpreadWarn the
        // grid drifted; this is the only confidence number mira has about its own beats,
        // so it is drawn rather than buried.
        double barSpread = 0.0;
    };

    void setGroove(GrooveOverlay newGroove);

    // Which lanes the user has switched off, independent of whether the current file has
    // data for them (a lane with no data never renders regardless).
    struct LaneVisibility
    {
        bool ruler = true;
        bool barGrid = true;
        bool spans = true;
        bool chords = true;
        bool notes = true;
        bool onsets = true;
        bool grooveGrid = true;
        bool grooveHistogram = true;
        bool meterBars = true;
    };
    LaneVisibility getLaneVisibility() const { return lanes; }
    void setLaneVisibility(LaneVisibility newVisibility);

    // "Declare a caption segment over this range." Raised by promoting a detected span
    // (TASKS.md: "a detected span should be promotable to a caption segment") and by the
    // context menu's own "+ Segment from selection". MainComponent routes it into the
    // same addSegment path the +Segment button uses, so either route still gets the
    // group/file scope question when it applies.
    std::function<void(double, double)> onAddSegmentRequested;

    // Raised whenever the visible window changes (zoom, pan, follow-the-playhead), so a
    // second view of the same timeline -- the stem activity matrix -- can stay locked to
    // it. Two pictures of one reel at different scales would be worse than useless.
    std::function<void(double viewStartFrac, double zoomFactor)> onViewChanged;

    // The View menu (zoom + lanes), built here because this class owns that state, but
    // shown from three places: the header's View button, this view's own right-click, and
    // the macOS menu bar. IDs come from the same range in all three, so there is one
    // definition of what the menu contains and one place that acts on it.
    void buildViewMenu(juce::PopupMenu& menu) const;
    void performViewAction(int actionId);
    static constexpr int kViewActionFirst = 800; // reserved range, kept clear of MainComponent's own ids
    static constexpr int kViewActionLast = 899;

    // Lets the owner (BottomPanel/MainComponent) prepend its own Tags/Segments sections to
    // this view's right-click menu, so the gesture reaches the same items the header does
    // without this class knowing anything about tags or the database.
    std::function<void(juce::PopupMenu&)> buildOwnerMenuSections;
    std::function<void(int)> onOwnerMenuAction;

    // The current click-drag selection in seconds, or nullopt when there isn't one (or
    // no file is loaded, so seconds are meaningless). This is what "add a segment here"
    // reads.
    std::optional<std::pair<double, double>> getSelectionSeconds() const;
    void clearSelection();
    // Selects [start, end] seconds, seeks there, and scrolls it into view when zoomed --
    // what selecting a segment child row in the list does (review round 3).
    void selectRange(double startSeconds, double endSeconds);

    // Play exactly one range and stop at its end (review round 7 item 6: "i will need to be
    // able to play the cue to hear it and name it"). Distinct from selectRange + play:
    // without a stop point, auditioning a 90-second cue on a 41-minute reel runs on into the
    // next four cues, and naming what you just heard means having heard only that.
    void playRange(double startSeconds, double endSeconds);
    void stopPlayback();
    bool isPlaying() const { return transportSource.isPlaying(); }
    // Fires when playback stops for ANY reason, including a ranged audition reaching its
    // own end. Without it the cue workspace's Play/Stop button would stay on "Stop" after
    // the cue finished by itself, which is the state it is least able to notice.
    std::function<void()> onPlaybackStopped;

    std::function<void()> onSelectionChanged;          // enables/disables the Add Segment control
    std::function<void(int64_t)> onSegmentRightClicked; // a band's own context menu (edit/delete)

    // Public so MainComponent's Space-bar key handler (mira's own equivalent of
    // Soundly's "space previews the selected file") can reach it without exposing the
    // rest of this class's playback internals.
    void togglePlayPause();

    // "the osx toolbar should have setting for audio -- like output and buffer" --
    // MiraMenuBarModel's Audio Settings... menu item opens a juce::AudioDeviceSelectorComponent
    // bound to this device manager rather than mira owning a second, redundant one.
    juce::AudioDeviceManager& getAudioDeviceManager() { return deviceManager; }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify(const juce::MouseEvent&, float scaleFactor) override; // trackpad pinch
    void mouseDoubleClick(const juce::MouseEvent&) override;                // -> Fit
    void mouseMove(const juce::MouseEvent&) override;                       // cursor feedback per zone

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override;
    void timerCallback() override;
    juce::Rectangle<int> getWaveformBounds() const;
    double xToFraction(int x) const; // -> absolute fraction of the whole file, accounting for current zoom/pan
    // anchorFrac pins one point of the file in place while zooming (the cursor, for
    // wheel and pinch); nullopt re-centres on the playhead, which is what the buttons want.
    void setZoom(double newZoomFactor, std::optional<double> anchorFrac = std::nullopt);
    void updateVolumeLabel();

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache { 32 }; // 32 thumbnails -- one selected file at a time in practice
    juce::AudioThumbnail thumbnail;
    juce::File currentFile;
    bool loadFailed = false;

    // Playback graph -- own AudioDeviceManager (0 inputs, 2 outputs: no mic permission
    // prompt needed since this never records) rather than sharing one with anything else
    // in mira_ui, since nothing else plays audio yet.
    juce::AudioDeviceManager deviceManager;
    juce::AudioSourcePlayer audioSourcePlayer;
    juce::AudioTransportSource transportSource;
    // Where a ranged audition must stop; <= 0 means "play to the end of the file" (ordinary
    // playback). Checked on the same 30Hz timer that already follows the playhead, which is
    // accurate to ~33ms -- inaudible against a cue boundary, and far simpler than a
    // PositionableAudioSource wrapper that would have to be torn down on every file change.
    double playStopAtSeconds = 0.0;
    std::unique_ptr<juce::AudioFormatReaderSource> readerSource;

    TransportPlayButton playButton;
    MuteButton muteButton;
    juce::Slider volumeSlider;
    juce::Label volumePercentLabel;
    juce::Label timeLabel;
    juce::TextButton zoomOutButton { juce::CharPointer_UTF8("\xe2\x88\x92") }; // minus sign
    juce::TextButton zoomInButton { "+" };
    juce::TextButton zoomResetButton { "Fit" };
    bool muted = false; // output gain forced to 0 while true; the slider itself keeps its own value

    // Click-drag selection in the waveform itself (Soundly reference screenshot: a
    // tinted region between two markers) -- fractions of total length, [0,1], only valid
    // when hasSelection is true.
    bool hasSelection = false;
    bool isDraggingSelection = false;
    double selectionStartFrac = 0.0, selectionEndFrac = 0.0;

    // Segment bands live in their own strip along the bottom of the waveform area, so
    // they never obscure the peaks themselves -- only their tint extends over the wave.
    std::vector<SegmentSpan> segments;
    juce::Rectangle<int> getSegmentBandBounds() const;
    std::optional<int64_t> segmentIdAt(juce::Point<int> position) const;
    // The active span under `position`, when the span lane is showing and the point is
    // inside it -- what the promote-to-segment click reads.
    std::optional<std::pair<double, double>> spanAt(juce::Point<int> position) const;
    static constexpr int kSegmentBandHeight = 16;

    // Lane stack. Laid out bottom-up inside the waveform area -- segment band lowest
    // (it is the oldest and the one that is clicked most), then chords, then spans, with
    // the peaks taking whatever is left. Notes are not in this stack at all: they are
    // drawn over the peaks themselves (decided in review -- a piano roll reads better
    // against the audio it transcribes than parked below it).
    //
    // A lane only takes height when the current file actually has data for it AND the
    // user hasn't switched it off AND the peaks would still be left at least
    // kMinPeaksHeight -- shrinking the bottom panel drops lanes rather than squeezing
    // the waveform down to nothing.
    std::vector<std::pair<double, double>> activeSpans;
    std::vector<ChordMark> chords;
    std::vector<NoteBlock> notes;
    std::vector<double> beats, downbeats;
    std::vector<double> onsets;
    GrooveOverlay groove;
    LaneVisibility lanes;
    int noteLowPitch = 0, noteHighPitch = 0; // recomputed in setNotes, for the overlay's y mapping

    struct LaneLayout
    {
        juce::Rectangle<int> ruler;       // empty when not shown; claimed from the TOP
        juce::Rectangle<int> peaks;
        juce::Rectangle<int> onsetLane;   // empty when not shown
        juce::Rectangle<int> spanLane;    // empty when not shown
        juce::Rectangle<int> chordLane;   // empty when not shown
        juce::Rectangle<int> segmentBand; // empty when not shown
    };
    LaneLayout computeLanes() const;
    void showLanesMenu();
    void showContextMenu();
    void addLaneItemsTo(juce::PopupMenu& menu) const;
    void applyLaneMenuResult(int result);
    void panByFraction(double deltaFrac);
    void scrubTo(int x);
    bool layoutRulerContains(juce::Point<int> position) const;
    // Nice-looking tick interval for the visible duration: the smallest of a fixed
    // 1/2/5/10/15/30/60/... ladder that still leaves ticks at least kMinTickSpacing
    // apart, so labels never collide and the numbers stay ones a person reads easily.
    static double chooseTickSeconds(double visibleSeconds, int widthPixels);
    // Seconds -> x within `area`, honouring the current zoom/pan. The inverse of
    // xToFraction, factored out because every lane needs it.
    int secondsToX(double seconds, juce::Rectangle<int> area) const;
    static juce::Colour chordColour(int rootPitchClass);

    juce::TextButton lanesButton { "Lanes" };
    static constexpr int kRulerHeight = 18;
    static constexpr int kSpanLaneHeight = 8;
    static constexpr int kOnsetLaneHeight = 12;
    static constexpr int kChordLaneHeight = 14;
    static constexpr int kMinPeaksHeight = 40;
    static constexpr int kMinTickSpacing = 64;   // px between time ticks, label width + air
    static constexpr int kMinBarNumberSpacing = 26; // px between bar numbers before they thin out

    // The phase histogram is drawn as a small inset panel over the peaks rather than as a
    // lane of its own: it has no time axis (it is the whole file folded onto one beat), so
    // parking it in the timeline stack would put a non-temporal picture in a row of
    // temporal ones and invite it to be read as if it had a position.
    void paintGrooveHistogram(juce::Graphics& g, juce::Rectangle<int> peaks) const;
    static constexpr int kGrooveHistogramWidth = 168;
    static constexpr int kGrooveHistogramHeight = 74;

    // What the current drag is doing. Panning and scrubbing are modes rather than
    // separate components because they share the same surface as selection -- which one
    // a press starts is decided once in mouseDown (zone + modifiers) and then held for
    // the whole drag, so a gesture can't change meaning halfway through.
    enum class DragMode { none, selecting, panning, scrubbing };
    DragMode dragMode = DragMode::none;
    int dragLastX = 0;            // panning: last mouse x, for the incremental delta
    bool wasPlayingBeforeScrub = false;

    // Zoom: "will be helpful for bigger files, 40min files -- zoom in zoom out and
    // reset". zoomFactor 1.0 == whole file visible; viewStartFrac is the left edge of
    // the visible window, as a fraction of total length, always <= 1 - 1/zoomFactor.
    double zoomFactor = 1.0;
    double viewStartFrac = 0.0;
    static constexpr double kMaxZoom = 64.0;
    static constexpr int kTransportRowHeight = 36; // shared by resized() and getWaveformBounds() -- must match
};
