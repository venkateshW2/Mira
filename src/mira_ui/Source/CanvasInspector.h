#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "MiraLookAndFeel.h"
#include "Sa3WorkerHub.h"

// ---- the canvas experiment: a generator per block ----------------------------------
//
// "each block has generator, generate the file, keep it and show up in the block."
//
// So a block is the unit of BOTH generation and arrangement. Its interior is a private
// take folder; its exterior is one piece of audio on the timeline. Generate into it as
// many times as you like -- only the take you choose ever reaches the track, and the ones
// you did not choose are not on the canvas to be tripped over or summed by accident.
//
// That last part is not just tidiness. Stacked alternates are what makes the mix clip: N
// takes each peaking near full scale sum to N times full scale. Keeping them inside the
// block means the timeline only ever sums things you meant to layer.
//
// This inspector is deliberately SMALLER than the generate window's pane. It carries
// prompt, LoRAs with blend, steps, cfg, seed and duration -- not the step gates, not
// inpainting. Those are real features, and duplicating their subtleties here is how two
// implementations start disagreeing about what a recipe means. They stay in the one
// window that owns them until this experiment earns them.
namespace mira::canvas {

class Inspector : public juce::Component
{
public:
    Inspector(const MiraLookAndFeel& lafIn, Sa3WorkerHub& hubIn, juce::File studioRootIn)
        : laf(lafIn), hub(hubIn), studioRoot(std::move(studioRootIn))
    {
        heading.setText("BLOCK", juce::dontSendNotification);
        heading.setFont(laf.sansSemiBold(MiraLookAndFeel::textSize(10.5f)));
        heading.setColour(juce::Label::textColourId, MiraLookAndFeel::accent);
        addAndMakeVisible(heading);

        blockName.setFont(laf.sansMedium(MiraLookAndFeel::textSize(13.0f)));
        blockName.setColour(juce::Label::textColourId, MiraLookAndFeel::text);
        addAndMakeVisible(blockName);

        prompt.setMultiLine(true, true);
        prompt.setReturnKeyStartsNewLine(true);
        prompt.setFont(laf.sansRegular(MiraLookAndFeel::textSize(12.0f)));
        prompt.onTextChange = [this] { pushSettings(); };
        addAndMakeVisible(prompt);

        for (int i = 0; i < kSlots; ++i)
        {
            auto& s = slots[(size_t) i];
            s.box.onChange = [this] { pushSettings(); };
            addAndMakeVisible(s.box);
            s.blend.setSliderStyle(juce::Slider::LinearHorizontal);
            s.blend.setRange(0.0, 1.5, 0.01);
            s.blend.setValue(1.0, juce::dontSendNotification);
            s.blend.setTextBoxStyle(juce::Slider::TextBoxRight, false, 46, 18);
            s.blend.onValueChange = [this] { pushSettings(); };
            addAndMakeVisible(s.blend);
        }

        auto number = [this](juce::Slider& s, double lo, double hi, double step, double value,
                             juce::Label& l, const char* text) {
            s.setSliderStyle(juce::Slider::LinearHorizontal);
            s.setRange(lo, hi, step);
            s.setValue(value, juce::dontSendNotification);
            s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 18);
            s.onValueChange = [this] { pushSettings(); };
            addAndMakeVisible(s);
            l.setText(text, juce::dontSendNotification);
            l.setFont(laf.sansRegular(MiraLookAndFeel::textSize(11.0f)));
            l.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
            addAndMakeVisible(l);
        };
        number(seconds, 5.0, 300.0, 1.0, 30.0, secondsLabel, "duration");
        number(steps,   1.0,  50.0, 1.0,  8.0, stepsLabel,   "steps");
        number(cfg,     1.0,  12.0, 0.1,  1.0, cfgLabel,     "cfg");
        number(seed,    0.0, 99999.0, 1.0, 26.0, seedLabel,  "seed");

        generate.setButtonText("Generate into this block");
        generate.onClick = [this] { startGenerate(); };
        addAndMakeVisible(generate);

        reseed.setButtonText("Re-seed");
        reseed.onClick = [this] {
            seed.setValue(juce::Random::getSystemRandom().nextInt(99999), juce::sendNotification);
        };
        addAndMakeVisible(reseed);

        takesHeading.setText("TAKES IN THIS BLOCK", juce::dontSendNotification);
        takesHeading.setFont(laf.sansSemiBold(MiraLookAndFeel::textSize(10.0f)));
        takesHeading.setColour(juce::Label::textColourId, MiraLookAndFeel::accent.withAlpha(0.8f));
        addAndMakeVisible(takesHeading);

        takeList.setModel(&takeModel);
        takeList.setRowHeight(22);
        takeList.setColour(juce::ListBox::backgroundColourId, MiraLookAndFeel::surface);
        addAndMakeVisible(takeList);

        status.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
        status.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        addAndMakeVisible(status);

        refreshLoras();
        setEnabledForBlock(false);
    }

