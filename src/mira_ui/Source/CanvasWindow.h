#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "MiraLookAndFeel.h"
#include "CanvasEngine.h"
#include "GenerateWindow.h"
#include "Timecode.h"

#include <array>

// ---- the canvas experiment: the picture --------------------------------------------
//
// A free surface with a time axis and no grid. Blocks of audio sit where you put them,
// several sound at once, and nothing asks for a tempo. Blockhead's actual radical idea is
// not the absence of TIME -- it has a horizontal time axis like any DAW -- it is the
// absence of a GRID: you make something first and derive the tempo afterwards, if you
// ever want one at all.
//
// That suits mira better than it suits most hosts, because mira MEASURES tempo. A
// generated take arrives with no tempo you chose, and mira works out its BPM, beats,
// downbeats and a confidence in that grid. So "make first, derive the grid later" is not
// a workflow mira has to adopt -- it is the one mira already has.
//
// EXPERIMENTAL AND SEPARATE, on purpose. This window touches no part of the generate
// window, the take stack or the browser. It reads a project's takes and plays them. If it
// turns out not to feel good it can be deleted in one commit, and the thing that works
// today is untouched.
namespace mira::canvas {

// ---- MIRA-VIDEO.md Phase 1: a clip of picture on the timeline -----------------------
//
// A `Block` gains nothing from this. A video clip is its own thing: it is never summed,
// never faded, never exported, and it is the only object on the canvas mira does not own
// the samples of. The file is referenced where it lies -- never copied, never re-encoded.
struct VideoClip
{
    juce::File file;           // the .mp4/.mov as given
    double start = 0.0;        // where it sits on the canvas timeline
    double length = 0.0;       // from an AVAsset query, NOT from getVideoDuration()
    double sourceOffset = 0.0; // where in the film `start` corresponds to
    // The picture's own clock, for the timecode ruler (Phase 3).
    double fps = 0.0;          // 0 = unread
    bool dropFrame = false;
    double startTimecode = 0.0;    // seconds; 01:00:00:00 is 3600.0
    juce::int64 audioBlockId = 0;  // the locked reference block (Phase 2), or 0
};

// ---- MIRA-BLOCKS.md step 2: what the library measured about a take ------------------
//
// The canvas has no database of its own and is not getting one (the same reasoning that
// keeps `loadSetting`/`saveSetting` as callbacks). So the owner runs the analysis, reads
// `files.machine` back, and hands the canvas THIS -- a finished answer in the canvas's
// own vocabulary rather than a JSON blob and a parser on both sides.
//
// Every number here is in SOURCE time (seconds into the file), because that is what the
// block stores and what survives a trim.
struct Measurement
{
    // False means the take has no usable grid -- the row is missing, the analysis failed,
    // or it produced no beats. `note` says which. Convention 6: a failed measurement is
    // never a substitute value.
    bool ok = false;
    juce::String note;
    double bpm = 0.0;
    // $.rhythm.beat_grid_stability -- the share of beat intervals within 25% of the
    // median. THE gate: 0.99-1.00 on grids known to be right, 0.64-0.85 on grids known to
    // be wrong (CLAUDE.md 2026-09-17, six tracks against the user's own tempos).
    double stability = 0.0;
    int    meter = 0;            // 0 = not measured
    double barSpread = 0.0;      // past 1.5 the beats drifted; the meter is not to be trusted
    double firstDownbeat = -1.0; // < 0 = none found
    // THE REAL BEATS, not a tempo to lay a grid out from. `$.rhythm.beat_this_beats` and
    // `beat_this_downbeats`. A synthetic grid built from one BPM scalar drifts away from
    // the audio on anything that is not metronomic -- which is what "the onsets are not
    // aligning with the bars" actually was -- and the browser has drawn real downbeats for
    // exactly this reason since the bar ruler was written.
    std::vector<double> beats, downbeats;
    juce::String key;
    // $.onset_times. Drawn in the footer (2.5) and the slice points step 5 will use.
    // Not gated by `stability`: an onset is a measurement of the AUDIO, and whether the
    // beat grid is trustworthy says nothing about whether a transient is where it is.
    std::vector<double> onsets;
};

class CanvasView : public juce::Component,
                   public juce::FileDragAndDropTarget,
                   private juce::Timer
{
public:
    CanvasView(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
               juce::AudioThumbnailCache& cache);
    ~CanvasView() override;

    void setProject(const juce::File& project);
    void attachTo(juce::AudioDeviceManager& device) { player.attachTo(device); }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;

    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    // What the toolbar drives.
    void togglePlay();
    void addFiles(const juce::Array<juce::File>& files, double atSeconds, int lane);
    void removeSelected();
    void setLoopFromSelection();
    void toggleLoop();
    // "stopping the playback the play head returns to the start position of the last
    // playback" -- the DAW default, and the only sane one when you are looping a bar to
    // judge it: stop, change something, play again from the same place.
    void setReturnOnStop(bool shouldReturn) { returnOnStop = shouldReturn; }
    bool getReturnOnStop() const { return returnOnStop; }

    bool isPlaying() const { return player.isPlaying(); }
    bool isLooping() const { return player.isLooping(); }
    double getPositionSeconds() const { return player.getPositionSeconds(); }
    double getLengthSeconds() const { return player.getLengthSeconds(); }
    int getBlockCount() const { return static_cast<int>(items.size()); }
    void zoomBy(double factor, int aroundX);
    // The generation strip drawn ON the block being generated into, so the progress is
    // where you are looking rather than on the far side of the window. Negative means
    // nothing is running.
    void setGenerationProgress(double fraction);
    // 0 = this block, 1 = its track, 2 = every track as its own file. All three render
    // through the player, so what lands on disk is what the canvas plays.
    void promptExport(int what, juce::int64 id);
    // Moves every take no block is showing to the Trash. Asks first, with the count and
    // the size.
    void promptCleanup();
    // ONE SCALE for the fader and the meter, on the tracks and on the master. That is what
    // makes a channel strip readable: a fader sitting at -12 lines up with a meter reading
    // -12, and you can see the headroom you have left without arithmetic on a decibel.
    //
    // Warped rather than linear, the way a console is: the top 18 dB -- where you actually
    // work -- gets nearly half the travel, and the bottom 30 dB, where the difference
    // between -52 and -58 matters to nobody, gets a quarter.
    static double dbToNorm(double db);
    static double normToDb(double norm);
    static constexpr double kFaderTopDb = 6.0, kFaderBottomDb = -60.0;
    void zoomVertical(double pixels);
    void panBy(double seconds);
    // END every block the playhead stands on, there. One block, its audio cut to the
    // playhead, ready to extend from -- which is what Cmd-E is for.
    void cutAtPlayhead();
    // Split into TWO, each half keeping its own end of the audio. The right half gets its
    // own name and folder, like a duplicate does. On the menu, not the keyboard: it makes
    // a second block with an empty generator, which is a thing you ask for deliberately.
    void splitAtPlayhead();
    void fit();
    void addEmptyBlock();

    // ---- the document (.mira) ---------------------------------------------------------
    //
    // A project is a FILE you open, the way .npr or .als is, not a folder you point the
    // app at. That distinction is the whole of "normal daw behaviour": a folder is
    // ambiguous -- is this a project, or the folder containing one? -- and mira has
    // already been bitten by exactly that, opening a parent folder as a project and
    // listing the real project inside it as a cue.
    //
    // The file lives IN the project folder with the block folders beside it, and block
    // paths are stored RELATIVE to it, so moving or renaming the whole folder keeps
    // working. Absolute paths are kept for anything outside.
    static constexpr const char* kExtension = ".mira";
    bool newDocument(const juce::File& folder, const juce::String& name);
    bool openDocument(const juce::File& miraFile);
    bool saveDocument();                       // false when there is nowhere to save yet
    bool saveDocumentAs(const juce::File& miraFile);
    juce::File getDocumentFile() const { return documentFile; }
    juce::String getDocumentName() const;
    bool isDirty() const { return dirty; }
    std::function<void()> onDocumentChanged;   // title, mostly
    std::function<void()> onSaveRequested, onOpenRequested, onNewRequested;
    void addLane();
    // Reorder the tracks. Drag a lane header up or down, or Canvas > Move Track Up/Down.
    // Everything keyed on the lane INDEX travels with it -- the blocks, the name, the
    // fader, the mute and solo bits, the meter, and which lane is the reference -- because
    // a reorder that moved the blocks but left the faders behind would be worse than no
    // reorder at all.
    void moveLane(int from, int to);
    void moveSelectedLane(int delta);
    // ---- moving BLOCKS from the keyboard --------------------------------------------
    // Dragging was the only way a block had ever been movable, and a block covered in grid
    // lines is a block whose drag has competition. The keyboard has none, lands exactly
    // where asked, and repeats.
    void nudgeSelection(double seconds);
    void moveSelectionByLane(int delta);
    // How far one arrow press moves a block. On the toolbar because the right amount is a
    // property of the music and the job, not of the app.
    //
    // A BEAT and a BAR are in the list because this canvas knows them now -- that is the
    // whole of step 2 paying for itself. They resolve against the SELECTED BLOCK's own
    // grid, which is the only tempo in the room: there is no project tempo here to nudge
    // by, and inventing one would be the global grid coming back through the toolbar.
    enum class Nudge { Frame, Ms10, Ms100, Sec1, Beat, Bar };
    Nudge nudgeUnit = Nudge::Ms100;
    void setNudgeUnit(Nudge n) { nudgeUnit = n; }
    Nudge getNudgeUnit() const { return nudgeUnit; }
    // Resolved at the moment of the press, not when the menu changed: a beat means
    // whatever the block you are nudging says a beat is, and you may have analysed it
    // since. Falls back to 100 ms when a beat is asked for and nothing knows one.
    double nudgeAmount() const;

    // ---- snapping an edge to the grid -------------------------------------------------
    // "once the file is analysed, closing the block from left or right can be snapped
    // according to bars or beats or the quantized value, so it's easy to make loops."
    //
    // This is the ONE place the canvas enforces anything, and it stays consistent with
    // "the grid is drawn, never enforced" by being OFF by default, per-block (it snaps to
    // the block's OWN grid, never a project one -- there is no project one), doing nothing
    // at all on a block with no tempo, and bypassable with alt while you drag.
    enum class Snap { Off, Bar, Beat, Half, Quarter };
    Snap snapUnit = Snap::Off;
    void setSnapUnit(Snap s) { snapUnit = s; }
    Snap getSnapUnit() const { return snapUnit; }


    // ---- the metronome ----------------------------------------------------------------
    // "generated at 140 bpm, analysed 142.9, but no way to know which is right since there
    // is no click." This is that. It clicks the SELECTED block's grid -- the measured beats
    // when it has been analysed -- so what you hear is the same answer `gridLinesOf` draws,
    // and the question "are those bar lines on the music" becomes one you can ask with your
    // ears instead of your eyes.
    // Step 3.0 -- halve or double the selected blocks' tempo, moving both the number and
    // the measured beats that are drawn. `tempoSource` is deliberately unchanged: choosing
    // an octave is not un-measuring anything.
    void shiftTempoOctave(int delta);
    // Steps 3.2-3.5 -- conform the selected block's take to `targetBpm`, as a NEW take in
    // its folder. The original is untouched and Choose Take switches between them.
    bool stretchSelectionTo(double targetBpm);
    // Rate AND phase: stretch the selected block to `parentId`'s tempo, then move it so its
    // bar 1 lands on the nearest line of that block's bar grid, EXTENDED past its own ends.
    // One rule covers layering (phase lock) and sequence (the bar count continues), and the
    // TRACK never enters into it -- a block moves between tracks freely.
    bool conformSelectionTo(juce::int64 parentId);

    // ---- MIRA-BLOCKS.md step 4: the child ---------------------------------------------
    // A link is a SOURCE FOR A NUMBER, not a second system: it fills the child's prompt
    // before you generate, it tells you when the parent has moved underneath it, and it
    // does nothing else. It never re-stretches audio on its own.
    void setFollows(juce::int64 childId, juce::int64 parentId);   // 0 = independent
    // A NEW block that follows an existing one: the parent's generator settings, its tempo
    // and key in the prompt, a whole number of bars long, starting on one of its bar lines.
    // This is the gesture step 4 was actually for -- a link fills the prompt BEFORE you
    // generate, so offering it only on blocks that already have audio is one generation
    // too late.
    void addBlockFollowing(juce::int64 parentId);
    // Walks the whole chain: A -> B -> C -> A is the same mistake with more rope.
    bool wouldCycle(juce::int64 childId, juce::int64 parentId) const;

    // The tempos the block menu last offered, so the callback can answer by index rather
    // than by encoding a bpm in a menu id.
    std::vector<double> stretchTargets;
    // Likewise for the Follows submenu: block ids, answered by index.
    std::vector<juce::int64> followTargets;
    void toggleMetronome();
    bool metronomeIsOn() const { return metronomeOn; }
    // Rebuilt whenever the selection, the grid or the geometry changes -- it is a list of
    // instants, so anything that moves a block moves its clicks.
    void rebuildClickTrack();
    // Removes a track and everything on it, and closes the gap -- blocks on the tracks
    // below move up, because a track numbered 4 with nothing above it is not a hole you
    // meant to leave.
    void removeLane(int lane);
    int getSelectedLane() const { return selectedLane; }
    juce::String nextBlockName() const;
    // Markers get their OWN strip between the ruler and the tracks. They were drawn on the
    // ruler first and collided with its time labels -- neither readable, on the one strip
    // every feature so far has wanted to put something on.
    int markerStripH() const { return markers.empty() ? 0 : 19; }
    int videoStripTop() const { return topRuler + markerStripH(); }
    // A COPY, not another version. The trim, the fades, the gain, the generator settings
    // and the same chosen take -- so four bars you like can become four bars you like
    // twice, which is arranging rather than generating.
    //
    // It gets a NEW NAME, and so a new folder. The name is the generation target: a
    // duplicate that kept its original's name sent its generations into the original's
    // folder, and `adoptTake` gave the audio to whichever block it found there first --
    // the original. The colour is unchanged on purpose; colour belongs to the TRACK.
    void duplicateSelection();
    int getLaneCount() const { return laneCount; }
    void writeTo(const juce::File& miraFile) const;
    bool readFrom(const juce::File& miraFile);
    // The document as text, and back. Split out of writeTo/readFrom because undo keeps
    // snapshots of exactly this -- one serialisation, so an edit cannot be undoable and
    // unsaveable at the same time.
    juce::String toJson(const juce::File& base) const;
    bool fromJson(const juce::String& json, const juce::File& base, bool refit);
    void undo();
    void redo();
    bool canUndo() const { return !undoStack.empty(); }
    bool canRedo() const { return !redoStack.empty(); }
    void markDirty();
    void applySettingsToSelection(const juce::var& settings);
    void chooseTakeForSelection(const juce::File& take);
    // name, block folder, generator settings, chosen take -- everything the inspector
    // needs, pushed rather than pulled so it cannot show a stale block.
    // Clicking a block asks the OWNER to open the real generate window bound to that
    // block's folder. Not a second generator: the canvas had its own cut-down one for
    // exactly one build, and a smaller copy of the generator is a copy that will drift
    // from it -- different LoRA list, no prompt builder, no step gates. There is one
    // generator in mira and this points it at a block.
    // Which block the side panel should be showing, or an empty name for none.
    // Name, folder AND the block's own generator settings. Settings are part of it
    // because a block OWNS its generator: pointing the panel at another block without
    // bringing its prompt and LoRAs along is what made every block look like it shared one
    // recipe -- only the title ever changed.
    std::function<void(const juce::String& name, const juce::File& folder,
                       const juce::var& settings)> onOpenGenerator;
    // Read the panel's current state back out, so the block you are leaving keeps what you
    // typed into it.
    std::function<juce::var()> onCaptureSettings;
    // The block's geometry, which the generator reads as its duration: length is the
    // duration to generate, tail is how much of it is empty and therefore what an extend
    // would fill.
    std::function<void(double lengthSeconds, double tailSeconds, bool hasAudio)> onBlockGeometry;
    // Extend or remix the selected block, both using the prompt exactly as it stands on
    // screen. EXTEND fills the empty tail and leaves the existing audio bit-exact; REMIX
    // regenerates the whole block, guided by the take it already has.
    std::function<void(const juce::File& take, double rangeStart, double totalSeconds,
                       bool remix)> onExtendRequested;
    // Why an extend did NOT run. Without it the button simply did nothing, which is
    // indistinguishable from a generation that failed (convention 6).
    std::function<void(const juce::String&)> onExtendRefused;
    // Something worth knowing about a take that just landed -- so far, that it stops
    // sounding well before its length.
    std::function<void(const juce::String&)> onTakeNote;

    // ---- MIRA-BLOCKS.md step 2 -----------------------------------------------------
    // Analyse this take. The owner registers it in the library if it is not there yet,
    // runs the analyzer, reads the row back and calls `analysisArrived`. Asynchronous by
    // nature -- a minute of work on a long take -- which is why the block carries a
    // Running state rather than this returning anything.
    std::function<void(const juce::File& take, juce::int64 blockId)> onAnalyseRequested;
    // What the library ALREADY knows about a take, without analysing anything. Used when
    // a document opens, so a block that was analysed in a previous session gets its
    // onsets back without re-measuring. Never touches the grid: the document already
    // holds the tempo that was adopted, and re-adopting it here would let a measurement
    // silently outrank a number you typed afterwards.
    std::function<Measurement(const juce::File& take)> onMeasurementLookup;
    // The answer. Safe to call for a block that has since been deleted or repointed at
    // another take -- it checks both before touching anything.
    void analysisArrived(juce::int64 blockId, const juce::File& take, const Measurement& m);
    // Analyse every selected block that has audio. The menu item and the header chip both
    // land here, so they cannot disagree about what Analyse means.
    void analyseSelection();

    // THE CONFIDENCE GATE (MIRA-BLOCKS.md 2.4), and it is a starting guess -- labelled
    // one until it has been measured against real generated takes rather than against the
    // six ground-truth tracks below.
    //
    // 0.90 is the gap between the two clusters the only ground truth this project has
    // produced: grids known to be RIGHT read 0.99/1.00/1.00, grids known to be WRONG read
    // 0.64/0.67/0.85 (CLAUDE.md 2026-09-17). It is also the number `mira analyze` already
    // uses to decide a grid is worth cross-checking against a second postprocessor, so
    // using a different one here would mean the analyzer doubted a grid the canvas
    // accepted without argument.
    //
    // Below it nothing is adopted. A wrong grid imposed confidently is the failure this
    // project has already had twice (MIRA-BLOCKS.md §10), and this is the whole defence.
    //
    // Measured against the library rather than assumed to be harmless: of 819 analysed
    // rows carrying a stability, 468 clear 0.90 and 385 clear 0.95. So this refuses
    // roughly four takes in ten -- it is a real gate that will fire often, not a
    // rubber stamp, and that is what it is for.
    static constexpr double kGridConfidenceGate = 0.90;
    // Past this the beats drifted through the bar and the meter is not a measurement of
    // anything (ANALYSIS.md; the groove panel already turns red here). It is the SAME
    // number the groove panel uses, deliberately: two thresholds for one question would
    // let the canvas adopt a meter the panel three windows away is drawing in red.
    //
    // Measured over the 386 rows that have a meter AND clear the stability gate above:
    // min 1.005, p25 1.075, p50 1.336, p75 1.647, max 39.4. So 1.5 sits around p65 and
    // takes the top third of the spread out -- stricter than the middle of the corpus,
    // which is the right direction for a number that overwrites something.
    static constexpr double kMeterSpreadLimit = 1.5;
    void extendSelection(bool remix);
    // Length, tail and whether there is audio, for the single selection. What decides
    // whether Extend and Remix can do anything.
    struct Geometry { double length = 0.0, tail = 0.0; bool hasAudio = false; };
    Geometry selectionGeometry() const;
    // Fold the panel's current state into whichever block it belongs to. Called before a
    // save and whenever the panel changes block, because otherwise a prompt typed and
    // never switched away from would not be in the document.
    void syncPanelSettings();
    // Double-click asks for the panel to be SHOWN, not just repointed -- a folded panel
    // that silently changed which block it was about would be a no-op you cannot see.
    std::function<void()> onRevealGenerator;
    // A take generated into a block's folder becomes that block's audio.
    void adoptTake(const juce::File& folder, const juce::File& take);
    float readAndClearPeak() { return player.readAndClearPeak(); }
    double getDeviceRate() const { return player.getDeviceRate(); }
    float readAndClearPeak(int channel) { return player.readAndClearPeak(channel); }
    void setMasterGain(float g) { player.setMasterGain(g); }
    float getMasterGain() const { return player.getMasterGain(); }
    juce::File getProjectFolder() const { return projectFolder; }

    // ---- picture (MIRA-VIDEO.md Phase 1) ----------------------------------------------
    //
    // ONE video track, with clips on it -- Phase 1 loads one, Phase 4 makes it several.
    // One track keeps the picture unambiguous: there is only ever one thing to look at.
    // Phase 4: several clips on the ONE video track, laid end to end. Appends after the
    // last one, so opening a second reel never moves the first.
    void addVideoClip(const juce::File& file, double lengthSeconds, double framesPerSecond);
    void removeVideoClip(int index);
    // Which clip sits under this timeline position, or -1.
    int videoClipAt(double seconds) const;
    // Right-click a clip in the PICTURE track.
    void showVideoClipMenu(int index, juce::Point<int> at);

    // ---- markers: the spotting notes (MIRA-VIDEO.md Phase 5) --------------------------
    //
    // A place on the timeline with a name -- "she turns", "titles out", "hit". This is
    // what a spotting session produces, and it is the thing a cue's length is decided by,
    // so it is a first-class object on the canvas rather than an annotation on a block.
    struct Marker { double seconds = 0.0; juce::String name; };
    void addMarkerAtPlayhead();
    void removeMarker(int index);
    void renameMarker(int index);
    // The marker nearest this x, within a few pixels, or -1.
    int markerNear(int x) const;
    // Which marker's LABEL is under this x in the markers row. The label runs from its
    // marker to the next one, so the whole plate is the grab handle -- an 8-pixel line is
    // not something to ask anyone to hit.
    int markerAtStripX(int x) const;
    void setMarkerName(int index, const juce::String& name);
    void setMarkerTime(int index, double seconds);
    // Put the playhead on it, and bring it into view if it is off screen.
    void gotoMarker(int index);
    // The list changed -- added, removed, renamed, moved. What the marker window listens to.
    std::function<void()> onMarkersChanged;
    // The first marker strictly after this position, or -1.
    int markerAfter(double seconds) const;
    const std::vector<Marker>& getMarkers() const { return markers; }
    // 5.2 -- a block at the playhead, as long as the gap to the next marker. The whole
    // gesture of scoring to picture: you know where the cue starts and where it has to be
    // out by, and the length follows from those two.
    void addBlockToNextMarker();
    // 5.4 -- every block as a row: name, in, out, length, key, tempo.
    void promptExportCueSheet();
    // ---- timecode (MIRA-VIDEO.md Phase 3) ---------------------------------------------
    //
    // SECONDS or TIMECODE, everywhere at once: the ruler, the transport clock and the
    // block headers all read the same way, because the number you say out loud and the
    // number on the screen have to be the same number.
    enum class Ruler { Seconds, Timecode };
    void setRulerMode(Ruler r);
    Ruler getRulerMode() const { return rulerMode; }
    // The format the picture is in. With no clip loaded it is 25 fps at 00:00:00:00 --
    // said out loud in the ruler menu rather than silently assumed.
    tc::Format timecodeFormat() const;
    void setTimecodeFps(double fps);
    void setTimecodeDropFrame(bool drop);
    void setTimecodeStart(double timecodeSeconds);
    // Timeline seconds as the ruler currently reads them.
    juce::String formatPosition(double seconds) const;
    // Right-click the ruler: seconds or timecode, the frame rate, drop-frame, the start.
    void showRulerMenu(juce::Point<int> at);
    void promptStartTimecode();
    void clearVideo();
    bool hasVideo() const { return !videoClips.empty(); }
    const std::vector<VideoClip>& getVideoClips() const { return videoClips; }
    // Fired when the clip CHANGES -- a load, or a document that brought one with it --
    // and not on an undo that left the same file in place, because reopening a 40-minute
    // film to undo a fade would be a three-minute undo.
    // The video track changed -- a clip added, removed, or brought in by a document.
    // Deliberately not "here is the clip": with several of them the listener has to look
    // at the whole track anyway, and a per-clip callback would be a second description of
    // the same list.
    std::function<void()> onVideoClipsChanged;
    std::function<void()> onVideoCleared;

    // ---- the reference track (MIRA-VIDEO.md Phase 2) ---------------------------------
    //
    // The film's own audio, on a RESERVED lane. It is an ordinary block with two
    // differences, and both are enforced where the gesture happens rather than by a flag
    // something downstream has to remember to check:
    //
    //   - it cannot be dragged, trimmed or removed on its own. It belongs to its picture,
    //     and a reference that has drifted from its film is worse than no reference.
    //   - it is skipped by every export path. You do not want dialogue in your stems, and
    //     you never want to FIND it there.
    //
    // It keeps its fader, its mute and its meter, because scoring to picture means riding
    // the reference under the cue constantly.
    void attachReference(const juce::File& audio, double startOnTimeline);
    // The reference lane, made if it does not exist yet. Several clips mean several
    // reference blocks on the one lane.
    int ensureReferenceLane();
    void detachReference();
    // How far the reference's waveform has got, or -1 when there is none or it is done.
    // MIRA-VIDEO.md 0.3 measured 12x realtime off an external drive -- 200 seconds for a
    // 40-minute film -- so this is a progress number, not a formality.
    double referenceWaveformProgress() const;
    bool isReferenceLane(int lane) const { return referenceLane >= 0 && lane == referenceLane; }
    int getReferenceLane() const { return referenceLane; }
    static constexpr double getTimelineRate() { return CanvasPlayer::getTimelineRate(); }
    std::function<void()> onStateChanged;

private:
    struct Visual
    {
        Block block;
        std::unique_ptr<juce::AudioThumbnail> thumb;
        // The generator that belongs to this block. Settings only -- takes live on disk,
        // in the block's folder, and are read back from there.
        juce::var settings;
        // How long the FILE is, as opposed to how long the block is. They used to be
        // forced equal; the difference between them is the empty tail you drag out past
        // the end of the audio, which is the range an extend or a remix fills in.
        double audioSeconds = 0.0;

        // ---- step 2, the analysis ---------------------------------------------------
        // Where this block is in the analyse cycle. `Refused` is its own state and not a
        // kind of `Failed`: the analysis SUCCEEDED and the grid it found was not good
        // enough to impose, which is the system working (convention 1) rather than
        // something that went wrong.
        enum class Analysis { None, Running, Measured, Refused, Failed };
        Analysis analysis = Analysis::None;
        // The sentence that goes with it, kept so it can be re-read from the block menu
        // rather than living only in a status line one repaint can overwrite.
        juce::String analysisNote;
        // Where this TAKE was stretched from, or empty. Read from the take's own sidecar, so
        // it survives a reopen with no document change -- it is a fact about the FILE.
        //
        // Deliberately separate from `block.conformedTo`, which is a fact about the BLOCK.
        // "this audio was stretched once" and "this block is tied to block 3" are different
        // claims, and one icon for both would make them look like the same thing.
        juce::String stretchedFrom;
        // The measured beats and downbeats in SOURCE seconds, when this block has been
        // analysed. Session state for the same reason the onsets are.
        std::vector<double> beats, downbeats;
        // Onset times in SOURCE seconds. Session state, not document state: they are
        // 12 KB of JSON per take and they are already in the library, so writing them
        // into every `.mira` would fatten the document with a copy of something that has
        // a home. Re-read on demand through `onMeasurementLookup`.
        std::vector<double> onsets;
    };

    // BarOne drags the grid footer, which moves where bar 1 sits WITHIN THE FILE -- see
    // MIRA-BLOCKS.md 1.4. It is its own verb rather than a modifier on Move because moving
    // a block and moving its grid are opposite intentions: one says "this audio belongs
    // later", the other says "this audio was always in phase, I had the downbeat wrong".
    enum class Drag { None, Move, TrimLeft, TrimRight, FadeIn, FadeOut, Playhead, Marquee, Pan, Gain, LaneMove, LaneResize, MarkerMove, BarOne, Tempo };

    const MiraLookAndFeel& laf;
    juce::AudioFormatManager& formats;
    juce::AudioThumbnailCache& cache;
    CanvasPlayer player;

    std::vector<std::unique_ptr<Visual>> items;
    std::set<juce::int64> selected;
    juce::int64 nextId = 1;
    juce::File projectFolder;      // the folder the document lives in
    juce::File documentFile;       // the .mira itself, or invalid for an unsaved canvas
    bool dirty = false;
    bool returnOnStop = true;
    double playedFrom = 0.0;


    // The view: seconds per pixel and the leftmost visible second. No bars, no beats --
    // there is no tempo here to have them in.
    double pixelsPerSecond = 40.0;
    double viewStart = 0.0;
    // Tall enough for the channel strip to BE one. At 64 the fader had 34 pixels of
    // travel, which is a control you aim at rather than set.
    // The DEFAULT track height, and the only thing shift-G/H moves.
    int laneHeight = 104;
    // A per-lane height, or 0 for "follow the global". These are the SAME concept as a
    // lock, which is why there is no second flag: a lane with a height of its own is
    // exactly a lane that global zoom leaves alone, and unlocking is setting it back to 0.
    // A second bool would let "locked" and "has its own height" drift apart, and then
    // there would be a state where a lock does nothing.
    std::vector<int> laneH;
    static constexpr int kLaneMin = 28, kLaneMax = 320;
    // What the reference lane gets when it arrives. Short on purpose: it is something you
    // glance at to find a cut, not something you read the waveform of.
    static constexpr int kReferenceHeight = 76;
    int laneHeightOf(int lane) const
    {
        return juce::isPositiveAndBelow(lane, (int) laneH.size()) && laneH[(size_t) lane] > 0
                   ? laneH[(size_t) lane] : laneHeight;
    }
    bool laneHeightLocked(int lane) const
    {
        return juce::isPositiveAndBelow(lane, (int) laneH.size()) && laneH[(size_t) lane] > 0;
    }
    void setLaneHeight(int lane, int height);
    void setLaneHeightLocked(int lane, bool locked);
    void ensureLaneArrays();
    // Right-click a lane header: lock or release its height, reorder it, remove it.
    void showLaneMenu(int lane, juce::Point<int> at);
    // The padlock in the lane header. Empty when the lane is too short to hold it, in
    // which case the right-click menu is the way in.
    juce::Rectangle<int> lockBoxFor(int lane) const;
    int resizingLane = -1;      // which lane's bottom edge is being dragged, or -1
    int resizeOriginH = 0;
    static constexpr int kLaneEdgeGrab = 5;
    // How tall the WAVEFORM is drawn inside its block, independent of the block. Quiet
    // takes are a flat line at 1.0 and you cannot see where the peaks are; loud ones fill
    // the block and you cannot see anything else. AudioThumbnail takes this directly.
    float waveZoom = 1.0f;
    int topRuler = 26;
    // Mute and solo live on the LANE, not the block: a lane is one take, and muting "this
    // take" is the whole point of stacking them. Bitmasks because that is what the audio
    // thread reads.
    juce::uint64 muteMask = 0, soloMask = 0;
    juce::StringArray laneNames;
    // A colour per track. The block takes its track's colour and so does the generator
    // header, so "which block am I editing" is answered before you read a word.
    static juce::Colour laneColour(int lane);
    // A colour per track. The block takes its track's colour and so does the generator
    // header, so "which block am I editing" is answered by the colour before you have
    // read a single word.
    // How many tracks EXIST, rather than however many fit the window. An empty canvas
    // with fifteen tracks in it is fifteen promises nobody made; one track and a
    // "+ Track" button is the same thing a DAW does.
    int laneCount = 1;
    // A fader per lane, in dB, -60 (off) to +6. Stacking drums against guitars is the
    // point of the canvas, and stacking without levels is just addition.
    std::vector<double> laneDb;
    // Decayed peak per lane, PER CHANNEL: [lane][0] left, [lane][1] right. A mono meter
    // cannot show a stereo take with a dead side, which is the fault a meter is for.
    std::vector<std::array<float, 2>> laneMeter;
    // Peak hold, decaying far slower than the bar. A transient is over before your eye
    // reaches the meter; the line is what lets you see it happened.
    std::vector<std::array<float, 2>> laneHold;
    // Has this lane hit full scale since the strip was last clicked? Latched, because a
    // clip that shows for 200 ms is a clip you will miss.
    std::vector<bool> laneClipped;
    int faderLane = -1;            // which lane's fader is being dragged, or -1
    // Where a dragged lane header would land, or -1. The move happens on mouse-UP, not as
    // you cross: one undo step for one gesture, and an insertion line you can aim.
    int laneDropTarget = -1;
    int dragTargetMarker = -1;   // which marker is being dragged, or -1
    // Which TRACK is selected, or -1. Separate from the block selection because deleting a
    // track and deleting the blocks on it are different things to want.
    int selectedLane = -1;
    std::unique_ptr<juce::TextEditor> renameEditor;
    int renamingLane = -1;
    double loopStart = 0.0, loopEnd = 0.0;

    Drag drag = Drag::None;
    juce::int64 dragTarget = 0;
    double dragGrabSeconds = 0.0;
    double dragOriginStart = 0.0, dragOriginLength = 0.0, dragOriginOffset = 0.0;
    double dragOriginFadeIn = 0.0, dragOriginFadeOut = 0.0, dragOriginGain = 0.0;
    juce::Point<int> dragStart;
    int dragOriginLane = 0;
    juce::Point<int> dragFrom;
    // Where every selected block WAS when the drag began. Offsets are applied from these,
    // never from the block's live position -- accumulating a delta per mouse event makes
    // a slow drag travel further than a fast one over the same distance.
    std::map<juce::int64, std::pair<double, int>> dragOrigins;   // id -> {start, lane}
    bool metronomeOn = false;
    double panFromView = 0.0;
    juce::Rectangle<int> marquee;

    double xToSeconds(int x) const { return viewStart + (x - kHeaderWidth) / pixelsPerSecond; }
    int secondsToX(double s) const { return kHeaderWidth + juce::roundToInt((s - viewStart) * pixelsPerSecond); }
    void applyMasks() { player.setLaneMasks(muteMask, soloMask); }
    juce::Rectangle<int> muteBoxFor(int lane) const;
    juce::Rectangle<int> soloBoxFor(int lane) const;
    // The whole mixer widget -- meter and fader in one, on one scale.
    juce::Rectangle<int> stripBoxFor(int lane) const;
    juce::Rectangle<int> faderBoxFor(int lane) const;
    juce::Rectangle<int> meterBoxFor(int lane) const;
    // ONE SCALE for the fader and the meter. That is what makes a channel strip readable:
    // a fader sitting at -12 lines up with a meter reading -12, and you can see the
    // headroom you have left without doing arithmetic on a decibel.
    //
    // Warped rather than linear, the way a console is: the top 18 dB -- where you actually
    // work -- gets nearly half the travel, and the bottom 30 dB, where the difference
    // between -52 and -58 matters to nobody, gets a quarter.

    juce::Rectangle<int> nameBoxFor(int lane) const;
    void beginRename(int lane);
    // Renaming a BLOCK, not a lane. The block's name IS its folder, so this moves the
    // takes with it -- see the definition.
    void beginRenameBlock(juce::int64 id);
    bool renderToFile(const juce::File& dest, int lane, double fromSeconds, double toSeconds,
                      juce::String& errorOut);
    juce::String exportNameFor(const Visual* v, const juce::String& suffix) const;
    void commitRename();
    double laneDbAt(int lane) const { return lane < (int) laneDb.size() ? laneDb[(size_t) lane] : 0.0; }
    void setLaneDb(int lane, double db);
    double faderDbAtY(int lane, int y) const;
    // Seconds of empty block past the end of its audio: what "extend" would fill. Zero
    // when the audio reaches the end of the block, or when there is no audio at all.
    double tailSecondsOf(const Visual& v) const;
    // How much of the block actually sounds, clamped by its length -- so trimming hides.
    double soundingSecondsOf(const Visual& v) const;
    // How much audio it HAS, before length is considered: the Cmd-E cut, or the file.
    double availableSecondsOf(const Visual& v) const;
    // Right-click on a block: mute it, change its fade shape, split or remove it. The
    // things a block IS, in one place, rather than five shortcuts to remember.
    void showBlockMenu(Visual& v);
    // The M on the block's own header. Mute lives ON the block as well as on the track,
    // because "not this bar" and "not this layer" are different questions and only one of
    // them has ever had a button.
    juce::Rectangle<int> blockMuteBox(const Visual& v) const;
    juce::Rectangle<int> blockGainBox(const Visual& v) const;
    // The Analyse chip, third in the header row after M and the gain box. A chip and not
    // a menu-only item because step 2 is a thing you do repeatedly while listening, and
    // it has a state you need to see -- a slow job that says nothing is indistinguishable
    // from one that never started.
    juce::Rectangle<int> blockAnalyseBox(const Visual& v) const;
    // What that button says -- also the block's analysis state in one word, so the thing
    // you press and the thing it told you are the same control.
    static juce::String analyseLabelOf(const Visual& v, bool wide);
    // The header row from the first pixel the NAME may use. ONE definition: the copy that
    // did not know about the Analyse chip drew the block's name straight over it.
    juce::Rectangle<int> blockHeaderRow(const Visual& v) const;
    // Where the waveform is drawn inside a block. ONE definition, because the grid and the
    // onsets are drawn OVER it and a second copy would put them a few pixels off the audio.
    juce::Rectangle<int> blockWaveArea(const Visual& v, juce::Rectangle<int> r) const;
    // Where the tempo and key sit in the block header -- ONE definition, so the painter,
    // the double-click and the editor cannot disagree about a 130-pixel box.
    juce::Rectangle<int> blockTagBox(const Visual& v) const;
    void paintGenerationStrip(juce::Graphics&, const Visual&, juce::Rectangle<int>) const;
    // Every wav in a block's folder, newest first -- the folder IS the take list, so
    // there is no index to keep in step with it.
    juce::Array<juce::File> takesOf(const Visual& v) const;
    // 100+i shows take i, 200+i moves it to the Trash.
    void chooseTake(juce::int64 blockId, int menuId);
    std::vector<VideoClip> videoClips;
    std::vector<Marker> markers;
    void paintMarkers(juce::Graphics&);
    // 5.3 -- the nearest marker to snap a dragged block's start to, or the position
    // unchanged. Snapping is by PIXELS, not by seconds: what "close" means depends on the
    // zoom, and a snap that is a second wide at one zoom and a frame wide at another is a
    // snap you cannot predict.
    double snapToMarker(double seconds) const;
    Ruler rulerMode = Ruler::Seconds;
    // Used only when there is no clip to take them from. A canvas with no picture can
    // still be laid out against a timecode an editor gave you over the phone.
    double fallbackFps = 25.0, fallbackStart = 0.0;
    bool fallbackDrop = false;
    // Which lane is the film's audio, or -1. One lane, because there is one video track.
    int referenceLane = -1;
    void paintVideoStrip(juce::Graphics&);
    double genFraction = -1.0;   // <0 = nothing generating
    std::unique_ptr<juce::TextEditor> blockRenameEditor;
    juce::int64 renamingBlock = 0;
    void commitBlockRename();
    void setSelectionMuted(bool muted);
    static constexpr int kFadeGrab = 9;    // px either side of a fade handle
    static constexpr int kFadeBand = 14;   // px down from the block top that drags a fade
    // The video strip sits between the ruler and the first track: a video track, at the
    // top, where a picture editor expects it. It has no height at all until there is a
    // clip -- an empty video track is a promise nobody made, the same reasoning that
    // stops the canvas opening with fifteen empty audio tracks.
    int videoStripH() const { return videoClips.empty() ? 0 : 34; }
    int lanesTop() const { return videoStripTop() + videoStripH(); }
    // Cumulative now that lanes can differ in height. Both walk the same list, so a lane
    // found by y and the y of that lane can never disagree.
    int laneToY(int lane) const;
    int yToLane(int y) const;
    // How far the stack of TRACKS is scrolled, in pixels. The ruler, the marker row and the
    // video strip sit above `lanesTop()` and deliberately do not move: a time axis that slid
    // away from the blocks it numbers would be worse than no time axis at all.
    int scrollY = 0;
    int lanesTotalHeight() const;
    int maxScrollY() const;
    void scrollVerticallyBy(int pixels);
    // Re-clamp after anything that changes the content height -- a track removed, a lane
    // shortened, the window grown. Without it, scrolling to the bottom of twelve tracks and
    // deleting ten leaves the canvas parked below everything it has.
    void clampScroll();
    // Declared after Visual, which they take by reference.
    juce::File blockFolderFor(const Visual&) const;
    Visual* singleSelection();
    void setFileOn(Visual&, const juce::File&);
    // Tempo and key from the take's own sidecar recipe (MIRA-BLOCKS.md 1.2). `force`
    // overrules a typed or measured grid, which only the double-click asks for.
    void musicFromTake(Visual&, bool force = false);
    void announceSelection();
    // One place that points the side panel at a block, because there were three and they
    // drifted: Cmd-D left the panel aimed at the ORIGINAL, so the next Generate landed on
    // the block you had just copied away from. Pass nullptr to point it at nothing.
    void pointPanelAt(const Visual* v);
    int zoomAnchorX() const;
    // Which block the panel is currently showing, or 0 for none.
    juce::int64 panelBlockId = 0;

    // A snapshot of the whole document plus which blocks were selected, by name.
    struct Snapshot { juce::String json; juce::StringArray selection; };
    std::vector<Snapshot> undoStack, redoStack;
    static constexpr int kUndoDepth = 64;
    // Taken BEFORE a change, so undo returns to the state you were in when you started it.
    void pushUndo();
    void restore(const Snapshot&);
    // One action, one snapshot. addEmptyBlock and a file drop both go through addLane,
    // which records its own -- so without this, adding a block took two undos to remove
    // and dropping four stems took five.
    bool undoSuppressed = false;
    struct UndoGuard
    {
        CanvasView& v;
        explicit UndoGuard(CanvasView& view) : v(view) { v.undoSuppressed = true; }
        ~UndoGuard() { v.undoSuppressed = false; }
    };
    juce::Rectangle<int> boundsOf(const Visual&) const;
    double dragOriginBarOne = 0.0;
    double dragOriginTempo = 0.0;

    // ---- the grid footer (MIRA-BLOCKS.md 1.3) --------------------------------------
    // How tall the strip along the bottom of a block is, and the shortest block that can
    // hold one. Below `kGridFooterMin` the block shows its tempo and key as a one-line
    // summary in its header instead, which is the same rule the per-track padlock and the
    // block's own name strip already follow: a control that does not fit is not drawn
    // smaller, it is replaced by words.
    static constexpr int kGridFooterHeight = 14;
    static constexpr int kGridFooterMin = 62;
    // ONE decision, asked by both the painter and the waveform -- otherwise the waveform
    // makes room for a footer that the density guard then refuses to draw, and the block
    // grows a mystery empty band.
    int gridFooterHeight(const Visual&, int blockHeight) const;
    // 2.5 -- whether the onset ticks are dense enough to be noise at this zoom. Asked by
    // gridFooterHeight and by the painter, and therefore defined once for the same reason
    // gridFooterHeight itself is.
    bool onsetTicksVisible(const Visual&) const;
    void paintBlockGrid(juce::Graphics&, const Visual&, juce::Rectangle<int>,
                        juce::Colour tint, bool isSelected);
    // Bars, beats and onsets drawn OVER the waveform, the way the browser draws them.
    // Step 1 argued for a footer only; the user looked at it and asked for the opposite.
    // The rule is unchanged -- nothing snaps -- and what keeps it scaffolding is the
    // weighting: bars read, beats are faint, and the onsets sit on top of both.
    void paintBlockOverlay(juce::Graphics&, const Visual&, juce::Rectangle<int>,
                           juce::Colour tint, bool isSelected);
    // ---- what lines a block actually draws ------------------------------------------
    // ONE answer, consumed by the footer, by the overlay, by the onset colouring and by the
    // percentage. Four painters deriving a grid four ways is how a bar line, a bar number
    // and an "on the grid" tick end up disagreeing about the same beat.
    struct GridLines
    {
        std::vector<double> beats;   // SOURCE seconds
        std::vector<char>   isBar;   // parallel: is this beat a downbeat
        std::vector<int>    barNo;   // parallel: bar number for a downbeat, else 0
        // True when these are MEASURED positions rather than a period laid out from one
        // BPM scalar. It is what decides solid vs dashed, and it is the honest difference
        // between "where the beats are" and "where they would be if the tempo were exact".
        bool measured = false;
    };
    GridLines gridLinesOf(const Visual&, double from, double to) const;
    // A block's bar lines on the TIMELINE, real downbeats inside it and nominal spacing
    // outside, covering at least [coverFrom, coverTo]. Declared here and not beside
    // conformSelectionTo because it takes a Visual, which is private and declared below.
    std::vector<double> parentBarGrid(const Visual& parent, double coverFrom, double coverTo) const;
    // Snap a SOURCE-time position to this block's grid. Returns `sourceSeconds` unchanged
    // when snapping is off, the block has no grid, or nothing is near enough to be meant.
    // Here rather than beside `snapUnit` because it takes a Visual, declared below.
    double snapSourceTime(const Visual&, double sourceSeconds) const;
    // Step 4 internals, here because they take a Visual.
    Visual* parentOf(const Visual&);
    const Visual* parentOf(const Visual&) const;
    // The parent's tempo is not the one this child was last reconciled with.
    bool isStale(const Visual&) const;
    // Write the parent's tempo and key into the CHILD'S PROMPT, and remember the tempo it
    // was reconciled with. Does not touch the child's own grid: its tempo describes the
    // audio it HAS, the prompt describes the audio it is about to ask for.
    bool adoptParentMusic(Visual& child);
    // Whether an onset lands on the grid: within 18% of a 16th, measured against the BEAT
    // IT FALLS IN rather than against a period extrapolated from bar 1. On a grid that
    // breathes even slightly those are different questions by the end of a take.
    static bool onsetOnGrid(double t, const GridLines&);
    // The two-tier picture as ONE number: what share of this block's onsets land on its
    // grid, or -1 when there is no grid or no onsets to ask about.
    double onGridShareOf(const Visual&) const;

    // ---- MIRA-BLOCKS.md step 2b: does the tempo hold ACROSS a take? -------------------
    //
    // The question this whole feature was built to be able to ask. An extension is joined
    // to the end of a take, so if SA3 drifts, the drift is at the END -- and one tempo for
    // a whole file cannot show that: a take that runs 140 for thirty seconds and 146 for
    // the last ten reports something in between and looks fine.
    //
    // Needs NO new analysis. The measured beats are already here, so this is arithmetic
    // over what step 2 already fetched -- which is the same reason the groove fix in
    // September was free: store everything, derive at read time.
    struct TempoSpan { double from = 0.0, to = 0.0, bpm = 0.0; int beats = 0; };
    std::vector<TempoSpan> tempoAcross(const Visual&, int windows = 4) const;
    // Those windows as one sentence: steady, or drifting and by how much.
    juce::String tempoDriftSummary(const Visual&) const;
    Visual* hitTest(juce::Point<int>, Drag& what);
    void rebuildAudio();
    void timerCallback() override;
    double contentEnd() const;

    // The lane headers on the left. Fixed, and the time axis starts after them -- a
    // header that scrolled with the canvas would stop saying which lane you were looking
    // at exactly when you needed it to.
    static constexpr int kHeaderWidth = 148;
    static constexpr int kEdgeGrab = 7;   // px either side of a block edge that trims
    // px either side of a BAR LINE that drags bar 1. Narrower than kEdgeGrab because a bar
    // line sits in the middle of a block where the competing gesture is Move, and moving a
    // block by accident is more expensive than missing the grid by three pixels.
    static constexpr int kBarLineGrab = 4;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CanvasView)
};

