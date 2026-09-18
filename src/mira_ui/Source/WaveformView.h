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

// A drawn icon rather than a character. "use the magnifier icon and use that instead of
// - + and stuff, standard UI" -- and drawn, not a glyph, because the magnifier and hand
// codepoints are emoji in most system fonts: they render as colour emoji or as a blank
// box depending on which font the button happens to resolve, and neither can be tinted to
// match the rest of the transport.
class GlyphButton : public juce::Button
{
public:
    enum class Glyph { ZoomIn, ZoomOut, Fit, Hand, ThumbUp, ThumbDown, Scissors, FullLength };
    explicit GlyphButton(Glyph g) : juce::Button("icon"), glyph(g) {}
    void setGlyph(Glyph g) { glyph = g; repaint(); }
    // Keep and Discard are the only two irreversible-feeling actions in the window and
    // they sit side by side as mirror images of one shape. Colour is what separates them
    // at a glance; without it the only difference is which way a small thumb points.
    void setTint(juce::Colour c) { tint = c; hasTint = true; repaint(); }

private:
    void paintButton(juce::Graphics& g, bool over, bool down) override;
    Glyph glyph;
    juce::Colour tint;
    bool hasTint = false;
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

    // Length of the loaded audio, 0 when nothing is loaded. Needed by callers that have
    // to describe the whole file (a full-length edit segment, say) without opening it a
    // second time.
    double getTotalLengthSeconds() const;

    // The lanes menu is about ANALYSIS overlays -- chords, notes, beats, the bars ruler.
    // A freshly generated take has none of that, so the button is only noise there:
    // "why have lanes here, we are doing nothing of those sorts now".
    void setLanesButtonVisible(bool shouldShow);

    // Fades, drawn ON the waveform and dragged by their handles, the way every DAW does
    // it. Two number sliders cost a whole row and still never told you where the fade
    // landed against the audio -- which is the only thing anyone actually wants to know
    // about a fade. Seconds, measured inwards from each end of the ACTIVE range (the
    // trim if there is one, the whole file otherwise).
    void setFades(double fadeInSeconds, double fadeOutSeconds);
    void setFadeRange(double startSeconds, double endSeconds); // 0,0 = whole file
    double getFadeIn() const { return fadeInSecs; }
    double getFadeOut() const { return fadeOutSecs; }
    std::function<void(double, double)> onFadesChanged;

