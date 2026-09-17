#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include <vector>

#include "MiraLookAndFeel.h"

// MIRA-GENERATE.md Phase 4 -- the take stack.
//
// Three sections, in this order: KEPT, TAKES, DISCARDED. "so ones u keep it goes away
// from the project window, instead move it on top so i can keep checking what the keep
// files are from the same window". A kept take leaving the window was wrong: keeping is
// the decision the whole session is FOR, and the record of what has been kept so far is
// the thing you steer by. Discarded rows stay too, greyed, as a record of what was
// already rejected -- which stops the same idea being generated and thrown away twice.
//
// Rows expand INDEPENDENTLY and nothing auto-collapses ("let the take not auto collapse,
// let two or be open so easy to play"). Two takes can sit open side by side for
// comparison. Exactly one of them is FOCUSED, and the focused one gets the live
// transport: there is one WaveformView in the window because it owns an
// AudioDeviceManager, so one row at a time can play. Other expanded rows draw their own
// static waveform from a thumbnail, which is cheap. Clicking an expanded row focuses it.
class TakeStack : public juce::Component
{
public:
    enum class State { Kept, Pending, Discarded };

    TakeStack(const MiraLookAndFeel& lafIn, juce::AudioFormatManager& formatManagerIn,
               juce::AudioThumbnailCache& cacheIn)
        : laf(lafIn), formatManager(formatManagerIn), cache(cacheIn)
    {
        repainter.owner = this;
    }

    static constexpr int kRowHeight = 28;
    static constexpr int kHeaderHeight = 22;
    static constexpr int kExpandedBody = 172; // static waveform + caption, below the row
    // The FOCUSED row is taller, by exactly what WaveformView spends on chrome it draws
    // inside itself: a time ruler along the top and a transport strip along the bottom.
    // Without this the playing take's waveform came out visibly SHORTER than the static
    // ones beside it -- the rows were the same height, but only one of them was giving
    // most of that height to the waveform.
    static constexpr int kTransportChrome = 64;

    void addTake(const juce::File& file, State state = State::Pending, bool expand = true)
    {
        for (auto& t : takes)
            if (t.file == file) { if (expand) focus(t.file); return; }

        Take take;
        take.file = file;
        take.state = state;
        take.expanded = expand;
        if (file.existsAsFile())
        {
            take.thumbnail = std::make_unique<juce::AudioThumbnail>(256, formatManager, cache);
            take.thumbnail->setSource(new juce::FileInputSource(file));
            take.thumbnail->addChangeListener(&repainter);
        }
        takes.push_back(std::move(take));
        if (expand) focusedFile = file;
        rebuild();
    }

    // Keep moved the file into the cue folder, so the row follows it there rather than
    // being removed and re-added -- the row keeps its place and its expansion.
    void markKept(const juce::File& oldFile, const juce::File& newFile)
    {
        for (auto& t : takes)
        {
            if (t.file != oldFile) continue;
            t.file = newFile;
            t.state = State::Kept;
            if (newFile.existsAsFile())
            {
                t.thumbnail = std::make_unique<juce::AudioThumbnail>(256, formatManager, cache);
                t.thumbnail->setSource(new juce::FileInputSource(newFile));
                t.thumbnail->addChangeListener(&repainter);
            }
            if (focusedFile == oldFile) focusedFile = newFile;
            rebuild();
            return;
        }
    }

    // The file is in the Trash; the row stays as a record. Its thumbnail goes, because
    // there is nothing left to draw and a stale one would be a picture of a file that no
    // longer exists.
    void markDiscarded(const juce::File& file)
    {
        for (auto& t : takes)
        {
            if (t.file != file) continue;
            t.state = State::Discarded;
            t.expanded = false;
            t.thumbnail.reset();
            if (focusedFile == file) focusedFile = juce::File();
            rebuild();
            return;
        }
    }

    void forget(const juce::File& file)
    {
        for (size_t i = 0; i < takes.size(); ++i)
            if (takes[i].file == file)
            {
                if (focusedFile == file) focusedFile = juce::File();
                takes.erase(takes.begin() + static_cast<long>(i));
                rebuild();
                return;
            }
    }

    void clear() { takes.clear(); focusedFile = juce::File(); rebuild(); }

    juce::File getFocusedFile() const { return focusedFile; }
    int getPendingCount() const { return countOf(State::Pending); }
    int getTotalCount() const { return static_cast<int>(takes.size()); }

    void setHostedComponents(std::vector<juce::Component*> components)
    {
        for (auto* c : hosted)
            if (c != nullptr && c->getParentComponent() == this) removeChildComponent(c);
        hosted = std::move(components);
        for (auto* c : hosted)
            if (c != nullptr) addAndMakeVisible(c);
        rebuild();
    }

