#include "CanvasWindow.h"
#include "mira/db/PathNormalise.h"
#include "NativeWindowChrome.h"

namespace mira::canvas {

// A generator with nothing in it. Not the same as "no settings": no settings means we have
// not looked yet and something else might know, and an empty recipe means there is nothing
// to know -- which is what a new block is.
// What a block IS, in two words, read off its own prompt. SA3 prompts are
// "Key: value, Key: value" so the facts are already in there -- "Keyscale: C minor,
// BPM: 64" -- and a block that says "C minor . 64" in its corner is one you can arrange
// against without opening anything.
//
// From the PROMPT rather than from mira's analysis on purpose: the prompt is what the
// block was asked for, it is there before a single sample exists, and an empty block can
// carry it. Analysis of the audio is the better answer once there IS audio, and it can
// replace this later without moving anything.
static juce::String promptField (const juce::String& prompt, const juce::String& key)
{
    const int at = prompt.indexOfIgnoreCase (key + ":");
    if (at < 0) return {};
    auto rest = prompt.substring (at + key.length() + 1);
    const int end = rest.indexOfChar (',');
    return (end >= 0 ? rest.substring (0, end) : rest).trim();
}

static juce::String keyAndTempoOf (const juce::var& settings)
{
    if (!settings.isObject()) return {};
    const auto prompt = settings.getProperty ("prompt", "").toString();
    if (prompt.isEmpty()) return {};
    const auto key = promptField (prompt, "Keyscale");
    const auto bpm = promptField (prompt, "BPM");
    if (key.isEmpty() && bpm.isEmpty()) return {};
    if (key.isEmpty()) return bpm + " bpm";
    if (bpm.isEmpty()) return key;
    return key + juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  ")) + bpm;
}

static juce::var emptyRecipe()
{
    auto* o = new juce::DynamicObject();
    o->setProperty("prompt", "");
    o->setProperty("negative_prompt", "");
    o->setProperty("loras", juce::var(juce::Array<juce::var>{}));
    return juce::var(o);
}


namespace {
juce::String formatTime(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const int total = static_cast<int>(seconds);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}
} // namespace

CanvasView::CanvasView(const MiraLookAndFeel& lafIn, juce::AudioFormatManager& formatsIn,
                       juce::AudioThumbnailCache& cacheIn)
    : laf(lafIn), formats(formatsIn), cache(cacheIn)
{
    setWantsKeyboardFocus(true);
    startTimerHz(30);
}

CanvasView::~CanvasView() { player.detach(); }

void CanvasView::setProject(const juce::File& project)
{
    commitRename();
    if (!project.isDirectory()) return;

    // Opening a FOLDER is the legacy path and stays only so an existing canvas.json is not
    // stranded: the first .mira in the folder wins, otherwise a canvas.json is read and
    // written back out as one. New work goes through New/Open.
    auto found = project.findChildFiles(juce::File::findFiles, false, juce::String("*") + kExtension);
    if (!found.isEmpty()) { openDocument(found[0]); return; }

    const auto legacy = project.getChildFile("canvas.json");
    if (legacy.existsAsFile())
    {
        projectFolder = project;
        readFrom(legacy);
        documentFile = project.getChildFile(project.getFileName() + kExtension);
        writeTo(documentFile);
        legacy.moveToTrash();
        dirty = false;
        if (onDocumentChanged) onDocumentChanged();
        return;
    }

    // A REAL FOLDER WITH NO DOCUMENT IN IT YET. It still becomes the project: a block has
    // to have somewhere to put its audio, and without a project folder blockFolderFor
    // returned nothing, pointPanelAt gave up silently, and the panel sat there saying "no
    // block selected" with Generate greyed out over a block that was plainly selected.
    projectFolder = project;
    documentFile = project.getChildFile(project.getFileName() + kExtension);
    writeTo(documentFile);
    dirty = false;
    if (onDocumentChanged) onDocumentChanged();
}

double CanvasView::contentEnd() const
{
    double end = 0.0;
    for (const auto& i : items) end = juce::jmax(end, i->block.end());
    return end;
}

void CanvasView::fit()
{
    const double end = contentEnd();
    const int w = juce::jmax(200, getWidth() - kHeaderWidth + 16);
    viewStart = 0.0;
    pixelsPerSecond = end > 0.0 ? juce::jlimit(0.5, 400.0, w / end) : 40.0;
    repaint();
}

void CanvasView::rebuildAudio()
{
    std::vector<Block> blocks;
    blocks.reserve(items.size());
    for (const auto& i : items) blocks.push_back(i->block);
    player.rebuild(blocks, formats);
    if (onStateChanged) onStateChanged();
}

// The header is a mixer strip: the name across the top, M/S and the fader under it, the
// meter down the right edge. The name used to start at x=60 with nothing to its left,
// because it shared a row with chips that were vertically centred somewhere else -- so it
// read as floating rather than as a title, and the rename box landed in the same odd spot.
// M and S sit to the RIGHT of the fader column, not above it. Stacked above, they ate the
// vertical space the fader needs -- and vertical space is the only thing a fader has.
juce::Rectangle<int> CanvasView::muteBoxFor(int lane) const
{
    return laneHeight >= 46
               ? juce::Rectangle<int>(kHeaderWidth - 62, laneToY(lane) + 24, 26, 18)
               : juce::Rectangle<int>(kHeaderWidth - 74, laneToY(lane) + laneHeight / 2 - 9, 22, 18);
}

juce::Rectangle<int> CanvasView::soloBoxFor(int lane) const
{
    return laneHeight >= 46
               ? juce::Rectangle<int>(kHeaderWidth - 32, laneToY(lane) + 24, 26, 18)
               : juce::Rectangle<int>(kHeaderWidth - 48, laneToY(lane) + laneHeight / 2 - 9, 22, 18);
}

// One warped scale, shared by the fader and the meter. Linear-in-dB spends half the
// travel between -60 and -30, where nothing you care about happens; a console spends it at
// the top. Both controls use this, which is the whole point -- a fader at -12 lines up
// with a meter reading -12.
double CanvasView::dbToNorm(double db)
{
    db = juce::jlimit(kFaderBottomDb, kFaderTopDb, db);
    if (db >= -12.0) return 0.55 + 0.45 * (db + 12.0) / 18.0;    // +6..-12 over the top 45%
    if (db >= -30.0) return 0.25 + 0.30 * (db + 30.0) / 18.0;    // -12..-30 over 30%
    return 0.25 * (db + 60.0) / 30.0;                             // -30..-60 over 25%
}

double CanvasView::normToDb(double n)
{
    n = juce::jlimit(0.0, 1.0, n);
    if (n >= 0.55) return -12.0 + (n - 0.55) / 0.45 * 18.0;
    if (n >= 0.25) return -30.0 + (n - 0.25) / 0.30 * 18.0;
    return -60.0 + n / 0.25 * 30.0;
}

// ONE CONTROL, the way a DAW mixer has it: the meter and the fader are a single tall
// widget on one scale, not two things side by side that happen to line up. The cap spans
// the whole width, so it reads as the handle of the thing the meter is part of.
juce::Rectangle<int> CanvasView::stripBoxFor(int lane) const
{
    if (laneHeight < 46) return {};
    const int top = laneToY(lane) + 22;
    return { 10, top, 30, juce::jmax(16, laneHeight - 30) };
}

juce::Rectangle<int> CanvasView::faderBoxFor(int lane) const
{
    auto strip = stripBoxFor(lane);
    if (strip.isEmpty()) return {};
    return { strip.getX() + 15, strip.getY(), 13, strip.getHeight() };
}

// Muted and deliberately NOT the accent: the accent means "selected" and "playing"
// everywhere else in mira, so a column of amber faders made every track look active at
// once. That is most of what "the yellow fader kills the ui" was.
juce::Colour CanvasView::laneColour(int lane)
{
    static const juce::Colour palette[] = {
        juce::Colour(0xff6f9bd1),   // slate blue
        juce::Colour(0xffc98a7a),   // clay
        juce::Colour(0xff7fae8c),   // sage
        juce::Colour(0xffb79bd0),   // lilac
        juce::Colour(0xffd0b06a),   // brass
        juce::Colour(0xff6fb0b5),   // teal
        juce::Colour(0xffd08f9f),   // rose
        juce::Colour(0xff9aa8c4)    // steel
    };
    return palette[(size_t) juce::jmax(0, lane) % (sizeof(palette) / sizeof(palette[0]))];
}

// The meter half of the same widget: two stereo bars down its left side. A mono meter
// cannot show the fault a meter exists to catch -- a take with a dead side.
juce::Rectangle<int> CanvasView::meterBoxFor(int lane) const
{
    if (laneHeight < 46)
        return { kHeaderWidth - 16, laneToY(lane) + 4, 9, juce::jmax(10, laneHeight - 8) };
    auto strip = stripBoxFor(lane);
    return { strip.getX() + 2, strip.getY(), 11, strip.getHeight() };
}

juce::Rectangle<int> CanvasView::nameBoxFor(int lane) const
{
    return laneHeight >= 46
               ? juce::Rectangle<int>(8, laneToY(lane) + 2, kHeaderWidth - 18, 18)
               : juce::Rectangle<int>(8, laneToY(lane), kHeaderWidth - 84, laneHeight);
}

void CanvasView::beginRename(int lane)
{
    commitRename();
    renamingLane = lane;
    renameEditor = std::make_unique<juce::TextEditor>();
    renameEditor->setText(laneNames[lane], juce::dontSendNotification);
    // IN PLACE: exactly the name's rectangle, the same font, no border and no box. The
    // editor used to be a plain TextEditor at a slightly different size, in a name box
    // that was itself offset from where the name appeared to be -- so renaming looked
    // like a text field opening somewhere else rather than the name becoming editable.
    renameEditor->setFont(laf.sansSemiBold(MiraLookAndFeel::textSize(12.5f)));
    renameEditor->setBounds(nameBoxFor(lane));
    renameEditor->setBorder(juce::BorderSize<int>(0));
    renameEditor->setIndents(0, 1);
    renameEditor->setJustification(juce::Justification::centredLeft);
    renameEditor->setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::surface.darker(0.2f));
    renameEditor->setColour(juce::TextEditor::outlineColourId, laneColour(lane).withAlpha(0.5f));
    renameEditor->setColour(juce::TextEditor::focusedOutlineColourId, laneColour(lane));
    renameEditor->setColour(juce::TextEditor::textColourId, laneColour(lane).brighter(0.2f));
    renameEditor->setColour(juce::TextEditor::highlightColourId, laneColour(lane).withAlpha(0.3f));
    renameEditor->onReturnKey = [this] { commitRename(); };
    renameEditor->onEscapeKey = [this] { renamingLane = -1; renameEditor.reset(); repaint(); };
    renameEditor->onFocusLost = [this] { commitRename(); };
    addAndMakeVisible(*renameEditor);
    renameEditor->selectAll();
    renameEditor->grabKeyboardFocus();
}

void CanvasView::commitRename()
{
    if (renameEditor == nullptr || renamingLane < 0) { renameEditor.reset(); renamingLane = -1; return; }
    if (laneNames[renamingLane] != renameEditor->getText().trim())
    {
        laneNames.set(renamingLane, renameEditor->getText().trim());
        markDirty();
    }
    renamingLane = -1;
    renameEditor.reset();
    grabKeyboardFocus();
    repaint();
}

void CanvasView::mouseDoubleClick(const juce::MouseEvent& e)
{
    // Double-click a lane's NAME to rename it. "takes / 3" says what the file was called,
    // not what the lane is for, and a lane you cannot name is one you have to identify by
    // its waveform every time.
    if (e.x < kHeaderWidth && e.y >= topRuler)
    {
        const int lane = yToLane(e.y);
        if (nameBoxFor(lane).contains(e.getPosition())) beginRename(lane);
        return;
    }

    // Double-click a block opens its generator -- which now means EXPANDING the panel if
    // it is folded, since the panel no longer opens as a window. Single click selects and
    // points the panel at it, which is what you do ninety times for every once you want
    // to go and look at the settings.
    Drag what = Drag::None;
    if (auto* hit = hitTest(e.getPosition(), what); hit != nullptr)
    {
        selected.clear();
        selected.insert(hit->block.id);
        if (onRevealGenerator) onRevealGenerator();
        pointPanelAt(hit);
        repaint();
    }
}

// Where a click on the fader lands, in dB, through the SAME warped scale the fader and
// meter are drawn with -- so the cap arrives under the pointer instead of near it.
double CanvasView::faderDbAtY(int lane, int y) const
{
    auto f = faderBoxFor(lane);
    if (f.isEmpty() || f.getHeight() <= 0) return laneDbAt(lane);
    const double norm = 1.0 - (double) (y - f.getY()) / (double) f.getHeight();
    return normToDb(norm);
}

void CanvasView::setLaneDb(int lane, double db)
{
    if (lane < 0 || lane >= CanvasAudioSource::kMaxLanes) return;
    if ((int) laneDb.size() <= lane) laneDb.resize((size_t) lane + 1, 0.0);
    laneDb[(size_t) lane] = juce::jlimit(-60.0, 6.0, db);
    player.setLaneGain(lane, laneDb[(size_t) lane] <= -60.0
                                 ? 0.0f
                                 : juce::Decibels::decibelsToGain((float) laneDb[(size_t) lane]));
}

juce::Rectangle<int> CanvasView::blockMuteBox(const Visual& v) const
{
    auto r = boundsOf(v);
    // Only where the header strip is actually drawn. A button you can hit but cannot see
    // is worse than no button.
    if (r.getHeight() < 46 || r.getWidth() < 52) return {};
    return { r.getX() + 5, r.getY() + 3, 15, 13 };
}

juce::File CanvasView::blockFolderFor(const Visual& v) const
{
    if (!projectFolder.isDirectory() || v.block.name.isEmpty()) return {};
    return projectFolder.getChildFile(juce::File::createLegalFileName(v.block.name));
}

CanvasView::Visual* CanvasView::singleSelection()
{
    if (selected.size() != 1) return nullptr;
    for (auto& i : items) if (selected.count(i->block.id)) return i.get();
    return nullptr;
}

void CanvasView::setFileOn(Visual& v, const juce::File& f)
{
    v.block.file = f;
    v.thumb.reset();
    if (f.existsAsFile())
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor(f));
        if (reader != nullptr && reader->sampleRate > 0.0)
        {
            v.audioSeconds = reader->lengthInSamples / reader->sampleRate;
            // A new take is new audio, so a cut made against the old one is meaningless --
            // it described where a different file went quiet.
            v.block.contentSeconds = 0.0;
            // THE BLOCK'S LENGTH IS WHAT YOU ASKED FOR, and a take fills it. Only a block
            // that has never had a length takes it from the file.
            //
            // The old rule clamped the block to the file, which made an empty tail
            // impossible -- and that tail is the whole extend gesture: drag the block out
            // past the end of its audio and the gap is the range to fill in.
            if (v.block.length <= 0.0) v.block.length = v.audioSeconds;
        }
        v.thumb = std::make_unique<juce::AudioThumbnail>(512, formats, cache);
        v.thumb->setSource(new juce::FileInputSource(f));
    }
}

// One place that names a block, so a dropped file and a "+ Block" cannot end up in
// different naming schemes -- which is exactly how the canvas grew two kinds of block.
juce::String CanvasView::nextBlockName() const
{
    int highest = 0;
    for (const auto& i : items)
    {
        const auto n = i->block.name;
        if (n.startsWith("block ")) highest = juce::jmax(highest, n.substring(6).getIntValue());
    }

    // Skip past any name whose FOLDER already exists on disk. Removing a block takes it
    // off the canvas but leaves its folder -- so the next "block 3" reused that folder and
    // opened showing the deleted block's files. A new block has to be new all the way
    // down, not just in the arrangement.
    for (int n = highest + 1; n < highest + 1000; ++n)
    {
        const auto name = "block " + juce::String(n);
        if (!projectFolder.isDirectory()) return name;
        if (!projectFolder.getChildFile(juce::File::createLegalFileName(name)).exists()) return name;
    }
    return "block " + juce::String(highest + 1);
}

