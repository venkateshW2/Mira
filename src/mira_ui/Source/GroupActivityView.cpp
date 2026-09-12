#include "GroupActivityView.h"

#include <algorithm>
#include <cmath>

void GroupActivityView::setReelLength(double seconds)
{
    reelSeconds = seconds;
    repaint();
}

void GroupActivityView::setStems(std::vector<StemRow> newStems)
{
    stems = std::move(newStems);
    // The mix last: it is the sum of the others, not one of them, so it reads as a summary
    // row under the parts rather than an equal sibling among them.
    std::stable_sort(stems.begin(), stems.end(),
                      [](const StemRow& a, const StemRow& b) { return a.isMix < b.isMix; });
    repaint();
}

void GroupActivityView::setCues(std::vector<CueMark> newCues)
{
    cues = std::move(newCues);
    std::sort(cues.begin(), cues.end(),
               [](const CueMark& a, const CueMark& b) { return a.startSeconds < b.startSeconds; });
    repaint();
}

void GroupActivityView::setView(double newViewStartFrac, double newZoomFactor)
{
    viewStartFrac = newViewStartFrac;
    zoomFactor = newZoomFactor;
    repaint();
}

int GroupActivityView::preferredHeight() const
{
    if (stems.empty()) return 0;
    return static_cast<int>(stems.size()) * rowHeight + kDensityHeight + 6
           + (showRuler ? kRulerHeight + kCueLaneHeight : 0);
}

void GroupActivityView::setRowHeight(int height)
{
    rowHeight = juce::jmax(6, height);
    repaint();
}

void GroupActivityView::setShowRuler(bool shouldShow)
{
    showRuler = shouldShow;
    repaint();
}

std::optional<std::pair<double, double>> GroupActivityView::getSelectedRange() const { return selectedRange; }

void GroupActivityView::clearSelectedRange()
{
    selectedRange.reset();
    repaint();
}

juce::Rectangle<int> GroupActivityView::rulerBounds() const
{
    if (!showRuler) return {};
    return getLocalBounds().removeFromTop(kRulerHeight).withTrimmedLeft(kGutterWidth);
}

juce::Rectangle<int> GroupActivityView::cueLaneBounds() const
{
    if (!showCueLane()) return {};
    auto bounds = getLocalBounds();
    bounds.removeFromTop(kRulerHeight);
    return bounds.removeFromTop(kCueLaneHeight).withTrimmedLeft(kGutterWidth);
}

juce::Rectangle<int> GroupActivityView::matrixBounds() const
{
    auto bounds = getLocalBounds().withTrimmedBottom(kDensityHeight);
    if (showRuler) bounds.removeFromTop(kRulerHeight);
    if (showCueLane()) bounds.removeFromTop(kCueLaneHeight);
    return bounds.withTrimmedLeft(kGutterWidth);
}

juce::Rectangle<int> GroupActivityView::densityBounds() const
{
    return getLocalBounds().removeFromBottom(kDensityHeight).withTrimmedLeft(kGutterWidth);
}

int GroupActivityView::secondsToX(double seconds, juce::Rectangle<int> area) const
{
    if (reelSeconds <= 0.0 || area.getWidth() <= 0) return area.getX();
    double windowFrac = 1.0 / zoomFactor;
    return area.getX() + static_cast<int>((seconds / reelSeconds - viewStartFrac) / windowFrac * area.getWidth());
}

double GroupActivityView::xToSeconds(int x, juce::Rectangle<int> area) const
{
    if (area.getWidth() <= 0) return 0.0;
    double windowFrac = 1.0 / zoomFactor;
    double frac = viewStartFrac + (x - area.getX()) / static_cast<double>(area.getWidth()) * windowFrac;
    return juce::jlimit(0.0, reelSeconds, frac * reelSeconds);
}

std::optional<GroupActivityView::BoundaryHit> GroupActivityView::cueBoundaryNear(int x) const
{
    auto area = matrixBounds();
    // Starts first, across every cue, so a shared boundary (one cue ending exactly where
    // the next begins) resolves to the START -- that is the two-neighbour drag round 6
    // describes, and grabbing it as an "end" would move only one side.
    //
    // The first cue's start used to be excluded, on the grounds that there is no cue before
    // it to hand the other half of the drag to. That confused "has no neighbour" with "must
    // not move": it is still this cue's own beginning, and there was no other way to change
    // it.
    for (size_t i = 0; i < cues.size(); ++i)
        if (std::abs(secondsToX(cues[i].startSeconds, area) - x) <= kBoundaryGrabPixels)
            return BoundaryHit { i, Edge::start };
    for (size_t i = 0; i < cues.size(); ++i)
        if (std::abs(secondsToX(cues[i].endSeconds, area) - x) <= kBoundaryGrabPixels)
            return BoundaryHit { i, Edge::end };
    return std::nullopt;
}

