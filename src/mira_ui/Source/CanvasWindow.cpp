#include "CanvasWindow.h"

namespace mira::canvas {

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
    }
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

juce::Rectangle<int> CanvasView::muteBoxFor(int lane) const
{
    return { 8, laneToY(lane) + laneHeight / 2 - 9, 22, 18 };
}

juce::Rectangle<int> CanvasView::soloBoxFor(int lane) const
{
    return { 34, laneToY(lane) + laneHeight / 2 - 9, 22, 18 };
}

juce::Rectangle<int> CanvasView::faderBoxFor(int lane) const
{
    // Only while the lane is tall enough to hold one. A fader squeezed into 28px would be
    // a control you cannot aim at, which is worse than a control that is not there.
    if (laneHeight < 44) return {};
    return { 8, laneToY(lane) + laneHeight - 18, kHeaderWidth - 30, 8 };
}

juce::Rectangle<int> CanvasView::meterBoxFor(int lane) const
{
    // VERTICAL, at the right edge of the header, running the lane's full height. A
    // horizontal meter stacked above a horizontal fader read as two faders, one of which
    // moved on its own -- and neither lined up with the other.
    return { kHeaderWidth - 14, laneToY(lane) + 5, 6, juce::jmax(10, laneHeight - 14) };
}

juce::Rectangle<int> CanvasView::nameBoxFor(int lane) const
{
    return laneHeight >= 44
               ? juce::Rectangle<int>(60, laneToY(lane) + 4, kHeaderWidth - 66, laneHeight / 2)
               : juce::Rectangle<int>(60, laneToY(lane), kHeaderWidth - 66, laneHeight);
}

void CanvasView::beginRename(int lane)
{
    commitRename();
    renamingLane = lane;
    renameEditor = std::make_unique<juce::TextEditor>();
    renameEditor->setText(laneNames[lane], juce::dontSendNotification);
    renameEditor->setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.0f)));
    renameEditor->setBounds(nameBoxFor(lane).withHeight(20));
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
    laneNames.set(renamingLane, renameEditor->getText().trim());
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

    // Double-click a block to open its generator. Single click selects and drags, which
    // is what you do ninety times for every once you want the settings.
    Drag what = Drag::None;
    if (auto* hit = hitTest(e.getPosition(), what); hit != nullptr && onOpenGenerator)
    {
        auto folder = blockFolderFor(*hit);
        if (folder != juce::File()) onOpenGenerator(hit->block.name, folder);
    }
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
            // "block length will be defined by what is generated": a fresh take sets the
            // frame's length. A block you have already trimmed keeps its trim -- redoing
            // that every time you audition another take would be maddening.
            const double len = reader->lengthInSamples / reader->sampleRate;
            if (v.block.length <= 0.0 || !v.block.hasAudio() || v.block.sourceOffset <= 0.0)
                v.block.length = len;
            v.block.length = juce::jmin(v.block.length, len);
        }
        v.thumb = std::make_unique<juce::AudioThumbnail>(512, formats, cache);
        v.thumb->setSource(new juce::FileInputSource(f));
    }
}

void CanvasView::addLane()
{
    if (laneCount >= CanvasAudioSource::kMaxLanes) return;
    ++laneCount;
    if (laneNames[laneCount - 1].isEmpty()) laneNames.set(laneCount - 1, "track " + juce::String(laneCount));
    markDirty();
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
    constexpr double kNewBlockSeconds = 8.0;
    int lane = 0;
    for (; lane < laneCount; ++lane)
    {
        bool clash = false;
        for (const auto& i : items)
            if (i->block.lane == lane && i->block.start < at + kNewBlockSeconds && i->block.end() > at) clash = true;
        if (!clash) break;
    }
    // Every existing track is busy here, so make one -- rather than dropping the block on
    // a track that does not exist yet and leaving a gap in the numbering.
    if (lane >= laneCount) { addLane(); lane = laneCount - 1; }

    auto v = std::make_unique<Visual>();
    v->block.lane = lane;
    v->block.start = at;
    v->block.length = kNewBlockSeconds;
    v->block.id = nextId++;
    v->block.name = "block " + juce::String(items.size() + 1);
    if (laneNames[lane].isEmpty()) laneNames.set(lane, "track " + juce::String(lane + 1));
    laneCount = juce::jmax(laneCount, lane + 1);
    selected.clear();
    selected.insert(v->block.id);
    items.push_back(std::move(v));
    announceSelection();
    markDirty();
    repaint();
}