void CanvasView::addLane()
{
    if (laneCount >= CanvasAudioSource::kMaxLanes) return;
    pushUndo();
    ++laneCount;
    if (laneNames[laneCount - 1].isEmpty()) laneNames.set(laneCount - 1, "track " + juce::String(laneCount));
    markDirty();
    repaint();
}

void CanvasView::removeLane(int lane)
{
    if (lane < 0 || lane >= laneCount || laneCount <= 1) return;
    pushUndo();
    UndoGuard oneEdit (*this);

    items.erase(std::remove_if(items.begin(), items.end(),
                                [lane](const std::unique_ptr<Visual>& v) {
                                    return v->block.lane == lane;
                                }),
                 items.end());
    // Everything below moves UP. A track numbered 4 with nothing above it is not a hole
    // anyone meant to leave, and the lane index is what mute, solo and the faders are
    // keyed on -- a gap in it is a gap in the mixer.
    for (auto& v : items)
        if (v->block.lane > lane) --v->block.lane;

    laneNames.remove(lane);
    if (lane < (int) laneDb.size()) laneDb.erase(laneDb.begin() + lane);

    // The mask bits above the removed lane shift down with it, or mute and solo would
    // apply to whichever track happened to slide into the slot.
    auto shift = [lane](juce::uint64 mask) {
        const juce::uint64 below = mask & ((juce::uint64 (1) << lane) - 1);
        const juce::uint64 above = mask >> (lane + 1);
        return below | (above << lane);
    };
    muteMask = shift(muteMask);
    soloMask = shift(soloMask);
    applyMasks();

    --laneCount;
    selectedLane = juce::jlimit(-1, laneCount - 1, selectedLane >= laneCount ? laneCount - 1 : selectedLane);
    selected.clear();
    pointPanelAt(nullptr);
    markDirty();
    rebuildAudio();
    repaint();
}

void CanvasView::addEmptyBlock()
{
    // Dropped on the first lane with nothing under the playhead, at the playhead. An
    // empty block is a FRAME: a length you meant, with nothing in it yet.
    const double at = juce::jmax(0.0, player.getPositionSeconds());
    // Short. A new block is a placeholder you will resize, not a claim that the part is
    // thirty seconds long -- and a full-window frame on an empty canvas reads as an error
    // rather than as an invitation.
    // 30 seconds, because that is the generator's default duration and the block's length
    // IS the duration now: a new block is a 30 second frame you will resize, and resizing
    // it is how you ask for a different length.
    constexpr double kNewBlockSeconds = 30.0;
    pushUndo();
    UndoGuard oneEdit (*this);
    // A NEW BLOCK ALWAYS OPENS ON A NEW TRACK. Hunting for a free gap on an existing
    // track put two unrelated blocks on one fader, and a track is the thing you mix with
    // -- so a block that arrives sharing one arrives already mixed into something else.
    // The exception is the very first block on an empty canvas, which has a track waiting.
    int lane = 0;
    if (!items.empty()) { addLane(); lane = laneCount - 1; }

    auto v = std::make_unique<Visual>();
    v->block.lane = lane;
    v->block.start = at;
    v->block.length = kNewBlockSeconds;
    v->block.id = nextId++;
    v->block.name = nextBlockName();
    v->block.colour = lane;    // born here, and it keeps this colour wherever it goes
    v->settings = emptyRecipe();   // a new block generates nothing until you tell it what
    if (laneNames[lane].isEmpty()) laneNames.set(lane, "track " + juce::String(lane + 1));
    laneCount = juce::jmax(laneCount, lane + 1);
    selected.clear();
    selected.insert(v->block.id);
    items.push_back(std::move(v));
    announceSelection();
    pointPanelAt(items.back().get());
    markDirty();
    repaint();
}

void CanvasView::duplicateSelection()
{
    if (selected.empty()) return;
    pushUndo();

    // Placed after the rightmost of what was copied, on the same lanes, so a duplicate
    // lands where you would have dragged it rather than on top of the original.
    double rightmost = 0.0;
    double leftmost = 1e12;
    for (const auto& i : items)
        if (selected.count(i->block.id))
        { rightmost = juce::jmax(rightmost, i->block.end()); leftmost = juce::jmin(leftmost, i->block.start); }
    const double shift = juce::jmax(0.25, rightmost - leftmost);

    // The sources, resolved before anything is added -- pushing into `items` while
    // iterating it is how this loop would eat itself.
    std::vector<Visual*> sources;
    for (const auto& i : items)
        if (selected.count(i->block.id)) sources.push_back(i.get());

    selected.clear();
    for (auto* src : sources)
    {
        auto v = std::make_unique<Visual>();
        v->block = src->block;               // trim, fades, gain and lane all come too
        v->block.id = nextId++;
        v->block.start = src->block.start + shift;
        v->settings = src->settings;
        // A NEW NAME, and therefore a new folder. The name is what says where a generation
        // lands: `adoptTake` finds the block whose folder the take was written into, so a
        // duplicate that kept its original's name meant generating on the duplicate put the
        // audio on the ORIGINAL -- the first block with that folder wins. The colour does
        // not change, because colour comes from the TRACK and the duplicate is on the same
        // one; what makes them tell apart is the name, which is the thing that has to differ
        // anyway.
        v->block.name = nextBlockName();
        // The SAME take, not a new one. A duplicate that regenerated would be a different
        // piece of audio wearing the same name.
        setFileOn(*v, src->block.file);
        v->block.length = src->block.length; // setFileOn may have reset it to the file's
        v->block.sourceOffset = src->block.sourceOffset;
        selected.insert(v->block.id);
        // Pushed one at a time so nextBlockName() can see the previous copy -- otherwise
        // duplicating three blocks gives all three the same name and the same folder,
        // which is the bug this whole function was fixing.
        items.push_back(std::move(v));
    }

    // AND point the panel at the copy. Generating is aimed by whatever the panel is
    // showing, so leaving it on the original is the other half of why "duplicate, then
    // generate" put the new audio back on the block you had duplicated FROM.
    pointPanelAt(items.empty() ? nullptr : items.back().get());
    markDirty();
    rebuildAudio();
    repaint();
}

void CanvasView::applySettingsToSelection(const juce::var& settings)
{
    if (auto* v = singleSelection()) { v->settings = settings; markDirty(); }
}

void CanvasView::chooseTakeForSelection(const juce::File& take)
{
    if (auto* v = singleSelection())
    {
        pushUndo();
        setFileOn(*v, take);
        rebuildAudio();
        markDirty();
        repaint();
    }
}

void CanvasView::setSelectionMuted(bool muted)
{
    if (selected.empty()) return;
    pushUndo();
    for (auto& i : items)
        if (selected.count(i->block.id)) i->block.muted = muted;
    // A rebuild rather than an atomic bit, unlike the TRACK mute. A muted block is left
    // out of the arrangement entirely, which is also what keeps it from crossfading with
    // the block next to it -- an inaudible block pulling its neighbour down is worse than
    // no mute at all. Readers are cached, so the rebuild costs nothing on disk.
    rebuildAudio();
    markDirty();
    repaint();
}

void CanvasView::showBlockMenu(Visual& v)
{
    // Right-clicking something you have not selected selects it first. Otherwise the menu
    // is about one block and the action lands on another.
    if (selected.count(v.block.id) == 0)
    {
        selected.clear();
        selected.insert(v.block.id);
        pointPanelAt(&v);
    }

    const bool muted = v.block.muted;
    const bool hasFades = v.block.fadeIn > 0.0 || v.block.fadeOut > 0.0;
    const int shape = (int) v.block.fadeShape;

    juce::PopupMenu fades;
    fades.addItem(10, "Linear",      true, shape == 0);
    fades.addItem(11, "Equal power", true, shape == 1);
    fades.addItem(12, "Exponential", true, shape == 2);

    juce::PopupMenu m;
    m.addSectionHeader(v.block.name);
    m.addItem(1, muted ? "Unmute block" : "Mute block");
    m.addSubMenu("Fade shape", fades);
    m.addItem(2, "Clear fades", hasFades);
    m.addItem(6, "Restore full take", v.block.contentSeconds > 0.0 && v.block.hasAudio());
    m.addSeparator();
    m.addItem(3, "Duplicate");
    m.addItem(7, "Cut at playhead");
    m.addItem(4, "Split into two at playhead");
    m.addItem(5, "Remove");

    juce::Component::SafePointer<CanvasView> safe (this);
    // AT THE MOUSE. withTargetComponent(this) anchors the menu to the whole canvas, which
    // is the size of the window -- so the menu opened at the canvas's top-left corner,
    // nowhere near the block you right-clicked.
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
                     [safe, muted] (int result)
                     {
                         if (safe == nullptr || result == 0) return;
                         auto& self = *safe;
                         if (result == 1) { self.setSelectionMuted(!muted); return; }
                         if (result == 2)
                         {
                             self.pushUndo();
                             for (auto& i : self.items)
                                 if (self.selected.count(i->block.id))
                                     i->block.fadeIn = i->block.fadeOut = 0.0;
                             self.rebuildAudio(); self.markDirty(); self.repaint();
                             return;
                         }
                         if (result == 6)
                         {
                             self.pushUndo();
                             // Undoing a CUT, not an edit to the file: the take never
                             // changed, only the block's claim about where it ended.
                             for (auto& i : self.items)
                                 if (self.selected.count(i->block.id))
                                 {
                                     i->block.contentSeconds = 0.0;
                                     i->block.length = juce::jmax(i->block.length,
                                                                   self.availableSecondsOf(*i));
                                 }
                             self.rebuildAudio(); self.markDirty();
                             self.announceSelection(); self.repaint();
                             return;
                         }
                         if (result == 3) { self.duplicateSelection(); return; }
                         if (result == 4) { self.splitAtPlayhead();   return; }
                         if (result == 7) { self.cutAtPlayhead();     return; }
                         if (result == 5) { self.removeSelected();    return; }
                         if (result >= 10 && result <= 12)
                         {
                             self.pushUndo();
                             for (auto& i : self.items)
                                 if (self.selected.count(i->block.id))
                                     i->block.fadeShape = (FadeShape) (result - 10);
                             self.rebuildAudio(); self.markDirty(); self.repaint();
                         }
                     });
}

// How much audio this block HAS to give, before its length is considered: the cut, if one
// was made with Cmd-E, otherwise whatever the file has left after the block's offset.
double CanvasView::availableSecondsOf(const Visual& v) const
{
    if (!v.block.hasAudio() || v.audioSeconds <= 0.0) return 0.0;
    const double inFile = juce::jmax(0.0, v.audioSeconds - v.block.sourceOffset);
    return v.block.contentSeconds > 0.0 ? juce::jmin(v.block.contentSeconds, inFile) : inFile;
}

// How much of this block actually sounds. CLAMPED BY THE BLOCK'S LENGTH, which is what
// makes trimming a hide rather than a cut: pull the right edge in and less of the file
// sounds, pull it back out and it is all there again.
//
// Without the clamp a trimmed block still claimed the whole file, and the waveform was
// drawn squashed -- thirty seconds of audio painted into twenty seconds of block.
double CanvasView::soundingSecondsOf(const Visual& v) const
{
    return juce::jmin(availableSecondsOf(v), v.block.length);
}

double CanvasView::tailSecondsOf(const Visual& v) const
{
    if (!v.block.hasAudio()) return 0.0;
    return juce::jmax(0.0, v.block.length - soundingSecondsOf(v));
}

CanvasView::Geometry CanvasView::selectionGeometry() const
{
    for (const auto& i : items)
        if (selected.size() == 1 && selected.count(i->block.id))
            return { i->block.length, tailSecondsOf(*i), i->block.hasAudio() };
    return { 0.0, 0.0, false };
}

void CanvasView::extendSelection(bool remix)
{
    auto* v = singleSelection();
    if (v == nullptr || onExtendRequested == nullptr) return;
    if (!v->block.hasAudio()) return;
    if (!remix && tailSecondsOf(*v) <= 0.05) return;   // nothing to fill

    // THE PROMPT ON SCREEN IS THE PROMPT THAT RUNS. Extend used to re-apply the block's
    // stored recipe first, which overwrote whatever you had just typed -- you edited the
    // prompt, pressed the button, and watched your edit disappear and the old trigger
    // generate again. A visible, editable field that is silently ignored is worse than no
    // field at all.
    //
    // Capturing it into the block first is what keeps the block's record honest: what it
    // says it was made with is what it was actually made with.
    syncPanelSettings();

    // Where the audio runs out INSIDE the block. Trimming the left edge moves the offset
    // rather than the audio, so the sounding part is shorter than the file by exactly that
    // offset -- and the range has to start where you can hear it stop, not where the file
    // does.
    // Where the audio ends INSIDE the block -- the cut, when you made one. A take that
    // ended in ten seconds of silence used to hand the inpainter a range starting after
    // the silence, so the silence stayed baked in and the continuation began late.
    const double sounding = soundingSecondsOf(*v);
    onExtendRequested(v->block.file, sounding, v->block.length, remix);
}

void CanvasView::announceSelection()
{
    // Called at every selection change, which is what keeps Extend and Remix honest: they
    // are about ONE block with an empty tail, so a marquee that picks up three blocks has
    // to switch them off again.
    if (onBlockGeometry == nullptr) return;
    const auto g = selectionGeometry();
    onBlockGeometry(g.length, g.tail, g.hasAudio);
}

void CanvasView::syncPanelSettings()
{
    if (onCaptureSettings == nullptr || panelBlockId == 0) return;
    for (auto& i : items)
        if (i->block.id == panelBlockId) { i->settings = onCaptureSettings(); return; }
}

void CanvasView::pointPanelAt(const Visual* v)
{
    if (onOpenGenerator == nullptr) return;

    // Whatever is on screen belongs to the block we are LEAVING. Taken before anything is
    // replaced, or a prompt typed and then clicked away from is simply lost.
    syncPanelSettings();

    if (v == nullptr)
    {
        panelBlockId = 0;
        onOpenGenerator({}, {}, {});
        if (onBlockGeometry) onBlockGeometry(0.0, 0.0, false);
        return;
    }
    const auto folder = blockFolderFor(*v);
    if (folder == juce::File())
    {
        // NEVER SILENTLY. A block with no project has nowhere to put audio, and the panel
        // has to say that rather than leave Generate grey with no explanation.
        panelBlockId = v->block.id;
        onOpenGenerator(v->block.name + "  -  no project yet", {}, {});
        if (onBlockGeometry) onBlockGeometry(0.0, 0.0, false);
        return;
    }

    // A block the document has never carried settings for gets them from ITS OWN TAKE --
    // the `.json` sidecar written beside every generated wav, which is the recipe that
    // made exactly this sound. Inheriting whatever was on screen instead is what made two
    // blocks with completely different audio show one identical prompt: every block on a
    // freshly opened project had no stored settings, so every block copied the last one
    // looked at, and nothing ever appeared to change but the title.
    //
    // With no take and no sidecar the generator opens EMPTY. It used to inherit whatever
    // was on screen, which meant a brand-new block arrived carrying the last block's
    // prompt and LoRAs -- a recipe nobody chose for it, ready to generate from by
    // accident. Copying a previous block's settings is what Duplicate is for, and it
    // copies them explicitly.
    auto* mutableV = const_cast<Visual*>(v);
    if (mutableV->settings.isVoid() && v->block.hasAudio())
    {
        auto sidecar = v->block.file.withFileExtension("json");
        if (sidecar.existsAsFile())
        {
            auto parsed = juce::JSON::parse(sidecar.loadFileAsString());
            if (parsed.isObject()) mutableV->settings = parsed;
        }
    }
    if (mutableV->settings.isVoid()) mutableV->settings = emptyRecipe();

    panelBlockId = v->block.id;
    onOpenGenerator(v->block.name
                        + (v->block.hasAudio()
                               ? "  -  " + v->block.file.getFileNameWithoutExtension()
                               : juce::String("  -  empty")),
                    folder, v->settings);
    // AFTER the settings. applySettings restores the `seconds` the block was last
    // generated at, and the block's length is the newer answer -- you resized the frame
    // since then, and the frame is what says how long the part should be.
    if (onBlockGeometry) onBlockGeometry(v->block.length, tailSecondsOf(*v), v->block.hasAudio());
}

