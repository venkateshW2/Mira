#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "MiraLookAndFeel.h"

// The "something is happening" strip, sitting where the take will land.
//
// Two things were wrong with what it replaces. It lived at the bottom of the RIGHT pane,
// next to the prompt -- so the eye had to be in one place to start a generation and
// another to receive it -- and it was a bare indeterminate bar beside a "generating...
// [230s]" label, which says how long you have waited and nothing about how long is left.
//
// It now runs full width across the TOP of the window, directly under the status line it
// belongs to, and it is one line tall with its own caption inside it. It was briefly at
// the top of the takes column -- where the take lands, which reads well -- but a 44px
// block there costs a take row for the whole generation, and the takes column is the part
// of this window that is always short of room.
//
// The estimate is MEASURED, not invented (convention 2, and 6: never substitute a made-up
// number for one you do not have). Generation cost is very close to linear in
// steps x seconds, so one constant k = t / (steps x seconds) describes this machine. k is
// learned from the user's own completed generations and persisted, so it is only ever
// unknown once -- on the very first run, where the bar deliberately claims NO position and
// shows a sweeping shimmer instead. A bar that invents a position is worse than one that
// admits it does not know.
//
// The shimmer runs in both modes, so the strip is never frozen even when the estimate has
// been overshot and the fill has nowhere left to go.
class GenerateProgress : public juce::Component, private juce::Timer
{
public:
    static constexpr int kHeight = 18;

    // Model load is a one-off cost per worker process, not part of the per-step work, so
    // it is added to the estimate rather than folded into k -- otherwise the first
    // generation after a restart would teach k a number that is wrong for every run after.
    static constexpr double kModelLoadSeconds = 44.0;

    void start(int steps, double seconds, bool includeModelLoad)
    {
        running = true;
        startMs = juce::Time::getMillisecondCounter();
        estimate = (calibration > 0.0)
                       ? calibration * steps * seconds + (includeModelLoad ? kModelLoadSeconds : 0.0)
                       : 0.0;   // 0 = we have never timed a run; say so rather than guess
        setVisible(true);
        startTimerHz(20);
        repaint();
    }

    void stop() { running = false; stopTimer(); setVisible(false); }

    // Zero until start() has actually run. getMillisecondCounter() is time since BOOT,
    // so an unstarted strip that subtracts a zero startMs reports the machine's uptime as
    // its elapsed time -- which is exactly what happened, as "generating 754:13".
    double elapsedSeconds() const
    {
        return running ? (juce::Time::getMillisecondCounter() - startMs) / 1000.0 : 0.0;
    }

    // Seconds per (step x second-of-audio) on this machine. Persisted by the owner.
    void setCalibration(double k) { calibration = k > 0.0 ? k : 0.0; }
    double getCalibration() const { return calibration; }

    // Fold a finished run into the constant. A slow exponential rather than a replacement:
    // one generation competing with a busy machine should nudge the estimate, not redefine
    // it.
    void learn(double tookSeconds, int steps, double seconds, bool hadModelLoad)
    {
        const double work = static_cast<double>(steps) * seconds;
        if (work <= 0.0) return;
        const double net = tookSeconds - (hadModelLoad ? kModelLoadSeconds : 0.0);
        if (net <= 0.0) return;
        const double k = net / work;
        calibration = (calibration > 0.0) ? calibration * 0.7 + k * 0.3 : k;
    }

    void paint(juce::Graphics& g) override
    {
        auto bar = getLocalBounds().reduced(0, 2).toFloat();

        g.setColour(MiraLookAndFeel::surface2);
        g.fillRoundedRectangle(bar, 3.0f);

        const double elapsed = elapsedSeconds();
        // Never 1.0 before the file exists: a bar that reads full while the user is still
        // waiting is the one thing worse than no bar at all.
        const double frac = estimate > 0.0 ? juce::jlimit(0.01, 0.985, elapsed / estimate) : 1.0;

        auto filled = bar.withWidth(juce::jmax(3.0f, static_cast<float>(bar.getWidth() * frac)));
        g.setColour(MiraLookAndFeel::accent.withAlpha(estimate > 0.0 ? 0.5f : 0.22f));
        g.fillRoundedRectangle(filled, 3.0f);

        // The shimmer: diagonal bands travelling along whatever is filled. This is the
        // part that is honestly decorative -- it carries no information beyond "still
        // alive", which is information the user explicitly asked for.
        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion(filled.getSmallestIntegerContainer());
            g.setColour(juce::Colours::white.withAlpha(0.06f));
            const float period = 26.0f;
            const float shift = std::fmod(static_cast<float>(elapsed) * 34.0f, period);
            for (float x = filled.getX() - period + shift; x < filled.getRight(); x += period)
            {
                juce::Path band;
                band.startNewSubPath(x, bar.getBottom());
                band.lineTo(x + period * 0.45f, bar.getBottom());
                band.lineTo(x + period * 0.45f + bar.getHeight(), bar.getY());
                band.lineTo(x + bar.getHeight(), bar.getY());
                band.closeSubPath();
                g.fillPath(band);
            }
        }

        if (estimate > 0.0)
        {
            g.setColour(MiraLookAndFeel::accent);
            g.fillRect(filled.getRight() - 2.0f, bar.getY(), 2.0f, bar.getHeight());
        }

        g.setColour(MiraLookAndFeel::text);
        g.setFont(juce::Font(juce::FontOptions(MiraLookAndFeel::textSize(10.0f))));
        g.drawText(caption(elapsed), getLocalBounds().withTrimmedLeft(8),
                    juce::Justification::centredLeft, true);
    }

private:
    void timerCallback() override { repaint(); }

    static juce::String clock(double secs)
    {
        const int s = juce::jmax(0, juce::roundToInt(secs));
        return juce::String(s / 60) + ":" + juce::String(s % 60).paddedLeft('0', 2);
    }

    juce::String caption(double elapsed) const
    {
        if (estimate <= 0.0)
            return "generating " + clock(elapsed) + "  -  timing this run to estimate the next";
        if (elapsed >= estimate)
            return "generating " + clock(elapsed) + "  -  past the " + clock(estimate)
                   + " estimate, still running";
        return "generating " + clock(elapsed) + " of about " + clock(estimate);
    }

    bool running = false;
    juce::uint32 startMs = 0;
    double estimate = 0.0;
    double calibration = 0.0;
};