void CanvasView::duplicateSelection()
{
    if (selected.empty()) return;

    // Placed after the rightmost of what was copied, on the same lanes, so a duplicate
    // lands where you would have dragged it rather than on top of the original.
    double rightmost = 0.0;
    double leftmost = 1e12;
    for (const auto& i : items)
        if (selected.count(i->block.id))
        { rightmost = juce::jmax(rightmost, i->block.end()); leftmost = juce::jmin(leftmost, i->block.start); }
    const double shift = juce::jmax(0.25, rightmost - leftmost);

    std::vector<std::unique_ptr<Visual>> copies;
    for (const auto& i : items)
    {
        if (selected.count(i->block.id) == 0) continue;
        auto v = std::make_unique<Visual>();
        v->block = i->block;                 // trim, fades, gain, lane, name all come too
        v->block.id = nextId++;
        v->block.start = i->block.start + shift;
        v->settings = i->settings;
        // The SAME take, not a new one. A duplicate that regenerated would be a different
        // piece of audio wearing the same name.
        setFileOn(*v, i->block.file);
        v->block.length = i->block.length;   // setFileOn may have reset it to the file's
        v->block.sourceOffset = i->block.sourceOffset;
        copies.push_back(std::move(v));
    }

    selected.clear();
    for (auto& c : copies) { selected.insert(c->block.id); items.push_back(std::move(c)); }
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
        setFileOn(*v, take);
        rebuildAudio();
        markDirty();
        repaint();
    }
}

void CanvasView::announceSelection() {}

