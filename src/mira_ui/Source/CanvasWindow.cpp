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
    // A canvas project is a FOLDER with a canvas.json in it, and one subfolder per block
    // holding that block's takes. No cue/take walk any more: the canvas has no idea what a
    // cue is, which is what "forget the idea of cue and takes" actually means in code.
    commitRename();
    projectFolder = project;
    if (project.isDirectory()) project.createDirectory();
    load();
    announceSelection();
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
    return { 8, laneToY(lane) + laneHeight - 16, kHeaderWidth - 18, 8 };
}

juce::Rectangle<int> CanvasView::meterBoxFor(int lane) const
{
    if (laneHeight < 44) return {};
    return { 8, laneToY(lane) + laneHeight - 26, kHeaderWidth - 18, 6 };
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
    for (; lane < CanvasAudioSource::kMaxLanes; ++lane)
    {
        bool clash = false;
        for (const auto& i : items)
            if (i->block.lane == lane && i->block.start < at + kNewBlockSeconds && i->block.end() > at) clash = true;
        if (!clash) break;
    }

    auto v = std::make_unique<Visual>();
    v->block.lane = lane;
    v->block.start = at;
    v->block.length = kNewBlockSeconds;
    v->block.id = nextId++;
    v->block.name = "block " + juce::String(items.size() + 1);
    if (laneNames[lane].isEmpty()) laneNames.set(lane, "track " + juce::String(lane + 1));
    selected.clear();
    selected.insert(v->block.id);
    items.push_back(std::move(v));
    announceSelection();
    save();
    repaint();
}

void CanvasView::applySettingsToSelection(const juce::var& settings)
{
    if (auto* v = singleSelection()) { v->settings = settings; save(); }
}

void CanvasView::chooseTakeForSelection(const juce::File& take)
{
    if (auto* v = singleSelection())
    {
        setFileOn(*v, take);
        rebuildAudio();
        save();
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
            save();
            repaint();
            return;
        }
}

// --- persistence. One canvas.json in the project root: blocks, where they sit, and each
// block's generator. Takes are NOT listed -- they are whatever is in the block's folder,
// so a take added or removed outside mira is simply seen next time.
void CanvasView::save() const
{
    if (!projectFolder.isDirectory()) return;
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
        if (i->block.hasAudio()) o->setProperty("file", i->block.file.getFullPathName());
        if (!i->settings.isVoid()) o->setProperty("settings", i->settings);
        blocks.add(juce::var(o));
    }
    auto* root = new juce::DynamicObject();
    root->setProperty("blocks", juce::var(blocks));
    root->setProperty("lanes", juce::var(juce::Array<juce::var>()));
    juce::Array<juce::var> names;
    for (const auto& n : laneNames) names.add(n);
    root->setProperty("laneNames", juce::var(names));
    root->setProperty("muteMask", juce::String(muteMask));
    projectFolder.getChildFile("canvas.json")
        .replaceWithText(juce::JSON::toString(juce::var(root), false));
}