    // Where the live transport and the per-take buttons go: inside the FOCUSED expanded
    // row. Empty when nothing is focused, and the owner then leaves those components
    // unplaced rather than guessing at a position.
    juce::Rectangle<int> getFocusedContentArea() const
    {
        int y = 0;
        for (const auto& row : rows)
        {
            if (row.kind == RowKind::Header) { y += kHeaderHeight; continue; }
            const auto& take = takes[row.takeIndex];
            if (take.file == focusedFile && take.expanded)
                return { 6, y + kRowHeight, juce::jmax(0, getWidth() - 12),
                          kExpandedBody + kTransportChrome - 6 };
            y += kRowHeight + bodyHeightFor(take);
        }
        return {};
    }

    std::function<void(const juce::File&)> onFocused;
    std::function<void()> onHeightChanged;

    int getIdealHeight() const
    {
        int h = 0;
        for (const auto& row : rows)
        {
            if (row.kind == RowKind::Header) { h += kHeaderHeight; continue; }
            h += kRowHeight + bodyHeightFor(takes[row.takeIndex]);
        }
        return juce::jmax(h, kRowHeight);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(MiraLookAndFeel::surface);

        if (takes.empty())
        {
            g.setColour(MiraLookAndFeel::textDim);
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText("no takes yet", getLocalBounds().reduced(10, 0), juce::Justification::centredLeft);
            return;
        }

        int y = 0;
        for (const auto& row : rows)
        {
            if (row.kind == RowKind::Header)
            {
                paintHeader(g, { 0, y, getWidth(), kHeaderHeight }, row.headerText, row.headerCount);
                y += kHeaderHeight;
                continue;
            }
            const auto& take = takes[row.takeIndex];
            paintRow(g, take, { 0, y, getWidth(), kRowHeight });
            y += kRowHeight + bodyHeightFor(take);
        }
    }

    void resized() override { rebuild(); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int y = 0;
        for (const auto& row : rows)
        {
            if (row.kind == RowKind::Header) { y += kHeaderHeight; continue; }
            auto& take = takes[row.takeIndex];
            const int bodyHeight = bodyHeightFor(take);

            if (e.y >= y && e.y < y + kRowHeight)
            {
                // The triangle toggles open/shut; anywhere else on the row focuses it.
                // Separated on purpose: with several rows open, "click to play this one"
                // and "click to close this one" must not be the same gesture.
                if (e.x < 22) { take.expanded = !take.expanded; if (take.expanded) focusedFile = take.file; rebuild(); }
                else          { take.expanded = true; focus(take.file); }
                return;
            }
            // A click in an open row's body focuses it without collapsing anything --
            // this is what makes two open takes A/B-able in one click.
            if (bodyHeight > 0 && e.y >= y + kRowHeight && e.y < y + kRowHeight + bodyHeight)
            {
                if (take.file != focusedFile) focus(take.file);
                return;
            }
            y += kRowHeight + bodyHeight;
        }
    }

private:
    struct Take
    {
        juce::File file;
        State state = State::Pending;
        bool expanded = false;
        std::unique_ptr<juce::AudioThumbnail> thumbnail;
    };

    enum class RowKind { Header, Take };
    struct Row
    {
        RowKind kind = RowKind::Take;
        size_t takeIndex = 0;
        juce::String headerText;
        int headerCount = 0;
    };

    struct Repainter : juce::ChangeListener
    {
        juce::Component* owner = nullptr;
        void changeListenerCallback(juce::ChangeBroadcaster*) override { if (owner) owner->repaint(); }
    };

    int bodyHeightFor(const Take& t) const
    {
        if (!t.expanded) return 0;
        return kExpandedBody + (t.file == focusedFile ? kTransportChrome : 0);
    }

    int countOf(State s) const
    {
        int n = 0;
        for (const auto& t : takes) if (t.state == s) ++n;
        return n;
    }

    void focus(const juce::File& file)
    {
        focusedFile = file;
        rebuild();
        if (onFocused) onFocused(file);
    }

    // Rebuilds the visible row order: KEPT, then TAKES, then DISCARDED, each with a
    // header and only when it has anything in it. Order within a section is the order
    // takes were added, so the list does not reshuffle under the pointer.
    void rebuild()
    {
        rows.clear();
        auto section = [this](State state, const char* title) {
            const int n = countOf(state);
            if (n == 0) return;
            Row header;
            header.kind = RowKind::Header;
            header.headerText = title;
            header.headerCount = n;
            rows.push_back(header);
            for (size_t i = 0; i < takes.size(); ++i)
                if (takes[i].state == state) { Row r; r.takeIndex = i; rows.push_back(r); }
        };
        section(State::Kept, "KEPT");
        section(State::Pending, "TAKES");
        section(State::Discarded, "DISCARDED");

        auto area = getFocusedContentArea();
        for (auto* c : hosted)
            if (c != nullptr) c->setVisible(!area.isEmpty());

        if (onHeightChanged) onHeightChanged();
        repaint();
    }