void CanvasView::adoptTake(const juce::File& folder, const juce::File& take)
{
    for (auto& i : items)
        if (blockFolderFor(*i) == folder)
        {
            setFileOn(*i, take);
            rebuildAudio();
            markDirty();
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
    const auto base = miraFile.getParentDirectory();
    juce::Array<juce::var> blocks;
    for (const auto& i : items)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("name", i->block.name);
        o->setProperty("lane", i->block.lane);
        o->setProperty("start", i->block.start);
        o->setProperty("length", i->block.length);
        o->setProperty("offset", i->block.sourceOffset);
        o->setProperty("fadeIn", i->block.fadeIn);
        o->setProperty("fadeOut", i->block.fadeOut);
        o->setProperty("gainDb", i->block.gainDb);
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
    root->setProperty("muteMask", juce::String(muteMask));
    miraFile.replaceWithText(juce::JSON::toString(juce::var(root), false));
}

bool CanvasView::readFrom(const juce::File& miraFile)
{
    items.clear();
    selected.clear();
    laneNames.clear();
    laneDb.clear();
    muteMask = soloMask = 0;
    laneCount = 1;

    const auto root = juce::JSON::parse(miraFile.loadFileAsString());
    if (!root.isObject()) return false;
    const auto base = miraFile.getParentDirectory();

    if (auto* names = root.getProperty("laneNames", {}).getArray())
        for (int i = 0; i < names->size(); ++i) laneNames.set(i, (*names)[i].toString());
    if (auto* gains = root.getProperty("laneGainDb", {}).getArray())
        for (int i = 0; i < gains->size(); ++i) setLaneDb(i, (double) (*gains)[i]);
    muteMask = (juce::uint64) root.getProperty("muteMask", "0").toString().getLargeIntValue();
    laneCount = juce::jlimit(1, CanvasAudioSource::kMaxLanes, (int) root.getProperty("laneCount", 1));

    if (auto* blocks = root.getProperty("blocks", {}).getArray())
        for (const auto& b : *blocks)
        {
            auto v = std::make_unique<Visual>();
            v->block.name = b.getProperty("name", "block").toString();
            v->block.lane = (int) b.getProperty("lane", 0);
            v->block.start = (double) b.getProperty("start", 0.0);
            v->block.length = (double) b.getProperty("length", 8.0);
            v->block.sourceOffset = (double) b.getProperty("offset", 0.0);
            v->block.fadeIn = (double) b.getProperty("fadeIn", 0.0);
            v->block.fadeOut = (double) b.getProperty("fadeOut", 0.0);
            v->block.gainDb = (double) b.getProperty("gainDb", 0.0);
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
    if (!items.empty()) fit();
    rebuildAudio();
    repaint();
    return true;
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
        auto r = juce::Rectangle<int>(0, laneToY(lane), getWidth(), laneHeight);
        if (lane % 2 == 1) { g.setColour(MiraLookAndFeel::surface2.withAlpha(0.35f)); g.fillRect(r); }
        g.setColour(MiraLookAndFeel::border.withAlpha(0.5f));
        g.drawHorizontalLine(r.getBottom() - 1, 0.0f, static_cast<float>(getWidth()));
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

        g.setColour(MiraLookAndFeel::surface3.withAlpha(laneMuted ? 0.45f : 1.0f));
        g.fillRoundedRectangle(r.toFloat(), 5.0f);

        if (item->thumb != nullptr && item->thumb->getTotalLength() > 0.0)
        {
            // The name strip only costs height while there is height to spare; below that
            // the waveform gets all of it, which is the point of zooming in vertically.
            const int nameStrip = r.getHeight() >= 46 ? 16 : 0;
            auto wave = r.reduced(4, 3).withTrimmedTop(nameStrip);
            g.setColour(MiraLookAndFeel::text.withAlpha(laneMuted ? 0.18f
                                                                  : (isSelected ? 0.85f : 0.6f)));
            item->thumb->drawChannels(g, wave, item->block.sourceOffset,
                                       item->block.sourceOffset + item->block.length, 1.0f);
        }

        // The fades, drawn as the wedges they are, same as the take waveform does.
        if (item->block.fadeIn > 0.0 || item->block.fadeOut > 0.0)
        {
            g.setColour(MiraLookAndFeel::surface.withAlpha(0.55f));
            if (item->block.fadeIn > 0.0)
            {
                const int w = juce::roundToInt(item->block.fadeIn * pixelsPerSecond);
                juce::Path p;
                p.startNewSubPath((float) r.getX(), (float) r.getY());
                p.lineTo((float) (r.getX() + w), (float) r.getY());
                p.lineTo((float) r.getX(), (float) r.getBottom());
                p.closeSubPath();
                g.fillPath(p);
            }
            if (item->block.fadeOut > 0.0)
            {
                const int w = juce::roundToInt(item->block.fadeOut * pixelsPerSecond);
                juce::Path p;
                p.startNewSubPath((float) r.getRight(), (float) r.getY());
                p.lineTo((float) (r.getRight() - w), (float) r.getY());
                p.lineTo((float) r.getRight(), (float) r.getBottom());
                p.closeSubPath();
                g.fillPath(p);
            }
        }

        g.setColour(isSelected ? MiraLookAndFeel::accent : MiraLookAndFeel::border);
        g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 5.0f, isSelected ? 1.8f : 1.0f);

        if (r.getHeight() >= 46)
        {
            g.setColour(isSelected ? MiraLookAndFeel::text : MiraLookAndFeel::textDim);
            g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
            // "block1 _ name of the file": the block's own name AND what is in it. The
            // block name alone says nothing about which take you chose, and the filename
            // alone loses which part of the piece this is.
            const auto label = item->block.name
                             + (item->block.hasAudio()
                                    ? "  -  " + item->block.file.getFileNameWithoutExtension()
                                    : juce::String());
            g.drawText(label, r.reduced(6, 2).removeFromTop(14),
                        juce::Justification::centredLeft, true);
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

            g.setColour(muted ? MiraLookAndFeel::textFaint : MiraLookAndFeel::textDim);
            g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.0f)));
            if (lane != renamingLane)
                g.drawText(laneNames[lane].isNotEmpty() ? laneNames[lane] : juce::String(lane + 1),
                            nameBoxFor(lane), juce::Justification::centredLeft, true);

            if (auto meterBox = meterBoxFor(lane); !meterBox.isEmpty())
            {
                const float level = lane < (int) laneMeter.size() ? laneMeter[(size_t) lane] : 0.0f;
                g.setColour(MiraLookAndFeel::surface3);
                g.fillRoundedRectangle(meterBox.toFloat(), 2.5f);
                if (level > 0.0005f)
                {
                    // dBFS mapped over -48..0, which is the range you actually judge a
                    // balance in; a linear meter spends nine tenths of itself on the top
                    // 20 dB and tells you nothing about anything quiet.
                    const float db = juce::Decibels::gainToDecibels(level);
                    const float frac = juce::jlimit(0.0f, 1.0f, (db + 48.0f) / 48.0f);
                    const float h = juce::jmax(2.0f, meterBox.getHeight() * frac);
                    g.setColour(db > -1.0f ? MiraLookAndFeel::warn
                                           : (db > -6.0f ? MiraLookAndFeel::accent
                                                         : MiraLookAndFeel::active));
                    g.fillRoundedRectangle(meterBox.toFloat().withTrimmedTop(meterBox.getHeight() - h), 2.5f);
                }
            }

            if (auto fader = faderBoxFor(lane); !fader.isEmpty())
            {
                const double db = laneDbAt(lane);
                const float frac = (float) ((db + 60.0) / 66.0);
                g.setColour(MiraLookAndFeel::surface3);
                g.fillRoundedRectangle(fader.toFloat(), 3.0f);
                g.setColour(muted ? MiraLookAndFeel::textFaint : MiraLookAndFeel::accent.withAlpha(0.8f));
                g.fillRoundedRectangle(fader.toFloat().withWidth(juce::jmax(3.0f, fader.getWidth() * frac)), 3.0f);
                // Unity marked, because "where was 0 dB again" is the one question a
                // fader with no numbers has to answer at a glance.
                const int unity = fader.getX() + juce::roundToInt(fader.getWidth() * (60.0f / 66.0f));
                g.setColour(MiraLookAndFeel::text.withAlpha(0.35f));
                g.drawVerticalLine(unity, (float) fader.getY() - 1.0f, (float) fader.getBottom() + 1.0f);
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
        if (lane < CanvasAudioSource::kMaxLanes)
        {
            const juce::uint64 bit = juce::uint64 (1) << lane;
            if (muteBoxFor(lane).contains(e.getPosition())) muteMask ^= bit;
            else if (soloBoxFor(lane).contains(e.getPosition())) soloMask ^= bit;
            else if (auto fader = faderBoxFor(lane);
                     !fader.isEmpty() && fader.expanded(0, 5).contains(e.getPosition()))
            {
                faderLane = lane;
                if (e.mods.isCommandDown()) setLaneDb(lane, 0.0);   // cmd-click = unity
                else setLaneDb(lane, -60.0 + 66.0 * (e.x - fader.getX()) / (double) fader.getWidth());
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
        if (!e.mods.isShiftDown()) selected.clear();
        announceSelection();
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

    drag = what;
    dragTarget = hit->block.id;
    announceSelection();
    // Selecting a block IS opening its generator now that the panel is always on screen.
    if (onOpenGenerator)
    {
        auto folder = blockFolderFor(*hit);
        const auto label = hit->block.name
                         + (hit->block.hasAudio()
                                ? "  -  " + hit->block.file.getFileNameWithoutExtension()
                                : juce::String("  -  empty"));
        if (folder != juce::File()) onOpenGenerator(label, folder);
    }
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
        if (auto fader = faderBoxFor(faderLane); !fader.isEmpty())
            setLaneDb(faderLane, -60.0 + 66.0 * (e.x - fader.getX()) / (double) fader.getWidth());
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
            b.lane = juce::jmax(0, origin->second.second + laneShift);
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
            b.length = juce::jmax(0.05, dragOriginLength + deltaSeconds);
        }
    }

    repaint();
}