void CanvasView::adoptTake(const juce::File& folder, const juce::File& take)
{
    // A generation IS an edit. The wav stays on disk whatever happens, so undo here means
    // "put the block back on the take it was showing" -- which is exactly the answer to
    // "I tried an extend and I do not want to keep it".
    bool pushed = false;
    for (auto& i : items)
        // Convention 9: never `==` on paths. A block named with an accent in it produces
        // one byte sequence here and another from whatever handed us `folder`.
        if (mira::pathsEquivalent(blockFolderFor(*i).getFullPathName().toStdString(),
                                  folder.getFullPathName().toStdString()))
        {
            if (!pushed) { pushUndo(); pushed = true; }
            setFileOn(*i, take);
            rebuildAudio();
            markDirty();
            if (onBlockGeometry) onBlockGeometry(i->block.length, tailSecondsOf(*i), i->block.hasAudio());
            repaint();
            return;
        }
}

// --- the document ---------------------------------------------------------------------
//
// One .mira file per project, beside the block folders it names. Takes are NOT listed --
// they are whatever is in a block's folder, so a take added or removed outside mira is
// simply seen next time.

juce::String CanvasView::getDocumentName() const
{
    return documentFile != juce::File() ? documentFile.getFileNameWithoutExtension()
                                        : juce::String("Untitled");
}

void CanvasView::markDirty()
{
    if (dirty) return;
    dirty = true;
    if (onDocumentChanged) onDocumentChanged();
}

void CanvasView::writeTo(const juce::File& miraFile) const
{
    miraFile.replaceWithText(toJson(miraFile.getParentDirectory()));
}

juce::String CanvasView::toJson(const juce::File& base) const
{
    juce::Array<juce::var> blocks;
    for (const auto& i : items)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("name", i->block.name);
        o->setProperty("lane", i->block.lane);
        o->setProperty("start", i->block.start);
        o->setProperty("length", i->block.length);
        o->setProperty("offset", i->block.sourceOffset);
        o->setProperty("content", i->block.contentSeconds);
        o->setProperty("fadeIn", i->block.fadeIn);
        o->setProperty("fadeOut", i->block.fadeOut);
        o->setProperty("gainDb", i->block.gainDb);
        o->setProperty("colour", i->block.colour);
        o->setProperty("muted", i->block.muted);
        o->setProperty("fadeShape", (int) i->block.fadeShape);
        if (i->block.hasAudio())
        {
            // Relative when it lives under the document, absolute when it does not. A
            // project you can rename or move to another drive is the difference between a
            // document and a folder with a pointer in it.
            const auto full = i->block.file.getFullPathName();
            const bool inside = full.startsWith(base.getFullPathName() + "/");
            o->setProperty("file", inside ? i->block.file.getRelativePathFrom(base) : full);
        }
        if (!i->settings.isVoid()) o->setProperty("settings", i->settings);
        blocks.add(juce::var(o));
    }

    juce::Array<juce::var> names;
    for (const auto& n : laneNames) names.add(n);
    juce::Array<juce::var> gains;
    for (auto g : laneDb) gains.add(g);

    auto* root = new juce::DynamicObject();
    root->setProperty("format", "mira-canvas");
    root->setProperty("version", 1);
    root->setProperty("blocks", juce::var(blocks));
    root->setProperty("laneNames", juce::var(names));
    root->setProperty("laneGainDb", juce::var(gains));
    root->setProperty("laneCount", laneCount);
    root->setProperty("waveZoom", (double) waveZoom);
    root->setProperty("muteMask", juce::String(muteMask));
    return juce::JSON::toString(juce::var(root), false);
}

bool CanvasView::readFrom(const juce::File& miraFile)
{
    return fromJson(miraFile.loadFileAsString(), miraFile.getParentDirectory(), true);
}

bool CanvasView::fromJson(const juce::String& json, const juce::File& base, bool refit)
{
    items.clear();
    selected.clear();
    panelBlockId = 0;      // the block it was showing no longer exists
    laneNames.clear();
    laneDb.clear();
    muteMask = soloMask = 0;
    laneCount = 1;

    const auto root = juce::JSON::parse(json);
    if (!root.isObject()) return false;

    if (auto* names = root.getProperty("laneNames", {}).getArray())
        for (int i = 0; i < names->size(); ++i) laneNames.set(i, (*names)[i].toString());
    if (auto* gains = root.getProperty("laneGainDb", {}).getArray())
        for (int i = 0; i < gains->size(); ++i) setLaneDb(i, (double) (*gains)[i]);
    muteMask = (juce::uint64) root.getProperty("muteMask", "0").toString().getLargeIntValue();
    laneCount = juce::jlimit(1, CanvasAudioSource::kMaxLanes, (int) root.getProperty("laneCount", 1));
    waveZoom = juce::jlimit(0.15f, 16.0f, (float) (double) root.getProperty("waveZoom", 1.0));

    if (auto* blocks = root.getProperty("blocks", {}).getArray())
        for (const auto& b : *blocks)
        {
            auto v = std::make_unique<Visual>();
            v->block.name = b.getProperty("name", "block").toString();
            v->block.lane = (int) b.getProperty("lane", 0);
            v->block.start = (double) b.getProperty("start", 0.0);
            v->block.length = (double) b.getProperty("length", 8.0);
            v->block.sourceOffset = (double) b.getProperty("offset", 0.0);
            v->block.contentSeconds = (double) b.getProperty("content", 0.0);
            v->block.fadeIn = (double) b.getProperty("fadeIn", 0.0);
            v->block.fadeOut = (double) b.getProperty("fadeOut", 0.0);
            v->block.gainDb = (double) b.getProperty("gainDb", 0.0);
            // Documents written before blocks owned a colour fall back to their track,
            // which is exactly what they looked like when they were saved.
            v->block.colour = (int) b.getProperty("colour", v->block.lane);
            v->block.muted = (bool) b.getProperty("muted", false);
            v->block.fadeShape = (FadeShape) juce::jlimit(0, 2, (int) b.getProperty("fadeShape", 0));
            v->block.id = nextId++;
            v->settings = b.getProperty("settings", {});
            laneCount = juce::jmax(laneCount, v->block.lane + 1);

            const auto path = b.getProperty("file", "").toString();
            // A take that has been moved or deleted leaves the block EMPTY rather than
            // silently vanishing: the frame and its generator are still what you meant.
            if (path.isNotEmpty())
                setFileOn(*v, juce::File::isAbsolutePath(path) ? juce::File(path)
                                                               : base.getChildFile(path));
            items.push_back(std::move(v));
        }

    applyMasks();
    // An UNDO must not move the view. Refitting after every undone edit would answer a
    // question nobody asked -- you undid a trim, not a zoom -- and lose the place you were
    // looking at, which is the one thing undo is supposed to give back.
    if (refit && !items.empty()) fit();
    rebuildAudio();
    repaint();
    return true;
}

// ---- undo ---------------------------------------------------------------------------
//
// Snapshots, not a command log. The document already serialises to JSON and back, so the
// cheapest correct undo is to keep the JSON: no per-edit inverse to write, and no edit
// that can be added later and forgotten about here. A canvas of a few dozen blocks is a
// few kilobytes, which is nothing next to the audio it points at.
//
// What it CANNOT undo is a generation -- the wav is on disk and stays there. What it does
// instead is exactly what you want after a bad extend: the block goes back to the take it
// was showing, and the new one is still in the folder if you change your mind.

void CanvasView::pushUndo()
{
    if (undoSuppressed) return;

    // Selection by NAME. Ids are handed out fresh on every load, so an id snapshotted now
    // means nothing after a restore; names are what the document actually carries.
    Snapshot snap;
    snap.json = toJson(projectFolder);
    for (const auto& i : items)
        if (selected.count(i->block.id)) snap.selection.add(i->block.name);

    undoStack.push_back(std::move(snap));
    if ((int) undoStack.size() > kUndoDepth) undoStack.erase(undoStack.begin());
    redoStack.clear();      // a new edit is a new branch
}

void CanvasView::restore(const Snapshot& snap)
{
    fromJson(snap.json, projectFolder, false);
    selected.clear();
    for (auto& i : items)
        if (snap.selection.contains(i->block.name)) selected.insert(i->block.id);

    // The panel has to be repointed: every Visual is new, so the id it was holding is
    // gone. Pointing it at the restored selection keeps "undo, then look at what came
    // back" from needing a click.
    panelBlockId = 0;
    pointPanelAt(singleSelection());
    announceSelection();
    markDirty();
    repaint();
}

void CanvasView::undo()
{
    if (undoStack.empty()) return;
    Snapshot now;
    now.json = toJson(projectFolder);
    for (const auto& i : items)
        if (selected.count(i->block.id)) now.selection.add(i->block.name);
    redoStack.push_back(std::move(now));

    auto snap = undoStack.back();
    undoStack.pop_back();
    restore(snap);
}

void CanvasView::redo()
{
    if (redoStack.empty()) return;
    Snapshot now;
    now.json = toJson(projectFolder);
    for (const auto& i : items)
        if (selected.count(i->block.id)) now.selection.add(i->block.name);
    undoStack.push_back(std::move(now));

    auto snap = redoStack.back();
    redoStack.pop_back();
    restore(snap);
}

bool CanvasView::newDocument(const juce::File& folder, const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName(name.trim());
    if (clean.isEmpty()) return false;
    const auto root = folder.getChildFile(clean);
    if (!root.isDirectory() && !root.createDirectory().wasOk()) return false;

    items.clear(); selected.clear(); laneNames.clear(); laneDb.clear();
    muteMask = soloMask = 0;
    laneCount = 1;
    projectFolder = root;
    documentFile = root.getChildFile(clean + kExtension);
    dirty = false;
    writeTo(documentFile);
    applyMasks();
    rebuildAudio();
    if (onDocumentChanged) onDocumentChanged();
    repaint();
    return true;
}

bool CanvasView::openDocument(const juce::File& miraFile)
{
    if (!miraFile.existsAsFile()) return false;
    projectFolder = miraFile.getParentDirectory();
    documentFile = miraFile;
    if (!readFrom(miraFile)) return false;
    dirty = false;
    if (onDocumentChanged) onDocumentChanged();
    return true;
}

bool CanvasView::saveDocument()
{
    if (documentFile == juce::File()) return false;   // caller has to ask where
    // What is on screen has not reached its block until now if you never clicked away.
    syncPanelSettings();
    writeTo(documentFile);
    dirty = false;
    if (onDocumentChanged) onDocumentChanged();
    return true;
}

bool CanvasView::saveDocumentAs(const juce::File& miraFile)
{
    auto target = miraFile;
    if (!target.getFileName().endsWithIgnoreCase(kExtension))
        target = target.getParentDirectory().getChildFile(target.getFileName() + kExtension);
    projectFolder = target.getParentDirectory();
    documentFile = target;
    return saveDocument();
}

void CanvasView::clearAll()
{
    items.clear();
    selected.clear();
    laneNames.clear();
    muteMask = soloMask = 0;
    applyMasks();
    rebuildAudio();
    repaint();
}

juce::Rectangle<int> CanvasView::boundsOf(const Visual& v) const
{
    const int x = secondsToX(v.block.start);
    const int w = juce::jmax(3, juce::roundToInt(v.block.length * pixelsPerSecond));
    return { x, laneToY(v.block.lane) + 3, w, laneHeight - 6 };
}

// ---- painting ----------------------------------------------------------------------