void GroupActivityView::paint(juce::Graphics& g)
{
    auto bounds = getLocalBounds();
    g.setColour(MiraLookAndFeel::surface3);
    g.fillRect(bounds);

    if (stems.empty() || reelSeconds <= 0.0)
    {
        g.setColour(MiraLookAndFeel::textFaint);
        g.setFont(juce::Font(juce::FontOptions(11.5f)));
        g.drawText("No synced stem set for this file", bounds, juce::Justification::centred, true);
        return;
    }

    auto area = matrixBounds();
    double windowFrac = 1.0 / zoomFactor;

    if (showRuler)
    {
        // Its own ruler in the full cue window, where there is no waveform above to borrow
        // one from. Same 1/2/5/10/15/30/60 ladder and the same >=64px rule the waveform's
        // ruler uses, so the two never disagree about where a minute is.
        auto ruler = rulerBounds();
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(getLocalBounds().removeFromTop(kRulerHeight));
        double visible = reelSeconds / zoomFactor;
        static const double ladder[] = { 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0, 600.0 };
        double tick = ladder[std::size(ladder) - 1];
        for (double candidate : ladder)
            if (candidate / visible * ruler.getWidth() >= 64.0) { tick = candidate; break; }
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        double from = viewStartFrac * reelSeconds;
        double to = juce::jmin(reelSeconds, from + visible);
        for (double t = std::ceil(from / tick) * tick; t <= to; t += tick)
        {
            int x = secondsToX(t, ruler);
            g.setColour(MiraLookAndFeel::textFaint);
            g.drawVerticalLine(x, static_cast<float>(ruler.getBottom() - 5), static_cast<float>(ruler.getBottom()));
            g.setColour(MiraLookAndFeel::textDim);
            g.drawText(juce::String(static_cast<int>(t) / 60) + ":"
                            + juce::String(static_cast<int>(t) % 60).paddedLeft('0', 2),
                        x + 3, ruler.getY(), 46, kRulerHeight - 3, juce::Justification::centredLeft, false);
        }
    }
    double viewStart = viewStartFrac * reelSeconds;
    double viewEnd = juce::jmin(reelSeconds, (viewStartFrac + windowFrac) * reelSeconds);

    // --- The numbered cue lane ----------------------------------------------------------
    // "Can't tell where cues are", and "start and end cue not understanding some have some
    // dont". Both had the same cause: the only thing drawn per cue was a single vertical
    // line at its START. A cue whose neighbour began somewhere else -- which is every cue
    // with silence after it -- therefore had a visible beginning and no visible end, so
    // half of them looked like boundaries and half like ranges.
    //
    // A cue is a RANGE, so it is now drawn as a bar that occupies its range, numbered with
    // the same number the cue list on the right uses. That number is what ties the two
    // together: "9" in the list is the bar labelled 9, and the timings beside it are that
    // bar's start and length.
    if (auto lane = cueLaneBounds(); !lane.isEmpty())
    {
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(getLocalBounds().withTrimmedTop(kRulerHeight).removeFromTop(kCueLaneHeight));
        for (size_t i = 0; i < cues.size(); ++i)
        {
            const auto& cue = cues[i];
            if (cue.endSeconds < viewStart || cue.startSeconds > viewEnd) continue;
            int x0 = secondsToX(juce::jmax(cue.startSeconds, viewStart), lane);
            int x1 = secondsToX(juce::jmin(cue.endSeconds, viewEnd), lane);
            // Floored at 3px so a degenerate cue still shows up as something to click and
            // delete, rather than vanishing and looking like the list is lying.
            int width = juce::jmax(3, x1 - x0);
            auto colour = cue.edited ? MiraLookAndFeel::good : MiraLookAndFeel::accent;

            auto bar = juce::Rectangle<int>(x0, lane.getY() + 2, width, lane.getHeight() - 4);
            g.setColour(colour.withAlpha(cue.edited ? 0.42f : 0.22f));
            g.fillRect(bar);
            g.setColour(colour);
            g.drawRect(bar, 1); // a closed box on all four sides, whatever its neighbours do

            // Number always; name too when the bar is wide enough to hold it. The number is
            // the point -- it is what makes the cue list navigable.
            auto text = juce::String(i + 1);
            if (cue.label.isNotEmpty() && width > 64) text += "  " + cue.label;
            if (width > 14)
            {
                g.setColour(MiraLookAndFeel::text);
                g.setFont(juce::Font(juce::FontOptions(9.5f)));
                g.drawText(text, bar.reduced(3, 0), juce::Justification::centredLeft, false);
            }
        }
        g.setColour(MiraLookAndFeel::textFaint);
        g.setFont(juce::Font(juce::FontOptions(9.0f)));
        g.drawText("CUES", 6, lane.getY(), kGutterWidth - 10, lane.getHeight(),
                    juce::Justification::centredLeft, false);
    }

    // --- Cue bands, painted first so the activity blocks sit on top of them -------------
    // Alternating tint rather than a line per boundary: a band says "this stretch is one
    // cue", which is the thing being judged, while a bare line leaves the eye to pair them
    // up. Edited cues are tinted more strongly -- they are the ones that survive a
    // re-detect, so they deserve to look settled rather than provisional.
    for (size_t i = 0; i < cues.size(); ++i)
    {
        const auto& cue = cues[i];
        if (cue.endSeconds < viewStart || cue.startSeconds > viewEnd) continue;
        int x0 = secondsToX(juce::jmax(cue.startSeconds, viewStart), area);
        int x1 = secondsToX(juce::jmin(cue.endSeconds, viewEnd), area);
        auto colour = cue.edited ? MiraLookAndFeel::good : MiraLookAndFeel::accent;
        g.setColour(colour.withAlpha(cue.edited ? 0.13f : (i % 2 == 0 ? 0.08f : 0.04f)));
        g.fillRect(x0, area.getY(), juce::jmax(1, x1 - x0), area.getHeight());
    }

    // --- One row per stem ---------------------------------------------------------------
    g.setFont(juce::Font(juce::FontOptions(9.5f)));
    for (size_t i = 0; i < stems.size(); ++i)
    {
        const auto& stem = stems[i];
        int y = area.getY() + static_cast<int>(i) * rowHeight;

        g.setColour(stem.isCurrent ? MiraLookAndFeel::text : MiraLookAndFeel::textFaint);
        g.drawText(stem.label, 6, y, kGutterWidth - 10, rowHeight, juce::Justification::centredLeft, false);

        // The currently selected file is brighter, so the matrix doubles as "where am I" --
        // the waveform above is showing this row's file.
        auto blockColour = stem.isMix ? MiraLookAndFeel::accent
                                       : (stem.isCurrent ? MiraLookAndFeel::text : MiraLookAndFeel::active);
        g.setColour(blockColour.withAlpha(stem.isCurrent ? 0.95f : 0.6f));
        for (const auto& [spanStart, spanEnd] : stem.spans)
        {
            if (spanEnd < viewStart || spanStart > viewEnd) continue;
            int x0 = secondsToX(juce::jmax(spanStart, viewStart), area);
            int x1 = secondsToX(juce::jmin(spanEnd, viewEnd), area);
            // Floored at 1px for the same reason the waveform's span lane is: a sub-second
            // hit on a 41-minute reel would otherwise round away to nothing and read as
            // "this stem never plays here".
            g.fillRect(x0, y + 1, juce::jmax(1, x1 - x0), rowHeight - 3);
        }
    }

    // --- Density strip -------------------------------------------------------------------
    // How many stems are playing, per pixel column. This is the signal a cue boundary shows
    // up in when nothing goes silent: the height steps rather than dropping to zero.
    auto density = densityBounds();
    g.setColour(MiraLookAndFeel::surface2);
    g.fillRect(density);
    int partCount = 0;
    for (const auto& stem : stems)
        if (!stem.isMix) ++partCount;
    if (partCount > 0)
    {
        for (int x = density.getX(); x < density.getRight(); ++x)
        {
            double t = xToSeconds(x, density);
            int playing = 0;
            for (const auto& stem : stems)
            {
                if (stem.isMix) continue;
                for (const auto& [spanStart, spanEnd] : stem.spans)
                    if (spanStart <= t && t < spanEnd) { ++playing; break; }
            }
            if (playing == 0) continue;
            int h = juce::jmax(1, playing * (density.getHeight() - 2) / partCount);
            g.setColour(MiraLookAndFeel::active.withAlpha(0.35f + 0.45f * playing / partCount));
            g.fillRect(x, density.getBottom() - 1 - h, 1, h);
        }
    }

    // --- Cue boundaries, on top of everything -------------------------------------------
    for (size_t i = 0; i < cues.size(); ++i)
    {
        const auto& cue = cues[i];
        if (cue.endSeconds < viewStart || cue.startSeconds > viewEnd) continue;
        bool draggingStart = draggingBoundary && draggingBoundary->index == i
                              && draggingBoundary->edge == Edge::start;
        bool draggingEnd = draggingBoundary && draggingBoundary->index == i
                            && draggingBoundary->edge == Edge::end;
        bool dragging = draggingStart;
        bool startVisible = cue.startSeconds >= viewStart && cue.startSeconds <= viewEnd;
        double at = dragging ? dragSeconds : cue.startSeconds;
        int x = secondsToX(at, area);
        if (startVisible || dragging)
        {
            g.setColour(dragging ? MiraLookAndFeel::text
                                 : (cue.edited ? MiraLookAndFeel::good : MiraLookAndFeel::accent));
            g.drawLine(static_cast<float>(x), static_cast<float>(area.getY()), static_cast<float>(x),
                       static_cast<float>(density.getBottom()), dragging ? 2.0f : 1.0f);
        }

        // The END edge, drawn exactly like the start. It was dashed, and skipped entirely
        // where the next cue began at the same instant -- the reasoning being that a shared
        // boundary shouldn't be drawn twice. On screen that produced "so 12, 14 has start
        // and end cue line why does 16 and 17 not have": cues butted against a neighbour
        // looked closed on both sides (the neighbour's start supplied the second line),
        // while cues with silence after them looked open. Same object, two appearances,
        // for a reason nobody can see. Both edges are solid now; a shared boundary drawing
        // twice at the same x is invisible anyway.
        bool endIsNextStart = i + 1 < cues.size()
                               && std::abs(cues[i + 1].startSeconds - cue.endSeconds) < 0.5;
        double endAt = draggingEnd ? dragSeconds : cue.endSeconds;
        if ((!endIsNextStart || draggingEnd) && endAt >= viewStart && endAt <= viewEnd)
        {
            int xEnd = secondsToX(endAt, area);
            g.setColour(draggingEnd ? MiraLookAndFeel::text
                                     : (cue.edited ? MiraLookAndFeel::good : MiraLookAndFeel::accent));
            g.drawLine(static_cast<float>(xEnd), static_cast<float>(area.getY()),
                       static_cast<float>(xEnd), static_cast<float>(density.getBottom()),
                       draggingEnd ? 2.0f : 1.0f);
        }

        if (cue.label.isNotEmpty() && startVisible)
        {
            int x1 = secondsToX(cue.endSeconds, area);
            if (x1 - x > 40)
            {
                g.setColour(MiraLookAndFeel::text);
                g.setFont(juce::Font(juce::FontOptions(9.5f)));
                g.drawText(cue.label, x + 3, area.getY(), juce::jmin(120, x1 - x - 4), 11,
                            juce::Justification::centredLeft, false);
            }
        }
    }

    // Swept range, on top of everything: it is the thing about to become a cue, so it
    // should not be hidden behind the material it covers.
    auto sweep = sweeping ? std::optional<std::pair<double, double>>({ juce::jmin(sweepAnchor, sweepEnd),
                                                                        juce::jmax(sweepAnchor, sweepEnd) })
                           : selectedRange;
    if (sweep)
    {
        int x0 = secondsToX(sweep->first, area);
        int x1 = secondsToX(sweep->second, area);
        g.setColour(MiraLookAndFeel::text.withAlpha(0.12f));
        g.fillRect(x0, area.getY(), juce::jmax(1, x1 - x0), area.getHeight());
        g.setColour(MiraLookAndFeel::text.withAlpha(0.8f));
        g.drawVerticalLine(x0, static_cast<float>(area.getY()), static_cast<float>(area.getBottom()));
        g.drawVerticalLine(x1, static_cast<float>(area.getY()), static_cast<float>(area.getBottom()));
    }

    g.setColour(MiraLookAndFeel::border);
    g.drawVerticalLine(kGutterWidth - 1, static_cast<float>(bounds.getY()), static_cast<float>(bounds.getBottom()));
}

