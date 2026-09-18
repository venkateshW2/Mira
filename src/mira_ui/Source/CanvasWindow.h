#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include "MiraLookAndFeel.h"
#include "CanvasEngine.h"
#include "CanvasInspector.h"

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
    bool isPlaying() const { return player.isPlaying(); }
    bool isLooping() const { return player.isLooping(); }
    double getPositionSeconds() const { return player.getPositionSeconds(); }
    double getLengthSeconds() const { return player.getLengthSeconds(); }
    int getBlockCount() const { return static_cast<int>(items.size()); }
    void zoomBy(double factor, int aroundX);
    void fit();
    void clearAll();
    void addEmptyBlock();
    void save() const;
    void load();
    void applySettingsToSelection(const juce::var& settings);
    void chooseTakeForSelection(const juce::File& take);
    // name, block folder, generator settings, chosen take -- everything the inspector
    // needs, pushed rather than pulled so it cannot show a stale block.
    std::function<void(const juce::String&, const juce::File&, const juce::var&, const juce::File&)> onSelectionChanged;
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
    juce::File projectFolder;

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
    // A fader per lane, in dB, -60 (off) to +6. Stacking drums against guitars is the
    // point of the canvas, and stacking without levels is just addition.
    std::vector<double> laneDb;
    std::vector<float> laneMeter;  // decayed peak per lane, for the header meters
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
    CanvasWindow(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
                 juce::AudioThumbnailCache& cache, Sa3WorkerHub& hub, juce::File studioRoot);
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
