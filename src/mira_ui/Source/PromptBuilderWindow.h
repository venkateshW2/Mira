#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <functional>
#include <map>
#include <vector>

#include "MiraLookAndFeel.h"
#include "NativeWindowChrome.h"

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
    // A die, drawn rather than fetched: three pips on a rounded square, which reads as
    // "roll this" at 18px where a word would not fit at all.
    struct DiceButton : juce::Button {
        DiceButton() : juce::Button("roll") {}
        void paintButton(juce::Graphics& g, bool over, bool down) override {
            auto r = getLocalBounds().toFloat().reduced(2.0f);
            if (over || down)
                { g.setColour(MiraLookAndFeel::surface2.brighter(down ? 0.22f : 0.10f));
                  g.fillRoundedRectangle(r, 4.0f); }
            const auto ink = isEnabled() ? (over ? MiraLookAndFeel::text : MiraLookAndFeel::textDim)
                                          : MiraLookAndFeel::textFaint;
            g.setColour(ink);
            auto face = r.reduced(r.getWidth() * 0.18f, r.getHeight() * 0.18f);
            g.drawRoundedRectangle(face, 3.0f, 1.3f);
            const float pip = juce::jmax(1.4f, face.getWidth() * 0.11f);
            auto dot = [&](float fx, float fy) {
                g.fillEllipse(face.getX() + face.getWidth() * fx - pip,
                              face.getY() + face.getHeight() * fy - pip, pip * 2.0f, pip * 2.0f);
            };
            dot(0.28f, 0.28f); dot(0.5f, 0.5f); dot(0.72f, 0.72f);
        }
    };

    struct Field {
        juce::String key;          // emitted as "Key: ..."; empty field is omitted
        juce::Label label;
        juce::ComboBox picker;     // empty when the field is free text only
        juce::TextEditor value;
        // "per field randomizer": rolls THIS row only. Randomise-all is the same code in
        // a loop, so the two can never drift into disagreeing about what a field's
        // vocabulary is.
        DiceButton roll;
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

    // One row. Writes the text box and leaves the picker to the caller, because
    // randomise() syncs every picker once at the end rather than nineteen times.
    // Declared after Field, which it takes by reference.
    void randomiseField(Field& f);

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
        // mira's own title bar, not the OS one -- every window in the app matches the
        // browser now. The native bar cannot take MiraLookAndFeel's colours at all, so a
        // window wearing one sits visibly apart from the rest.
        setUsingNativeTitleBar(false);
        setTitleBarHeight(30);
        content = existing;
        setContentNonOwned(content, false);
        setResizable(true, false);
        centreWithSize(660, 680);   // tall enough for all 19 rows; the Viewport covers smaller screens
        setAlwaysOnTop(true);
        setVisible(true);
        mira_ui::chrome::applyRoundedCorners(*this, 10.0f); // needs the peer, so after setVisible
        toFront(true);
    }

    void closeButtonPressed() override { if (onClosed) onClosed(); }
    std::function<void()> onClosed;
    PromptBuilderContent* content = nullptr;
};