juce::Rectangle<int> GroupActivityView::cueBarBounds(int64_t cueId) const
{
    auto lane = cueLaneBounds();
    if (lane.isEmpty()) return {};
    double windowFrac = 1.0 / zoomFactor;
    double viewStart = viewStartFrac * reelSeconds;
    double viewEnd = juce::jmin(reelSeconds, (viewStartFrac + windowFrac) * reelSeconds);
    for (const auto& cue : cues)
    {
        if (cue.id != cueId) continue;
        if (cue.endSeconds < viewStart || cue.startSeconds > viewEnd) return {};
        int x0 = secondsToX(juce::jmax(cue.startSeconds, viewStart), lane);
        int x1 = secondsToX(juce::jmin(cue.endSeconds, viewEnd), lane);
        return { x0, lane.getY() + 2, juce::jmax(3, x1 - x0), lane.getHeight() - 4 };
    }
    return {};
}

void GroupActivityView::mouseDoubleClick(const juce::MouseEvent& e)
{
    auto lane = cueLaneBounds();
    if (lane.isEmpty() || !lane.contains(e.x, e.y)) return;
    double t = xToSeconds(e.x, lane);
    for (const auto& cue : cues)
        if (t >= cue.startSeconds && t <= cue.endSeconds)
        {
            if (onCueRenameRequested) onCueRenameRequested(cue.id, cueBarBounds(cue.id));
            return;
        }
}

