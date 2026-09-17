#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>

#include "MiraLookAndFeel.h"

// The LoRA step windows, as one picture you can drag.
//
// This replaces a pair of number sliders per LoRA, and the reason is worth keeping: the
// pair asked for the two ENDS of a window, so "give me less structure" moved the left one
// UP while "give me less timbre" moved the right one DOWN. Two controls where the same
// intention moves them in opposite directions. Nobody can hold that in their head, and
// the user reported exactly that -- "moving two sliders doesn't make sense to me".
//
// What people actually do is assign roles: this LoRA carries the structure, that one the
// timbre, that one everything. Verified in use before this was built -- amt on structure,
// sub on timbre, gsl full, and it made musical sense. So the control is the assignment,
// not the arithmetic.
//
// Direct manipulation over a grid, not percentages. At 8 sampler steps the run is 8
// cells: a percentage would quantise invisibly (70% and 80% landing on the same step)
// while a grid shows the real resolution and is easy to hit. The coarseness is the truth
// and is better shown than hidden.
//
// The other thing no slider could do: with two or three LoRAs loaded you can see at a
// glance whether they DIVIDE the run or fight over it, which is the only question that
// matters when blending.
class LoraLanes : public juce::Component
{
public:
    static constexpr int kLanes = 3;
    static constexpr int kLaneHeight = 26;
    static constexpr int kHeaderHeight = 16;
    static constexpr int kNameWidth = 104;
    static constexpr int kRoleWidth = 78;

    struct Lane
    {
        juce::String name;      // empty = no LoRA in this slot
        int lo = 1, hi = 8;     // inclusive sampler-step window
    };

    // Fires with the slot index whenever a window changes, so the owner can write it back.
    std::function<void(int, int, int)> onWindowChanged;

    void setSteps(int steps)
    {
        totalSteps = juce::jmax(1, steps);
        for (auto& lane : lanes)
        {
            lane.lo = juce::jlimit(1, totalSteps, lane.lo);
            lane.hi = juce::jlimit(lane.lo, totalSteps, lane.hi);
        }
        repaint();
    }

    void setLane(int index, const juce::String& name, int lo, int hi)
    {
        if (index < 0 || index >= kLanes) return;
        lanes[static_cast<size_t>(index)].name = name;
        lanes[static_cast<size_t>(index)].lo = juce::jlimit(1, totalSteps, lo);
        lanes[static_cast<size_t>(index)].hi = juce::jlimit(lanes[static_cast<size_t>(index)].lo, totalSteps, hi);
        repaint();
    }

    Lane getLane(int index) const { return lanes[static_cast<size_t>(juce::jlimit(0, kLanes - 1, index))]; }

    static int idealHeight() { return kHeaderHeight + kLanes * kLaneHeight + 4; }

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds();

        // Header: the step ruler, so the cells mean something.
        auto header = r.removeFromTop(kHeaderHeight);
        auto grid = header.withTrimmedLeft(kNameWidth).withTrimmedRight(kRoleWidth);
        g.setColour(MiraLookAndFeel::textDim);
        g.setFont(juce::Font(juce::FontOptions(9.5f)));
        g.drawText("steps", header.removeFromLeft(kNameWidth).withTrimmedLeft(2),
                    juce::Justification::centredLeft);
        g.drawText("1", grid, juce::Justification::centredLeft);
        g.drawText(juce::String(totalSteps), grid, juce::Justification::centredRight);

        for (int i = 0; i < kLanes; ++i)
            paintLane(g, i, r.removeFromTop(kLaneHeight));
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int lane = laneAt(e.y);
        if (lane < 0) return;
        auto& L = lanes[static_cast<size_t>(lane)];

        if (roleArea(lane).contains(e.getPosition())) { clickRole(lane, e.x); return; }
        if (L.name.isEmpty()) return; // an empty slot has no window to set
        if (!gridArea(lane).contains(e.getPosition())) return;

        const int step = stepAt(e.x);
        const int xLo = stepToX(L.lo), xHi = stepToX(L.hi + 1);
        // Edges win over the body, and the body means "move the whole window" -- the same
        // grammar as a clip in a DAW timeline, which is where this gesture is already
        // learned.
        if (std::abs(e.x - xLo) <= 6)      drag = Drag::Lo;
        else if (std::abs(e.x - xHi) <= 6) drag = Drag::Hi;
        else if (step >= L.lo && step <= L.hi) { drag = Drag::Move; grabOffset = step - L.lo; }
        else                                   { drag = Drag::Hi; L.lo = step; L.hi = step; }
        dragLane = lane;
        mouseDrag(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (drag == Drag::None || dragLane < 0) return;
        auto& L = lanes[static_cast<size_t>(dragLane)];
        const int step = stepAt(e.x);

        if (drag == Drag::Lo)   L.lo = juce::jlimit(1, L.hi, step);
        else if (drag == Drag::Hi) L.hi = juce::jlimit(L.lo, totalSteps, step);
        else
        {
            const int width = L.hi - L.lo;
            L.lo = juce::jlimit(1, totalSteps - width, step - grabOffset);
            L.hi = L.lo + width;
        }
        repaint();
        if (onWindowChanged) onWindowChanged(dragLane, L.lo, L.hi);
    }

    void mouseUp(const juce::MouseEvent&) override { drag = Drag::None; dragLane = -1; }

private:
    enum class Drag { None, Lo, Hi, Move };