void CanvasView::paint(juce::Graphics& g)
{
    g.fillAll(MiraLookAndFeel::surface2);

    const int lanes = laneCount;
    for (int lane = 0; lane < lanes; ++lane)
    {
        // EACH TRACK IS ITS OWN SLAB, with a gap between it and the next. A row of
        // alternating tints separated by a hairline read as one striped surface -- "the
        // track looks joined with other track" -- and a track is the thing you mix with,
        // so it has to look like a thing.
        auto r = juce::Rectangle<int>(0, laneToY(lane), getWidth(), laneHeight).reduced(0, 2);
        const bool chosen = lane == selectedLane;
        g.setColour(chosen ? laneColour(lane).withAlpha(0.10f)
                           : MiraLookAndFeel::surface.withAlpha(0.55f));
        g.fillRect(r);
        // A colour rail down the left of the lane body, so which track a block is on is
        // answerable from the canvas as well as from the header.
        g.setColour(laneColour(lane).withAlpha(chosen ? 0.9f : 0.35f));
        g.fillRect(kHeaderWidth, r.getY(), 2, r.getHeight());
        if (chosen)
        {
            g.setColour(laneColour(lane).withAlpha(0.55f));
            g.drawRect(r, 1);
        }
    }

    // The loop region, under everything, so a block sitting in it still reads normally.
    if (loopEnd > loopStart)
    {
        const int a = secondsToX(loopStart), b = secondsToX(loopEnd);
        g.setColour(MiraLookAndFeel::accent.withAlpha(player.isLooping() ? 0.14f : 0.06f));
        g.fillRect(a, topRuler, juce::jmax(1, b - a), getHeight() - topRuler);
    }

    // --- ruler. SECONDS, not bars. There is no tempo on this canvas and inventing one to
    // have something to draw would be the exact lie Blockhead avoids: the grid comes from
    // the music, once there is music, or it does not come at all.
    {
        auto r = juce::Rectangle<int>(0, 0, getWidth(), topRuler);
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(r);
        g.setColour(MiraLookAndFeel::border);
        g.drawHorizontalLine(topRuler - 1, 0.0f, static_cast<float>(getWidth()));

        // A tick spacing that stays legible at any zoom, chosen from the 1-2-5 ladder
        // rather than a fixed number of seconds.
        static const double steps[] = { 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        double step = 600.0;
        for (double s : steps) if (s * pixelsPerSecond >= 64.0) { step = s; break; }

        g.setFont(laf.monoRegular(MiraLookAndFeel::textSize(9.5f)));
        const double first = std::floor(viewStart / step) * step;
        for (double t = first; secondsToX(t) < getWidth(); t += step)
        {
            const int x = secondsToX(t);
            if (x < kHeaderWidth) continue;
            g.setColour(MiraLookAndFeel::border);
            g.drawVerticalLine(x, 0.0f, static_cast<float>(topRuler));
            g.setColour(MiraLookAndFeel::textDim);
            g.drawText(formatTime(t), x + 3, 0, 60, topRuler, juce::Justification::centredLeft, false);
        }
    }

    // --- blocks
    for (const auto& item : items)
    {
        auto r = boundsOf(*item);
        if (r.getRight() < 0 || r.getX() > getWidth()) continue;
        const bool isSelected = selected.count(item->block.id) > 0;

        const bool laneMuted = item->block.lane < CanvasAudioSource::kMaxLanes
                            && ((muteMask & (juce::uint64 (1) << item->block.lane)) != 0
                                || (soloMask != 0
                                    && (soloMask & (juce::uint64 (1) << item->block.lane)) == 0));

        // An EMPTY block is a frame, not a slab: it is a length you meant with nothing in
        // it yet, and it has to read as waiting rather than as silent audio.
        if (!item->block.hasAudio())
        {
            g.setColour(MiraLookAndFeel::accent.withAlpha(0.06f));
            g.fillRoundedRectangle(r.toFloat(), 5.0f);
            juce::Path frame;
            frame.addRoundedRectangle(r.toFloat().reduced(1.0f), 5.0f);
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType(isSelected ? 2.0f : 1.2f).createDashedStroke(frame, frame, dashes, 2);
            g.setColour(MiraLookAndFeel::accent.withAlpha(isSelected ? 0.95f : 0.55f));
            g.fillPath(frame);
            g.setColour(MiraLookAndFeel::accent.withAlpha(0.9f));
            g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
            g.drawText(item->block.name + "  -  empty, generate into it",
                        r.reduced(8, 2), juce::Justification::centredLeft, true);
            continue;
        }

        // The BLOCK's colour, not the track's. Dragging a block to another track used to
        // recolour it, so the one thing you were following down a stack changed identity
        // exactly when you moved it.
        const auto tint = laneColour(item->block.colour);
        g.setColour(tint.withAlpha(laneMuted ? 0.07f : 0.17f));
        g.fillRoundedRectangle(r.toFloat(), 5.0f);

        if (item->thumb != nullptr && item->thumb->getTotalLength() > 0.0)
        {
            // The name strip only costs height while there is height to spare; below that
            // the waveform gets all of it, which is the point of zooming in vertically.
            const int nameStrip = r.getHeight() >= 46 ? 16 : 0;
            auto wave = r.reduced(4, 3).withTrimmedTop(nameStrip);
            // The waveform occupies only as much of the block as it actually fills. The
            // rest is the TAIL, and drawing the thumbnail across it would show empty space
            // as if it were silence someone recorded.
            const double tail = tailSecondsOf(*item);
            const double sounding = soundingSecondsOf(*item);
            if (tail > 0.0)
                wave = wave.withWidth(juce::jmax(2, juce::roundToInt(sounding * pixelsPerSecond)));
            g.setColour(MiraLookAndFeel::text.withAlpha(laneMuted ? 0.18f
                                                                  : (isSelected ? 0.85f : 0.6f)));
            item->thumb->drawChannels(g, wave, item->block.sourceOffset,
                                       item->block.sourceOffset + sounding, waveZoom);

            // The empty tail: what Extend or Remix would fill in. Dashed, because it is a
            // frame with nothing in it -- the same language an empty block speaks.
            if (tail > 0.02)
            {
                auto gap = r.withTrimmedLeft(juce::roundToInt(sounding * pixelsPerSecond))
                            .reduced(2, 3);
                if (gap.getWidth() > 3)
                {
                    juce::Path dash;
                    dash.addRoundedRectangle(gap.toFloat(), 3.0f);
                    const float pattern[] = { 4.0f, 3.0f };
                    juce::PathStrokeType(1.0f).createDashedStroke(dash, dash, pattern, 2);
                    g.setColour(MiraLookAndFeel::accent.withAlpha(0.7f));
                    g.fillPath(dash);
                    if (gap.getWidth() > 54 && gap.getHeight() > 16)
                    {
                        g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.5f)));
                        g.drawText(juce::String(tail, 1) + "s to fill", gap,
                                    juce::Justification::centred, false);
                    }
                }
            }
        }

        // The fades, drawn along the CURVE the mixer actually applies -- fadeGain() is the
        // same function CanvasEngine uses per sample. A straight wedge over a sine fade is
        // a picture of something the audio is not doing, and the take editor has already
        // been caught drawing a selection nobody could see.
        {
            const float top = (float) r.getY(), bottom = (float) r.getBottom();
            const float h = bottom - top;
            auto wedge = [&] (float edgeX, float w, bool rising)
            {
                if (w <= 0.5f) return;
                juce::Path p;
                p.startNewSubPath(edgeX, bottom);
                constexpr int kSteps = 24;
                for (int k = 0; k <= kSteps; ++k)
                {
                    const float t = (float) k / (float) kSteps;
                    const float gain = fadeGain(t, item->block.fadeShape);
                    p.lineTo(edgeX + (rising ? t * w : -t * w), bottom - gain * h);
                }
                p.lineTo(edgeX + (rising ? w : -w), top);
                p.lineTo(edgeX, top);
                p.closeSubPath();
                g.setColour(MiraLookAndFeel::surface.withAlpha(0.6f));
                g.fillPath(p);
                g.setColour(tint.brighter(0.5f).withAlpha(0.7f));
                g.strokePath(p, juce::PathStrokeType(1.0f));
            };
            wedge((float) r.getX(),     (float) (item->block.fadeIn  * pixelsPerSecond), true);
            wedge((float) r.getRight(), (float) (item->block.fadeOut * pixelsPerSecond), false);

            // The handles, on the selected block only. Always shown once selected, even at
            // zero fade -- a grab point you cannot see is a feature nobody finds.
            if (isSelected && r.getHeight() >= 26)
            {
                g.setColour(MiraLookAndFeel::text.withAlpha(0.9f));
                const int fi = r.getX() + juce::roundToInt(item->block.fadeIn * pixelsPerSecond);
                const int fo = r.getRight() - juce::roundToInt(item->block.fadeOut * pixelsPerSecond);
                for (int hx : { fi, fo })
                    g.fillRoundedRectangle((float) (juce::jlimit(r.getX(), r.getRight() - 7, hx - 3)),
                                            (float) (r.getY() + 3), 7.0f, 7.0f, 2.0f);
            }
        }

        // A MUTED BLOCK has to read as muted at a glance, not on inspection: hatched, so
        // it is distinguishable from a quiet one even in a screenshot.
        if (item->block.muted)
        {
            g.setColour(MiraLookAndFeel::surface.withAlpha(0.72f));
            g.fillRoundedRectangle(r.toFloat(), 5.0f);
            // CLIPPED to the block. Without this the hatching runs the full width of the
            // lane, so a muted block reads as a muted TRACK -- the one thing it is not.
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion(r);
            g.setColour(tint.withAlpha(0.35f));
            for (int x = r.getX() - r.getHeight(); x < r.getRight(); x += 9)
                g.drawLine((float) x, (float) r.getBottom(),
                            (float) (x + r.getHeight()), (float) r.getY(), 1.0f);
        }

        g.setColour(isSelected ? MiraLookAndFeel::text : tint.withAlpha(0.55f));
        g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 5.0f, isSelected ? 1.8f : 1.0f);

        if (r.getHeight() >= 46)
        {
            // The block's own header: an M you can hit, then the name. A right-click menu
            // is where you go to find something; a button on the thing itself is where you
            // go to DO it, and mute is the second kind.
            const auto mb = blockMuteBox(*item);
            if (!mb.isEmpty())
            {
                g.setColour(item->block.muted ? MiraLookAndFeel::accent.withAlpha(0.85f)
                                               : tint.withAlpha(0.35f));
                g.fillRoundedRectangle(mb.toFloat(), 2.5f);
                g.setColour(item->block.muted ? MiraLookAndFeel::surface
                                               : MiraLookAndFeel::text.withAlpha(0.75f));
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.0f)));
                g.drawText("M", mb, juce::Justification::centred, false);
            }

            g.setColour(isSelected ? MiraLookAndFeel::text : tint.brighter(0.4f));
            g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
            // "block1 _ name of the file": the block's own name AND what is in it. The
            // block name alone says nothing about which take you chose, and the filename
            // alone loses which part of the piece this is.
            const auto label = item->block.name
                             + (item->block.hasAudio()
                                    ? "  -  " + item->block.file.getFileNameWithoutExtension()
                                    : juce::String());
            auto headerRow = r.reduced(6, 2).removeFromTop(14)
                               .withTrimmedLeft(mb.isEmpty() ? 0 : mb.getWidth() + 4);

            // Key and tempo on the RIGHT of the same row, so the name can be as long as it
            // likes without pushing them off.
            if (const auto tags = keyAndTempoOf(item->settings); tags.isNotEmpty()
                                                                 && headerRow.getWidth() > 150)
            {
                auto tagBox = headerRow.removeFromRight(juce::jmin(130, headerRow.getWidth() / 2));
                g.setColour(isSelected ? MiraLookAndFeel::text.withAlpha(0.75f)
                                       : tint.brighter(0.15f).withAlpha(0.8f));
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.5f)));
                g.drawText(tags, tagBox, juce::Justification::centredRight, true);
                g.setColour(isSelected ? MiraLookAndFeel::text : tint.brighter(0.4f));
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
            }

            g.drawText(label, headerRow, juce::Justification::centredLeft, true);
        }
    }

    if (!marquee.isEmpty())
    {
        g.setColour(MiraLookAndFeel::accent.withAlpha(0.15f));
        g.fillRect(marquee);
        g.setColour(MiraLookAndFeel::accent.withAlpha(0.6f));
        g.drawRect(marquee, 1);
    }

    // --- lane headers, painted AFTER the blocks so a block scrolled left disappears
    // under them rather than over them.
    {
        auto strip = juce::Rectangle<int>(0, topRuler, kHeaderWidth, getHeight() - topRuler);
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(strip);
        g.setColour(MiraLookAndFeel::border);
        g.drawVerticalLine(kHeaderWidth - 1, static_cast<float>(topRuler), static_cast<float>(getHeight()));

        for (int lane = 0; lane < lanes; ++lane)
        {
            const bool muted  = lane < CanvasAudioSource::kMaxLanes
                             && (muteMask & (juce::uint64 (1) << lane)) != 0;
            const bool soloed = lane < CanvasAudioSource::kMaxLanes
                             && (soloMask & (juce::uint64 (1) << lane)) != 0;

            auto drawChip = [&](juce::Rectangle<int> box, const char* letter, bool on, juce::Colour tint) {
                g.setColour(on ? tint : MiraLookAndFeel::surface3);
                g.fillRoundedRectangle(box.toFloat(), 3.5f);
                g.setColour(on ? MiraLookAndFeel::surface : MiraLookAndFeel::textDim);
                g.setFont(laf.sansMedium(MiraLookAndFeel::textSize(10.0f)));
                g.drawText(letter, box, juce::Justification::centred, false);
            };
            drawChip(muteBoxFor(lane), "M", muted,  MiraLookAndFeel::warn);
            drawChip(soloBoxFor(lane), "S", soloed, MiraLookAndFeel::accent);

            g.setColour(muted ? MiraLookAndFeel::textFaint : laneColour(lane).brighter(0.2f));
            g.setFont(laf.sansSemiBold(MiraLookAndFeel::textSize(12.5f)));
            if (lane != renamingLane)
                g.drawText(laneNames[lane].isNotEmpty() ? laneNames[lane] : juce::String(lane + 1),
                            nameBoxFor(lane), juce::Justification::centredLeft, true);

            auto fader = faderBoxFor(lane);
            auto meterBox = meterBoxFor(lane);
            auto stripBox = stripBoxFor(lane);
            const bool strip = !fader.isEmpty();

            // One well, containing both. This is what makes it read as a single mixer
            // control rather than as two neighbours: the meter is INSIDE the fader's
            // widget, on the fader's scale.
            if (strip)
            {
                g.setColour(MiraLookAndFeel::surface.darker(0.5f));
                g.fillRoundedRectangle(stripBox.toFloat(), 3.5f);
                g.setColour(MiraLookAndFeel::border.withAlpha(0.5f));
                g.drawRoundedRectangle(stripBox.toFloat().reduced(0.5f), 3.5f, 1.0f);
            }

            // ONE mapping, used by both. `norm` is 0 at -60 dB and 1 at +6, warped so the
            // working range gets the travel -- see dbToNorm.
            auto yFor = [&](juce::Rectangle<int> box, double db) {
                return (float) box.getBottom() - (float) box.getHeight() * (float) dbToNorm(db);
            };

            // --- the scale, drawn BETWEEN the fader and the meter so it reads for both.
            if (strip)
            {
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(8.0f)));
                // A scale with more marks than it has room for is a smear. Drop to the two
                // that matter -- unity and -12 -- when the lane is short, and label them
                // only when there is width to the right of the meter to label them in.
                const bool roomy = fader.getHeight() >= 70;
                const bool labels = kHeaderWidth - stripBox.getRight() >= 26;
                for (double tick : roomy ? std::vector<double>{ 6.0, 0.0, -6.0, -12.0, -24.0, -40.0 }
                                         : std::vector<double>{ 0.0, -12.0 })
                {
                    const float y = yFor(fader, tick);
                    if (y < fader.getY() + 4 || y > fader.getBottom() - 2) continue;
                    // The tick crosses BOTH controls. That line is the whole argument for
                    // one scale: you can see where the fader is against where the signal
                    // is, in one look, without reading two numbers.
                    g.setColour(MiraLookAndFeel::border.withAlpha(tick == 0.0 ? 0.9f : 0.45f));
                    g.fillRect((float) stripBox.getX() + 1.0f, y, (float) stripBox.getWidth() - 2.0f, 1.0f);
                    if (!labels) continue;
                    g.setColour(MiraLookAndFeel::textFaint.withAlpha(tick == 0.0 ? 0.9f : 0.6f));
                    g.drawText(tick > 0 ? "+" + juce::String((int) tick) : juce::String((int) tick),
                                juce::Rectangle<int>(stripBox.getRight() + 3, (int) y - 5, 22, 10),
                                juce::Justification::centredLeft, false);
                }
            }

            // --- the meter: two bars, left and right, against that same scale.
            if (!meterBox.isEmpty())
            {
                const auto lv = lane < (int) laneMeter.size() ? laneMeter[(size_t) lane]
                                                              : std::array<float, 2>{ 0.0f, 0.0f };
                const auto hd = lane < (int) laneHold.size()  ? laneHold[(size_t) lane]
                                                              : std::array<float, 2>{ 0.0f, 0.0f };
                const float barW = (meterBox.getWidth() - 3.0f) * 0.5f;
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float x = meterBox.getX() + 1.0f + ch * (barW + 1.0f);
                    if (lv[(size_t) ch] > 0.0005f)
                    {
                        const double db = juce::Decibels::gainToDecibels(lv[(size_t) ch]);
                        // Three bands, so the colour is where the LEVEL is rather than one
                        // colour for the whole column -- a peak touching red shows red at
                        // the top and green below it, the way a real meter does.
                        auto band = [&](double lo, double hi, juce::Colour c) {
                            if (db < lo) return;
                            const float y0 = yFor(meterBox, juce::jmin(hi, db)), y1 = yFor(meterBox, lo);
                            if (y0 >= y1) return;
                            g.setColour(c);
                            g.fillRect(juce::Rectangle<float>(x, y0, barW, y1 - y0));
                        };
                        band(kFaderBottomDb, -6.0, MiraLookAndFeel::active);
                        band(-6.0, -1.0, MiraLookAndFeel::accent);
                        band(-1.0, kFaderTopDb, MiraLookAndFeel::warn);
                    }
                    if (hd[(size_t) ch] > 0.0005f)
                    {
                        const double db = juce::Decibels::gainToDecibels(hd[(size_t) ch]);
                        g.setColour(db > -1.0 ? MiraLookAndFeel::warn
                                              : MiraLookAndFeel::text.withAlpha(0.85f));
                        g.fillRect(x, yFor(meterBox, db) - 1.0f, barW, 1.5f);
                    }
                }

                // CLIP, latched. A clip that shows for 200 ms is a clip you will miss, and
                // the canvas sums tracks -- overs are the failure mode it invites. Click
                // the strip to clear it.
                if (lane < (int) laneClipped.size() && laneClipped[(size_t) lane])
                {
                    g.setColour(MiraLookAndFeel::warn);
                    g.fillRect((float) stripBox.getX(), (float) stripBox.getY() - 4.0f,
                                (float) stripBox.getWidth(), 3.0f);
                }
            }

            // --- the fader: a groove with a proper cap, on the same scale.
            if (strip)
            {
                const double db = laneDbAt(lane);
                auto groove = fader.toFloat().withSizeKeepingCentre(3.0f, (float) fader.getHeight());
                g.setColour(MiraLookAndFeel::surface.darker(0.35f));
                g.fillRoundedRectangle(groove, 1.5f);

                const float capY = yFor(fader, db);
                g.setColour(muted ? MiraLookAndFeel::textFaint : laneColour(lane).withAlpha(0.75f));
                g.fillRoundedRectangle(groove.withTop(capY), 1.5f);

                // The cap spans the WHOLE widget, meter included -- that is what makes the
                // two halves one control rather than two. A centre line so its exact
                // position is readable against the scale; a filled bar alone cannot say
                // where the control is when the value is at the bottom.
                auto cap = juce::Rectangle<float>((float) stripBox.getX() + 1.0f, capY - 4.5f,
                                                   (float) stripBox.getWidth() - 2.0f, 9.0f);
                g.setColour(muted ? MiraLookAndFeel::surface3 : laneColour(lane).brighter(0.3f));
                g.fillRoundedRectangle(cap, 2.5f);
                g.setColour(MiraLookAndFeel::surface.darker(0.6f));
                g.drawRoundedRectangle(cap, 2.5f, 1.0f);
                g.fillRect(cap.getX() + 2.0f, cap.getCentreY() - 0.5f, cap.getWidth() - 4.0f, 1.0f);
            }

            // --- the number. A fader without one is a gesture you cannot repeat.
            if (strip)
            {
                const double db = laneDbAt(lane);
                g.setColour(muted ? MiraLookAndFeel::textFaint : MiraLookAndFeel::textDim);
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.5f)));
                g.drawText(db <= kFaderBottomDb ? juce::String("-inf")
                                                : juce::String(db, 1) + " dB",
                            juce::Rectangle<int>(kHeaderWidth - 62, laneToY(lane) + 44, 56, 12),
                            juce::Justification::centredLeft, false);
            }
        }
        // The ruler's own corner, so the seconds do not run under the headers.
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(0, 0, kHeaderWidth, topRuler);
        g.setColour(MiraLookAndFeel::border);
        g.drawVerticalLine(kHeaderWidth - 1, 0.0f, static_cast<float>(topRuler));
    }

    // --- playhead, over everything
    {
        const int x = secondsToX(player.getPositionSeconds());
        if (x >= kHeaderWidth && x < getWidth())
        {
            g.setColour(MiraLookAndFeel::accent);
            g.drawVerticalLine(x, 0.0f, static_cast<float>(getHeight()));
            juce::Path head;
            head.addTriangle((float) x - 5, 0.0f, (float) x + 5, 0.0f, (float) x, 8.0f);
            g.fillPath(head);
        }
    }

    if (items.empty())
    {
        g.setColour(MiraLookAndFeel::textFaint);
        g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(13.0f)));
        g.drawText("drop audio here - no tempo, no grid, put it where you want it",
                    getLocalBounds(), juce::Justification::centred, false);
    }
}

