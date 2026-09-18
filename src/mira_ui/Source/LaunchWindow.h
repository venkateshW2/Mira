#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

#include "MiraLookAndFeel.h"

// What mira shows when it opens, instead of dropping you straight into the library.
//
// The problem it solves is not visual. mira has two quite different jobs -- organise and
// analyse a library, or run a project and generate into it -- and until now it opened the
// browser every time and left you to work out that the other half existed and how to
// reach it. Every editor worth using asks this question at launch (Xcode, VS Code,
// Ableton, Logic) for the same reason: the first screen should name the choices rather
// than assume one.
//
// Deliberately small, and deliberately not a window you can get stuck in: picking
// anything closes it, and closing it without picking leaves the app running with the
// glyph in the menu bar.
class LaunchContent : public juce::Component
{
public:
    std::function<void()> onNewProject;
    std::function<void()> onOpenProject;
    std::function<void()> onOpenLibrary;
    std::function<void(juce::File)> onRecentChosen;

    LaunchContent(const MiraLookAndFeel& lafIn, juce::StringArray recentPaths)
        : laf(lafIn), recent(std::move(recentPaths))
    {
        title.setText("MIRA", juce::dontSendNotification);
        title.setFont(juce::Font(juce::FontOptions(26.0f, juce::Font::bold)));
        title.setColour(juce::Label::textColourId, MiraLookAndFeel::text);
        addAndMakeVisible(title);

        subtitle.setText("Open a project to generate, or the library to organise and analyse.",
                          juce::dontSendNotification);
        subtitle.setFont(juce::Font(juce::FontOptions(12.0f)));
        subtitle.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
        addAndMakeVisible(subtitle);

        auto button = [this](juce::TextButton& b, const juce::String& text, std::function<void()>* cb) {
            b.setButtonText(text);
            b.onClick = [cb] { if (cb && *cb) (*cb)(); };
            addAndMakeVisible(b);
        };
        button(newProjectButton, "New Project...", &onNewProject);
        button(openProjectButton, "Open Project...", &onOpenProject);
        button(libraryButton, "Open Library", &onOpenLibrary);

        recentLabel.setText(recent.isEmpty() ? "No recent projects" : "Recent", juce::dontSendNotification);
        recentLabel.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
        recentLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::accent.withAlpha(0.85f));
        addAndMakeVisible(recentLabel);

        recentList.setModel(this ? nullptr : nullptr); // set below, after the model exists
        addAndMakeVisible(recentList);
        recentList.setRowHeight(24);
        recentList.setColour(juce::ListBox::backgroundColourId, MiraLookAndFeel::surface2);
        recentList.setModel(&model);
        model.owner = this;
    }

    void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(24);
        title.setBounds(r.removeFromTop(32));
        subtitle.setBounds(r.removeFromTop(20));
        r.removeFromTop(16);

        auto row = r.removeFromTop(30);
        newProjectButton.setBounds(row.removeFromLeft(140));
        row.removeFromLeft(8);
        openProjectButton.setBounds(row.removeFromLeft(140));
        row.removeFromLeft(8);
        libraryButton.setBounds(row.removeFromLeft(130));

        r.removeFromTop(18);
        recentLabel.setBounds(r.removeFromTop(16));
        r.removeFromTop(4);
        recentList.setBounds(r);
    }

private:
    struct Model : juce::ListBoxModel
    {
        LaunchContent* owner = nullptr;
        int getNumRows() override { return owner ? owner->recent.size() : 0; }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (owner == nullptr || !juce::isPositiveAndBelow(row, owner->recent.size())) return;
            if (selected) { g.setColour(MiraLookAndFeel::accent.withAlpha(0.20f)); g.fillRect(0, 0, w, h); }
            juce::File f (owner->recent[row]);
            g.setColour(MiraLookAndFeel::text);
            g.setFont(juce::Font(juce::FontOptions(12.0f)));
            g.drawText(f.getFileName(), 10, 0, w - 20, h, juce::Justification::centredLeft, true);
            // The parent folder, dimmed: two projects can share a name and the path is
            // the only thing that tells them apart.
            g.setColour(MiraLookAndFeel::textFaint);
            g.setFont(juce::Font(juce::FontOptions(10.0f)));
            g.drawText(f.getParentDirectory().getFullPathName(), 10, 0, w - 20, h,
                        juce::Justification::centredRight, true);
        }
        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override { choose(row); }
        void returnKeyPressed(int row) override { choose(row); }
        void choose(int row)
        {
            if (owner == nullptr || !juce::isPositiveAndBelow(row, owner->recent.size())) return;
            if (owner->onRecentChosen) owner->onRecentChosen(juce::File(owner->recent[row]));
        }
    };

    const MiraLookAndFeel& laf;
    juce::StringArray recent;
    juce::Label title, subtitle, recentLabel;
    juce::TextButton newProjectButton, openProjectButton, libraryButton;
    juce::ListBox recentList;
    Model model;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(LaunchContent)
};

class LaunchWindow : public juce::DocumentWindow
{
public:
    std::function<void()> onClosed;

    LaunchWindow(const MiraLookAndFeel& laf, juce::StringArray recent)
        : juce::DocumentWindow("MIRA", MiraLookAndFeel::surface, juce::DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(false);
        setTitleBarHeight(30);
        content = new LaunchContent(laf, std::move(recent));
        setContentOwned(content, false);
        setResizable(false, false);
        centreWithSize(540, 380);
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override { if (onClosed) onClosed(); }

    LaunchContent* content = nullptr;
};