    // What the canvas tells us when the selection changes.
    void showBlock(const juce::String& name, const juce::File& folder,
                   const juce::var& settings, const juce::File& chosen)
    {
        pushing = true;
        currentFolder = folder;
        currentChosen = chosen;
        blockName.setText(name.isEmpty() ? "(no block selected)" : name, juce::dontSendNotification);

        if (auto* o = settings.getDynamicObject())
        {
            prompt.setText(o->getProperty("prompt").toString(), juce::dontSendNotification);
            seconds.setValue((double) o->getProperty("seconds"), juce::dontSendNotification);
            steps.setValue((double) o->getProperty("steps"), juce::dontSendNotification);
            cfg.setValue((double) o->getProperty("cfg"), juce::dontSendNotification);
            seed.setValue((double) o->getProperty("seed"), juce::dontSendNotification);
            auto loras = o->getProperty("loras");
            for (int i = 0; i < kSlots; ++i)
            {
                auto& s = slots[(size_t) i];
                s.box.setSelectedId(1, juce::dontSendNotification);
                s.blend.setValue(1.0, juce::dontSendNotification);
                if (auto* arr = loras.getArray(); arr != nullptr && i < arr->size())
                    if (auto* e = (*arr)[i].getDynamicObject())
                    {
                        const auto wanted = e->getProperty("name").toString();
                        for (int k = 0; k < loraFiles.size(); ++k)
                            if (loraFiles[k].getFileNameWithoutExtension() == wanted)
                                s.box.setSelectedId(k + 2, juce::dontSendNotification);
                        s.blend.setValue((double) e->getProperty("strength"), juce::dontSendNotification);
                    }
            }
        }
        pushing = false;

        takeModel.files = folder.isDirectory()
                              ? folder.findChildFiles(juce::File::findFiles, false, "*.wav")
                              : juce::Array<juce::File>();
        takeModel.files.sort();
        takeModel.chosen = chosen;
        takeList.updateContent();
        takeList.repaint();
        setEnabledForBlock(folder != juce::File());
    }

    // Everything the canvas needs back.
    juce::var currentSettings() const;
    std::function<void(juce::var)> onSettingsChanged;     // store on the selected block
    std::function<void(juce::File)> onTakeChosen;         // this file is the block's audio
    std::function<void()> onBusyChanged;
    bool isBusy() const { return busy; }

    void resized() override;
    void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface2); }

    void refreshLoras();

private:
    static constexpr int kSlots = 3;

    struct TakeModel : juce::ListBoxModel
    {
        juce::Array<juce::File> files;
        juce::File chosen;
        std::function<void(juce::File)> onPick;
        const MiraLookAndFeel* laf = nullptr;

        int getNumRows() override { return files.size(); }
        void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
        {
            if (row < 0 || row >= files.size()) return;
            const bool isChosen = files[row] == chosen;
            if (selected) { g.setColour(MiraLookAndFeel::accentSoft); g.fillRect(0, 0, w, h); }
            // The chosen take is marked, not merely selected: "which one is on the track"
            // and "which one am I looking at" are different questions and a list that
            // answers only the second is the one that loses your pick.
            g.setColour(isChosen ? MiraLookAndFeel::accent : MiraLookAndFeel::textDim);
            g.setFont(laf != nullptr ? laf->sansRegular(MiraLookAndFeel::textSize(10.5f))
                                      : juce::Font(juce::FontOptions(11.0f)));
            g.drawText((isChosen ? juce::String(juce::CharPointer_UTF8("\xe2\x97\x8f  "))
                                  : juce::String("    ")) + files[row].getFileNameWithoutExtension(),
                        4, 0, w - 8, h, juce::Justification::centredLeft, true);
        }
        void listBoxItemClicked(int row, const juce::MouseEvent&) override
        {
            if (row >= 0 && row < files.size() && onPick) onPick(files[row]);
        }
    };

    void setEnabledForBlock(bool have)
    {
        for (auto* c : { (juce::Component*) &prompt, (juce::Component*) &generate,
                          (juce::Component*) &reseed })
            c->setEnabled(have && !busy);
        generate.setEnabled(have && !busy);
    }

    void pushSettings()
    {
        if (pushing) return;
        if (onSettingsChanged) onSettingsChanged(currentSettings());
    }

    void startGenerate();

    const MiraLookAndFeel& laf;
    Sa3WorkerHub& hub;
    juce::File studioRoot;
    juce::File currentFolder, currentChosen;
    juce::Array<juce::File> loraFiles;
    bool busy = false, pushing = false;

    juce::Label heading, blockName, secondsLabel, stepsLabel, cfgLabel, seedLabel, takesHeading, status;
    juce::TextEditor prompt;
    struct Slot { juce::ComboBox box; juce::Slider blend; };
    std::array<Slot, kSlots> slots;
    juce::Slider seconds, steps, cfg, seed;
    juce::TextButton generate, reseed;
    TakeModel takeModel;
    juce::ListBox takeList;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Inspector)
};

} // namespace mira::canvas