void CanvasView::mouseUp(const juce::MouseEvent&)
{
    faderLane = -1;
    const bool changed = drag == Drag::Move || drag == Drag::TrimLeft || drag == Drag::TrimRight;
    drag = Drag::None;
    marquee = {};
    if (changed) { rebuildAudio(); markDirty(); }
    repaint();
}

void CanvasView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Shift-wheel zooms VERTICALLY: taller lanes mean a taller waveform, which is the
    // only way to judge a quiet take against a loud one by eye. Separate from the
    // horizontal zoom because time and amplitude are separate questions.
    if (e.mods.isShiftDown())
    {
        laneHeight = juce::jlimit(28, 320, laneHeight + (wheel.deltaY > 0 ? 6 : -6));
        repaint();
        return;
    }
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomBy(wheel.deltaY > 0 ? 1.15 : 1.0 / 1.15, e.x);
        return;
    }
    viewStart = juce::jmax(0.0, viewStart - wheel.deltaX * 240.0 / pixelsPerSecond);
    repaint();
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
    if (key.getTextCharacter() == '=' || key.getTextCharacter() == '+')
        { laneHeight = juce::jmin(320, laneHeight + 8); repaint(); return true; }
    if (key.getTextCharacter() == '-')
        { laneHeight = juce::jmax(28, laneHeight - 8); repaint(); return true; }
    return false;
}

