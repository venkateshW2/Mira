#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <map>
#include <memory>
#include <vector>

#include "MiraLookAndFeel.h"

// MIRA-GENERATE.md Phase 4 -- the take stack.
//
// Generation produces ten takes for every one worth keeping, and until now the window
// held exactly ONE: each result replaced the last, so comparing two meant regenerating
// or digging through Finder. This is the list, newest first, with the selected take
// expanded in place.
//
// ONE component paints every row rather than a child Component per take. Rows are a
// filename, a mini waveform and a state dot; a Component each would be a hundred lines of
// lifetime management for something that cannot receive focus and has no controls of its
// own -- the expanded row is where the controls live, and those are HOSTED (see below).
//
// The expanded row does not draw its own waveform either. It leaves a hole, and the
// components handed to setHostedComponents are positioned into it. That is deliberate and
// it is the whole reason this class exists in this shape: WaveformView opens an
// AudioDeviceManager in its constructor, so one per row would open one audio device per
// take. There is exactly one WaveformView, one transport and one drag-out tile in the
// window, and expanding a row moves them rather than making more.
class TakeStack : public juce::Component
{
public:
    TakeStack(const MiraLookAndFeel& lafIn, juce::AudioFormatManager& formatManagerIn,
               juce::AudioThumbnailCache& cacheIn)
        : laf(lafIn), formatManager(formatManagerIn), cache(cacheIn)
    {
    }

    static constexpr int kRowHeight = 30;
    static constexpr int kExpandedHeight = 188;

    // Newest first: the take just generated is the one being judged, so it goes to the
    // top and opens. A take already in the list is re-selected rather than duplicated
    // (regenerating over the same filename is possible, if unusual).
    void addTake(const juce::File& file)
    {
        for (size_t i = 0; i < takes.size(); ++i)
            if (takes[i].file == file) { select(static_cast<int>(i)); return; }

        Take take;
        take.file = file;
        take.thumbnail = std::make_unique<juce::AudioThumbnail>(256, formatManager, cache);
        take.thumbnail->setSource(new juce::FileInputSource(file));
        take.thumbnail->addChangeListener(&repainter);
        takes.insert(takes.begin(), std::move(take));
        // The selection is an index, so inserting at the front moves whatever was
        // selected down one. Tracking the FILE instead would be tidier in the abstract
        // and worse here: two rows can briefly share a name during a rename.
        if (selected >= 0) ++selected;
        select(0);
    }

    // Kept or discarded -- either way the take leaves the stack. It does NOT leave on
    // its own: "rows survive until kept or discarded" is the point, so nothing here is
    // driven by a timer or by the file vanishing.
    void removeTake(const juce::File& file)
    {
        for (size_t i = 0; i < takes.size(); ++i)
        {
            if (takes[i].file != file) continue;
            takes.erase(takes.begin() + static_cast<long>(i));
            // Select the row that took its place, so a run of Discards keeps working
            // without moving the mouse. Falls back to the new last row, then to nothing.
            int next = juce::jmin(static_cast<int>(i), static_cast<int>(takes.size()) - 1);
            selected = -1;
            if (next >= 0) select(next);
            else { updateLayout(); if (onSelected) onSelected({}); }
            return;
        }
    }

    juce::File getSelectedFile() const
    {
        if (selected < 0 || selected >= static_cast<int>(takes.size())) return {};
        return takes[static_cast<size_t>(selected)].file;
    }

    int getTakeCount() const { return static_cast<int>(takes.size()); }

    // The window's single WaveformView, drag tile and per-take buttons. They become
    // children of this component so they scroll with the row they belong to; passing an
    // empty list detaches them again.
    void setHostedComponents(std::vector<juce::Component*> components)
    {
        for (auto* c : hosted)
            if (c != nullptr && c->getParentComponent() == this) removeChildComponent(c);
        hosted = std::move(components);
        for (auto* c : hosted)
            if (c != nullptr) addAndMakeVisible(c);
        updateLayout();
    }

    // The rectangle the hosted components get, inside the expanded row. The caller lays
    // its own components out within this -- the stack knows where the hole is, not what
    // goes in it.
    juce::Rectangle<int> getExpandedContentArea() const
    {
        if (selected < 0) return {};
        int y = selected * kRowHeight + kRowHeight;
        return { 6, y, juce::jmax(0, getWidth() - 12), kExpandedHeight - kRowHeight - 6 };
    }