void GroupActivityView::mouseMove(const juce::MouseEvent& e)
{
    if (cueBoundaryNear(e.x)) { setMouseCursor(juce::MouseCursor::LeftRightResizeCursor); return; }
    // An I-beam over the cue lane: the bars are renameable in place, and a cursor is the
    // only thing that says so before someone tries.
    auto lane = cueLaneBounds();
    setMouseCursor(!lane.isEmpty() && lane.contains(e.x, e.y) ? juce::MouseCursor::IBeamCursor
                                                              : juce::MouseCursor::NormalCursor);
}

void GroupActivityView::mouseDown(const juce::MouseEvent& e)
{
    // The gutter is the stem list, and clicking a name in it selects that file -- which is
    // what makes the waveform, the details sidebar and the segment list all follow
    // ("i cant select a track /file so idont know the segemetn names"). Handled before
    // anything else because the gutter is outside matrixBounds(): a sweep started here
    // would map to a nonsense time anyway.
    if (e.x < kGutterWidth)
    {
        auto area = matrixBounds();
        if (rowHeight > 0 && e.y >= area.getY())
        {
            int index = (e.y - area.getY()) / rowHeight;
            if (index >= 0 && index < static_cast<int>(stems.size()) && onStemClicked)
                onStemClicked(index);
        }
        return;
    }

    draggingBoundary = cueBoundaryNear(e.x);
    if (draggingBoundary)
    {
        const auto& cue = cues[draggingBoundary->index];
        dragSeconds = draggingBoundary->edge == Edge::start ? cue.startSeconds : cue.endSeconds;
        return;
    }

    auto t = xToSeconds(e.x, matrixBounds());

    // A click in the cue lane is a click on that cue, not the start of a sweep -- the lane
    // is the cue index, so sweeping a new range across it would be the wrong gesture there.
    if (auto lane = cueLaneBounds(); !lane.isEmpty() && lane.contains(e.x, e.y))
    {
        for (const auto& cue : cues)
            if (t >= cue.startSeconds && t <= cue.endSeconds)
            {
                if (e.mods.isPopupMenu()) { if (onCueRightClicked) onCueRightClicked(cue.id); }
                else if (onCueClicked) onCueClicked(cue.id);
                return;
            }
        return;
    }

    // A right-click inside a cue is that cue's menu; a left press starts a sweep, which
    // becomes a click if it never moves (handled in mouseUp).
    if (e.mods.isPopupMenu())
    {
        for (const auto& cue : cues)
            if (t >= cue.startSeconds && t <= cue.endSeconds)
            {
                if (onCueRightClicked) onCueRightClicked(cue.id);
                return;
            }
        return;
    }

    sweeping = true;
    sweepAnchor = sweepEnd = t;
    selectedRange.reset();
    repaint();
}

