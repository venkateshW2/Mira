#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <functional>
#include <map>
#include <vector>

#include "MiraLookAndFeel.h"

// A prompt is only as good as its SHAPE. underfit builds every training caption as
// "<trigger>, Key: value, Key: value, ..." (see the demo prompts in any run's
// demos/*.json), and a LoRA learns "Instruments: strings, brass" -- not a bare list of
// words after the trigger. Typing "dkt,menace, strings,brass" by hand is off
// distribution, and the higher the CFG the harder the model chases that malformed text.
//
// This panel is a FORMATTER, not a recommender. Every dropdown offers every value found
// anywhere in the local latent sidecars, deliberately NOT filtered to the LoRA you have
// loaded: prompting one corpus's vocabulary at another's adapter is an experiment worth
// being able to run, not a mistake to be prevented. Everything is free-text editable
// afterwards, and Construct writes into the prompt box rather than replacing the box.
class PromptBuilderContent : public juce::Component
{
public:
    PromptBuilderContent(const MiraLookAndFeel& laf, juce::File studioRoot);

    void resized() override;
    void paint(juce::Graphics&) override;

    // Fired by Construct. The Generate window drops the text into its prompt editor.
    std::function<void(const juce::String&)> onConstruct;

private:
    // value -> how many sidecars carry it, over every sa3-studio/latents/<film>/ folder.
    // Frequency only decides menu ORDER -- the most-used words sit at the top, where
    // they are easiest to hit -- it never hides anything.
    using Vocab = std::map<juce::String, int>;
    std::map<juce::String, Vocab> vocab;
    void scanVocabulary(const juce::File& studioRoot);
    std::vector<juce::String> byFrequency(const juce::String& field) const;

    juce::String build() const;

    // One dropdown, one editable text field. The dropdown APPENDS to the field rather
    // than replacing it, which is what makes multi-value fields (instruments, moods)
    // work without a custom multi-select component -- and it keeps every field
    // hand-editable, including to values no corpus has ever used.
    struct Field {
        juce::String key;          // emitted as "Key: ..."; empty field is omitted
        juce::Label label;
        juce::ComboBox picker;     // empty when the field is free text only
        juce::TextEditor value;
        bool multi = false;
    };

    const MiraLookAndFeel& laf;
    juce::Label headerLabel;
    std::vector<std::unique_ptr<Field>> fields;
    Field& addField(const juce::String& key, const juce::String& shown,
                    const juce::String& vocabKey, bool multi);

    juce::TextButton constructButton { "Construct prompt" };
    juce::TextButton clearButton { "Clear" };
    juce::Label previewLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PromptBuilderContent)
};

// Floating, like GenerateWindow: it is used WHILE the generate window is open, so it
// must not fall behind it.
class PromptBuilderWindow : public juce::DocumentWindow
{
public:
    // Takes the content rather than making it: the Generate window OWNS it, so closing
    // this window (which Construct does) keeps every dropdown and field exactly as it
    // was. Reopening resumes instead of starting over.
    explicit PromptBuilderWindow(PromptBuilderContent* existing)
        : juce::DocumentWindow("Construct Prompt", MiraLookAndFeel::surface,
                                juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        content = existing;
        setContentNonOwned(content, false);
        setResizable(true, false);
        centreWithSize(660, 560);
        setAlwaysOnTop(true);
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override { if (onClosed) onClosed(); }
    std::function<void()> onClosed;
    PromptBuilderContent* content = nullptr;
};