void CanvasView::resized() {}

// ---- interaction -------------------------------------------------------------------

CanvasView::Visual* CanvasView::hitTest(juce::Point<int> p, Drag& what)
{
    // Back to front, so the block drawn on top is the one you grab.
    for (auto it = items.rbegin(); it != items.rend(); ++it)
    {
        auto r = boundsOf(**it);
        if (!r.contains(p)) continue;
        const auto& b = (*it)->block;

        // The fade handles ride the TOP of the block, where the wedge meets the edge, and
        // they win over trimming there. Trim still has the whole height below the band, so
        // one corner is not asked to mean two things at the same y.
        if (r.getHeight() >= 26 && p.y - r.getY() <= kFadeBand)
        {
            const int fi = r.getX() + juce::roundToInt(b.fadeIn * pixelsPerSecond);
            const int fo = r.getRight() - juce::roundToInt(b.fadeOut * pixelsPerSecond);
            if (std::abs(p.x - fi) <= kFadeGrab) { what = Drag::FadeIn;  return it->get(); }
            if (std::abs(p.x - fo) <= kFadeGrab) { what = Drag::FadeOut; return it->get(); }
        }

        if (p.x - r.getX() <= kEdgeGrab)        what = Drag::TrimLeft;
        else if (r.getRight() - p.x <= kEdgeGrab) what = Drag::TrimRight;
        else                                     what = Drag::Move;
        return it->get();
    }
    what = Drag::None;
    return nullptr;
}

void CanvasView::mouseMove(const juce::MouseEvent& e)
{
    Drag what = Drag::None;
    hitTest(e.getPosition(), what);
    setMouseCursor(what == Drag::TrimLeft || what == Drag::TrimRight
                       ? juce::MouseCursor::LeftRightResizeCursor
                   : (what == Drag::FadeIn || what == Drag::FadeOut)
                       ? juce::MouseCursor::PointingHandCursor
                       : juce::MouseCursor::NormalCursor);
}

void CanvasView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragFrom = e.getPosition();

    // The lane headers first: they sit over everything on the left, so a click there is
    // never a click on a block.
    if (e.x < kHeaderWidth && e.y >= topRuler)
    {
        const int lane = yToLane(e.y);
        if (lane >= laneCount) return;
        // Any click on the strip acknowledges the clip. Latched indicators need a way to
        // be cleared or they stop meaning "this happened" and start meaning "this happened
        // at some point, once, maybe ages ago".
        if (lane < (int) laneClipped.size()) laneClipped[(size_t) lane] = false;
        // Clicking a header SELECTS the track. That is what makes "delete this track" a
        // thing you can ask for, and it costs nothing -- M, S and the fader all still do
        // their own jobs because they are tested before this takes effect.
        selectedLane = lane;
        if (lane < CanvasAudioSource::kMaxLanes)
        {
            const juce::uint64 bit = juce::uint64 (1) << lane;
            if (muteBoxFor(lane).contains(e.getPosition())) muteMask ^= bit;
            else if (soloBoxFor(lane).contains(e.getPosition())) soloMask ^= bit;
            else if (auto fader = faderBoxFor(lane);
                     !fader.isEmpty() && fader.expanded(6, 4).contains(e.getPosition()))
            {
                faderLane = lane;
                if (e.mods.isCommandDown()) setLaneDb(lane, 0.0);   // cmd-click = unity
                else setLaneDb(lane, faderDbAtY(lane, e.y));
                repaint();
                return;
            }
            else return;
            applyMasks();
            repaint();
        }
        return;
    }

    if (e.y < topRuler)
    {
        drag = Drag::Playhead;
        player.setPositionSeconds(juce::jmax(0.0, xToSeconds(e.x)));
        repaint();
        return;
    }
    if (e.mods.isMiddleButtonDown() || e.mods.isAltDown())
    {
        drag = Drag::Pan;
        panFromView = viewStart;
        return;
    }

    Drag what = Drag::None;
    auto* hit = hitTest(e.getPosition(), what);
    if (hit == nullptr)
    {
        selectedLane = -1;      // clicking the canvas is not about a track
        if (!e.mods.isShiftDown()) selected.clear();
        announceSelection();
        pointPanelAt(nullptr);
        drag = Drag::Marquee;
        marquee = { e.x, e.y, 0, 0 };
        repaint();
        return;
    }

    if (e.mods.isShiftDown() || e.mods.isCommandDown())
    {
        if (selected.count(hit->block.id)) selected.erase(hit->block.id);
        else selected.insert(hit->block.id);
    }
    else if (selected.count(hit->block.id) == 0)
    {
        selected.clear();
        selected.insert(hit->block.id);
    }

    if (auto mb = blockMuteBox(*hit); !mb.isEmpty() && mb.contains(e.getPosition())
                                       && !e.mods.isPopupMenu())
    {
        drag = Drag::None;
        pushUndo();
        hit->block.muted = !hit->block.muted;
        rebuildAudio();
        markDirty();
        repaint();
        return;
    }

    if (e.mods.isPopupMenu())
    {
        drag = Drag::None;
        repaint();
        showBlockMenu(*hit);
        return;
    }

    // ONE snapshot per gesture, taken as the drag begins -- not per mouse event, or
    // undoing a slow drag would take fifty presses to get back where you started.
    if (what == Drag::Move || what == Drag::TrimLeft || what == Drag::TrimRight
        || what == Drag::FadeIn || what == Drag::FadeOut) pushUndo();

    drag = what;
    dragTarget = hit->block.id;
    dragOriginFadeIn  = hit->block.fadeIn;
    dragOriginFadeOut = hit->block.fadeOut;
    announceSelection();
    // Selecting a block IS opening its generator now that the panel is always on screen.
    pointPanelAt(hit);
    dragOrigins.clear();
    for (const auto& i : items)
        if (selected.count(i->block.id)) dragOrigins[i->block.id] = { i->block.start, i->block.lane };
    dragGrabSeconds = xToSeconds(e.x);
    dragOriginStart = hit->block.start;
    dragOriginLength = hit->block.length;
    dragOriginOffset = hit->block.sourceOffset;
    dragOriginLane = hit->block.lane;
    repaint();
}

void CanvasView::mouseDrag(const juce::MouseEvent& e)
{
    if (faderLane >= 0)
    {
        setLaneDb(faderLane, faderDbAtY(faderLane, e.y));
        repaint();
        return;
    }
    if (drag == Drag::Playhead)
    {
        player.setPositionSeconds(juce::jmax(0.0, xToSeconds(e.x)));
        repaint();
        return;
    }
    if (drag == Drag::Pan)
    {
        viewStart = juce::jmax(0.0, panFromView - (e.x - dragFrom.x) / pixelsPerSecond);
        repaint();
        return;
    }
    if (drag == Drag::Marquee)
    {
        marquee = juce::Rectangle<int>(dragFrom, e.getPosition());
        selected.clear();
        for (const auto& i : items)
            if (boundsOf(*i).intersects(marquee)) selected.insert(i->block.id);
        repaint();
        return;
    }
    if (drag == Drag::None) return;

    const double deltaSeconds = xToSeconds(e.x) - dragGrabSeconds;

    for (auto& i : items)
    {
        if (selected.count(i->block.id) == 0) continue;
        auto& b = i->block;

        if (drag == Drag::Move)
        {
            // Every selected block moves by the SAME delta, from where it started, and
            // the lane shift is taken from the dragged block -- so a multi-block
            // selection keeps its shape instead of collapsing onto one lane.
            const auto origin = dragOrigins.find(b.id);
            if (origin == dragOrigins.end()) continue;
            const int laneShift = yToLane(e.y) - dragOriginLane;
            b.start = juce::jmax(0.0, origin->second.first + deltaSeconds);
            // CLAMPED TO TRACKS THAT EXIST. Dragging below the last track used to drop the
            // block onto empty space -- a lane with no header, no fader and no mute, which
            // is not a track, so the block was somewhere you could not mix it from.
            b.lane = juce::jlimit(0, juce::jmax(0, laneCount - 1),
                                  origin->second.second + laneShift);
        }
        else if (drag == Drag::TrimLeft && b.id == dragTarget)
        {
            // Trimming the left edge moves where in the FILE the block starts, so the
            // audio under the block stays put on the canvas instead of sliding.
            const double want = juce::jlimit(dragOriginStart - dragOriginOffset,
                                              dragOriginStart + dragOriginLength - 0.05,
                                              dragOriginStart + deltaSeconds);
            const double moved = want - dragOriginStart;
            b.start = want;
            b.sourceOffset = juce::jmax(0.0, dragOriginOffset + moved);
            b.length = juce::jmax(0.05, dragOriginLength - moved);
        }
        else if (drag == Drag::TrimRight && b.id == dragTarget)
        {
            // TRIMMING HIDES, IT DOES NOT CUT. Pulling the right edge in shows less of the
            // take; pulling it back out shows it again. An earlier version treated pulling
            // in as a cut and remembered it, so trimming a block destroyed its audio as
            // far as the canvas was concerned and there was no way back but a menu.
            //
            // Cutting is Cmd-E, and only Cmd-E: a deliberate "the audio ends here", which
            // is the thing an extend needs to know and a trim never meant to say.
            b.length = juce::jmax(0.05, dragOriginLength + deltaSeconds);
        }
        else if (drag == Drag::FadeIn && b.id == dragTarget)
        {
            // A fade can reach the whole block but no further -- past that it would be
            // asked to fade for longer than there is audio to fade.
            b.fadeIn = juce::jlimit(0.0, b.length, dragOriginFadeIn + deltaSeconds);
        }
        else if (drag == Drag::FadeOut && b.id == dragTarget)
        {
            b.fadeOut = juce::jlimit(0.0, b.length, dragOriginFadeOut - deltaSeconds);
        }
    }

    repaint();
}

void CanvasView::mouseUp(const juce::MouseEvent&)
{
    faderLane = -1;
    const bool changed = drag == Drag::Move || drag == Drag::TrimLeft || drag == Drag::TrimRight
                      || drag == Drag::FadeIn || drag == Drag::FadeOut;
    drag = Drag::None;
    marquee = {};
    if (changed)
    {
        rebuildAudio();
        markDirty();
        // Resizing the block IS setting the duration, so it has to reach the generator on
        // mouse-up rather than the next time you happen to reselect the block.
        if (onBlockGeometry)
        {
            const auto g = selectionGeometry();
            if (g.length > 0.0) onBlockGeometry(g.length, g.tail, g.hasAudio);
        }
    }
    repaint();
}

void CanvasView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // A MOUSE WHEEL HAS ONE AXIS, and that is the whole bug. A trackpad reports deltaX and
    // deltaY, so panning read deltaX and felt right; a wheel only ever reports deltaY, so
    // the pan branch was handed 0.0 on every event and the canvas simply would not move.
    // Everything below works off whichever axis actually moved instead of a fixed one.
    //
    // (macOS turns a shift-held wheel into deltaX itself, which is a second way the same
    // assumption broke: shift-zoom read deltaY and got nothing.)
    const float dx = wheel.deltaX, dy = wheel.deltaY;
    const float d  = std::abs (dx) > std::abs (dy) ? dx : dy;
    if (d == 0.0f) return;

    // A notched wheel sends a few big events; a trackpad sends a stream of small ones.
    // Scaling by the delta alone makes the wheel crawl, and a fixed step per event makes
    // the trackpad lurch a screen at a time. So: proportional when smooth, a step when not.
    const double step = wheel.isSmooth ? (double) d
                                       : (d > 0.0f ? 1.0 : -1.0) * 0.28;

    // Shift zooms VERTICALLY: taller lanes mean a taller waveform, which is the only way
    // to judge a quiet take against a loud one by eye. Separate from the horizontal zoom
    // because time and amplitude are separate questions.
    if (e.mods.isShiftDown())    { zoomVertical (step * 22.0); return; }
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
                                 { zoomBy (std::pow (1.6, step), e.x); return; }

    // Plain wheel pans the timeline. A wheel user has no other way to get there, and a
    // canvas that only scrolls for trackpads is a canvas half the input devices cannot
    // navigate.
    panBy (-step * 520.0 / pixelsPerSecond);
}

void CanvasView::panBy (double seconds)
{
    viewStart = juce::jmax (0.0, viewStart + seconds);
    repaint();
}

void CanvasView::zoomVertical (double pixels)
{
    const int was = laneHeight;
    laneHeight = juce::jlimit (28, 320, laneHeight + juce::roundToInt (pixels));
    // A zoom that rounds to no change at all should still not repaint forever.
    if (laneHeight != was) repaint();
}

// Where a keyboard zoom should anchor. The playhead when you can see it -- that is the
// thing you are looking at -- and the middle of the view when you cannot, rather than the
// left edge, which throws away half the zoom.
int CanvasView::zoomAnchorX() const
{
    const int p = secondsToX (player.getPositionSeconds());
    if (p >= kHeaderWidth && p <= getWidth()) return p;
    return kHeaderWidth + (getWidth() - kHeaderWidth) / 2;
}

void CanvasView::zoomBy(double factor, int aroundX)
{
    // The second under the cursor stays under the cursor, which is the only zoom that
    // does not feel like the canvas jumped.
    const double anchor = xToSeconds(aroundX);
    pixelsPerSecond = juce::jlimit(0.5, 400.0, pixelsPerSecond * factor);
    viewStart = juce::jmax(0.0, anchor - (aroundX - kHeaderWidth) / pixelsPerSecond);
    repaint();
}