void GroupActivityView::mouseDrag(const juce::MouseEvent& e)
{
    if (sweeping)
    {
        sweepEnd = xToSeconds(e.x, matrixBounds());
        repaint();
        return;
    }
    if (!draggingBoundary) return;

    // Clamped so neither the cue being dragged nor the neighbour sharing the boundary can
    // be inverted or collapsed. A zero-length cue is not a thing anyone means to create --
    // and five of them are sitting in the user's library because an earlier version of this
    // had no clamp at all.
    size_t i = draggingBoundary->index;
    double lower = 0.0, upper = reelSeconds;
    if (draggingBoundary->edge == Edge::start)
    {
        // Can't pass the previous cue's start, and can't reach this cue's own end.
        lower = i > 0 ? cues[i - 1].startSeconds + 1.0 : 0.0;
        upper = cues[i].endSeconds - 1.0;
    }
    else
    {
        // Can't reach this cue's own start; can't pass the next cue's end when the two are
        // adjacent (dragging the shared edge carries that cue's start along with it).
        lower = cues[i].startSeconds + 1.0;
        bool adjacent = i + 1 < cues.size()
                         && std::abs(cues[i + 1].startSeconds - cues[i].endSeconds) < 0.5;
        upper = adjacent ? cues[i + 1].endSeconds - 1.0
                         : (i + 1 < cues.size() ? cues[i + 1].startSeconds : reelSeconds);
    }
    dragSeconds = juce::jlimit(lower, juce::jmax(lower, upper), xToSeconds(e.x, matrixBounds()));

    // Snap to a nearby stem edge. At fit zoom on a 41-minute reel one pixel is about 1.7
    // seconds, so unaided dragging can't land on the moment a stem actually starts -- and
    // that moment is almost always what a cue boundary wants to be.
    double bestDistance = 1.0e9;
    double bestEdge = dragSeconds;
    double snapWindow = (1.0 / zoomFactor) * reelSeconds * 0.01; // 1% of the visible window
    for (const auto& stem : stems)
    {
        if (stem.isMix) continue;
        for (const auto& [spanStart, spanEnd] : stem.spans)
            for (double edge : { spanStart, spanEnd })
            {
                double distance = std::abs(edge - dragSeconds);
                if (distance < bestDistance && distance <= snapWindow)
                {
                    bestDistance = distance;
                    bestEdge = edge;
                }
            }
    }
    if (bestDistance < 1.0e8) dragSeconds = juce::jlimit(lower, juce::jmax(lower, upper), bestEdge);
    repaint();
}