    // Space held back at the LEFT of the transport row for the owner's own controls, so
    // an edit strip can share the transport line instead of costing a second row. The
    // area comes back in this component's coordinates; the owner converts.
    void setTransportReserve(int px);
    juce::Rectangle<int> getTransportReserveArea() const { return reserveArea; }

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
        // The tempo of the grid the meter bars are actually drawn on (beat_this), which
        // is NOT necessarily `bpm` above (the fitted grid). Shown when they disagree.
        double beatGridBpm = 0.0;
        // `$.rhythm.beat_grid_stability` -- the share of beat intervals within 25% of the
        // median. Below kGridStabilityWarn the tracker changed pulse level partway
        // through and the grid drawn from this tempo is not one grid. Drawn in the ruler
        // rather than in a panel, because it qualifies every line on screen.
        double gridStability = 0.0;
    };

    void setGroove(GrooveOverlay newGroove);

    // Which lanes the user has switched off, independent of whether the current file has
    // data for them (a lane with no data never renders regardless).
    // What the ruler counts in. mira showed clock time and nothing else, and on a piece
    // of music that is the wrong unit for most questions asked of it: "where does the
    // second chorus start" has the answer "bar 33", not "1:34.2". The bar numbers that
    // did exist were drawn on the grid lines down the lanes, thinned out to whatever fit,
    // and never in the ruler itself.
    //
    // Bars also makes a wrong tempo visible instead of arguable: if the tempo is wrong
    // the bar numbers walk off the music, and the eye catches that in a second where it
    // will never catch it in a BPM readout.
    enum class RulerMode
    {
        Time, // m:ss, with a fractional part once the ticks go under a second
        Bars  // bar numbers, and bar.beat once the beats are far enough apart to read
    };

    struct LaneVisibility
    {
        bool ruler = true;
        // BARS by default. This is a music tool and bars are the unit its questions are
        // asked in -- "where does the second chorus start" has the answer "bar 33", not
        // "1:34.2". The paint code falls back to time ticks on its own for any file with
        // no downbeats (a one-shot, an unanalysed file), so this costs nothing where
        // bars are meaningless.
        RulerMode rulerMode = RulerMode::Bars;
        bool barGrid = true;
        bool spans = true;
        bool chords = true;
        bool notes = true;
        bool onsets = true;
        // The three DIAGNOSTIC layers, off by default since 2026-09-17.
        //
        // With all of them on, the waveform carried four different claims about where
        // the beat is -- detected downbeats, the fitted groove grid, the meter's own bar
        // lines, and the ruler -- in four colours, none of them labelled as the answer.
        // The user's words: "there are a lot of things that don't align... multiple
        // things now confusing." That is a fair description and it was the view's fault,
        // not the data's. A picture showing four rival hypotheses at once is a debugging
        // tool, and debugging tools should be opt-in.
        //
        // What is left on is one grid and the evidence for it: bar lines from the
        // reported tempo, numbered in the ruler from the same tempo, and the onsets they
        // are supposed to land on. If the lines drift off the hits, the tempo is wrong --
        // which is the one question this view exists to answer.
        bool grooveGrid = false;
        bool grooveHistogram = false;
        bool meterBars = false;
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

    // MIRA-GENERATE.md Phase 5: "audition plays the EDIT, not the raw take". An edit is a
    // trim plus fades plus a gain, and the trim is playRange above -- these two are the
    // rest of it. The envelope is applied by the caller stepping this from a timer, which
    // is an APPROXIMATION of what export renders sample-by-sample; it is accurate enough
    // to judge a fade by ear and is not what writes the file.
    void setPlaybackGain(float gain);
    double getPlayPositionSeconds() const;
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
    juce::AudioDeviceManager& getAudioDeviceManager() { return *device; }

    // Play through someone else's device manager instead of this view's own.
    //
    // Every WaveformView used to own one, so a browser and two project windows meant
    // three independent audio devices -- and Audio Settings only ever reconfigured the
    // browser's. Changing the driver there did nothing to what a project window played
    // through, which is exactly what "changing the driver doesn't take" looks like from
    // the outside. One device for the app is the fix; the owned one stays as the default
    // so a WaveformView built on its own still works.
    void useSharedDeviceManager(juce::AudioDeviceManager& shared);

    // Audio settings did not survive a relaunch: initialiseWithDefaultDevices ran every
    // launch and there was nowhere to put a choice (the app has no PropertiesFile at
    // all). These two move the state as a STRING, not a file path or a database handle --
    // this class opens audio files, not databases, and that boundary is deliberate (see
    // the note above SegmentSpan). MainComponent owns where it is kept.
    juce::String getAudioDeviceState() const
    {
        if (auto xml = device->createStateXml()) return xml->toString();
        return {};
    }

    void restoreAudioDeviceState(const juce::String& xmlText)
    {
        if (xmlText.isEmpty()) return;
        auto xml = juce::parseXML(xmlText);
        if (xml == nullptr) return;
        // selectDefaultDeviceOnFailure: a saved interface that is not plugged in today
        // must fall back to the built-in output, not leave playback silently dead.
        device->initialise(0, 2, xml.get(), true);
    }

    // Fires whenever the device setup actually changes, so the new state can be stored.
    std::function<void()> onAudioDeviceChanged;

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
    float userGain() const;
    static constexpr double kVolumeMinDb = -60.0;  // bottom of the track = silence
    static constexpr double kVolumeMaxDb =   6.0;  // a little headroom above unity

    juce::AudioFormatManager formatManager;
    juce::AudioThumbnailCache thumbnailCache { 32 }; // 32 thumbnails -- one selected file at a time in practice
    juce::AudioThumbnail thumbnail;
    juce::File currentFile;
    bool loadFailed = false;

    // Playback graph -- own AudioDeviceManager (0 inputs, 2 outputs: no mic permission
    // prompt needed since this never records) rather than sharing one with anything else
    // in mira_ui, since nothing else plays audio yet.
    juce::AudioDeviceManager ownedDeviceManager;
    juce::AudioDeviceManager* device = &ownedDeviceManager;
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
    GlyphButton zoomOutButton { GlyphButton::Glyph::ZoomOut };
    GlyphButton zoomInButton  { GlyphButton::Glyph::ZoomIn };
    GlyphButton zoomResetButton { GlyphButton::Glyph::Fit };
    // Drag in the waveform selects by default and PANS while this is on -- "use the hand
    // symbol for moving for wav scrolling". A mode, not a modifier, because the selection
    // drag is the primary gesture here and must not need a key held to stay itself.
    GlyphButton panButton { GlyphButton::Glyph::Hand };
    bool lanesShown = true;
    double fadeInSecs = 0.0, fadeOutSecs = 0.0;
    double fadeRangeStart = 0.0, fadeRangeEnd = 0.0;   // 0,0 = whole file
    int transportReserve = 0;
    juce::Rectangle<int> reserveArea;
    bool panMode = false;
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

    // One beat, in seconds, of the grid this view draws and numbers -- taken from the
    // REPORTED tempo (groove.beatGridBpm, i.e. what the BPM readout, the caption and the
    // training data all say), never re-derived from the beat list. Everything on screen
    // that claims to know where a beat is reads this, so the ruler's bar numbers and the
    // bar lines under them cannot drift apart. 0 when there is no tempo.
    double gridBeatSeconds() const;

    // Bar line times -- every `meter`-th DETECTED beat, starting at the chosen phase.
    // This is `bar_lines_from` from the reference tool, and it is why the lines sit on
    // the music: they ARE beats, not a straight line laid out from a BPM. A BPM grid can
    // only agree with a real performance at one point and drifts away either side of it,
    // which is exactly what "the grids don't align with the waveform" looked like.
    std::vector<double> barLineTimes() const;
    // Nice-looking tick interval for the visible duration: the smallest of a fixed
    // 1/2/5/10/15/30/60/... ladder that still leaves ticks at least kMinTickSpacing
    // apart, so labels never collide and the numbers stay ones a person reads easily.
    static double chooseTickSeconds(double visibleSeconds, int widthPixels);
    // Seconds -> x within `area`, honouring the current zoom/pan. The inverse of
    // xToFraction, factored out because every lane needs it.
    int secondsToX(double seconds, juce::Rectangle<int> area) const;
    static juce::Colour chordColour(int rootPitchClass);

    juce::TextButton lanesButton { "Lanes" };
    // 22, not 18: the bar ruler carries bold bar numbers and dimmer bar.beat labels on
    // a tinted band, and 18 px left no room between the digits and the tick marks.
    static constexpr int kRulerHeight = 22;
    static constexpr int kSpanLaneHeight = 8;
    static constexpr int kOnsetLaneHeight = 12;
    static constexpr int kChordLaneHeight = 14;
    static constexpr int kMinPeaksHeight = 40;
    static constexpr int kMinTickSpacing = 64;   // px between time ticks, label width + air
    static constexpr int kMinBarNumberSpacing = 26; // px between bar numbers before they thin out
    // bar.beat labels are longer than a bare bar number, so they need more room before
    // they start colliding with each other.
    static constexpr int kMinBeatNumberSpacing = 34;
    // Matches kGridCrossCheckBelow in Mir.cpp -- the same line the analyzer uses to
    // decide a grid needs a second opinion. One threshold, two places that must agree.
    static constexpr double kGridStabilityWarn = 0.90;

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
    enum class DragMode { none, selecting, panning, scrubbing, fadeIn, fadeOut };
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