void CanvasView::togglePlay()
{
    if (player.isPlaying()) player.stop(); else player.play();
    if (onStateChanged) onStateChanged();
    repaint();
}

void CanvasView::removeSelected()
{
    if (selected.empty()) return;
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
    addFiles(keep, juce::jmax(0.0, xToSeconds(x)), yToLane(y));
}

void CanvasView::addFiles(const juce::Array<juce::File>& files, double atSeconds, int lane)
{
    double at = atSeconds;
    for (const auto& f : files)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor(f));
        if (reader == nullptr) continue;
        const double len = reader->sampleRate > 0.0 ? reader->lengthInSamples / reader->sampleRate : 0.0;
        if (len <= 0.0) continue;

        auto v = std::make_unique<Visual>();
        v->block.file = f;
        v->block.lane = lane;
        v->block.start = at;
        v->block.length = len;
        v->block.name = f.getFileNameWithoutExtension();
        v->block.id = nextId++;
        v->thumb = std::make_unique<juce::AudioThumbnail>(512, formats, cache);
        v->thumb->setSource(new juce::FileInputSource(f));
        // A drop below the last track MAKES that track, rather than leaving a region
        // floating on a lane with no header, no fader and no mute -- which is what
        // happened, and is why it looked like a region belonging to nothing.
        laneCount = juce::jlimit(1, CanvasAudioSource::kMaxLanes, juce::jmax(laneCount, lane + 1));
        if (laneNames[v->block.lane].isEmpty())
            laneNames.set(v->block.lane, "track " + juce::String(v->block.lane + 1));
        selected.clear();
        selected.insert(v->block.id);
        items.push_back(std::move(v));
        at += len;
    }
    markDirty();
    rebuildAudio();
    repaint();
}

