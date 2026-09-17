#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>

#include "MiraLookAndFeel.h"

// "the inpaint can now show the input waveform, range selection from the waveform - drag
// one of the current takes into inpainting - drag and drop from finder also."
//
// The range used to be two bare number sliders with no picture of the audio they were
// cutting into, so choosing a range meant guessing seconds and listening to the result.
// This is the audio, with the region to be regenerated shown on it.
//
// It draws a thumbnail; it does NOT play. Playback lives in one place in this window (the
// take stack's WaveformView, which owns the AudioDeviceManager) and a second transport
// here would be a second audio device for no gain -- the take is already auditionable in
// the row above.
class InpaintStrip : public juce::Component,
                      public juce::FileDragAndDropTarget,
                      public juce::ChangeListener
{
public:
    InpaintStrip(juce::AudioFormatManager& fm, juce::AudioThumbnailCache& cacheIn)
        : formatManager(fm), cache(cacheIn)
    {
    }

    ~InpaintStrip() override { if (thumbnail) thumbnail->removeChangeListener(this); }

    void setFile(const juce::File& f)
    {
        file = f;
        if (thumbnail) thumbnail->removeChangeListener(this);
        thumbnail.reset();
        if (file.existsAsFile())
        {
            thumbnail = std::make_unique<juce::AudioThumbnail>(256, formatManager, cache);
            thumbnail->addChangeListener(this);
            thumbnail->setSource(new juce::FileInputSource(file));
        }
        repaint();
    }

    juce::File getFile() const { return file; }
    double getAudioSeconds() const { return thumbnail ? thumbnail->getTotalLength() : 0.0; }

    // "impaint works like extensions... so start from the waveform and end it the next
    // time bound, so it's like extending the track also."
    //
    // It does, and the sampler already supports it: sa3_mlx.py ZERO-PADS init audio up to
    // the requested duration before encoding it. So a 30 s take asked for at 60 s, with
    // the range set 30..60, keeps the first half bit-exact and generates a second half
    // that has to follow from it. That is an extension.
    //
    // Which means the timeline here is the DURATION being generated, not the length of
    // the file -- the end handle has to be able to travel past where the audio stops, or
    // the one thing this is best at cannot be asked for.
    void setTimeline(double seconds)
    {
        timelineSeconds = juce::jmax(0.0, seconds);
        repaint();
    }

    double getTimelineSeconds() const
    {
        // Falls back to the audio's own length before a duration is set, so the strip is
        // never drawing against a zero-width timeline.
        return timelineSeconds > 0.0 ? timelineSeconds : getAudioSeconds();
    }

    void setRange(double startSec, double endSec)
    {
        rangeStart = startSec;
        rangeEnd = endSec;
        repaint();
    }

    double getRangeStart() const { return rangeStart; }
    double getRangeEnd() const { return rangeEnd; }

    // Fires while dragging a handle, so the number sliders stay the source of truth and
    // this stays a view onto them rather than a second copy of the value.
    std::function<void(double, double)> onRangeChanged;
    std::function<void(const juce::File&)> onFileDropped;

    void paint(juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRoundedRectangle(r.toFloat(), 4.0f);

        if (file == juce::File() || thumbnail == nullptr || thumbnail->getTotalLength() <= 0.0)
        {
            g.setColour(dragging ? MiraLookAndFeel::accent : MiraLookAndFeel::textDim);
            g.setFont(juce::Font(juce::FontOptions(11.5f)));
            g.drawText("drop a take or an audio file here", r, juce::Justification::centred);
            if (dragging)
            {
                g.setColour(MiraLookAndFeel::accent.withAlpha(0.6f));
                g.drawRoundedRectangle(r.toFloat().reduced(1.0f), 4.0f, 1.5f);
            }
            return;
        }

        const double audioLen = thumbnail->getTotalLength();
        auto wave = r.reduced(2);

        // The audio occupies only its own share of the timeline. The rest is empty --
        // that is the part an extension writes into, and it is drawn as empty rather
        // than left looking like audio that happens to be silent.
        const int audioRight = secondsToX(audioLen);
        juce::Rectangle<int> audioArea = wave.withRight(juce::jmax(wave.getX() + 1, audioRight));
        g.setColour(MiraLookAndFeel::textDim.withAlpha(0.7f));
        thumbnail->drawChannels(g, audioArea, 0.0, audioLen, 1.0f);

        if (audioRight < wave.getRight() - 1)
        {
            juce::Rectangle<int> empty = wave.withLeft(audioRight);
            g.setColour(MiraLookAndFeel::textDim.withAlpha(0.14f));
            for (int x = empty.getX(); x < empty.getRight(); x += 5)
                g.fillRect(x, empty.getCentreY(), 2, 1);
            g.setColour(MiraLookAndFeel::textDim.withAlpha(0.5f));
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            if (empty.getWidth() > 60)
                g.drawText("new", empty, juce::Justification::centred);
            g.setColour(MiraLookAndFeel::textDim.withAlpha(0.4f));
            g.fillRect(audioRight, wave.getY(), 1, wave.getHeight());
        }

        // The region that will be REGENERATED. Everything outside it is kept bit-exact by
        // the sampler's paste-back, so the highlight is literally "this part goes away".
        const int x0 = secondsToX(rangeStart);
        const int x1 = secondsToX(rangeEnd);
        juce::Rectangle<int> sel (x0, wave.getY(), juce::jmax(1, x1 - x0), wave.getHeight());
        g.setColour(MiraLookAndFeel::accent.withAlpha(0.22f));
        g.fillRect(sel);
        g.setColour(MiraLookAndFeel::accent);
        g.fillRect(x0 - 1, wave.getY(), 2, wave.getHeight());
        g.fillRect(x1 - 1, wave.getY(), 2, wave.getHeight());

        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.setColour(MiraLookAndFeel::text);
        g.drawText(juce::String(rangeStart, 1) + "s - " + juce::String(rangeEnd, 1) + "s",
                    sel.expanded(40, 0), juce::Justification::centredTop);

        if (dragging)
        {
            g.setColour(MiraLookAndFeel::accent.withAlpha(0.6f));
            g.drawRoundedRectangle(r.toFloat().reduced(1.0f), 4.0f, 1.5f);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (getTimelineSeconds() <= 0.0) return;
        // Whichever edge is nearer, unless the click is well outside the selection, in
        // which case it starts a new one.
        const int x0 = secondsToX(rangeStart), x1 = secondsToX(rangeEnd);
        if (std::abs(e.x - x0) <= 6)      grab = Grab::Start;
        else if (std::abs(e.x - x1) <= 6) grab = Grab::End;
        else
        {
            grab = Grab::End;
            rangeStart = juce::jlimit(0.0, getTimelineSeconds(), xToSeconds(e.x));
            rangeEnd = rangeStart;
        }
        mouseDrag(e);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (grab == Grab::None || getTimelineSeconds() <= 0.0) return;
        const double t = juce::jlimit(0.0, getTimelineSeconds(), xToSeconds(e.x));
        if (grab == Grab::Start) rangeStart = t;
        else                     rangeEnd = t;
        if (rangeEnd < rangeStart) std::swap(rangeStart, rangeEnd);
        repaint();
        if (onRangeChanged) onRangeChanged(rangeStart, rangeEnd);
    }

    void mouseUp(const juce::MouseEvent&) override { grab = Grab::None; }

    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        for (const auto& f : files)
            if (f.endsWithIgnoreCase(".wav") || f.endsWithIgnoreCase(".aif")
                || f.endsWithIgnoreCase(".aiff") || f.endsWithIgnoreCase(".flac")
                || f.endsWithIgnoreCase(".mp3"))
                return true;
        return false;
    }

    void fileDragEnter(const juce::StringArray&, int, int) override { dragging = true; repaint(); }
    void fileDragExit(const juce::StringArray&) override { dragging = false; repaint(); }

    void filesDropped(const juce::StringArray& files, int, int) override
    {
        dragging = false;
        repaint();
        for (const auto& f : files)
        {
            juce::File dropped { f };
            if (!dropped.existsAsFile()) continue;
            setFile(dropped);
            if (onFileDropped) onFileDropped(dropped);
            return; // one at a time: inpainting has exactly one source
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override { repaint(); }

private:
    enum class Grab { None, Start, End };

    int secondsToX(double seconds) const
    {
        const double length = getTimelineSeconds();
        if (length <= 0.0) return getLocalBounds().getX() + 2;
        auto wave = getLocalBounds().reduced(2);
        return wave.getX() + juce::roundToInt(seconds / length * wave.getWidth());
    }

    double xToSeconds(int x) const
    {
        auto wave = getLocalBounds().reduced(2);
        if (wave.getWidth() <= 0) return 0.0;
        return (static_cast<double>(x - wave.getX()) / wave.getWidth()) * getTimelineSeconds();
    }

    juce::AudioFormatManager& formatManager;
    juce::AudioThumbnailCache& cache;
    std::unique_ptr<juce::AudioThumbnail> thumbnail;
    juce::File file;
    double rangeStart = 0.0, rangeEnd = 10.0;
    double timelineSeconds = 0.0; // the DURATION being generated, not the file's length
    Grab grab = Grab::None;
    bool dragging = false;
};