// The window: a toolbar and the surface. Deliberately thin -- everything that matters is
// in CanvasView, so throwing the experiment away means deleting two files and one menu
// item.
class CanvasWindow : public juce::DocumentWindow
{
public:
    // Takes a ready-made GenerateContent and hosts it in the side panel. Built by the
    // owner because it needs the database, the studio root and the shared worker -- none
    // of which the canvas has any business knowing about.
    CanvasWindow(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
                 juce::AudioThumbnailCache& cache, GenerateContent* panel);
    // Defined in the .cpp, where Content is a complete type -- a unique_ptr to a forward
    // declared struct cannot be destroyed anywhere the definition is not visible.
    ~CanvasWindow() override;

    void closeButtonPressed() override { if (onClosed) onClosed(); }
    std::function<void()> onClosed;

    CanvasView& getView() { return *view; }

    // What the macOS Canvas menu reaches that the view does not own: the document itself
    // lives in Content (the project folder, the dirty flag, the save-as prompt), so the
    // menu asks the window rather than reaching past it into the view.
    void saveProject();
    // MIRA-VIDEO.md Phase 1.3 -- pick a film, put it on the video track, open the picture.
    void openVideo();
    // Reopen the picture for a clip the document already holds. Closing the picture
    // window does not throw the clip away: the film is still in the session, you have
    // just stopped looking at it.
    void showPicture();
    // The marker list, beside the work -- the same shape as the LoRA library window.
    void showMarkers();
    bool hasVideo() const;

    // ui_settings, reached through the owner. The canvas has no database of its own, and
    // giving it one so a window could remember its size would be the wrong trade.
    // A film arrived or went away. The macOS menu bar bakes each item's enabled state in
    // when the menu is built, so Show Picture has to be told rather than asked.
    std::function<void()> onVideoChanged;
    std::function<juce::String(const juce::String& key)> loadSetting;
    std::function<void(const juce::String& key, const juce::String& value)> saveSetting;

private:
    struct Content;
    std::unique_ptr<Content> content;
    CanvasView* view = nullptr;
};

} // namespace mira::canvas