bool CanvasView::keyPressed(const juce::KeyPress& key)
{
    // The document shortcuts every app has. Handled here rather than in the app menu bar
    // because the canvas is the only thing that has a document, and a global Cmd+S that
    // sometimes meant the canvas and sometimes meant nothing would be worse than none.
    if (key.getModifiers().isCommandDown())
    {
        if (key.getKeyCode() == 'S' && onSaveRequested) { onSaveRequested(); return true; }
        if (key.getKeyCode() == 'O' && onOpenRequested) { onOpenRequested(); return true; }
        if (key.getKeyCode() == 'N' && onNewRequested)  { onNewRequested();  return true; }
        if (key.getKeyCode() == 'E') { cutAtPlayhead(); return true; }
        // Cmd-Z / Cmd-shift-Z, the two every app has. Handled here rather than in the menu
        // bar because the canvas is the only thing in mira with a document to undo.
        if (key.getKeyCode() == 'Z')
        {
            if (key.getModifiers().isShiftDown()) redo(); else undo();
            return true;
        }
    }

    // ZOOM FROM THE KEYBOARD. G and H horizontally, shift-G and shift-H vertically --
    // left-to-right reading as less-to-more, so H opens the view out under your eye and G
    // pulls it back. On the keyboard because scroll gestures are not the same on every
    // device and a shortcut is: it does the same thing on a trackpad, a wheel, and a
    // laptop with neither to hand.
    if (key.getKeyCode() == 'G' || key.getKeyCode() == 'H')
    {
        const bool in = key.getKeyCode() == 'H';
        if (key.getModifiers().isShiftDown()) zoomVertical (in ? 10.0 : -10.0);
        else                                  zoomBy (in ? 1.25 : 1.0 / 1.25, zoomAnchorX());
        return true;
    }
    if (key == juce::KeyPress::spaceKey)       { togglePlay(); return true; }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
                                               { removeSelected(); return true; }
    if (key == juce::KeyPress::returnKey)      { player.setPositionSeconds(0.0); repaint(); return true; }
    if (key.getTextCharacter() == 'm' || key.getTextCharacter() == 's')
    {
        // Acts on the lanes of whatever is selected, so "mute this take" is one key after
        // clicking it rather than a trip to the header.
        juce::uint64 bits = 0;
        for (const auto& i : items)
            if (selected.count(i->block.id) && i->block.lane < CanvasAudioSource::kMaxLanes)
                bits |= juce::uint64 (1) << i->block.lane;
        if (bits == 0) return true;
        (key.getTextCharacter() == 'm' ? muteMask : soloMask) ^= bits;
        applyMasks();
        repaint();
        return true;
    }
    if (key.getTextCharacter() == 'l')         { setLoopFromSelection(); return true; }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'D')
        { duplicateSelection(); return true; }
    if (key.getTextCharacter() == 'f')         { fit(); return true; }
    // WAVEFORM height, which is not block height. A quiet take is a flat line you cannot
    // edit against and a loud one fills the block and shows nothing; the peaks you are
    // looking for are in neither. Drawn taller or shorter without moving anything.
    if (key.getTextCharacter() == ']')
        { waveZoom = juce::jmin(16.0f, waveZoom * 1.35f); repaint(); return true; }
    if (key.getTextCharacter() == '[')
        { waveZoom = juce::jmax(0.15f, waveZoom / 1.35f); repaint(); return true; }
    if (key.getTextCharacter() == '=' || key.getTextCharacter() == '+')
        { laneHeight = juce::jmin(320, laneHeight + 8); repaint(); return true; }
    if (key.getTextCharacter() == '-')
        { laneHeight = juce::jmax(28, laneHeight - 8); repaint(); return true; }
    return false;
}

void CanvasView::cutAtPlayhead()
{
    // CUT, not split. Cmd-E ends the block at the playhead and leaves ONE block -- the
    // part before the cut -- ready to extend from there.
    //
    // It used to split into two, which made a second block with a second folder and an
    // empty generator in it: you asked to end a take and got a new empty thing to explain.
    // Splitting is still available, on the right-click menu, where it reads as the
    // deliberate two-block operation it is.
    //
    // The audio is not touched. Only the block's claim about where it ends moves, so
    // "Restore full take" brings it all back and undo is one keypress.
    const double at = player.getPositionSeconds();

    std::vector<Visual*> victims;
    for (const auto& i : items)
    {
        const bool spans = i->block.start < at - 1.0e-6 && i->block.end() > at + 1.0e-6;
        if (!spans) continue;
        if (selected.empty() || selected.count(i->block.id)) victims.push_back(i.get());
    }
    if (victims.empty()) return;
    pushUndo();

    for (auto* v : victims)
    {
        const double left = at - v->block.start;
        v->block.length = left;
        v->block.contentSeconds = left;      // the audio ends here too, not just the frame
        v->block.fadeIn  = juce::jmin(v->block.fadeIn,  left);
        v->block.fadeOut = juce::jmin(v->block.fadeOut, left);
    }

    markDirty();
    rebuildAudio();
    announceSelection();
    repaint();
}

void CanvasView::splitAtPlayhead()
{
    const double at = player.getPositionSeconds();

    // What to cut: the selection when there is one, and otherwise everything the playhead
    // is standing on. "The block at the cursor" is the second case, and having to select
    // first would make the shortcut two gestures instead of one.
    std::vector<Visual*> victims;
    for (const auto& i : items)
    {
        const bool spans = i->block.start < at - 1.0e-6 && i->block.end() > at + 1.0e-6;
        if (!spans) continue;
        if (selected.empty() || selected.count(i->block.id)) victims.push_back(i.get());
    }
    if (victims.empty()) return;
    pushUndo();

    selected.clear();
    for (auto* v : victims)
    {
        const double leftLength = at - v->block.start;

        auto right = std::make_unique<Visual>();
        right->block = v->block;
        right->block.id = nextId++;
        right->block.start = at;
        right->block.length = v->block.length - leftLength;
        // The right half starts further INTO the file. Trimming moves the offset rather
        // than the audio, which is the same rule the left-edge drag follows.
        right->block.sourceOffset = v->block.sourceOffset + leftLength;
        // A new name, and so a new folder -- for the same reason a duplicate gets one. The
        // name is the generation target, and two blocks sharing a folder is exactly the
        // bug where generating on one put the audio on the other.
        right->block.name = nextBlockName();
        right->settings = v->settings;
        // A cut is a statement about where the audio ends, on both halves. Without it the
        // left half still claims the whole file, and dragging its right edge out would
        // reveal exactly the audio the cut was meant to remove.
        right->block.contentSeconds = v->block.length - leftLength;
        // The fade-out belongs to the piece that still has the end of the sound; the
        // fade-in to the piece that still has the start. Splitting in the middle of a fade
        // would otherwise leave both halves fading the wrong way.
        right->block.fadeIn = 0.0;
        v->block.fadeOut = 0.0;
        right->block.fadeOut = juce::jmin (v->block.fadeOut, right->block.length);

        setFileOn (*right, v->block.file);
        right->block.length = v->block.length - leftLength;   // setFileOn may have reset it
        right->block.sourceOffset = v->block.sourceOffset + leftLength;

        v->block.length = leftLength;
        v->block.contentSeconds = leftLength;
        v->block.fadeIn = juce::jmin (v->block.fadeIn, leftLength);

        selected.insert (right->block.id);
        items.push_back (std::move (right));   // one at a time, so the namer sees the last
    }

    pointPanelAt (items.back().get());
    markDirty();
    rebuildAudio();
    repaint();
}

void CanvasView::togglePlay()
{
    if (player.isPlaying())
    {
        player.stop();
        if (returnOnStop) player.setPositionSeconds(playedFrom);
    }
    else
    {
        playedFrom = player.getPositionSeconds();
        player.play();
    }
    if (onStateChanged) onStateChanged();
    repaint();
}

void CanvasView::removeSelected()
{
    // A selected TRACK is what Remove is about when there is one: you clicked the header,
    // not a block, and deleting "the selection" has to mean the thing you selected.
    if (selected.empty() && selectedLane >= 0) { removeLane(selectedLane); return; }
    if (selected.empty()) return;
    pushUndo();
    items.erase(std::remove_if(items.begin(), items.end(),
                                [this](const std::unique_ptr<Visual>& v) {
                                    return selected.count(v->block.id) > 0;
                                }),
                 items.end());
    selected.clear();
    rebuildAudio();
    repaint();
}

void CanvasView::setLoopFromSelection()
{
    if (selected.empty()) { loopStart = loopEnd = 0.0; player.setLoop(false, 0, 0); repaint(); return; }
    double a = 1e12, b = 0.0;
    for (const auto& i : items)
        if (selected.count(i->block.id))
        { a = juce::jmin(a, i->block.start); b = juce::jmax(b, i->block.end()); }
    if (b <= a) return;
    loopStart = a; loopEnd = b;
    player.setLoop(true, a, b);
    if (onStateChanged) onStateChanged();
    repaint();
}

void CanvasView::toggleLoop()
{
    if (loopEnd <= loopStart) { setLoopFromSelection(); return; }
    player.setLoop(!player.isLooping(), loopStart, loopEnd);
    if (onStateChanged) onStateChanged();
    repaint();
}

// ---- dropping in --------------------------------------------------------------------

bool CanvasView::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (formats.findFormatForFileExtension(juce::File(f).getFileExtension()) != nullptr)
            return true;
    return false;
}

void CanvasView::filesDropped(const juce::StringArray& files, int x, int y)
{
    juce::Array<juce::File> keep;
    for (const auto& f : files) keep.add(juce::File(f));
    // Clamped to ONE past the last track. Dropping low on an empty canvas used to create
    // every lane up to the cursor -- three tracks from one file, two of them empty.
    addFiles(keep, juce::jmax(0.0, xToSeconds(x)), juce::jmin(yToLane(y), laneCount));
}

void CanvasView::addFiles(const juce::Array<juce::File>& files, double atSeconds, int lane)
{
    // A DROPPED FILE BECOMES A BLOCK LIKE ANY OTHER, and that is the whole point of this
    // function now. The first version named the block after the file and left the audio
    // where it was, so the block's folder was empty, its generator showed "no takes yet",
    // and its label read "dun-s26-... - dun-s26-..." -- a block that looked like a take
    // and a generator with nothing in it. Two kinds of block, one of them broken.
    //
    // Now: the block is named "block N" like the rest, its folder is created, and the file
    // is COPIED into it as that block's first take. Drop it, and it is a take you can hear,
    // re-generate against, and keep beside alternatives -- the same object in every case.
    pushUndo();
    UndoGuard oneEdit (*this);
    double at = atSeconds;
    for (const auto& f : files)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor(f));
        if (reader == nullptr) continue;
        const double len = reader->sampleRate > 0.0 ? reader->lengthInSamples / reader->sampleRate : 0.0;
        if (len <= 0.0) continue;
        reader.reset();

        auto v = std::make_unique<Visual>();
        // Each dropped file is its own block, so each gets its own track -- the same rule
        // "+ Block" follows. Dropping four stems used to lay them end to end on one fader.
        if (!items.empty()) { addLane(); lane = laneCount - 1; }
        v->block.lane = juce::jlimit(0, juce::jmax(0, laneCount - 1), lane);
        v->block.colour = v->block.lane;
        v->block.start = at;
        v->block.length = len;
        v->block.id = nextId++;
        v->block.name = nextBlockName();
        laneCount = juce::jlimit(1, CanvasAudioSource::kMaxLanes,
                                  juce::jmax(laneCount, v->block.lane + 1));
        if (laneNames[v->block.lane].isEmpty())
            laneNames.set(v->block.lane, "track " + juce::String(v->block.lane + 1));

        auto landed = f;
        if (auto folder = blockFolderFor(*v); folder != juce::File())
        {
            if (folder.createDirectory().wasOk())
            {
                auto target = folder.getChildFile(f.getFileName());
                for (int n = 2; target.existsAsFile(); ++n)
                    target = folder.getChildFile(f.getFileNameWithoutExtension() + "-" + juce::String(n)
                                                  + f.getFileExtension());
                // Copied, not moved or referenced: the file may be someone else's, and a
                // project that stops working because a sample was tidied up elsewhere is
                // not a project. Its sidecar comes too when it has one.
                if (f.copyFileTo(target))
                {
                    landed = target;
                    if (auto side = f.withFileExtension("json"); side.existsAsFile())
                        side.copyFileTo(target.withFileExtension("json"));
                }
            }
        }

        setFileOn(*v, landed);
        v->block.length = len;
        selected.clear();
        selected.insert(v->block.id);
        items.push_back(std::move(v));
        // NOT `at += len`. Four stems dropped together belong at the same moment on four
        // tracks, not one after another down a queue.
    }
    markDirty();
    rebuildAudio();
    announceSelection();
    // A dropped file IS a block, so the panel follows it like any other selection.
    if (!items.empty()) pointPanelAt(items.back().get());
    repaint();
}

void CanvasView::timerCallback()
{
    if (player.isPlaying())
    {
        const int lanes = juce::jmax(4, (getHeight() - topRuler) / laneHeight + 1);
        const std::array<float, 2> zero { 0.0f, 0.0f };
        if ((int) laneMeter.size()  < lanes) laneMeter.resize((size_t) lanes, zero);
        if ((int) laneHold.size()   < lanes) laneHold.resize((size_t) lanes, zero);
        if ((int) laneClipped.size() < lanes) laneClipped.resize((size_t) lanes, false);
        for (int lane = 0; lane < lanes && lane < CanvasAudioSource::kMaxLanes; ++lane)
            for (int ch = 0; ch < 2; ++ch)
            {
                const float hit = player.readAndClearLanePeak(lane, ch);
                auto& held = laneMeter[(size_t) lane][(size_t) ch];
                // Instant attack, slow release: a meter that falls as fast as it rises is
                // a flicker you cannot read at 30 fps.
                held = hit > held ? hit : held * 0.80f;
                auto& hold = laneHold[(size_t) lane][(size_t) ch];
                hold = hit > hold ? hit : hold * 0.985f;   // ~2 s to fall away
                // LATCHED. Full scale reached even once is the thing you need to know
                // about, and it is over before the meter has finished a frame.
                if (hit >= 0.999f) laneClipped[(size_t) lane] = true;
            }
        repaint();
        return;
    }
    // Let the meters fall to nothing after a stop rather than freezing mid-level.
    bool alive = false;
    for (auto& m : laneMeter)
        for (auto& c : m) { if (c > 0.0005f) { c *= 0.8f; alive = true; } else c = 0.0f; }
    for (auto& h : laneHold)
        for (auto& c : h) { if (c > 0.0005f) { c *= 0.9f; alive = true; } else c = 0.0f; }
    if (alive) { repaint(); return; }

    // Thumbnails load on a background thread and finish whenever they finish. Without
    // this the canvas draws whatever had arrived by the time the window opened and never
    // again -- which is why every block first appeared as a thin sliver of waveform with
    // the rest of it blank. Polled rather than listened to because the alternative is one
    // ChangeListener registration per block, unregistered on every delete.
    for (const auto& i : items)
        if (i->thumb != nullptr && !i->thumb->isFullyLoaded()) { repaint(); return; }
}

// ---- window -------------------------------------------------------------------------


// ---- the side panel's tabs ----------------------------------------------------------
//
// A vertical tab strip down the panel's inside edge, the way Blockhead has it: the panel
// is one column of window and the tabs say which tool is in it, rather than each tool
// getting a window of its own to arrange. Adding the next one is a row in an enum and a
// component -- which is the point of doing it this way rather than stacking panes.
enum class SideTab { Generate = 0, Master, Files, Count };

static const char* sideTabName (SideTab t)
{
    switch (t)
    {
        case SideTab::Generate: return "GENERATE";
        case SideTab::Master:   return "MASTER";
        case SideTab::Files:    return "FILES";
        default:                return "";
    }
}

class TabStrip : public juce::Component
{
public:
    explicit TabStrip (const MiraLookAndFeel& lafIn) : laf (lafIn) {}

    std::function<void(SideTab)> onTab;
    SideTab current = SideTab::Generate;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (MiraLookAndFeel::surface2);
        g.setColour (MiraLookAndFeel::border);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

        for (int i = 0; i < (int) SideTab::Count; ++i)
        {
            auto box = boxFor (i);
            const bool on = (SideTab) i == current;
            if (on)
            {
                g.setColour (MiraLookAndFeel::surface);
                g.fillRect (box);
                // The marker is on the INSIDE edge, against the content it selects, so the
                // tab reads as attached to the panel rather than as a button near it.
                g.setColour (MiraLookAndFeel::accent);
                g.fillRect (box.getRight() - 2, box.getY(), 2, box.getHeight());
            }

            // Rotated, because a vertical strip wide enough for horizontal words is not a
            // strip any more -- it is a second panel in front of the panel.
            juce::Graphics::ScopedSaveState state (g);
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi,
                                                              (float) box.getCentreX(),
                                                              (float) box.getCentreY()));
            g.setColour (on ? MiraLookAndFeel::text : MiraLookAndFeel::textFaint);
            g.setFont (laf.sansMedium (MiraLookAndFeel::textSize (10.0f)));
            g.drawText (sideTabName ((SideTab) i),
                        juce::Rectangle<int> (box.getCentreX() - box.getHeight() / 2,
                                               box.getCentreY() - box.getWidth() / 2,
                                               box.getHeight(), box.getWidth()),
                        juce::Justification::centred, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < (int) SideTab::Count; ++i)
            if (boxFor (i).contains (e.getPosition()))
            {
                current = (SideTab) i;
                if (onTab) onTab (current);
                repaint();
                return;
            }
    }