    void paintHeader(juce::Graphics& g, juce::Rectangle<int> area, const juce::String& text, int count)
    {
        g.setColour(MiraLookAndFeel::textDim);
        g.setFont(juce::Font(juce::FontOptions(10.0f, juce::Font::bold)));
        g.drawText(text + "  (" + juce::String(count) + ")", area.reduced(10, 0),
                    juce::Justification::centredLeft);
        g.setColour(MiraLookAndFeel::textDim.withAlpha(0.2f));
        g.fillRect(area.getX() + 10, area.getBottom() - 1, area.getWidth() - 20, 1);
    }

    void paintRow(juce::Graphics& g, const Take& take, juce::Rectangle<int> row)
    {
        const bool isFocused = take.file == focusedFile;
        const bool dimmed = take.state == State::Discarded;

        if (take.expanded)
        {
            g.setColour(isFocused ? MiraLookAndFeel::surface2
                                  : MiraLookAndFeel::surface2.withAlpha(0.45f));
            g.fillRect(row.withHeight(kRowHeight + bodyHeightFor(take)));
        }
        if (isFocused)
        {
            // A left edge marking which row the transport belongs to. With several rows
            // open, nothing else says which one the Play button will play.
            g.setColour(MiraLookAndFeel::accent);
            g.fillRect(row.getX(), row.getY(), 3, kRowHeight);
        }

        auto text = row.reduced(10, 0);
        auto tri = text.removeFromLeft(12).toFloat();
        if (!dimmed)
        {
            juce::Path p;
            float cx = tri.getCentreX(), cy = tri.getCentreY();
            if (take.expanded) p.addTriangle(cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
            else               p.addTriangle(cx - 2, cy - 4, cx + 3, cy, cx - 2, cy + 4);
            g.setColour(take.expanded ? MiraLookAndFeel::text : MiraLookAndFeel::textDim);
            g.fillPath(p);
        }

        text.removeFromLeft(6);
        auto wave = text.removeFromRight(juce::jmin(190, text.getWidth() / 3));

        g.setColour(dimmed ? MiraLookAndFeel::textDim.withAlpha(0.5f)
                           : (isFocused ? MiraLookAndFeel::text : MiraLookAndFeel::textDim));
        g.setFont(juce::Font(juce::FontOptions(12.0f)));
        g.drawText(take.file.getFileNameWithoutExtension(), text, juce::Justification::centredLeft, true);

        // The mini waveform is the COLLAPSED row's only picture of the take. An expanded
        // row already shows the full-size one below, and drawing both was just two
        // waveforms of the same audio on one row ("there is also a small waveform, why
        // two waveforms").
        if (!take.expanded && take.thumbnail != nullptr && take.thumbnail->getTotalLength() > 0.0)
        {
            g.setColour(MiraLookAndFeel::textDim.withAlpha(0.45f));
            take.thumbnail->drawChannels(g, wave.reduced(2, 5), 0.0, take.thumbnail->getTotalLength(), 1.0f);
        }

        // An expanded row that is NOT focused draws its own large waveform here. The
        // focused one leaves this area empty because the live WaveformView is sitting in
        // it -- one transport, whichever row holds it.
        if (take.expanded && !isFocused && take.thumbnail != nullptr
            && take.thumbnail->getTotalLength() > 0.0)
        {
            juce::Rectangle<int> body { row.getX() + 6, row.getBottom(),
                                         row.getWidth() - 12, kExpandedBody - 40 };
            g.setColour(MiraLookAndFeel::textDim.withAlpha(0.55f));
            take.thumbnail->drawChannels(g, body.reduced(2), 0.0, take.thumbnail->getTotalLength(), 1.0f);
            g.setColour(MiraLookAndFeel::textDim);
            g.setFont(juce::Font(juce::FontOptions(11.0f)));
            g.drawText("click to play this take", body.withY(body.getBottom() + 6).withHeight(20),
                        juce::Justification::centred);
        }
    }

    const MiraLookAndFeel& laf;
    juce::AudioFormatManager& formatManager;
    juce::AudioThumbnailCache& cache;
    std::vector<Take> takes;
    std::vector<Row> rows;
    juce::File focusedFile;
    std::vector<juce::Component*> hosted;
    Repainter repainter;
};