    juce::Rectangle<int> laneBounds(int i) const
    {
        return { 0, kHeaderHeight + i * kLaneHeight, getWidth(), kLaneHeight };
    }
    juce::Rectangle<int> gridArea(int i) const
    {
        return laneBounds(i).withTrimmedLeft(kNameWidth).withTrimmedRight(kRoleWidth).reduced(0, 5);
    }
    juce::Rectangle<int> roleArea(int i) const
    {
        return laneBounds(i).removeFromRight(kRoleWidth).reduced(1, 4);
    }
    int laneAt(int y) const
    {
        const int i = (y - kHeaderHeight) / kLaneHeight;
        return (y < kHeaderHeight || i < 0 || i >= kLanes) ? -1 : i;
    }
    int stepToX(int step) const
    {
        auto g = gridArea(0);
        const double frac = static_cast<double>(step - 1) / totalSteps;
        return g.getX() + juce::roundToInt(frac * g.getWidth());
    }
    int stepAt(int x) const
    {
        auto g = gridArea(0);
        if (g.getWidth() <= 0) return 1;
        const double frac = static_cast<double>(x - g.getX()) / g.getWidth();
        return juce::jlimit(1, totalSteps, 1 + static_cast<int>(frac * totalSteps));
    }

    // Three roles, one click each, because that is how the thing is actually used.
    // "Custom" is not a button -- it is simply what a dragged window is.
    void clickRole(int lane, int x)
    {
        auto& L = lanes[static_cast<size_t>(lane)];
        if (L.name.isEmpty()) return;
        auto area = roleArea(lane);
        const int third = area.getWidth() / 3;
        const int which = juce::jlimit(0, 2, (x - area.getX()) / juce::jmax(1, third));
        const int mid = juce::jmax(1, totalSteps / 2);
        if (which == 0)      { L.lo = 1;       L.hi = mid; }          // structure
        else if (which == 1) { L.lo = mid + 1; L.hi = totalSteps; }   // timbre
        else                 { L.lo = 1;       L.hi = totalSteps; }   // full
        L.hi = juce::jmax(L.lo, L.hi);
        repaint();
        if (onWindowChanged) onWindowChanged(lane, L.lo, L.hi);
    }

    juce::String roleOf(const Lane& L) const
    {
        const int mid = juce::jmax(1, totalSteps / 2);
        if (L.lo <= 1 && L.hi >= totalSteps) return "full";
        if (L.lo <= 1 && L.hi == mid) return "structure";
        if (L.lo == mid + 1 && L.hi >= totalSteps) return "timbre";
        return "custom";
    }

    void paintLane(juce::Graphics& g, int i, juce::Rectangle<int> row)
    {
        const auto& L = lanes[static_cast<size_t>(i)];
        const bool active = L.name.isNotEmpty();

        g.setColour(active ? MiraLookAndFeel::text : MiraLookAndFeel::textDim.withAlpha(0.5f));
        g.setFont(juce::Font(juce::FontOptions(10.5f)));
        g.drawText(active ? L.name : "(empty)", row.removeFromLeft(kNameWidth).withTrimmedRight(6),
                    juce::Justification::centredLeft, true);

        auto grid = gridArea(i);
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRoundedRectangle(grid.toFloat(), 2.0f);

        // Cell divisions: the run's real resolution, drawn rather than implied.
        g.setColour(MiraLookAndFeel::surface.withAlpha(0.8f));
        for (int s = 1; s < totalSteps && totalSteps <= 64; ++s)
            g.fillRect(stepToX(s + 1), grid.getY(), 1, grid.getHeight());

        if (active)
        {
            juce::Rectangle<int> bar (stepToX(L.lo), grid.getY(),
                                       juce::jmax(3, stepToX(L.hi + 1) - stepToX(L.lo)), grid.getHeight());
            // One colour per slot so two lanes can be told apart at a glance -- the whole
            // point of the picture is seeing how they overlap.
            const juce::Colour tint = i == 0 ? MiraLookAndFeel::accent
                                              : (i == 1 ? juce::Colour(0xff5aa9e6) : juce::Colour(0xff7ec98f));
            g.setColour(tint.withAlpha(0.55f));
            g.fillRoundedRectangle(bar.toFloat(), 2.0f);
            g.setColour(tint);
            g.fillRect(bar.getX(), bar.getY(), 2, bar.getHeight());
            g.fillRect(bar.getRight() - 2, bar.getY(), 2, bar.getHeight());

            g.setColour(MiraLookAndFeel::text);
            g.setFont(juce::Font(juce::FontOptions(9.5f)));
            if (bar.getWidth() > 70)
                g.drawText(juce::String(L.lo) + "-" + juce::String(L.hi) + "  " + roleOf(L),
                            bar, juce::Justification::centred);
        }

        // Role buttons: S | T | F. Compact on purpose -- they are a shortcut into the bar
        // above, not the primary control, and the bar is what tells you what happened.
        auto roles = roleArea(i);
        const char* names[3] = { "S", "T", "F" };
        const int third = roles.getWidth() / 3;
        const auto current = roleOf(L);
        for (int k = 0; k < 3; ++k)
        {
            auto cell = roles.withX(roles.getX() + k * third).withWidth(third).reduced(1, 0);
            const bool on = active && ((k == 0 && current == "structure") || (k == 1 && current == "timbre")
                                        || (k == 2 && current == "full"));
            g.setColour(on ? MiraLookAndFeel::accent.withAlpha(0.35f) : MiraLookAndFeel::surface2);
            g.fillRoundedRectangle(cell.toFloat(), 2.0f);
            g.setColour(active ? (on ? MiraLookAndFeel::text : MiraLookAndFeel::textDim)
                                : MiraLookAndFeel::textDim.withAlpha(0.4f));
            g.setFont(juce::Font(juce::FontOptions(9.5f, juce::Font::bold)));
            g.drawText(names[k], cell, juce::Justification::centred);
        }
    }

    std::array<Lane, kLanes> lanes;
    int totalSteps = 8;
    Drag drag = Drag::None;
    int dragLane = -1;
    int grabOffset = 0;
};