private:
    juce::Rectangle<int> boxFor (int i) const
    {
        constexpr int kTab = 86;
        return { 0, 6 + i * (kTab + 2), getWidth(), kTab };
    }

    const MiraLookAndFeel& laf;
};

// ---- MASTER --------------------------------------------------------------------------
//
// The same strip the tracks have, one size larger, on the same scale. It is the sum that
// leaves mira, so it is the one meter that can answer "is this going to clip" -- stacking
// N takes that each peak near full scale is N times full scale, and the tracks' own meters
// each say everything is fine.
class MasterStrip : public juce::Component, private juce::Timer
{
public:
    MasterStrip (const MiraLookAndFeel& lafIn, CanvasView& viewIn) : laf (lafIn), view (viewIn)
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (MiraLookAndFeel::surface);

        auto r = getLocalBounds().reduced (14, 14);
        g.setColour (MiraLookAndFeel::accent);
        g.setFont (laf.sansMedium (MiraLookAndFeel::textSize (11.0f)));
        g.drawText ("MASTER", r.removeFromTop (18), juce::Justification::centredLeft, false);
        r.removeFromTop (10);

        auto strip = r.removeFromLeft (44);
        r.removeFromLeft (36);        // the scale's own column, labelled below

        g.setColour (MiraLookAndFeel::surface.darker (0.5f));
        g.fillRoundedRectangle (strip.toFloat(), 4.0f);
        g.setColour (MiraLookAndFeel::border.withAlpha (0.5f));
        g.drawRoundedRectangle (strip.toFloat().reduced (0.5f), 4.0f, 1.0f);

        auto yFor = [&] (double db) {
            return (float) strip.getBottom() - (float) strip.getHeight() * (float) CanvasView::dbToNorm (db);
        };

        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (9.0f)));
        for (double tick : { 6.0, 0.0, -6.0, -12.0, -24.0, -40.0 })
        {
            const float y = yFor (tick);
            g.setColour (MiraLookAndFeel::border.withAlpha (tick == 0.0 ? 0.9f : 0.45f));
            g.fillRect ((float) strip.getX() + 1.0f, y, (float) strip.getWidth() - 2.0f, 1.0f);
            g.setColour (MiraLookAndFeel::textFaint.withAlpha (tick == 0.0 ? 0.9f : 0.6f));
            g.drawText (tick > 0 ? "+" + juce::String ((int) tick) : juce::String ((int) tick),
                        juce::Rectangle<int> (strip.getRight() + 5, (int) y - 6, 28, 12),
                        juce::Justification::centredLeft, false);
        }

        // The stereo meter, inside the same well as the fader -- one control.
        auto meterArea = juce::Rectangle<int> (strip.getX() + 3, strip.getY(), 16, strip.getHeight());
        const float barW = (meterArea.getWidth() - 3.0f) * 0.5f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float x = meterArea.getX() + 1.0f + ch * (barW + 1.0f);
            if (level[(size_t) ch] > 0.0005f)
            {
                const double db = juce::Decibels::gainToDecibels (level[(size_t) ch]);
                auto band = [&] (double lo, double hi, juce::Colour c) {
                    if (db < lo) return;
                    const float y0 = yFor (juce::jmin (hi, db)), y1 = yFor (lo);
                    if (y0 >= y1) return;
                    g.setColour (c);
                    g.fillRect (juce::Rectangle<float> (x, y0, barW, y1 - y0));
                };
                band (-60.0, -6.0, MiraLookAndFeel::active);
                band (-6.0, -1.0, MiraLookAndFeel::accent);
                band (-1.0, 6.0, MiraLookAndFeel::warn);
            }
            if (hold[(size_t) ch] > 0.0005f)
            {
                const double db = juce::Decibels::gainToDecibels (hold[(size_t) ch]);
                g.setColour (db > -1.0 ? MiraLookAndFeel::warn : MiraLookAndFeel::text.withAlpha (0.85f));
                g.fillRect (x, yFor (db) - 1.0f, barW, 1.5f);
            }
        }

        // The fader groove and its cap, spanning the whole well.
        auto groove = juce::Rectangle<float> ((float) strip.getX() + 26.0f, (float) strip.getY(),
                                               3.0f, (float) strip.getHeight());
        g.setColour (MiraLookAndFeel::surface.darker (0.35f));
        g.fillRoundedRectangle (groove, 1.5f);
        const float capY = yFor (db());
        g.setColour (MiraLookAndFeel::accent.withAlpha (0.75f));
        g.fillRoundedRectangle (groove.withTop (capY), 1.5f);
        auto cap = juce::Rectangle<float> ((float) strip.getX() + 1.0f, capY - 5.0f,
                                            (float) strip.getWidth() - 2.0f, 10.0f);
        g.setColour (MiraLookAndFeel::accent.brighter (0.2f));
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (MiraLookAndFeel::surface.darker (0.6f));
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);
        g.fillRect (cap.getX() + 3.0f, cap.getCentreY() - 0.5f, cap.getWidth() - 6.0f, 1.0f);

        // The numbers, beside the strip. A master with no readout is a control you cannot
        // put back where it was.
        auto right = r.withHeight (22);
        g.setColour (MiraLookAndFeel::text);
        g.setFont (laf.sansMedium (MiraLookAndFeel::textSize (15.0f)));
        g.drawText (db() <= -60.0 ? juce::String ("-inf") : juce::String (db(), 1) + " dB",
                    right, juce::Justification::topLeft, false);

        const float peakDb = juce::Decibels::gainToDecibels (juce::jmax (hold[0], hold[1], 1.0e-6f));
        g.setColour (clipped ? MiraLookAndFeel::warn : MiraLookAndFeel::textDim);
        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (10.5f)));
        g.drawText (clipped ? "CLIP " + juce::String (peakDb, 1) : "peak " + juce::String (peakDb, 1) + " dB",
                    right.withY (right.getBottom() + 2).withHeight (16),
                    juce::Justification::topLeft, false);

        g.setColour (MiraLookAndFeel::textFaint);
        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (9.5f)));
        g.drawFittedText ("drag to set\ncmd-click unity\ndouble-click clears clip",
                          right.withY (right.getBottom() + 26).withHeight (54),
                          juce::Justification::topLeft, 3);
        faderBox = strip;
    }

    void mouseDown (const juce::MouseEvent& e) override { drag (e); }
    void mouseDrag (const juce::MouseEvent& e) override { drag (e); }
    void mouseDoubleClick (const juce::MouseEvent&) override { clipped = false; repaint(); }

private:
    void drag (const juce::MouseEvent& e)
    {
        if (faderBox.isEmpty() || !faderBox.expanded (8, 4).contains (e.getPosition())) return;
        if (e.mods.isCommandDown()) { setDb (0.0); return; }
        setDb (CanvasView::normToDb (1.0 - (double) (e.y - faderBox.getY())
                                              / (double) faderBox.getHeight()));
    }

    double db() const
    {
        const float g = view.getMasterGain();
        return g <= 0.0f ? -60.0 : juce::jlimit (-60.0, 6.0, (double) juce::Decibels::gainToDecibels (g));
    }

    void setDb (double d)
    {
        d = juce::jlimit (-60.0, 6.0, d);
        view.setMasterGain (d <= -60.0 ? 0.0f : juce::Decibels::decibelsToGain ((float) d));
        repaint();
    }

    void timerCallback() override
    {
        bool moved = false;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float hit = view.isPlaying() ? view.readAndClearPeak (ch) : 0.0f;
            auto& l = level[(size_t) ch];
            auto& h = hold[(size_t) ch];
            const float wasL = l, wasH = h;
            l = hit > l ? hit : l * 0.80f;
            h = hit > h ? hit : h * 0.995f;
            if (hit >= 0.999f) clipped = true;
            if (std::abs (l - wasL) > 0.0005f || std::abs (h - wasH) > 0.0005f) moved = true;
        }
        if (moved) repaint();
    }

    const MiraLookAndFeel& laf;
    CanvasView& view;
    juce::Rectangle<int> faderBox;
    std::array<float, 2> level { 0.0f, 0.0f }, hold { 0.0f, 0.0f };
    bool clipped = false;
};

// ---- FILES ----------------------------------------------------------------------------
//
// Every wav the project holds, across every block, in one list. The block folders ARE the
// pool -- there is no separate library to keep in step with them -- so this is a view of
// the filesystem rather than a second index that can disagree with it.
class FilesPanel : public juce::Component,
                   private juce::ListBoxModel,
                   private juce::Timer
{
public:
    FilesPanel (const MiraLookAndFeel& lafIn, CanvasView& viewIn,
                juce::AudioFormatManager& formatsIn)
        : laf (lafIn), view (viewIn), formats (formatsIn)
    {
        list.setModel (this);
        list.setRowHeight (34);
        list.setColour (juce::ListBox::backgroundColourId, MiraLookAndFeel::surface);
        addAndMakeVisible (list);
        startTimer (1500);      // the folder changes underneath us every generation
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        header = r.removeFromTop (34);
        list.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (MiraLookAndFeel::surface);
        g.setColour (MiraLookAndFeel::accent);
        g.setFont (laf.sansMedium (MiraLookAndFeel::textSize (11.0f)));
        g.drawText ("FILES", header.removeFromTop (16), juce::Justification::centredLeft, false);
        g.setColour (MiraLookAndFeel::textFaint);
        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (10.0f)));
        g.drawText (juce::String (rows.size()) + " takes in this project"
                     + juce::String (" - double-click to place on a new track"),
                    header, juce::Justification::centredLeft, false);
    }

    void refresh()
    {
        rows.clear();
        const auto root = view.getProjectFolder();
        if (root.isDirectory())
            for (const auto& f : root.findChildFiles (juce::File::findFiles, true, "*.wav"))
            {
                Row r;
                r.file = f;
                // The BLOCK it belongs to, which is its parent folder. That is the only
                // grouping this list needs, and it costs nothing to read.
                r.block = f.getParentDirectory() == root ? juce::String ("-")
                                                         : f.getParentDirectory().getFileName();
                rows.push_back (r);
            }
        std::sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b) {
            return a.file.getLastModificationTime() > b.file.getLastModificationTime();
        });
        list.updateContent();
        repaint();
    }

private:
    struct Row { juce::File file; juce::String block; };

    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        const auto& r = rows[(size_t) row];
        if (selected) { g.setColour (MiraLookAndFeel::surface3); g.fillRect (0, 0, w, h); }

        g.setColour (MiraLookAndFeel::text);
        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (11.0f)));
        g.drawText (r.file.getFileNameWithoutExtension(),
                    juce::Rectangle<int> (8, 2, w - 16, 16), juce::Justification::centredLeft, true);

        g.setColour (MiraLookAndFeel::textFaint);
        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (9.5f)));
        g.drawText (r.block + "   " + juce::File::descriptionOfSizeInBytes (r.file.getSize()),
                    juce::Rectangle<int> (8, 17, w - 16, 14), juce::Justification::centredLeft, true);
        g.setColour (MiraLookAndFeel::border.withAlpha (0.4f));
        g.fillRect (0, h - 1, w, 1);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        // Placed the way a dropped file is: its own block, on its own track, at the
        // playhead. One rule for how audio arrives on the canvas.
        view.addFiles ({ rows[(size_t) row].file }, juce::jmax (0.0, view.getPositionSeconds()), 0);
    }

    juce::var getDragSourceDescription (const juce::SparseSet<int>& selectedRows) override
    {
        if (selectedRows.isEmpty()) return {};
        const int row = selectedRows[0];
        if (row < 0 || row >= (int) rows.size()) return {};
        return rows[(size_t) row].file.getFullPathName();
    }

    void timerCallback() override
    {
        // Polled rather than watched: a generation lands from another thread and a
        // directory watcher for one folder that changes every few minutes is more moving
        // parts than the problem has.
        const auto root = view.getProjectFolder();
        const auto stamp = root.isDirectory() ? root.getLastModificationTime() : juce::Time();
        if (stamp != lastStamp || (int) rows.size() == 0) { lastStamp = stamp; refresh(); }
    }

    const MiraLookAndFeel& laf;
    CanvasView& view;
    juce::AudioFormatManager& formats;
    juce::ListBox list;
    std::vector<Row> rows;
    juce::Rectangle<int> header;
    juce::Time lastStamp;
};