void CanvasView::load()
{
    items.clear();
    selected.clear();
    laneNames.clear();
    muteMask = soloMask = 0;

    const auto file = projectFolder.getChildFile("canvas.json");
    if (!file.existsAsFile()) { applyMasks(); rebuildAudio(); repaint(); return; }

    const auto root = juce::JSON::parse(file.loadFileAsString());
    if (auto* names = root.getProperty("laneNames", {}).getArray())
        for (int i = 0; i < names->size(); ++i) laneNames.set(i, (*names)[i].toString());
    muteMask = (juce::uint64) root.getProperty("muteMask", "0").toString().getLargeIntValue();

    if (auto* blocks = root.getProperty("blocks", {}).getArray())
        for (const auto& b : *blocks)
        {
            auto v = std::make_unique<Visual>();
            v->block.name = b.getProperty("name", "block").toString();
            v->block.lane = (int) b.getProperty("lane", 0);
            v->block.start = (double) b.getProperty("start", 0.0);
            v->block.length = (double) b.getProperty("length", 30.0);
            v->block.sourceOffset = (double) b.getProperty("offset", 0.0);
            v->block.fadeIn = (double) b.getProperty("fadeIn", 0.0);
            v->block.fadeOut = (double) b.getProperty("fadeOut", 0.0);
            v->block.gainDb = (double) b.getProperty("gainDb", 0.0);
            v->block.id = nextId++;
            v->settings = b.getProperty("settings", {});
            const auto path = b.getProperty("file", "").toString();
            // A take that has been moved or deleted leaves the block EMPTY rather than
            // silently vanishing: the frame and its generator are still what you meant.
            if (path.isNotEmpty()) setFileOn(*v, juce::File(path));
            items.push_back(std::move(v));
        }

    applyMasks();
    if (!items.empty()) fit();
    rebuildAudio();
    repaint();
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
    g.fillAll(MiraLookAndFeel::surface);

    const int lanes = juce::jmax(4, (getHeight() - topRuler) / laneHeight + 1);
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
            g.drawText(item->block.name.isNotEmpty() ? item->block.name
                                                      : item->block.file.getFileNameWithoutExtension(),
                        r.reduced(6, 2).removeFromTop(14), juce::Justification::centredLeft, true);
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
                    g.setColour(level >= 1.0f ? MiraLookAndFeel::warn : MiraLookAndFeel::active);
                    g.fillRoundedRectangle(meterBox.toFloat().withWidth(
                        juce::jmax(2.0f, meterBox.getWidth() * frac)), 2.5f);
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
    if (changed) { rebuildAudio(); save(); }
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
        selected.clear();
        selected.insert(v->block.id);
        items.push_back(std::move(v));
        at += len;
    }
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
            juce::AudioThumbnailCache& cache)
        : view(laf, formats, cache)
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

        button(newProjectButton, "New project...");
        button(addBlockButton, "+ Block");
        newProjectButton.onClick = [this] { promptNewProject(); };
        addBlockButton.onClick   = [this] { view.addEmptyBlock(); };

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
        startTimerHz(10);
        addAndMakeVisible(view);
    }

    void promptNewProject()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "New canvas project - choose or create a folder",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory));
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectDirectories
                                 | juce::FileBrowserComponent::saveMode,
                              [this, chooser](const juce::FileChooser& fc) {
            const auto folder = fc.getResult();
            if (folder == juce::File()) return;
            if (!folder.isDirectory() && !folder.createDirectory().wasOk()) return;
            view.setProject(folder);
            setName("Canvas - " + folder.getFileName());
            if (auto* w = findParentComponentOfClass<juce::DocumentWindow>())
                w->setName("Canvas - " + folder.getFileName());
        });
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
        addBlockButton.setBounds(bar.removeFromLeft(74));
        bar.removeFromLeft(6);
        newProjectButton.setBounds(bar.removeFromLeft(110));
        bar.removeFromLeft(12);
        clock.setBounds(bar.removeFromRight(190));
        bar.removeFromRight(8);
        meter.setBounds(bar.removeFromRight(110));
        hint.setBounds(bar);

        view.setBounds(r);
    }

    void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface2); }

    void timerCallback() override
    {
        // The peak matters MORE here than in a normal DAW. Stacking alternates means N
        // takes that each peak near full scale summing into one bus: two is +6 dB over,
        // four is +12. Nothing normalises that, so the number has to be on screen or the
        // first thing the canvas teaches you is that it distorts.
        const float p = view.readAndClearPeak();
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
    juce::TextButton playButton, loopButton, fitButton, deleteButton, addBlockButton, newProjectButton;
    juce::Label hint, clock, meter;
};

CanvasWindow::CanvasWindow(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
                           juce::AudioThumbnailCache& cache)
    : juce::DocumentWindow("Canvas (experimental)", MiraLookAndFeel::surface,
                            juce::DocumentWindow::closeButton)
{
    content = std::make_unique<Content>(laf, formats, cache);
    view = &content->view;
    setUsingNativeTitleBar(true);
    setContentNonOwned(content.get(), false);
    setResizable(true, false);
    centreWithSize(1100, 640);
    setVisible(true);
}

CanvasWindow::~CanvasWindow() = default;

} // namespace mira::canvas