void CanvasView::timerCallback()
{
    if (player.isPlaying())
    {
        const int lanes = juce::jmax(4, (getHeight() - topRuler) / laneHeight + 1);
        if ((int) laneMeter.size() < lanes) laneMeter.resize((size_t) lanes, 0.0f);
        for (int lane = 0; lane < lanes && lane < CanvasAudioSource::kMaxLanes; ++lane)
        {
            const float hit = player.readAndClearLanePeak(lane);
            auto& held = laneMeter[(size_t) lane];
            // Instant attack, slow release: a meter that falls as fast as it rises is a
            // flicker you cannot read at 30 fps.
            held = hit > held ? hit : held * 0.80f;
        }
        repaint();
        return;
    }
    // Let the meters fall to nothing after a stop rather than freezing mid-level.
    bool alive = false;
    for (auto& m : laneMeter) { if (m > 0.0005f) { m *= 0.8f; alive = true; } else m = 0.0f; }
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

struct CanvasWindow::Content : juce::Component, private juce::Timer
{
    Content(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
            juce::AudioThumbnailCache& cache, GenerateContent* panelIn)
        : view(laf, formats, cache), panel(panelIn)
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

        hint.setText("space play - L loop - M/S mute solo - F fit - alt-drag pan - cmd-wheel zoom - shift-wheel lane height",
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
        button(addTrackButton, "+ Track");
        button(panelToggle, "Generate >");
        panelToggle.onClick = [this] {
            panelCollapsed = !panelCollapsed;
            panelToggle.setButtonText(panelCollapsed ? "Generate <" : "Generate >");
            if (panel != nullptr) panel->setVisible(!panelCollapsed);
            blockLabel.setVisible(!panelCollapsed);
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
            addAndMakeVisible(panel);
        }
        // Selecting a block points the panel at that block's folder. Nothing opens, moves
        // or floats -- the panel is always there and always about whatever is selected,
        // which is what "feels united" means in practice.
        view.onOpenGenerator = [this](const juce::String& name, const juce::File& folder) {
            if (panel == nullptr) return;
            // The same label the block carries on the canvas, so the panel and the track
            // cannot disagree about which block you are editing.
            blockLabel.setText(name.isEmpty() ? "no block selected" : name, juce::dontSendNotification);
            currentBlockFolder = folder;
            if (folder != juce::File()) panel->setOutputFolder(folder);
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
        auto bar = r.removeFromTop(34).reduced(8, 5);
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
        clock.setBounds(bar.removeFromRight(180));
        bar.removeFromRight(8);
        meter.setBounds(bar.removeFromRight(72));
        bar.removeFromRight(4);
        masterMeter = bar.removeFromRight(120).withSizeKeepingCentre(120, 10);
        hint.setBounds(bar);

        if (panel != nullptr && !panelCollapsed)
        {
            const int wanted = juce::roundToInt(r.getWidth() * panelFraction);
            auto side = r.removeFromRight(juce::jlimit(260, juce::jmax(280, r.getWidth() - 320), wanted));
            blockLabel.setBounds(side.removeFromTop(22).reduced(10, 0));
            panel->setBounds(side);
            sideDivider = r.removeFromRight(6);
        }
        else sideDivider = {};
        view.setBounds(r);
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

        clock.setText(formatTime(view.getPositionSeconds()) + " / " + formatTime(view.getLengthSeconds())
                       + "   " + juce::String(view.getBlockCount()) + " blocks",
                      juce::dontSendNotification);
        meter.setText(held > 1.0f ? "CLIP +" + juce::String(dB, 1) + " dB"
                                  : juce::String(dB, 1) + " dB",
                      juce::dontSendNotification);
        meter.setColour(juce::Label::textColourId,
                        held > 1.0f ? MiraLookAndFeel::warn : MiraLookAndFeel::textFaint);
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
};

CanvasWindow::CanvasWindow(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
                           juce::AudioThumbnailCache& cache, GenerateContent* panel)
    : juce::DocumentWindow("Canvas (experimental)", MiraLookAndFeel::surface,
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