struct CanvasWindow::Content : juce::Component, private juce::Timer
{
    Content(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
            juce::AudioThumbnailCache& cache, GenerateContent* panelIn)
        : view(laf, formats, cache), panel(panelIn), tabs(laf)
    {
        auto button = [this](juce::TextButton& b, const juce::String& text) {
            b.setButtonText(text);
            addAndMakeVisible(b);
        };
        button(playButton, "Play");
        button(loopButton, "Loop selection");
        button(fitButton, "Fit");
        button(deleteButton, "Remove");

        playButton.onClick   = [this] { view.togglePlay(); };
        loopButton.onClick   = [this] { view.toggleLoop(); };
        fitButton.onClick    = [this] { view.fit(); };
        deleteButton.onClick = [this] { view.removeSelected(); };

        hint.setText("space play - L loop - M/S mute solo - F fit - G/H zoom - [ ] wave height - cmd-E cut - cmd-Z undo",
                      juce::dontSendNotification);
        hint.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
        hint.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        addAndMakeVisible(hint);

        button(newProjectButton, "New");
        button(openProjectButton, "Open...");
        button(saveProjectButton, "Save");
        button(addBlockButton, "+ Block");
        button(duplicateButton, "Duplicate");
        duplicateButton.onClick = [this] { view.duplicateSelection(); };

        // Extend and Remix used to live here. They are a BLOCK's actions, driven by the
        // block's own prompt, so they belong in the block's generator -- on the toolbar
        // they read as something the canvas does.
        if (panel != nullptr)
        {
            panel->onExtend = [this] { view.extendSelection(false); };
            panel->onRemix  = [this] { view.extendSelection(true);  };
        }
        button(addTrackButton, "+ Track");
        addAndMakeVisible(tabs);
        master = std::make_unique<MasterStrip>(laf, view);
        files  = std::make_unique<FilesPanel>(laf, view, formats);
        addChildComponent(*master);
        addChildComponent(*files);
        tabs.onTab = [this](SideTab t) {
            tab = t;
            // Switching tabs OPENS the panel. A tab you can click while the panel is
            // folded that then does nothing visible is a control that lies.
            if (panelCollapsed)
            {
                panelCollapsed = false;
                panelToggle.setButtonText("Panel >");
            }
            applyTab();
            resized();
        };

        button(panelToggle, "Panel >");
        panelToggle.onClick = [this] {
            panelCollapsed = !panelCollapsed;
            panelToggle.setButtonText(panelCollapsed ? "Panel <" : "Panel >");
            applyTab();
            blockLabel.setVisible(!panelCollapsed && tab == SideTab::Generate);
            resized();
        };
        newProjectButton.onClick  = [this] { promptNewProject(); };
        openProjectButton.onClick = [this] { promptOpenProject(); };
        saveProjectButton.onClick = [this] { saveOrAsk(); };
        view.onDocumentChanged = [this] { updateTitle(); };
        addBlockButton.onClick   = [this] { view.addEmptyBlock(); };
        addTrackButton.onClick   = [this] { view.addLane(); };

        // The generate pane, IN the side panel, in column mode: controls over takes, no
        // project tree and no second takes column. The canvas already is the arrangement
        // view, so those would be three views of the same project fighting over 340px.
        if (panel != nullptr)
        {
            panel->setPanelOnly(true);
            panel->setTrainingBenchVisible(false);
            // Starts with nothing selected, so it never opens pointed at the default
            // ~/Music/mira-generated it was constructed with.
            panel->setNoTarget();
            addAndMakeVisible(panel);
        }
        // Selecting a block points the panel at that block's folder. Nothing opens, moves
        // or floats -- the panel is always there and always about whatever is selected,
        // which is what "feels united" means in practice.
        view.onRevealGenerator = [this] {
            // Double-clicking a block is asking for its GENERATOR, so the tab follows.
            tabs.current = tab = SideTab::Generate;
            if (!panelCollapsed) { applyTab(); resized(); return; }
            panelCollapsed = false;
            panelToggle.setButtonText("Panel >");
            if (panel != nullptr) panel->setVisible(true);
            blockLabel.setVisible(true);
            resized();
        };
        view.onBlockGeometry = [this](double lengthSeconds, double tailSeconds, bool hasAudio) {
            // The block's length IS the duration. Resizing the frame is how you ask for a
            // different length, rather than typing a number somewhere else on screen.
            if (panel != nullptr) panel->setDuration(lengthSeconds);
            // Extend needs somewhere to put the audio; remix only needs audio to remix.
            if (panel != nullptr)
                panel->setBlockActions(tailSeconds > 0.05, hasAudio && lengthSeconds > 0.0);
        };
        view.onExtendRequested = [this](const juce::File& take, double rangeStart,
                                         double totalSeconds, bool remix) {
            if (panel == nullptr) return;
            // Neither one touches the prompt. What is on screen is what runs.
            if (remix) panel->generateRemix(take, totalSeconds);
            else       panel->generateExtension(take, rangeStart, totalSeconds);
        };
        view.onCaptureSettings = [this]() -> juce::var {
            return panel != nullptr ? panel->captureSettings() : juce::var();
        };
        view.onOpenGenerator = [this](const juce::String& name, const juce::File& folder,
                                       const juce::var& settings) {
            if (panel == nullptr) return;
            // The same label the block carries on the canvas, so the panel and the track
            // cannot disagree about which block you are editing.
            blockLabel.setText(name.isEmpty() ? "no block selected" : name, juce::dontSendNotification);
            currentBlockFolder = folder;
            // No block means no target: an empty panel that says "select a block" rather
            // than 62 takes from a folder you never chose.
            if (folder != juce::File()) panel->setOutputFolder(folder);
            // The label already says which block and why; the status says what to do.
            else if (name.isNotEmpty())  panel->setNoTarget("New or Save the canvas first "
                                                             "- a block needs a project to generate into");
            else                         panel->setNoTarget();
            // The block's own recipe. Without this the panel changed its title and
            // nothing else, so every block appeared to share one prompt and one LoRA set.
            if (!settings.isVoid()) panel->applySettings(settings);
        };

        view.onStateChanged = [this] {
            playButton.setButtonText(view.isPlaying() ? "Stop" : "Play");
            loopButton.setToggleState(view.isLooping(), juce::dontSendNotification);
        };
        // A clock, and while the experiment is young an instrument: position over length
        // says in one glance whether a transport that reports "playing" is actually being
        // pulled by the device, which a playhead sitting at 0:00 cannot distinguish from a
        // paint bug.
        clock.setFont(laf.monoRegular(MiraLookAndFeel::textSize(11.0f)));
        clock.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
        clock.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(clock);
        meter.setFont(laf.monoRegular(MiraLookAndFeel::textSize(11.0f)));
        meter.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(meter);
        blockLabel.setText("no block selected", juce::dontSendNotification);
        blockLabel.setFont(laf.sansMedium(MiraLookAndFeel::textSize(12.0f)));
        blockLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::accent);
        addAndMakeVisible(blockLabel);
        startTimerHz(10);
        addAndMakeVisible(view);
    }

    // The title says the document and whether it has unsaved changes, the way every DAW
    // does. Without it a window called "Canvas (experimental)" is the same window whatever
    // you have open, which is how you save over the wrong project.
    void updateTitle()
    {
        const juce::String t = "Canvas - " + view.getDocumentName() + (view.isDirty() ? " *" : "");
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->setName(t);
        saveProjectButton.setEnabled(view.isDirty() || view.getDocumentFile() == juce::File());
    }

    bool saveOrAsk()
    {
        if (view.saveDocument()) return true;
        promptSaveAs();
        return false;
    }

    void promptSaveAs()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Save project as", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            juce::String("*") + mira::canvas::CanvasView::kExtension);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, chooser](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            view.saveDocumentAs(f);
            updateTitle();
        });
    }

    void promptOpenProject()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Open a mira project", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            juce::String("*") + mira::canvas::CanvasView::kExtension);
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                              [this, chooser](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            view.openDocument(f);
            updateTitle();
        });
    }

    void promptNewProject()
    {
        // A NAME, then a place to put it -- which is what makes a project a document
        // rather than a folder someone pointed at. The folder is created for you, and the
        // .mira lands inside it with the block folders as its siblings.
        auto* w = new juce::AlertWindow("New project", "Name this project.",
                                         juce::AlertWindow::NoIcon, this);
        w->addTextEditor("name", "Untitled", "Name");
        w->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result) {
            const auto name = w->getTextEditorContents("name").trim();
            delete w;
            if (result != 1 || name.isEmpty()) return;

            auto chooser = std::make_shared<juce::FileChooser>(
                "Where should \"" + name + "\" live?",
                juce::File::getSpecialLocation(juce::File::userMusicDirectory));
            chooser->launchAsync(juce::FileBrowserComponent::openMode
                                     | juce::FileBrowserComponent::canSelectDirectories,
                                  [this, chooser, name](const juce::FileChooser& fc) {
                const auto parent = fc.getResult();
                if (parent == juce::File()) return;
                if (!view.newDocument(parent, name))
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "New project",
                        "Could not create the project folder in " + parent.getFullPathName());
                updateTitle();
            });
        }), false);
    }

    void resized() override
    {
        auto r = getLocalBounds();

        // THE TOOLBAR LIVES IN THE TITLE BAR. The window's content runs under it, so the
        // row that sat below the traffic lights is now beside them -- the unified strip
        // every native macOS app has, and a whole row of canvas back.
        //
        // Measured from the window, not assumed: the bar's height and how far the traffic
        // lights reach are both AppKit's to decide, and a hardcoded inset is a button
        // hiding under a close button on the first machine that disagrees.
        titleBarHeight = mira_ui::chrome::useFullSizeContentView(*this);
        titleInset     = mira_ui::chrome::trafficLightInset(*this);

        const int barHeight = juce::jmax(34, titleBarHeight);
        auto bar = r.removeFromTop(barHeight).reduced(8, 5);
        if (titleInset > 0) bar.removeFromLeft(juce::jmax(0, titleInset - 8));
        playButton.setBounds(bar.removeFromLeft(70));
        bar.removeFromLeft(6);
        loopButton.setBounds(bar.removeFromLeft(110));
        bar.removeFromLeft(6);
        fitButton.setBounds(bar.removeFromLeft(54));
        bar.removeFromLeft(6);
        deleteButton.setBounds(bar.removeFromLeft(80));
        bar.removeFromLeft(6);
        addTrackButton.setBounds(bar.removeFromLeft(74));
        bar.removeFromLeft(6);
        addBlockButton.setBounds(bar.removeFromLeft(74));
        bar.removeFromLeft(6);
        duplicateButton.setBounds(bar.removeFromLeft(86));
        bar.removeFromLeft(6);
        panelToggle.setBounds(bar.removeFromRight(96));
        bar.removeFromRight(8);
        newProjectButton.setBounds(bar.removeFromLeft(56));
        bar.removeFromLeft(4);
        openProjectButton.setBounds(bar.removeFromLeft(70));
        bar.removeFromLeft(4);
        saveProjectButton.setBounds(bar.removeFromLeft(56));
        bar.removeFromLeft(12);
        clock.setBounds(bar.removeFromRight(250));
        bar.removeFromRight(8);
        meter.setBounds(bar.removeFromRight(72));
        bar.removeFromRight(4);
        masterMeter = bar.removeFromRight(120).withSizeKeepingCentre(120, 10);
        hint.setBounds(bar);

        if (!panelCollapsed)
        {
            // EACH TAB ASKS FOR WHAT IT NEEDS. The master strip is a fader, a meter and
            // two numbers; giving it a third of the window because the generator wants one
            // is a third of the window spent on empty panel.
            const int wanted = tab == SideTab::Master
                                   ? 228
                               : tab == SideTab::Files
                                   ? juce::jmax(300, juce::roundToInt(r.getWidth() * panelFraction * 0.8))
                                   : juce::roundToInt(r.getWidth() * panelFraction);
            const int floorW = tab == SideTab::Master ? 200 : 300;
            auto side = r.removeFromRight(juce::jlimit(floorW,
                                                        juce::jmax(floorW + 20, r.getWidth() - 320),
                                                        wanted + kTabStripWidth));
            // The tab strip is part of the panel and sits on its INSIDE edge, against the
            // canvas -- so the tabs are next to the thing they change.
            tabs.setBounds(side.removeFromLeft(kTabStripWidth));
            if (tab == SideTab::Generate)
            {
                blockLabel.setBounds(side.removeFromTop(22).reduced(10, 0));
                if (panel != nullptr) panel->setBounds(side);
            }
            else if (tab == SideTab::Master) master->setBounds(side);
            else                             files->setBounds(side);
            sideDivider = r.removeFromRight(6);
        }
        else { tabs.setBounds({}); sideDivider = {}; }
        view.setBounds(r);
    }

    // One place that decides what the panel column is showing. Three setVisible calls in
    // three different handlers is how a panel ends up with two tools drawn over each other.
    void applyTab()
    {
        const bool open = !panelCollapsed;
        if (panel != nullptr) panel->setVisible(open && tab == SideTab::Generate);
        blockLabel.setVisible(open && tab == SideTab::Generate);
        master->setVisible(open && tab == SideTab::Master);
        files->setVisible(open && tab == SideTab::Files);
        tabs.setVisible(open);
        if (open && tab == SideTab::Files) files->refresh();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(MiraLookAndFeel::surface2);
        // The master meter, beside the number. Green up to -6, amber to -1, red over --
        // the same reading every console gives you, so the colour alone answers "am I
        // anywhere near clipping" without doing arithmetic on a decibel.
        if (!masterMeter.isEmpty())
        {
            g.setColour(MiraLookAndFeel::surface3);
            g.fillRoundedRectangle(masterMeter.toFloat(), 3.0f);
            if (held > 0.0005f)
            {
                const float db = juce::Decibels::gainToDecibels(held);
                const float frac = juce::jlimit(0.0f, 1.0f, (db + 48.0f) / 48.0f);
                g.setColour(db > -1.0f ? MiraLookAndFeel::warn
                                       : (db > -6.0f ? MiraLookAndFeel::accent
                                                     : MiraLookAndFeel::active));
                g.fillRoundedRectangle(masterMeter.toFloat().withWidth(
                    juce::jmax(3.0f, masterMeter.getWidth() * frac)), 3.0f);
            }
            // -6 dBFS marked, which is where you start caring.
            g.setColour(MiraLookAndFeel::text.withAlpha(0.3f));
            g.drawVerticalLine(masterMeter.getX() + juce::roundToInt(masterMeter.getWidth() * (42.0f / 48.0f)),
                                (float) masterMeter.getY(), (float) masterMeter.getBottom());
        }

        if (!sideDivider.isEmpty())
        {
            g.setColour(MiraLookAndFeel::border);
            const int cx = sideDivider.getCentreX(), cy = sideDivider.getCentreY();
            for (int i = -1; i <= 1; ++i)
                g.fillRect(cx - 1, cy + i * 10 - 6, 2, 12);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        setMouseCursor(sideDivider.expanded(3, 0).contains(e.getPosition())
                           ? juce::MouseCursor::LeftRightResizeCursor
                           : juce::MouseCursor::NormalCursor);
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        draggingSide = sideDivider.expanded(3, 0).contains(e.getPosition());
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!draggingSide) return;
        panelFraction = juce::jlimit(0.18, 0.6, (double) (getWidth() - e.x) / juce::jmax(1, getWidth()));
        resized();
    }
    void mouseUp(const juce::MouseEvent&) override { draggingSide = false; }

    void timerCallback() override
    {
        // The peak matters MORE here than in a normal DAW. Stacking alternates means N
        // takes that each peak near full scale summing into one bus: two is +6 dB over,
        // four is +12. Nothing normalises that, so the number has to be on screen or the
        // first thing the canvas teaches you is that it distorts.
        // Only while the TRANSPORT is playing. A BufferingAudioSource fills its read-ahead
        // buffer whether or not the transport is running, so the source kept producing
        // peaks with nothing playing and the readout sat at some arbitrary level forever.
        // Stopped means silence, and the meter has to say so.
        const float p = view.isPlaying() ? view.readAndClearPeak() : 0.0f;
        if (!view.isPlaying()) held = 0.0f;
        if (p > held) held = p;
        held *= 0.92f;                                     // a slow fall, so a hit is readable
        const float dB = juce::Decibels::gainToDecibels(juce::jmax(held, 1.0e-6f));

        // The rates, said out loud. The timeline is 44,100 because that is all SA3
        // generates; the interface is whatever you set it to, and when they differ the
        // transport resamples the whole mix. That resample is the honest explanation for
        // "the canvas sounds different from the preview", and hiding it made the
        // difference feel like a fault rather than a conversion.
        const double devRate = view.getDeviceRate();
        auto khz = [](double r) { return juce::String(r / 1000.0, 1) + "k"; };
        const auto rates = devRate <= 0.0
                               ? juce::String("no device")
                               : (std::abs(devRate - CanvasView::getTimelineRate()) < 1.0
                                      ? khz(devRate)
                                      : khz(CanvasView::getTimelineRate()) + " -> " + khz(devRate));

        clock.setText(formatTime(view.getPositionSeconds()) + " / " + formatTime(view.getLengthSeconds())
                       + "   " + juce::String(view.getBlockCount()) + " blocks   " + rates,
                      juce::dontSendNotification);
        meter.setText(held > 1.0f ? "CLIP +" + juce::String(dB, 1) + " dB"
                                  : juce::String(dB, 1) + " dB",
                      juce::dontSendNotification);
        meter.setColour(juce::Label::textColourId,
                        held > 1.0f ? MiraLookAndFeel::warn : MiraLookAndFeel::textFaint);
        // The labels are child components and repaint themselves; the master meter is
        // drawn by THIS component's paint, which nothing was asking to run again. The
        // number moved and the bar never did.
        repaint(masterMeter.expanded(2, 2));
    }

    float held = 0.0f;

    CanvasView view;
    GenerateContent* panel = nullptr;      // owned by the window, not by this
    // How much of the window the side panel takes. Dragged, not fixed -- a panel that is
    // always a third is wrong at 1100px and wrong again at 2400.
    double panelFraction = 0.30;
    juce::Rectangle<int> sideDivider;
    bool draggingSide = false;
    juce::File currentBlockFolder;
    juce::Label blockLabel;
    juce::TextButton playButton, loopButton, fitButton, deleteButton,
                     addBlockButton, addTrackButton, duplicateButton,
                     newProjectButton, openProjectButton, saveProjectButton, panelToggle;
    juce::Rectangle<int> masterMeter;
    bool panelCollapsed = false;
    juce::Label hint, clock, meter;
    int titleBarHeight = 0, titleInset = 0;
    // The side panel is one column with a tab strip, not a stack of panes fighting for
    // height. GENERATE is the block's generator; MASTER is the sum; FILES is every take
    // the project holds. Adding the next tool is an enum row and a component.
    TabStrip tabs;
    std::unique_ptr<MasterStrip> master;
    std::unique_ptr<FilesPanel> files;
    SideTab tab = SideTab::Generate;
    static constexpr int kTabStripWidth = 26;
};

CanvasWindow::CanvasWindow(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
                           juce::AudioThumbnailCache& cache, GenerateContent* panel)
    : juce::DocumentWindow("Canvas", MiraLookAndFeel::surface,
                            juce::DocumentWindow::closeButton)
{
    content = std::make_unique<Content>(laf, formats, cache, panel);
    view = &content->view;
    setUsingNativeTitleBar(true);
    setContentNonOwned(content.get(), false);
    setResizable(true, false);
    centreWithSize(1100, 640);
    setVisible(true);
}

CanvasWindow::~CanvasWindow() = default;

} // namespace mira::canvas