    std::function<void(const juce::File&)> onSelected;
    // Fires when the layout changes height, so the owner can resize this inside its
    // Viewport -- a Viewport asks its content for a size, it is not told one.
    std::function<void()> onHeightChanged;

    int getIdealHeight() const
    {
        if (takes.empty()) return kRowHeight;
        return static_cast<int>(takes.size()) * kRowHeight + (selected >= 0 ? kExpandedHeight - kRowHeight : 0);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(MiraLookAndFeel::surface);

        if (takes.empty())
        {
            g.setColour(MiraLookAndFeel::textDim);
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText("no takes yet", getLocalBounds().reduced(8, 0), juce::Justification::centredLeft);
            return;
        }

        int y = 0;
        for (size_t i = 0; i < takes.size(); ++i)
        {
            const bool isSelected = static_cast<int>(i) == selected;
            juce::Rectangle<int> row { 0, y, getWidth(), kRowHeight };
            paintRow(g, takes[i], row, isSelected);
            y += isSelected ? kExpandedHeight : kRowHeight;
        }
    }

    void resized() override { updateLayout(); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int y = 0;
        for (size_t i = 0; i < takes.size(); ++i)
        {
            const bool isSelected = static_cast<int>(i) == selected;
            // Only the ROW strip toggles, never the expanded area below it -- a click on
            // the waveform is a scrub, and collapsing the row out from under it would be
            // the worst possible response to that.
            if (e.y >= y && e.y < y + kRowHeight) { select(static_cast<int>(i)); return; }
            y += isSelected ? kExpandedHeight : kRowHeight;
        }
    }

private:
    struct Take
    {
        juce::File file;
        std::unique_ptr<juce::AudioThumbnail> thumbnail;
    };

    // AudioThumbnail loads on a background thread and broadcasts when more of the peaks
    // are ready; without a listener the mini waveforms stay blank until something else
    // happens to repaint.
    struct Repainter : juce::ChangeListener
    {
        juce::Component* owner = nullptr;
        void changeListenerCallback(juce::ChangeBroadcaster*) override { if (owner) owner->repaint(); }
    };

    void select(int index)
    {
        if (index < 0 || index >= static_cast<int>(takes.size())) return;
        selected = index;
        updateLayout();
        if (onSelected) onSelected(takes[static_cast<size_t>(index)].file);
    }

    void updateLayout()
    {
        repainter.owner = this;
        auto area = getExpandedContentArea();
        for (auto* c : hosted)
            if (c != nullptr) c->setVisible(!area.isEmpty());
        if (onHeightChanged) onHeightChanged();
        repaint();
    }

    void paintRow(juce::Graphics& g, const Take& take, juce::Rectangle<int> row, bool isSelected)
    {
        if (isSelected)
        {
            g.setColour(MiraLookAndFeel::surface2);
            g.fillRect(row.withHeight(kExpandedHeight));
        }

        auto text = row.reduced(8, 0);

        // A disclosure triangle, pointing down when open. The only affordance saying the
        // row does anything at all.
        juce::Path tri;
        auto t = text.removeFromLeft(12).toFloat();
        float cx = t.getCentreX(), cy = t.getCentreY();
        if (isSelected) tri.addTriangle(cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
        else            tri.addTriangle(cx - 2, cy - 4, cx + 3, cy, cx - 2, cy + 4);
        g.setColour(isSelected ? MiraLookAndFeel::text : MiraLookAndFeel::textDim);
        g.fillPath(tri);

        text.removeFromLeft(4);
        auto wave = text.removeFromRight(juce::jmin(180, text.getWidth() / 3));

        g.setColour(isSelected ? MiraLookAndFeel::text : MiraLookAndFeel::textDim);
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawText(take.file.getFileNameWithoutExtension(), text, juce::Justification::centredLeft, true);

        if (take.thumbnail != nullptr && take.thumbnail->getTotalLength() > 0.0)
        {
            g.setColour(MiraLookAndFeel::textDim.withAlpha(isSelected ? 0.85f : 0.45f));
            take.thumbnail->drawChannels(g, wave.reduced(2, 5), 0.0, take.thumbnail->getTotalLength(), 1.0f);
        }
    }

    const MiraLookAndFeel& laf;
    juce::AudioFormatManager& formatManager;
    juce::AudioThumbnailCache& cache;
    std::vector<Take> takes;
    int selected = -1;
    std::vector<juce::Component*> hosted;
    Repainter repainter;
};
