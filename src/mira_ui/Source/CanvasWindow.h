#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "MiraLookAndFeel.h"
#include "CanvasEngine.h"
#include "GenerateWindow.h"

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
    void fit();
    void clearAll();
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
    juce::String nextBlockName() const;
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
    std::function<void(const juce::String& name, const juce::File& folder)> onOpenGenerator;
    // Double-click asks for the panel to be SHOWN, not just repointed -- a folded panel
    // that silently changed which block it was about would be a no-op you cannot see.
    std::function<void()> onRevealGenerator;
    // A take generated into a block's folder becomes that block's audio.
    void adoptTake(const juce::File& folder, const juce::File& take);
    float readAndClearPeak() { return player.readAndClearPeak(); }
    std::function<void()> onStateChanged;

private:
    struct Visual
    {
        Block block;
        std::unique_ptr<juce::AudioThumbnail> thumb;
        // The generator that belongs to this block. Settings only -- takes live on disk,
        // in the block's folder, and are read back from there.
        juce::var settings;
    };

    enum class Drag { None, Move, TrimLeft, TrimRight, Playhead, Marquee, Pan };

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
    int laneHeight = 64;
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
    std::vector<float> laneMeter;  // decayed peak per lane, for the header meters
    // Peak hold, decaying far slower than the bar. A transient is over before your eye
    // reaches the meter; the line is what lets you see it happened.
    std::vector<float> laneHold;
    int faderLane = -1;            // which lane's fader is being dragged, or -1
    std::unique_ptr<juce::TextEditor> renameEditor;
    int renamingLane = -1;
    double loopStart = 0.0, loopEnd = 0.0;

    Drag drag = Drag::None;
    juce::int64 dragTarget = 0;
    double dragGrabSeconds = 0.0;
    double dragOriginStart = 0.0, dragOriginLength = 0.0, dragOriginOffset = 0.0;
    int dragOriginLane = 0;
    juce::Point<int> dragFrom;
    // Where every selected block WAS when the drag began. Offsets are applied from these,
    // never from the block's live position -- accumulating a delta per mouse event makes
    // a slow drag travel further than a fast one over the same distance.
    std::map<juce::int64, std::pair<double, int>> dragOrigins;   // id -> {start, lane}
    double panFromView = 0.0;
    juce::Rectangle<int> marquee;

    double xToSeconds(int x) const { return viewStart + (x - kHeaderWidth) / pixelsPerSecond; }
    int secondsToX(double s) const { return kHeaderWidth + juce::roundToInt((s - viewStart) * pixelsPerSecond); }
    void applyMasks() { player.setLaneMasks(muteMask, soloMask); }
    juce::Rectangle<int> muteBoxFor(int lane) const;
    juce::Rectangle<int> soloBoxFor(int lane) const;
    juce::Rectangle<int> faderBoxFor(int lane) const;
    juce::Rectangle<int> meterBoxFor(int lane) const;
    juce::Rectangle<int> nameBoxFor(int lane) const;
    void beginRename(int lane);
    void commitRename();
    double laneDbAt(int lane) const { return lane < (int) laneDb.size() ? laneDb[(size_t) lane] : 0.0; }
    void setLaneDb(int lane, double db);
    int laneToY(int lane) const { return topRuler + lane * laneHeight; }
    int yToLane(int y) const { return juce::jmax(0, (y - topRuler) / laneHeight); }
    // Declared after Visual, which they take by reference.
    juce::File blockFolderFor(const Visual&) const;
    Visual* singleSelection();
    void setFileOn(Visual&, const juce::File&);
    void announceSelection();
    // One place that points the side panel at a block, because there were three and they
    // drifted: Cmd-D left the panel aimed at the ORIGINAL, so the next Generate landed on
    // the block you had just copied away from. Pass nullptr to point it at nothing.
    void pointPanelAt(const Visual* v);
    juce::Rectangle<int> boundsOf(const Visual&) const;
    Visual* hitTest(juce::Point<int>, Drag& what);
    void rebuildAudio();
    void timerCallback() override;
    double contentEnd() const;

    // The lane headers on the left. Fixed, and the time axis starts after them -- a
    // header that scrolled with the canvas would stop saying which lane you were looking
    // at exactly when you needed it to.
    static constexpr int kHeaderWidth = 148;
    static constexpr int kEdgeGrab = 7;   // px either side of a block edge that trims

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

private:
    struct Content;
    std::unique_ptr<Content> content;
    CanvasView* view = nullptr;
};

} // namespace mira::canvas
