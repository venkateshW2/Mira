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
    projectFolder = project;
    items.clear();
    selected.clear();
    if (!project.isDirectory()) { rebuildAudio(); repaint(); return; }

    // Everything the project holds, cue by cue, one lane per cue -- which is the mapping
    // we already agreed: a cue IS a track. Takes of the same cue land on the same lane,
    // laid end to end, so the canvas opens showing the project's actual shape rather than
    // an empty grid waiting to be told what to do.
    int lane = 0;
    double widest = 0.0;
    for (const auto& dir : project.findChildFiles(juce::File::findDirectories, false))
    {
        const auto name = dir.getFileName();
        if (name == "discarded" || name == "export") continue;

        auto wavs = dir.findChildFiles(juce::File::findFiles, false, "*.wav");
        if (wavs.isEmpty()) continue;
        wavs.sort();

        double at = 0.0;
        for (const auto& wav : wavs)
        {
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor(wav));
            if (reader == nullptr) continue;
            const double len = reader->sampleRate > 0.0
                                 ? reader->lengthInSamples / reader->sampleRate : 0.0;
            if (len <= 0.0) continue;

            auto v = std::make_unique<Visual>();
            v->block.file = wav;
            v->block.lane = lane;
            v->block.start = at;
            v->block.length = len;
            v->block.name = wav.getFileNameWithoutExtension();
            v->block.id = nextId++;
            v->thumb = std::make_unique<juce::AudioThumbnail>(512, formats, cache);
            v->thumb->setSource(new juce::FileInputSource(wav));
            items.push_back(std::move(v));

            at += len + 1.0;   // a second of air between takes, so edges are grabbable
        }
        widest = juce::jmax(widest, at);
        ++lane;
    }

    if (widest > 0.0) fit();
    rebuildAudio();
    repaint();
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
    const int w = juce::jmax(200, getWidth() - kGutter * 2);
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
            if (x < kGutter) continue;
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

        g.setColour(MiraLookAndFeel::surface3);
        g.fillRoundedRectangle(r.toFloat(), 5.0f);

        if (item->thumb != nullptr && item->thumb->getTotalLength() > 0.0)
        {
            auto wave = r.reduced(4, 16);
            g.setColour(MiraLookAndFeel::text.withAlpha(isSelected ? 0.85f : 0.6f));
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

        g.setColour(isSelected ? MiraLookAndFeel::text : MiraLookAndFeel::textDim);
        g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
        g.drawText(item->block.name, r.reduced(6, 2).removeFromTop(14),
                    juce::Justification::centredLeft, true);
    }

    if (!marquee.isEmpty())
    {
        g.setColour(MiraLookAndFeel::accent.withAlpha(0.15f));
        g.fillRect(marquee);
        g.setColour(MiraLookAndFeel::accent.withAlpha(0.6f));
        g.drawRect(marquee, 1);
    }

    // --- playhead, over everything
    {
        const int x = secondsToX(player.getPositionSeconds());
        if (x >= kGutter && x < getWidth())
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
    const bool changed = drag == Drag::Move || drag == Drag::TrimLeft || drag == Drag::TrimRight;
    drag = Drag::None;
    marquee = {};
    if (changed) rebuildAudio();
    repaint();
}

void CanvasView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
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
    viewStart = juce::jmax(0.0, anchor - (aroundX - kGutter) / pixelsPerSecond);
    repaint();
}

bool CanvasView::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)       { togglePlay(); return true; }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
                                               { removeSelected(); return true; }
    if (key == juce::KeyPress::returnKey)      { player.setPositionSeconds(0.0); repaint(); return true; }
    if (key.getTextCharacter() == 'l')         { setLoopFromSelection(); return true; }
    if (key.getTextCharacter() == 'f')         { fit(); return true; }
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
    if (player.isPlaying()) { repaint(); return; }

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

        hint.setText("space play  -  L loop selection  -  F fit  -  alt-drag pan  -  cmd-wheel zoom",
                      juce::dontSendNotification);
        hint.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
        hint.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        addAndMakeVisible(hint);

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
        startTimerHz(10);
        addAndMakeVisible(view);
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
        bar.removeFromLeft(12);
        clock.setBounds(bar.removeFromRight(190));
        hint.setBounds(bar);
        view.setBounds(r);
    }

    void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface2); }

    void timerCallback() override
    {
        clock.setText(formatTime(view.getPositionSeconds()) + " / " + formatTime(view.getLengthSeconds())
                       + "   " + juce::String(view.getBlockCount()) + " blocks",
                      juce::dontSendNotification);
    }

    CanvasView view;
    juce::TextButton playButton, loopButton, fitButton, deleteButton;
    juce::Label hint, clock;
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
