#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "MiraLookAndFeel.h"

#include <deque>
#include <mutex>
#include "NativeWindowChrome.h"

// Review round 5: "in the osx bar - can we have a log - so we get to see the cli logs and
// figure out."
//
// mira_ui runs the real `mira` binary as a child process for analysis (AnalyzeJob), and
// until now everything that process said was thrown away except the one `progress:` line
// the readout needed. That is the wrong thing to discard on a 12 x 41-minute score-stem
// run: when a stem comes back mislabelled, what the analyzer actually printed on the way
// there is the first evidence, and it was only ever visible by running the CLI by hand.
//
// A ring buffer, not a file: this is a live window into the current session, and mira
// already writes nothing to disk it doesn't have a reason to. kMaxLines is a few thousand
// so a long overnight run can't grow it without bound, and the oldest lines are the ones
// least likely to matter by the time anyone looks.
class LogStore : public juce::ChangeBroadcaster
{
public:
    enum class Source { app, analyze, scan };

    // Safe to call from any thread -- AnalyzeJob's reader thread is the main caller, and
    // it has no business marshalling every line through the message thread just to store
    // it. The change message it broadcasts is what reaches the UI, and JUCE already
    // delivers that on the message thread.
    void append(Source source, const juce::String& line)
    {
        if (line.isEmpty()) return;
        {
            const std::scoped_lock lock(mutex);
            lines.push_back({ juce::Time::getCurrentTime(), source, line });
            while (lines.size() > kMaxLines) lines.pop_front();
        }
        sendChangeMessage();
    }

    juce::String render() const
    {
        const std::scoped_lock lock(mutex);
        juce::StringArray out;
        for (const auto& entry : lines)
            out.add(entry.time.toString(false, true, true, true) + "  " + prefixFor(entry.source)
                     + "  " + entry.text);
        return out.joinIntoString("\n");
    }

    void clear()
    {
        {
            const std::scoped_lock lock(mutex);
            lines.clear();
        }
        sendChangeMessage();
    }

    bool isEmpty() const
    {
        const std::scoped_lock lock(mutex);
        return lines.empty();
    }

private:
    static juce::String prefixFor(Source source)
    {
        switch (source)
        {
            case Source::analyze: return "[analyze]";
            case Source::scan:    return "[scan]   ";
            default:              return "[mira]   ";
        }
    }

    struct Entry
    {
        juce::Time time;
        Source source;
        juce::String text;
    };

    static constexpr size_t kMaxLines = 4000;
    mutable std::mutex mutex;
    std::deque<Entry> lines;
};

// The window's content. Read-only multi-line editor rather than a TextEditor the user can
// type into, or a ListBox: this is log text that gets selected and copied into a bug
// report, and a plain editor already does selection, copy and find-by-eye correctly.
class LogComponent : public juce::Component, private juce::ChangeListener
{
public:
    explicit LogComponent(LogStore& storeIn, const MiraLookAndFeel& lafIn) : store(storeIn), laf(lafIn)
    {
        text.setMultiLine(true, false); // no word wrap -- a wrapped path is harder to read than a scrolled one
        text.setReadOnly(true);
        text.setScrollbarsShown(true);
        text.setCaretVisible(false);
        text.setFont(laf.monoRegular(11.5f));
        text.setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::bg);
        text.setColour(juce::TextEditor::textColourId, MiraLookAndFeel::textDim);
        text.setColour(juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        text.setColour(juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible(text);

        for (auto* b : { &followButton, &copyButton, &clearButton })
        {
            b->setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
            b->setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
            b->setColour(juce::TextButton::textColourOnId, MiraLookAndFeel::accent);
            addAndMakeVisible(*b);
        }
        followButton.setClickingTogglesState(true);
        followButton.setToggleState(true, juce::dontSendNotification);
        // Follow is a toggle, not always-on: scrolling back to read something and having
        // the next analyzed file yank you to the bottom is the single most annoying thing
        // a log window can do.
        copyButton.onClick = [this] { juce::SystemClipboard::copyTextToClipboard(store.render()); };
        clearButton.onClick = [this] { store.clear(); };

        store.addChangeListener(this);
        refresh();
    }

    ~LogComponent() override { store.removeChangeListener(this); }

    void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface); }

    void resized() override
    {
        auto bounds = getLocalBounds();
        auto controls = bounds.removeFromTop(30).reduced(8, 4);
        followButton.setBounds(controls.removeFromLeft(70));
        controls.removeFromLeft(6);
        copyButton.setBounds(controls.removeFromLeft(70));
        controls.removeFromLeft(6);
        clearButton.setBounds(controls.removeFromLeft(70));
        text.setBounds(bounds.reduced(8, 0).withTrimmedBottom(8));
    }

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }

    void refresh()
    {
        text.setText(store.render(), false);
        if (followButton.getToggleState()) text.moveCaretToEnd();
    }

    LogStore& store;
    const MiraLookAndFeel& laf;
    juce::TextEditor text;
    juce::TextButton followButton { "Follow" }, copyButton { "Copy" }, clearButton { "Clear" };
};

// Its own window rather than a panel in the main one: the log is a diagnostic you open
// when something looks wrong, want beside the main window while a long run drains, and
// close again -- not a permanent part of the layout competing for the space the file list
// and waveform already contend for.
class LogWindow : public juce::DocumentWindow
{
public:
    LogWindow(LogStore& store, const MiraLookAndFeel& laf)
        : juce::DocumentWindow("MIRA Log", MiraLookAndFeel::surface, juce::DocumentWindow::allButtons)
    {
        // mira's own title bar, not the OS one -- every window in the app matches the
        // browser now. The native bar cannot take MiraLookAndFeel's colours at all, so a
        // window wearing one sits visibly apart from the rest.
        setUsingNativeTitleBar(false);
        setTitleBarHeight(30);
        setContentOwned(new LogComponent(store, laf), false);
        setResizable(true, false);
        centreWithSize(820, 460);
        setVisible(true);
        mira_ui::chrome::applyRoundedCorners(*this, 10.0f); // needs the peer, so after setVisible
        // Explicitly, not just setVisible: a newly created DocumentWindow can otherwise
        // come up *behind* the main window, which looks exactly like the menu item having
        // done nothing (observed on macOS 15.5).
        toFront(true);
    }

    std::function<void()> onClosed;

    // The window owns nothing the app needs, so closing it is just closing it -- the
    // LogStore lives in MainComponent and keeps collecting either way, so reopening shows
    // everything that happened while it was shut.
    void closeButtonPressed() override
    {
        if (onClosed) onClosed();
    }
};