void GroupActivityView::mouseUp(const juce::MouseEvent& e)
{
    if (sweeping)
    {
        sweeping = false;
        double from = juce::jmin(sweepAnchor, sweepEnd);
        double to = juce::jmax(sweepAnchor, sweepEnd);
        // A sweep that never really moved is a click on whatever cue is under it, not a
        // zero-length range -- otherwise selecting a cue would arm a "+ Cue" for nothing.
        if (to - from < reelSeconds * 0.001)
        {
            for (const auto& cue : cues)
                if (from >= cue.startSeconds && from <= cue.endSeconds)
                {
                    if (onCueClicked) onCueClicked(cue.id);
                    break;
                }
            repaint();
            return;
        }
        selectedRange = std::make_pair(from, to);
        if (onRangeSelected) onRangeSelected(from, to);
        repaint();
        return;
    }
    if (!draggingBoundary) return;
    auto hit = *draggingBoundary;
    auto id = cues[hit.index].id;
    auto seconds = dragSeconds;
    draggingBoundary.reset();
    // One write on release, not one per mouse move.
    if (onCueBoundaryMoved) onCueBoundaryMoved(id, hit.edge, seconds);
}

void GroupActivityView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Same gestures the waveform uses, so the two timelines are driven identically: a
    // horizontal swipe pans, a vertical one zooms about the cursor. Reported outward rather
    // than applied locally, because in the bottom panel the waveform owns the view and this
    // is the follower; in the cue window there is no waveform and the owner is the page.
    if (reelSeconds <= 0.0 || !onViewChanged) return;
    double windowFrac = 1.0 / zoomFactor;
    if (std::abs(wheel.deltaX) > std::abs(wheel.deltaY))
    {
        if (zoomFactor <= 1.0) return;
        double shifted = juce::jlimit(0.0, juce::jmax(0.0, 1.0 - windowFrac),
                                       viewStartFrac - wheel.deltaX / zoomFactor * 0.6);
        onViewChanged(shifted, zoomFactor);
        return;
    }
    if (std::abs(wheel.deltaY) < 1.0e-4) return;
    double anchor = xToSeconds(e.x, matrixBounds()) / reelSeconds;
    double newZoom = juce::jlimit(1.0, 64.0, zoomFactor * std::pow(2.0, wheel.deltaY * 1.5));
    double newWindow = 1.0 / newZoom;
    double offset = juce::jlimit(0.0, 1.0, (anchor - viewStartFrac) / windowFrac);
    onViewChanged(juce::jlimit(0.0, juce::jmax(0.0, 1.0 - newWindow), anchor - offset * newWindow), newZoom);
}
