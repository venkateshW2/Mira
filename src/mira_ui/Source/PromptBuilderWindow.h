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
    // The same counts again, split by the trigger the sidecar carried. Randomise needs
    // this: "surprise me" is only useful if it surprises you with things THIS LoRA was
    // actually trained on.
    std::map<juce::String, std::map<juce::String, Vocab>> vocabByTrigger;
    void scanVocabulary(const juce::File& studioRoot);
    std::vector<juce::String> byFrequency(const juce::String& field) const;

    // Pools the counts for `field` over the triggers currently in the trigger row (or
    // over everything, when that row is empty). Two triggers means both corpora's counts
    // added together, so a word common in one and absent in the other is likelier but
    // the other's own words can still come up -- which is the interesting part of a
    // blend.
    Vocab pooled(const juce::String& field) const;
    // Frequency-WEIGHTED, not uniform: uniform picking makes "Rhythm: sparse" (7 of 72
    // NIN files) as likely as "driving" (95), which is how a prompt ends up asking for a
    // corner the model barely knows. Returns empty when there is nothing to draw from.
    juce::String weightedPick(const Vocab& from) const;
    void randomise();
    juce::Random rng;
    // Field key -> the vocabulary key it draws from, so randomise() knows where to look
    // without re-deriving it.
    std::map<juce::String, juce::String> vocabKeyForField;

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

    // The picker is a SHORTCUT INTO the text box, never the source of truth: build()
    // reads the text and nothing else. So anything that writes the text behind the
    // picker's back -- Clear, Randomise -- has to put the picker back in step, or it sits
    // showing a value the field no longer holds.
    //
    // That is not merely cosmetic. A ComboBox does NOT fire onChange when you re-select
    // what is already selected, so a stale picker makes the value it is stuck on
    // UNPICKABLE: choose "tonal", press Clear, choose "tonal" again -- nothing happens,
    // and the row looks broken. Call this after every programmatic write to `value`.
    static void syncPicker(Field& f);

    // Nineteen fields at 27px need 513px; the layout gave them ~414. removeFromTop on an
    // exhausted rectangle returns an empty one, so the last four -- Motion, Keyscale, BPM
    // and the free-text tail -- were laid out at zero height: present, invisible, and
    // reported as "BPM and key are missing from the prompt builder". They were never
    // missing. A Viewport means adding a field can never again silently delete one.
    struct FieldsHolder : juce::Component {
        void paint(juce::Graphics& g) override { g.fillAll(juce::Colour(0xff1a1a1a)); }
    };
    FieldsHolder fieldsHolder;
    juce::Viewport fieldsView;

    const MiraLookAndFeel& laf;
    juce::Label headerLabel;
    std::vector<std::unique_ptr<Field>> fields;
    Field& addField(const juce::String& key, const juce::String& shown,
                    const juce::String& vocabKey, bool multi);

    juce::TextButton randomiseButton { "Randomise" };
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
        centreWithSize(660, 680);   // tall enough for all 19 rows; the Viewport covers smaller screens
        setAlwaysOnTop(true);
        setVisible(true);
        toFront(true);
    }

    void closeButtonPressed() override { if (onClosed) onClosed(); }
    std::function<void()> onClosed;
    PromptBuilderContent* content = nullptr;
};
