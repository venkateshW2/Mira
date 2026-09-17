#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

#include "mira/db/Database.h"
#include "MiraLookAndFeel.h"
#include "NativeWindowChrome.h"

// "Prepare for Training" — captions, encodes, and (optionally) ships a folder to a GPU
// box, without a terminal.
//
// It DRIVES sa3-studio/prepare-lora.sh rather than reimplementing it. That seam is the
// whole point:
//
//   - mira never holds an SSH key and gains no SSH dependency; credentials stay in
//     ~/.ssh where ssh already knows how to find them.
//   - Switching GPU provider is a field in this window, not a rebuild.
//   - The CLI and the UI cannot drift, because there is one implementation. Same reason
//     caption/TagVocabulary.h is shared between them.
//
// What the script does, in the order it MUST happen (see its header for why): mira
// captions beside each source file -> MLX pre-encode to .npy + .json -> 8 KB silent
// stand-in .wav files (underfit finds tags by walking for AUDIO, so a latents-only folder
// silently trains on the trigger alone) -> verify the tags survived -> zip -> rsync and
// register the dataset.
class PrepareContent : public juce::Component
{
public:
    PrepareContent(const MiraLookAndFeel& lafIn, juce::File studioRootIn,
                    mira::Database& databaseIn);
    ~PrepareContent() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    // Pre-fills the folder, so the folder tree's right-click can open this already aimed
    // at what was clicked.
    void setFolder(const juce::File& folder);

private:
    void chooseFolder();
    void run(bool push);
    void stop();
    void onProcessFinished(int exitCode);
    void refreshTagsLabel();
    void appendLog(const juce::String& text);
    void setRunning(bool running);

    juce::File hostSettingsFile() const;
    juce::String loadHost() const;
    void saveHost(const juce::String& host) const;

    const MiraLookAndFeel& laf;
    juce::File studioRoot;
    mira::Database& database;

    juce::File folder;
    juce::Label folderCaption, folderValue;
    juce::TextButton chooseButton { "Choose..." };

    juce::Label triggerCaption, tagsCaption, tagsValue, maxDurCaption, hostCaption;
    juce::TextEditor triggerField, maxDurField, hostField;

    juce::TextButton encodeButton { "Encode only" };
    juce::TextButton pushButton { "Encode & Push" };
    juce::TextButton stopButton { "Stop" };

    juce::TextEditor log;
    // readProcessOutput() BLOCKS until the child writes or its pipe closes, so it can
    // never run on the message thread: step 1 loops `mira caption` over every file and
    // emits nothing for a long stretch, which froze the whole app. A reader thread does
    // the blocking and posts lines back with callAsync.
    class OutputReader;
    std::unique_ptr<juce::ChildProcess> proc;
    std::unique_ptr<OutputReader> reader;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PrepareContent)
};

class PrepareWindow : public juce::DocumentWindow
{
public:
    PrepareWindow(const MiraLookAndFeel& laf, juce::File studioRoot, mira::Database& db)
        : juce::DocumentWindow("Prepare for Training", MiraLookAndFeel::surface,
                                juce::DocumentWindow::allButtons)
    {
        // mira's own title bar, not the OS one -- every window in the app matches the
        // browser now. The native bar cannot take MiraLookAndFeel's colours at all, so a
        // window wearing one sits visibly apart from the rest.
        setUsingNativeTitleBar(false);
        setTitleBarHeight(30);
        setContentOwned(new PrepareContent(laf, std::move(studioRoot), db), false);
        setResizable(true, false);
        centreWithSize(700, 620);
        setVisible(true);
        mira_ui::chrome::applyRoundedCorners(*this, 10.0f); // needs the peer, so after setVisible
        toFront(true);
    }

    std::function<void()> onClosed;

    void setFolder(const juce::File& f)
    {
        if (auto* c = dynamic_cast<PrepareContent*>(getContentComponent())) c->setFolder(f);
    }

    void closeButtonPressed() override { if (onClosed) onClosed(); }
};
