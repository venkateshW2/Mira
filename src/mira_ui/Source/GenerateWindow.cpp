#include "GenerateWindow.h"

#include "mira/scan/Scanner.h"

#include "mira/caption/CaptionFields.h"
#include "Export.h"
#include "mira/caption/Sa3Renderer.h"

#include <mach/mach.h>
#include <sys/sysctl.h>

namespace {

// Mirrors sa3_gradio.py's loras/<model>/ convention so the same checkpoints appear in
// both, and sa3-studio/generate.sh keeps populating it.
juce::File loraDirFor(const juce::File& studioRoot) {
    return studioRoot.getChildFile("stable-audio-3/optimized/mlx/loras/sa3-medium");
}

juce::File pythonFor(const juce::File& studioRoot) {
    return studioRoot.getChildFile("stable-audio-3/optimized/mlx/.venv/bin/python");
}

// setTooltip lives on SettableTooltipClient, which most widgets inherit but Component
// does not -- so this resolves it dynamically rather than forcing every caller to know
// which widgets happen to support it.
// Free RAM and swap, straight from the kernel. Both matter: free memory alone looks
// fine right up until the machine is paging, and swap pressure is what actually makes a
// generation crawl.
struct MemState { double freePct = 0.0, swapUsedGb = 0.0; int pressure = 1; };

MemState readMemory() {
    MemState m;
    vm_size_t pageSize = 0;
    host_page_size(mach_host_self(), &pageSize);
    vm_statistics64_data_t vmStat{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vmStat), &count) == KERN_SUCCESS) {
        // "Available" is free + inactive + speculative: inactive pages are reclaimable,
        // so counting only `free` would report near-zero on any warm machine.
        const double avail = static_cast<double>(vmStat.free_count + vmStat.inactive_count
                                                 + vmStat.speculative_count);
        const double total = avail + static_cast<double>(vmStat.active_count
                                                          + vmStat.wire_count
                                                          + vmStat.compressor_page_count);
        if (total > 0.0) m.freePct = 100.0 * avail / total;
    }
    xsw_usage sw{};
    size_t len = sizeof(sw);
    if (sysctlbyname("vm.swapusage", &sw, &len, nullptr, 0) == 0)
        m.swapUsedGb = static_cast<double>(sw.xsu_used) / 1e9;

    // The kernel's own verdict: 1 normal, 2 warning, 4 critical. This is the signal to
    // warn on, NOT a swap used/total ratio -- macOS grows and shrinks the swapfile to fit
    // demand, so that ratio sits near 100% whenever swap is used at all and would warn
    // constantly on a perfectly healthy machine (measured: ratio 88%, pressure 1).
    int level = 1;
    len = sizeof(level);
    if (sysctlbyname("kern.memorystatus_vm_pressure_level", &level, &len, nullptr, 0) == 0)
        m.pressure = level;
    return m;
}

void tip(juce::Component& c, const juce::String& text) {
    if (auto* client = dynamic_cast<juce::SettableTooltipClient*>(&c)) client->setTooltip(text);
}

} // namespace

// ── ResultTile ────────────────────────────────────────────────────────────────

GenerateContent::ResultTile::ResultTile(const MiraLookAndFeel& lafIn) : laf(lafIn) {
    setTooltip("Drag this straight into your DAW, or double-click to play it.\n\n"
               "This is the thing a browser cannot do: gradio can only offer a download.");
}

void GenerateContent::ResultTile::setFile(juce::File f) { file = std::move(f); repaint(); }

void GenerateContent::ResultTile::paint(juce::Graphics& g) {
    auto r = getLocalBounds().toFloat().reduced(1.0f);
    g.setColour(juce::Colours::white.withAlpha(file.existsAsFile() ? 0.10f : 0.04f));
    g.fillRoundedRectangle(r, 6.0f);
    g.setColour(juce::Colours::white.withAlpha(0.18f));
    g.drawRoundedRectangle(r, 6.0f, 1.0f);
    g.setColour(juce::Colours::white.withAlpha(file.existsAsFile() ? 0.85f : 0.35f));
    g.setFont(juce::FontOptions(MiraLookAndFeel::textSize(13.0f)));
    g.drawFittedText(file.existsAsFile()
                         ? "  " + file.getFileName() + "\n  drag me into your DAW"
                         : "  generated audio appears here",
                     getLocalBounds().reduced(8, 4), juce::Justification::centredLeft, 2);
}

void GenerateContent::ResultTile::mouseDrag(const juce::MouseEvent& event) {
    if (!file.existsAsFile() || event.getDistanceFromDragStart() < 5) return;
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this)) {
        if (!container->isDragAndDropActive()) {
            // The trailing `true` is the whole point: it makes this an external file drag
            // macOS hands to the DAW, not an in-app drag. See spike/03_dragout.
            container->performExternalDragDropOfFiles({ file.getFullPathName() }, true, this);
        }
    }
}

void GenerateContent::ResultTile::mouseDoubleClick(const juce::MouseEvent&) {
    if (file.existsAsFile()) file.startAsProcess();
}

// ── GenerateContent ───────────────────────────────────────────────────────────

GenerateContent::GenerateContent(const MiraLookAndFeel& lafIn, juce::File studioRootIn,
                                  mira::Database& databaseIn, Sa3WorkerHub& hubIn)
    : laf(lafIn), database(databaseIn), studioRoot(std::move(studioRootIn)),
      hub(hubIn), resultTile(lafIn) {

    statusLabel.setText("starting worker...", juce::dontSendNotification);
    addAndMakeVisible(statusLabel);

    promptEditor.setMultiLine(true, true);
    promptEditor.setReturnKeyStartsNewLine(true);
    // The prompt is the one piece of text in this window you actually read word by word,
    // so it gets a real reading size rather than the default control size.
    promptEditor.setFont(laf.sansRegular(13.0f));
    promptEditor.onTextChange = [this] { resized(); };
    // Matches the CASE and shape the LoRA was actually trained on. underfit's prompt
    // builder capitalises tag names ("Instruments:", "Moods:", "BPM:") before the text
    // encoder ever sees them, so prompting in lower case feeds T5 different tokens than
    // training used -- which reads as "the model got worse" when nothing changed.
    //
    // Also deliberately WITHOUT rhythm/dynamics/texture: those fields were added to mira
    // after the Dune checkpoints were trained, so zvq has never seen them. They belong in
    // prompts for LoRAs trained on datasets that carry them.
    promptEditor.setText("zvq, Instruments: strings, low brass, Moods: dark, epic, film, "
                          "BPM: 70, TrackType: Music, Genre: Electronic: Ambient, "
                          "VocalType: Instrumental");
    tip(promptEditor, "Prompt SA3 is conditioned on. Keep the trigger token first.");
    rightPane.addAndMakeVisible(promptEditor);

    for (int i = 0; i < kLoraSlots; ++i) {
        auto& sl = slots[static_cast<size_t>(i)];
        sl.label.setText("LoRA " + juce::String(i + 1), juce::dontSendNotification);
        rightPane.addAndMakeVisible(sl.label);
        tip(sl.box, "LoRA for this slot. Fill two slots to blend styles.");
        sl.box.setLookAndFeel(&compactMenuLaf); // compact popup, this control only
        // The strip names its lanes from these, so picking a LoRA has to reach it --
        // otherwise the picture keeps showing the LoRA you just replaced.
        sl.box.onChange = [this] { syncLoraLanes(); };
        rightPane.addAndMakeVisible(sl.box);

        sl.strength.setRange(0.0, 2.0, 0.05);
        sl.strength.setValue(1.0, juce::dontSendNotification);
        // Its slot's colour, the same one the lane strip draws that LoRA in, so blend and
        // step-window read as two controls over one thing -- and so neither is mistaken
        // for a settings slider.
        sl.strength.setColour(juce::Slider::trackColourId, MiraLookAndFeel::slotTint(i));
        sl.strength.setDoubleClickReturnValue(true, 1.0);
        sl.strength.setSliderStyle(juce::Slider::LinearHorizontal);
        sl.strength.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 18);
        tip(sl.strength, "0 = base model (bypass), 1 = as trained, above 1 = overdriven.");
        rightPane.addAndMakeVisible(sl.strength);

        // Step gating: apply the LoRA only during part of the diffusion run. Early steps
        // shape structure and arrangement, late steps shape timbre and texture -- so this
        // separates "did it change the music?" from "did it just recolour the surface?".
        sl.blendLabel.setFont(juce::Font(juce::FontOptions(MiraLookAndFeel::textSize(10.0f))));
        sl.blendLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
        sl.blendLabel.setJustificationType(juce::Justification::centredRight);
        sl.blendLabel.setText("blend", juce::dontSendNotification);
        rightPane.addAndMakeVisible(sl.blendLabel);
        // The step window is not set here any more -- LoraLanes below owns it.
    }

    keepButton.setEnabled(false);
    keepButton.setTint(juce::Colour(0xff6fc47f));      // green: the take survives
    tip(keepButton, "Keep - file it into mira's library under \"Generated\", with its full recipe.");
    keepButton.onClick = [this] { keepResult(); };
    addAndMakeVisible(keepButton);

    discardButton.setEnabled(false);
    discardButton.setTint(juce::Colour(0xffe0685f));   // red: it goes to the Trash
    tip(discardButton, "Discard - move this take and its recipe to the Trash.");
    discardButton.onClick = [this] { discardResult(); };
    addAndMakeVisible(discardButton);

    tip(cleanupButton, "Trash every generation in the output folder you never pressed Keep on.");
    cleanupButton.onClick = [this] { cleanupUnkept(); };
    addAndMakeVisible(cleanupButton);

    tip(exportButton, "Render the kept takes -- trim, fades and gain -- to wav at 44.1 kHz.");
    exportButton.onClick = [this] { showExportMenu(); };
    addAndMakeVisible(exportButton);

    tip(buildPromptButton, "Build a prompt in the shape the LoRAs were trained on.");
    buildPromptButton.onClick = [this] {
        if (promptBuilder != nullptr) { promptBuilder->toFront(true); return; }
        if (promptBuilderContent == nullptr) {
            promptBuilderContent = std::make_unique<PromptBuilderContent>(laf, studioRoot);
            promptBuilderContent->onConstruct = [this](const juce::String& text) {
                promptEditor.setText(text);
                statusLabel.setText("prompt built - " + juce::String(text.length()) + " chars",
                                     juce::dontSendNotification);
                log("prompt: " + text);
                // Closing IS the confirmation: the panel goes away and the text is
                // visibly sitting in the prompt box behind it.
                juce::MessageManager::callAsync([this] { promptBuilder.reset(); });
            };
        }
        promptBuilder = std::make_unique<PromptBuilderWindow>(promptBuilderContent.get());
        promptBuilder->onClosed = [this] {
            juce::MessageManager::callAsync([this] { promptBuilder.reset(); });
        };
    };
    rightPane.addAndMakeVisible(buildPromptButton);

    // "add lora take it out of this -- let have it in the osx toolbar". The button is
    // gone; Window > LoRA Library... manages the folder and names the checkpoints, and
    // this window just reads the result. addLoraFile() stays as the implementation the
    // library window's own Add button reaches, so there is still one copy of it.

    auto setupNumber = [this](juce::Slider& s, double lo, double hi, double interval,
                              double value, const juce::String& tipText) {
        s.setRange(lo, hi, interval);
        s.setValue(value, juce::dontSendNotification);
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
        // Double-click returns to the value it was built with. Four sliders that each
        // reach into a 40-fold range need a way back that is not "remember what it said".
        s.setDoubleClickReturnValue(true, value);
        tip(s, tipText + "  Default " + juce::String(value, interval < 1.0 ? 1 : 0)
                    + " -- double-click to return to it.");
        rightPane.addAndMakeVisible(s);
    };
    stepsSlider.onValueChange = [this] {
        syncLoraStepRanges();
        if (inpaintStrip != nullptr) inpaintStrip->repaint();
    };
    secondsSlider.onValueChange = [this] {
        // The inpaint timeline IS the duration being generated -- that is what makes
        // extending a take possible at all (see InpaintStrip::setTimeline).
        if (inpaintStrip != nullptr) inpaintStrip->setTimeline(secondsSlider.getValue());
        syncInpaintSliderRanges();
    };
    setupNumber(secondsSlider, 10, 380, 1, 30,
                "Output length. CHANGING THIS RELOADS THE MODEL (~44s). RAM grows with it.");
    setupNumber(stepsSlider, 4, 50, 1, 8,
                "Diffusion steps. 8 is the tuned default.");
    setupNumber(seedSlider, 0, 100000, 1, 26,
                "Seed. Fix it when comparing anything.");
    setupNumber(cfgSlider, 1.0, 10.0, 0.5, 1.0,
                "CFG. 1 = the long-standing default (Avoid is ignored). 3-5 to steer; 7 clips ~5% of samples.");
    cfgLabel.setText("cfg", juce::dontSendNotification);
    rightPane.addAndMakeVisible(cfgLabel);

    negativeLabel.setText("avoid", juce::dontSendNotification);
    rightPane.addAndMakeVisible(negativeLabel);
    negativeEditor.setMultiLine(false);
    negativeEditor.setTextToShowWhenEmpty("needs cfg above 1 -- e.g. voice, vocals, singing",
                                           juce::Colours::grey);
    tip(negativeEditor, "Steer AWAY from these. Only acts when cfg > 1 (sa3_gradio guards it with `if cfg != 1.0`).");
    rightPane.addAndMakeVisible(negativeEditor);

    tip(generateButton, "Generate with the settings above.");
    generateButton.onClick = [this] { generate(); };
    // rightPane, NOT this. Every control laid out by layoutRightPane is a child of the
    // pane, and bounds from it are pane-relative -- added to the content instead, these
    // two were positioned in the wrong coordinate space and simply never appeared.
    rightPane.addChildComponent(extendButton);
    rightPane.addChildComponent(remixButton);
    extendButton.setTooltip("fill the block's empty tail, keeping what is already there");
    remixButton.setTooltip("regenerate the whole block, guided by the take it has");
    extendButton.onClick = [this] { if (onExtend) onExtend(); };
    remixButton.onClick  = [this] { if (onRemix)  onRemix();  };
    rightPane.addAndMakeVisible(generateButton);

    triggerLabel.setText("trigger", juce::dontSendNotification);
    rightPane.addAndMakeVisible(triggerLabel);
    // Deliberately EMPTY. It used to default to "xyr" (Mad Max), so every dataset
    // prepared without noticing this field was encoded under another set's trigger --
    // silent, unfalsifiable, and only visible once the LoRA trained wrong. encodeFolder
    // already refuses an empty trigger, which is the right failure.
    triggerEditor.setTextToShowWhenEmpty("e.g. zvq", MiraLookAndFeel::textFaint);
    tip(triggerEditor, "Rare token to key the DATASET BEING ENCODED to -- not the loaded "
                       "LoRA. Three or four letters that mean nothing in English.");
    rightPane.addAndMakeVisible(triggerEditor);

    tip(encodeButton, "Captions -> encode -> zip, for one film folder. Analysed files only.");
    encodeButton.onClick = [this] { chooseEncodeFolder(); };
    rightPane.addAndMakeVisible(encodeButton);

    outputFolder = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                       .getChildFile("mira-generated");
    outputFolder.createDirectory();
    takeFormatManager.registerBasicFormats();
    takeStack = std::make_unique<TakeStack>(laf, takeFormatManager, takeThumbnailCache);
    takeStack->onFocused = [this](const juce::File& f) {
        stopAudition();
        preview.setFile(f);
        resultTile.setFile(f);
        loadEditFor(f); // Phase 5: the take's trim and fades come back with it
        const bool have = f.existsAsFile();
        keepButton.setEnabled(have);
        discardButton.setEnabled(have);
        revealButton.setEnabled(have);
    };
    takeStack->onHeightChanged = [this] {
        // The BREAKDOWN, not the total. "Takes (22)" over a list whose first nine rows
        // are kept cues said the wrong thing about all of them -- the whole point of the
        // sections is that those are not takes any more.
        const int kept = takeStack->getKeptCount();
        const int pending = takeStack->getPendingCount();
        const int discarded = takeStack->getDiscardedCount();
        juce::StringArray bits;
        if (kept > 0) bits.add(juce::String(kept) + " kept");
        if (pending > 0) bits.add(juce::String(pending) + " takes");
        if (discarded > 0) bits.add(juce::String(discarded) + " discarded");
        takesLabel.setText(bits.isEmpty() ? juce::String("Takes")
                                           : bits.joinIntoString(juce::String(juce::CharPointer_UTF8("  \xc2\xb7  "))),
                           juce::dontSendNotification);
        resized();
    };
    rightPane.addAndMakeVisible(loraHeading);
    rightPane.addAndMakeVisible(settingsHeading);
    loraLanes.onWindowChanged = [this](int slot, int lo, int hi) {
        if (slot < 0 || slot >= kLoraSlots) return;
        slots[static_cast<size_t>(slot)].stepLo = lo;
        slots[static_cast<size_t>(slot)].stepHi = hi;
    };
    rightPane.addAndMakeVisible(loraLanes);

    rightPane.addAndMakeVisible(inpaintHeading);
    {
        struct Named { juce::Label* label; const char* text; };
        for (auto n : { Named{ &secondsLabel, "duration" }, Named{ &stepsLabel, "steps" },
                         Named{ &seedLabel, "seed" }, Named{ &cfgLabel, "cfg" } }) {
            n.label->setText(n.text, juce::dontSendNotification);
            n.label->setFont(juce::Font(juce::FontOptions(MiraLookAndFeel::textSize(11.5f), juce::Font::bold)));
            n.label->setColour(juce::Label::textColourId, MiraLookAndFeel::text);
            n.label->setJustificationType(juce::Justification::centredRight);
            rightPane.addAndMakeVisible(*n.label);
        }
    }

    // What inpainting actually does, in the window rather than in a doc. Checked against
    // sa3_mlx.py rather than assumed: the mask keeps everything OUTSIDE the range as
    // context and regenerates what is inside it, and a paste-back makes the kept part
    // bit-exact. The prompt conditions the WHOLE generation, not just the gap -- there is
    // one cross-attention prompt, so it describes the piece the gap has to belong to.
    inpaintHelp.setText("Regenerates only the highlighted range; everything outside it is kept exactly. "
                         "Drag the range past the end of the audio to EXTEND it. The prompt above "
                         "describes the whole piece, not just the gap.",
                         juce::dontSendNotification);
    inpaintHelp.setFont(juce::Font(juce::FontOptions(MiraLookAndFeel::textSize(11.0f))));
    inpaintHelp.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
    inpaintHelp.setJustificationType(juce::Justification::topLeft);
    rightPane.addAndMakeVisible(inpaintHelp);

    // Borrows the window's one audio device from the take preview rather than opening a
    // second output stream for the same hardware.
    inpaintStrip = std::make_unique<InpaintStrip>(takeFormatManager, takeThumbnailCache,
                                                    preview.getAudioDeviceManager());
    inpaintStrip->onFileDropped = [this](const juce::File& f) {
        // A drop IS the init audio -- the two were separate controls and it was never
        // obvious they were the same thing.
        initAudio = f;
        initLabel.setText(f.getFileName(), juce::dontSendNotification);
        inpaintToggle.setToggleState(true, juce::sendNotification);
        inpaintStrip->setTimeline(secondsSlider.getValue());
        syncInpaintSliderRanges();
        const double audio = inpaintStrip->getAudioSeconds();
        const double total = secondsSlider.getValue();
        if (audio > 0.0) {
            // Default to the EXTENSION when the dropped audio is shorter than the
            // duration: that is the reason to drop a take in here rather than a file, and
            // a default of "regenerate the first ten seconds of what you just made" would
            // be the least likely thing wanted.
            const double s0 = audio < total - 0.5 ? audio : 0.0;
            const double s1 = audio < total - 0.5 ? total : juce::jmin(audio, 10.0);
            inpaintStrip->setRange(s0, s1);
            inpaintStart.setValue(s0, juce::dontSendNotification);
            inpaintEnd.setValue(s1, juce::dontSendNotification);
        }
        resized();
    };
    inpaintStrip->onRangeChanged = [this](double a2, double b2) {
        // The sliders stay the source of truth; the strip is a view onto them.
        inpaintStart.setValue(a2, juce::sendNotificationSync);
        inpaintEnd.setValue(b2, juce::sendNotificationSync);
    };
    rightPane.addAndMakeVisible(*inpaintStrip);
    rightView.setViewedComponent(&rightPane, false); // false: this owns rightPane, not the viewport
    rightView.setScrollBarsShown(true, false);
    addAndMakeVisible(rightView);

    takesLabel.setFont(juce::Font(juce::FontOptions(MiraLookAndFeel::textSize(12.0f))));
    takesLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
    addAndMakeVisible(takesLabel);
    takesView.setViewedComponent(takeStack.get(), false);
    takesView.setScrollBarsShown(true, false);
    addAndMakeVisible(takesView);

    tip(outFolderButton, "Where generated WAVs go.");
    outFolderButton.onClick = [this] { chooseOutputFolder(); };
    rightPane.addAndMakeVisible(outFolderButton);

    nameEditor.setTextToShowWhenEmpty("filename (blank = timestamp)",
                                       juce::Colours::white.withAlpha(0.35f));
    tip(nameEditor, "Filename for the next take. Blank = timestamp. Never overwrites.");
    rightPane.addAndMakeVisible(nameEditor);

    tip(initAudioButton, "audio2audio: start from this audio instead of noise.");
    initAudioButton.onClick = [this] { chooseInitAudio(); };
    rightPane.addAndMakeVisible(initAudioButton);
    clearInitButton.onClick = [this] {
        initAudio = juce::File();
        initLabel.setText("no init audio", juce::dontSendNotification);
        if (inpaintStrip != nullptr) { inpaintStrip->setFile({}); resized(); }
    };
    tip(clearInitButton, "Clear init audio.");
    rightPane.addAndMakeVisible(clearInitButton);
    initLabel.setText("no init audio", juce::dontSendNotification);
    rightPane.addAndMakeVisible(initLabel);

    tip(inpaintToggle, "Regenerate only the range below, keep the rest bit-exact.");
    inpaintToggle.onClick = [this] { resized(); };
    rightPane.addAndMakeVisible(inpaintToggle);
    for (auto* sl : { &inpaintStart, &inpaintEnd }) {
        sl->setRange(0.0, 380.0, 0.5);
        sl->setSliderStyle(juce::Slider::LinearHorizontal);
        sl->setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 18);
        rightPane.addAndMakeVisible(*sl);
    }
    inpaintStart.setValue(0.0, juce::dontSendNotification);
    inpaintEnd.setValue(10.0, juce::dontSendNotification);
    tip(inpaintStart, "Range start, seconds.");
    tip(inpaintEnd, "Range end, seconds.");

    tip(revealButton, "Reveal the file in Finder.");
    revealButton.onClick = [this] {
        if (resultTile.getFile().existsAsFile()) resultTile.getFile().revealToUser();
    };
    revealButton.setEnabled(false);
    rightPane.addAndMakeVisible(revealButton);

    // NOT addAndMakeVisible(preview) -- the take stack hosts these, and adding them here
    // afterwards silently reparented them BACK to this component while resized() went on
    // giving them the stack's coordinates. That is what put a full-width waveform across
    // the top of the window with the expanded row left empty below it. Ownership of a
    // child is the thing that decides whose coordinate space its bounds are in.
    tip(preview, "Play and scrub. Drag the strip below to get the file into your DAW.");

    compactMenuLaf.setCompactMenus(true);
    syncLoraStepRanges();
    if (inpaintStrip != nullptr) inpaintStrip->setTimeline(secondsSlider.getValue());
    syncInpaintSliderRanges();

    // ---- Phase 5 edit controls. Hosted in the focused take row beside Keep/Discard,
    // because an edit belongs to ONE take and there is no sense in which the window has a
    // current trim independent of which take is open.
    tip(trimButton, "Trim to the selection. Drag across the waveform first. Non-destructive: "
                     "the file is untouched until export.");
    trimButton.onClick = [this] { applyTrimFromSelection(); };
    tip(clearTrimButton, "Remove the trim; the take goes back to full length.");
    clearTrimButton.onClick = [this] { clearTrim(); };
    // No "Play edit" button: the waveform's own Play applies the trim, fades and gain
    // (see timerCallback). auditionEdit() stays as the code path that drives it.

    editLabel.setFont(juce::Font(juce::FontOptions(MiraLookAndFeel::textSize(11.0f))));
    editLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);

    {
        struct Fader { juce::Slider* slider; juce::Label* label; const char* name;
                        double lo; double hi; double step; const char* suffix; const char* help; };
        for (auto f : { Fader{ &fadeInSlider, &fadeInLabel, "fade in", 0.0, 10.0, 0.1, " s",
                                "Fade in, applied from the trim start." },
                         Fader{ &fadeOutSlider, &fadeOutLabel, "fade out", 0.0, 10.0, 0.1, " s",
                                "Fade out, applied up to the trim end." },
                         Fader{ &gainSlider, &gainLabel, "gain", -24.0, 6.0, 0.5, " dB",
                                "Level applied to the whole edit." } }) {
            f.slider->setRange(f.lo, f.hi, f.step);
            f.slider->setValue(0.0, juce::dontSendNotification);
            f.slider->setSliderStyle(juce::Slider::LinearHorizontal);
            // 44, not 52: this now shares the transport row rather than owning a row of
            // its own, and every pixel the readout takes comes off the track you drag.
            f.slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 44, 18);
            f.slider->setTextValueSuffix(f.suffix);
            f.slider->onValueChange = [this] { writeEditFields(); };
            tip(*f.slider, f.help);
            f.label->setText(f.name, juce::dontSendNotification);
            f.label->setFont(juce::Font(juce::FontOptions(MiraLookAndFeel::textSize(11.0f))));
            f.label->setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
            f.label->setJustificationType(juce::Justification::centredRight);
        }
    }
    // A selection is the input to Trim, so the edit row has to react to one existing.
    // Without this the only hint that a drag on the waveform does anything was a tooltip
    // on a button -- which is why the gesture read as missing rather than as invisible.
    preview.onSelectionChanged = [this] { refreshEditControls(); };
    // The buttons say how many they are about to act on, so a bulk discard cannot be
    // mistaken for a single one.
    takeStack->onSelectionChanged = [this] {
        const int n = takeStack->getSelectedCount();
        // Icons have nowhere to put "Keep 4", so the count goes where it can still be
        // read before acting. Leaving it unsaid would make a four-take discard look
        // exactly like a one-take discard.
        keepButton.setTooltip(n > 1 ? "Keep these " + juce::String(n) + " takes"
                                    : juce::String("Keep - file it into mira's library"));
        discardButton.setTooltip(n > 1 ? "Discard these " + juce::String(n) + " takes"
                                       : juce::String("Discard - move to the Trash"));
    };
    refreshEditControls();

    // Last, so nothing added above can take these back (see the note beside preview).
    // A freshly generated take has no chords, notes or beat grid to overlay, so the
    // lanes menu here would only ever open onto greyed-out items.
    preview.setLanesButtonVisible(false);
    // The fade handles write straight through to the same sliders the rest of the code
    // still reads, so the storage path (writeEditFields) and the audition envelope did
    // not have to learn about a second source of truth.
    preview.onFadesChanged = [this](double in, double out) {
        fadeInSlider.setValue(in, juce::dontSendNotification);
        fadeOutSlider.setValue(out, juce::dontSendNotification);
        writeEditFields();
    };
    takeStack->setHostedComponents({ &preview, &resultTile, &keepButton, &discardButton,
                                      &trimButton, &clearTrimButton,
                                      &gainLabel, &gainSlider });
    // fadeInSlider / fadeOutSlider are deliberately NOT hosted: they are storage now,
    // written by the waveform's fade handles and read by writeEditFields. Hosting them
    // would reparent two invisible widgets into the take rows for no reason.

    tip(stopButton, "Kill and restart the worker. Next run reloads the model (~44s).");
    stopButton.onClick = [this] { stopGeneration(); };
    stopButton.setEnabled(false);
    rightPane.addAndMakeVisible(stopButton);

    tip(pressureLabel, "Free RAM / swap. Once swap fills, every step pages to disk.");
    addAndMakeVisible(pressureLabel);
    updatePressure();
    startTimerHz(1);   // pressure keeps updating even when idle

    tip(consoleButton, "Worker output: model loads, LoRA plans, tracebacks. Opens in its own window.");
    consoleButton.onClick = [this] {
        if (consoleWindow == nullptr) consoleWindow = std::make_unique<LogWindow>(workerLog, laf);
        consoleWindow->setVisible(true);
        consoleWindow->toFront(true);
    };
    rightPane.addAndMakeVisible(consoleButton);

    // The progress strip lives in the LEFT pane, under the takes header: the bar fills
    // in the rectangle the take then lands in. Calibration is per machine and persists,
    // so the "we have never timed a run" state is entered once in the app's life.
    audioInCollapse.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface2);
    audioInCollapse.setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
    audioInCollapse.onClick = [this] { audioInCollapsed = !audioInCollapsed; resized(); };
    tip(audioInCollapse, "Fold the init-audio and inpaint controls away.");
    rightPane.addAndMakeVisible(audioInCollapse);

    if (const auto k = database.getSetting("gen_calibration"))
        genProgress.setCalibration(juce::String(*k).getDoubleValue());
    // addChildComponent, NOT addAndMakeVisible: the latter sets visible to TRUE, which
    // silently undid the setVisible(false) that used to sit right above it. The strip
    // was therefore up from launch, never started, reading the raw millisecond counter
    // as its start time -- so it showed the machine's UPTIME as a generation that was
    // not running. (The ProgressBar it replaced had the identical mistake; at progress 0
    // it just drew an empty bar, so nobody ever saw it.)
    addChildComponent(genProgress);

    generatePaneToggle.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface2);
    generatePaneToggle.setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
    generatePaneToggle.onClick = [this] { generatePaneCollapsed = !generatePaneCollapsed; resized(); };
    tip(generatePaneToggle, "Fold the generate controls away and give the width to the waveform.");
    addAndMakeVisible(generatePaneToggle);

    datasetsLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.45f));
    datasetsLabel.setFont(juce::FontOptions(MiraLookAndFeel::textSize(11.0f)));
    tip(datasetsLabel, "Prepared datasets: folder -> trigger (latent count).");
    rightPane.addAndMakeVisible(datasetsLabel);
    refreshDatasets();
    refreshLoras();
    startWorker();
    setSize(720, 640);
}

GenerateContent::~GenerateContent() {
    stopTimer();
    // Detach before compactMenuLaf dies. It is declared AFTER the slots, so it would be
    // destroyed first and leave three ComboBoxes holding a dangling LookAndFeel pointer
    // through their own destructors -- a use-after-free that JUCE asserts on in debug and
    // simply crashes in release.
    for (auto& sl : slots) sl.box.setLookAndFeel(nullptr);
    // Detach, do NOT stop: the worker is shared and another window may be mid-generation.
    // Leaving the listener registered would fan a log line into a destroyed object.
    if (hubToken != 0) hub.removeListener(hubToken);
}

juce::String GenerateContent::triggerOfFolder(const juce::File& folder) const {
    // Any one sidecar answers it: prepare writes the whole folder with one trigger.
    for (const auto& j : folder.findChildFiles(juce::File::findFiles, false, "*.json")) {
        if (j.getFileName() == "details.json") continue;
        const auto t = juce::JSON::parse(j.loadFileAsString())
                           .getProperty("trigger", "").toString();
        if (t.isNotEmpty()) return t;
    }
    return {};
}

void GenerateContent::refreshDatasets() {
    const auto root = studioRoot.getChildFile("latents");
    juce::StringArray entries;
    if (root.isDirectory()) {
        for (const auto& d : root.findChildFiles(juce::File::findDirectories, false)) {
            const auto trig = triggerOfFolder(d);
            const auto n = d.findChildFiles(juce::File::findFiles, false, "*.npy").size();
            entries.add(d.getFileName() + " -> " + (trig.isEmpty() ? "?" : trig)
                        + " (" + juce::String(n) + ")");
        }
    }
    datasetsLabel.setText(entries.isEmpty() ? "no datasets prepared yet"
                                            : "datasets:  " + entries.joinIntoString("   |   "),
                          juce::dontSendNotification);
}

namespace {
// underfit names checkpoints "<run>-step<N>-epoch<M>.safetensors". Everything before
// "-step" is the run, which is what a person actually picks by ("the nin one"); the
// numbers only choose WHICH checkpoint of that run. Splitting there lets the menu show
// the run once as a heading and the checkpoints under it, instead of thirteen long
// near-identical filenames that differ in the middle.
juce::String loraRunName(const juce::File& f) {
    const auto stem = f.getFileNameWithoutExtension();
    const auto cut = stem.indexOf("-step");
    return cut > 0 ? stem.substring(0, cut) : stem;
}

int loraStep(const juce::File& f) {
    const auto stem = f.getFileNameWithoutExtension();
    const auto cut = stem.indexOf("-step");
    if (cut < 0) return -1;
    // "=" survives when a checkpoint is copied in by hand rather than through
    // addLoraFile(), which strips it -- tolerate both spellings.
    return stem.substring(cut + 5).trimCharactersAtStart("=")
               .upToFirstOccurrenceOf("-", false, false).getIntValue();
}

juce::String loraShortLabel(const juce::File& f) {
    const auto stem = f.getFileNameWithoutExtension();
    const int step = loraStep(f);
    if (step <= 0) return stem;
    const auto epoch = stem.fromFirstOccurrenceOf("epoch", false, false)
                           .trimCharactersAtStart("=");
    return "step " + juce::String(step)
         + (epoch.isNotEmpty() ? "   (epoch " + epoch + ")" : juce::String());
}
} // namespace

void GenerateContent::refreshLoras() {
    loraFiles.clear();
    const auto dir = loraDirFor(studioRoot);
    if (dir.isDirectory())
        for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.safetensors"))
            loraFiles.add(f);

    // Run name, then newest checkpoint first -- the one just trained is the one most
    // likely wanted, and it used to be buried in whatever order the filesystem returned.
    std::sort(loraFiles.begin(), loraFiles.end(), [](const juce::File& a, const juce::File& b) {
        const auto ra = loraRunName(a), rb = loraRunName(b);
        if (ra != rb) return ra < rb;
        return loraStep(a) > loraStep(b);
    });

    for (int i = 0; i < kLoraSlots; ++i) {
        auto& box = slots[static_cast<size_t>(i)].box;
        const int previous = box.getSelectedId();
        box.clear(juce::dontSendNotification);
        box.addItem("(empty)", 1);
        juce::String lastRun;
        for (int k = 0; k < loraFiles.size(); ++k) {
            const auto run = loraRunName(loraFiles[k]);
            if (run != lastRun) { box.addSectionHeading(run); lastRun = run; }
            // Ids stay k+2 over the SORTED array, which is the same array
            // buildLoraSpecs() indexes -- section headings consume no id.
            // The name from the LoRA Library if it has one, the filename-derived
            // label otherwise -- never both, and never a name invented here.
            auto named = LoraLibraryContent::displayNameFor(database, loraFiles[k]);
            box.addItem(named.isNotEmpty() ? named : loraShortLabel(loraFiles[k]), k + 2);
        }
        // Slot 1 preselects the FIRST entry now that the list is sorted -- id 2 -- rather
        // than the last one the filesystem happened to return.
        const int wanted = previous > 0 ? previous
                          : (i == 0 && !loraFiles.isEmpty() ? 2 : 1);
        box.setSelectedId(box.indexOfItemId(wanted) >= 0 ? wanted : 1,
                          juce::dontSendNotification);
    }
    syncLoraLanes(); // names may have changed under the boxes
}

void GenerateContent::addLoraFile() {
    auto chooser = std::make_shared<juce::FileChooser>("Choose a .safetensors LoRA",
                                                        juce::File(), "*.safetensors");
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
                         [this, chooser](const juce::FileChooser& fc) {
        const auto src = fc.getResult();
        if (!src.existsAsFile()) return;
        auto dir = loraDirFor(studioRoot);
        dir.createDirectory();
        // '=' is legal in a filename but awkward on a command line, and underfit names
        // checkpoints "name-step=1500-epoch=39.safetensors" -- normalise on the way in,
        // exactly as generate.sh does, so both paths agree.
        const auto dest = dir.getChildFile(src.getFileName().replace("=", ""));
        if (src.copyFileTo(dest)) {
            log("added LoRA: " + dest.getFileName());
            refreshLoras();
        } else {
            log("could not copy " + src.getFileName() + " into " + dir.getFullPathName());
        }
    });
}

// Registers the current result the way the scanner would, then stores the recipe in
// `human` and files it under a "Generated" collection. Deliberately manual -- see the
// keepButton comment in the header.
namespace {
// MIRA-GENERATE.md §3.7. "Short Film" -> shortfilm, "A minor" -> Aminor, 120.4 -> 120.
// Lowercased and stripped to alphanumerics: a filename token that has to survive being
// mailed, zipped, and dropped into someone else's DAW on someone else's filesystem.
juce::String slugify(const juce::String& text) {
    juce::String out;
    for (auto c : text)
        if (juce::CharacterFunctions::isLetterOrDigit(c)) out += juce::String::charToString(c).toLowerCase();
    return out;
}

// Editable combo box, not a plain text field: the cues already in the project are the
// list, and typing a new name is how a new cue is made. That is the whole of "prompts
// for a cue name, autocompleting from cues already in the project" -- picking is the
// common case (ten takes for one cue), typing is the occasional one.
void promptForCue(const juce::StringArray& existingCues, const juce::String& initialValue,
                   juce::Component* anchor, std::function<void(juce::String)> onConfirm) {
    // Same owning-shared_ptr pattern FolderTreeView::promptForText documents, and for
    // the same reason: deleteWhenDismissed=false, we own the lifetime here.
    auto aw = std::make_shared<juce::AlertWindow>("Keep Take", "Which cue does this belong to?",
                                                   juce::MessageBoxIconType::NoIcon, anchor);
    aw->addComboBox("cue", existingCues, "Cue");
    if (auto* box = aw->getComboBoxComponent("cue")) {
        box->setEditableText(true);
        if (initialValue.isNotEmpty()) box->setText(initialValue, juce::dontSendNotification);
        else if (existingCues.isEmpty()) box->setText("cue01", juce::dontSendNotification);
    }
    aw->addButton("Keep", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    aw->enterModalState(true, juce::ModalCallbackFunction::create([aw, onConfirm](int result) {
        if (result != 1) return;
        if (auto* box = aw->getComboBoxComponent("cue")) onConfirm(box->getText());
    }), false);
}
} // namespace

// The take's own settings, from the .json written beside it at render time. Falls back
// to lastRecipe only when the sidecar is missing (a take generated before sidecars, or
// one whose write failed), and returns void rather than guessing when neither exists.
void GenerateContent::syncInpaintSliderRanges() {
    const double total = juce::jmax(1.0, secondsSlider.getValue());
    for (auto* sl : { &inpaintStart, &inpaintEnd }) {
        const double held = sl->getValue();
        sl->setRange(0.0, total, 0.01);
        sl->setValue(juce::jlimit(0.0, total, held), juce::dontSendNotification);
    }
    if (inpaintStrip != nullptr)
        inpaintStrip->setRange(inpaintStart.getValue(), inpaintEnd.getValue());
}

void GenerateContent::syncLoraStepRanges() {
    const int nSteps = juce::jmax(1, static_cast<int>(stepsSlider.getValue()));
    for (auto& sl : slots) {
        // A window that reached the old last step meant "to the end" and still does, so
        // it follows the new end rather than being stranded by a change it had no
        // opinion about.
        const bool wasFull = sl.stepHi >= previousStepCount;
        sl.stepLo = juce::jlimit(1, nSteps, sl.stepLo);
        sl.stepHi = wasFull ? nSteps : juce::jlimit(sl.stepLo, nSteps, sl.stepHi);
    }
    previousStepCount = nSteps;
    loraLanes.setSteps(nSteps);
    syncLoraLanes();
}

// Names and windows into the strip. Called whenever a LoRA is picked or Steps moves, so
// the picture can never disagree with what buildLoraSpecs will send.
void GenerateContent::syncLoraLanes() {
    for (int i = 0; i < kLoraSlots; ++i) {
        auto& sl = slots[static_cast<size_t>(i)];
        const int sel = sl.box.getSelectedId();
        const bool have = sel > 1 && sel - 1 <= loraFiles.size();
        loraLanes.setLane(i, have ? sl.box.getText() : juce::String(), sl.stepLo, sl.stepHi);
        // An empty slot's blend slider was still drawn full and reading 1.00, in its own
        // colour -- three loaded LoRAs, as far as the picture was concerned. It has
        // nothing to scale, so it is disabled and reads as such.
        sl.strength.setEnabled(have);
        sl.blendLabel.setEnabled(have);
    }
}

juce::var GenerateContent::recipeFor(const juce::File& wav) const {
    auto sidecar = wav.withFileExtension("json");
    if (sidecar.existsAsFile()) {
        auto parsed = juce::JSON::parse(sidecar.loadFileAsString());
        if (!parsed.isVoid()) return parsed;
    }
    return lastRecipe;
}

void GenerateContent::setBlockActions(bool canExtend, bool canRemix)
{
    // Shown only where a block is driving this panel. The plain generate window never sets
    // onExtend/onRemix, so it never grows two buttons it has no meaning for.
    const bool show = onExtend != nullptr || onRemix != nullptr;
    if (show != showBlockActions) { showBlockActions = show; resized(); }
    extendButton.setVisible(show);
    remixButton.setVisible(show);
    extendButton.setEnabled(canExtend && !busy);
    remixButton.setEnabled(canRemix && !busy);
}

// ---- a block's generator settings ---------------------------------------------------
//
// The canvas needs the generator's whole state as one value, because a BLOCK owns its
// generator: clicking another block has to bring its prompt, its LoRAs and its numbers
// with it. The panel used to change only its TITLE, so every block appeared to share one
// recipe -- the same confusion the take stack caused, one level up.

juce::var GenerateContent::captureSettings() const
{
    auto* r = new juce::DynamicObject();
    r->setProperty("prompt", promptEditor.getText());
    r->setProperty("negative_prompt", negativeEditor.getText());
    r->setProperty("seed", static_cast<int>(seedSlider.getValue()));
    r->setProperty("cfg", cfgSlider.getValue());
    r->setProperty("steps", static_cast<int>(stepsSlider.getValue()));
    r->setProperty("seconds", secondsSlider.getValue());

    // LoRAs by NAME, not by index or path: the index moves when the library gains an
    // entry, and a path goes stale the moment the file does. The name is what the
    // dropdown showed when the choice was made.
    juce::Array<juce::var> used;
    for (int i = 0; i < kLoraSlots; ++i) {
        const auto& sl = slots[static_cast<size_t>(i)];
        const int sel = sl.box.getSelectedId();
        if (sel <= 1 || sel - 1 > loraFiles.size()) continue;
        auto* u = new juce::DynamicObject();
        u->setProperty("slot", i);
        u->setProperty("name", loraFiles[sel - 2].getFileNameWithoutExtension());
        u->setProperty("strength", sl.strength.getValue());
        u->setProperty("gate", juce::var(juce::Array<juce::var>{ sl.stepLo, sl.stepHi }));
        used.add(juce::var(u));
    }
    r->setProperty("loras", juce::var(used));
    return juce::var(r);
}

void GenerateContent::applySettings(const juce::var& settings)
{
    if (!settings.isObject()) return;

    promptEditor.setText(settings.getProperty("prompt", "").toString(),
                          juce::dontSendNotification);
    negativeEditor.setText(settings.getProperty("negative_prompt", "").toString(),
                            juce::dontSendNotification);
    seedSlider.setValue((double) settings.getProperty("seed", seedSlider.getValue()),
                         juce::dontSendNotification);
    cfgSlider.setValue((double) settings.getProperty("cfg", cfgSlider.getValue()),
                        juce::dontSendNotification);
    stepsSlider.setValue((double) settings.getProperty("steps", stepsSlider.getValue()),
                          juce::dontSendNotification);
    secondsSlider.setValue((double) settings.getProperty("seconds", secondsSlider.getValue()),
                            juce::dontSendNotification);

    // Every slot is cleared FIRST. Restoring only the slots the block names would leave
    // whichever LoRA the previous block had loaded sitting in slot 2, quietly joining a
    // generation nobody asked it to.
    for (int i = 0; i < kLoraSlots; ++i) {
        slots[static_cast<size_t>(i)].box.setSelectedId(1, juce::dontSendNotification);
        slots[static_cast<size_t>(i)].stepLo = 1;
        slots[static_cast<size_t>(i)].stepHi = static_cast<int>(stepsSlider.getValue());
    }

    if (auto* loras = settings.getProperty("loras", {}).getArray()) {
        int slot = 0;
        for (const auto& l : *loras) {
            const int i = juce::jlimit(0, kLoraSlots - 1, (int) l.getProperty("slot", slot));
            const auto name = l.getProperty("name", "").toString();
            for (int k = 0; k < loraFiles.size(); ++k)
                if (loraFiles[k].getFileNameWithoutExtension() == name) {
                    auto& sl = slots[static_cast<size_t>(i)];
                    sl.box.setSelectedId(k + 2, juce::dontSendNotification);
                    sl.strength.setValue((double) l.getProperty("strength", 1.0),
                                          juce::dontSendNotification);
                    if (auto* gate = l.getProperty("gate", {}).getArray(); gate != nullptr
                                                                            && gate->size() == 2) {
                        sl.stepLo = (int) (*gate)[0];
                        sl.stepHi = (int) (*gate)[1];
                    }
                    break;
                }
            ++slot;
        }
    }

    // The lane editor reads the slots, so it has to be told they changed -- otherwise the
    // step windows on screen belong to the block you just left.
    syncLoraLanes();
    resized();
    repaint();
}

double GenerateContent::maxDuration() const
{
    return secondsSlider.getMaximum();
}

void GenerateContent::setDuration(double seconds)
{
    if (seconds <= 0.0) return;
    // Clamped, not refused: a block dragged to four seconds is still a block, and the
    // slider's floor is what the model will accept.
    secondsSlider.setValue(juce::jlimit(secondsSlider.getMinimum(), secondsSlider.getMaximum(),
                                         seconds),
                            juce::sendNotificationSync);
}

bool GenerateContent::generateExtension(const juce::File& source, double rangeStart,
                                         double totalSeconds)
{
    if (!source.existsAsFile())
    {
        statusLabel.setText("the block has no take to extend", juce::dontSendNotification);
        return false;
    }
    if (totalSeconds > secondsSlider.getMaximum() + 0.001)
    {
        // SAID OUT LOUD. Silently generating 380 seconds for a 400 second block would put
        // audio on the canvas that does not reach the end of the frame, and nothing on
        // screen would explain why.
        const auto msg = "block is " + juce::String(totalSeconds, 1) + "s - the model tops out at "
                       + juce::String(secondsSlider.getMaximum(), 0) + "s";
        statusLabel.setText(msg, juce::dontSendNotification);
        log(msg);
        return false;
    }
    if (rangeStart >= totalSeconds - 0.05)
    {
        statusLabel.setText("nothing to extend - drag the block out past its audio",
                             juce::dontSendNotification);
        return false;
    }

    initAudio = source;
    secondsSlider.setValue(totalSeconds, juce::sendNotificationSync);
    inpaintToggle.setToggleState(true, juce::sendNotificationSync);
    inpaintStart.setValue(rangeStart, juce::sendNotificationSync);
    inpaintEnd.setValue(totalSeconds, juce::sendNotificationSync);
    if (inpaintStrip != nullptr)
    {
        inpaintStrip->setTimeline(totalSeconds);
        inpaintStrip->setRange(rangeStart, totalSeconds);
    }
    generate();
    return true;
}

bool GenerateContent::generateRemix(const juce::File& source, double totalSeconds)
{
    if (!source.existsAsFile())
    {
        statusLabel.setText("the block has no take to remix", juce::dontSendNotification);
        return false;
    }
    if (totalSeconds > secondsSlider.getMaximum() + 0.001)
    {
        const auto msg = "block is " + juce::String(totalSeconds, 1) + "s - the model tops out at "
                       + juce::String(secondsSlider.getMaximum(), 0) + "s";
        statusLabel.setText(msg, juce::dontSendNotification);
        log(msg);
        return false;
    }

    initAudio = source;
    // INPAINT OFF. The take becomes init audio -- guidance for a new generation of the
    // whole block -- rather than a thing to preserve and fill around.
    inpaintToggle.setToggleState(false, juce::sendNotificationSync);
    secondsSlider.setValue(totalSeconds, juce::sendNotificationSync);
    generate();
    return true;
}

// ---- MIRA-GENERATE.md Phase 7: share -----------------------------------------------

// Reads the child's stdout off the message thread, same shape as PrepareWindow's reader.
// rclone reports progress on stderr, so both are wanted -- an upload that prints nothing
// for four minutes is indistinguishable from one that hung.
class GenerateContent::Uploader : public juce::Thread
{
public:
    Uploader(GenerateContent& ownerIn, juce::ChildProcess& procIn)
        : juce::Thread("rclone reader"), owner(&ownerIn), proc(procIn) {}

    void run() override
    {
        char chunk[2048];
        for (;;)
        {
            const int n = proc.readProcessOutput(chunk, sizeof(chunk));
            if (n <= 0) break;
            auto safe = owner;
            const auto text = juce::String::fromUTF8(chunk, n).trim();
            if (text.isNotEmpty())
                juce::MessageManager::callAsync([safe, text] {
                    if (safe.getComponent() != nullptr) safe.getComponent()->log(text);
                });
            if (threadShouldExit()) return;
        }
        if (threadShouldExit()) return;

        proc.waitForProcessToFinish(10000);
        const int code = proc.getExitCode();
        auto safe = owner;
        juce::MessageManager::callAsync([safe, code] {
            if (safe.getComponent() != nullptr) safe.getComponent()->onUploadFinished(code);
        });
    }

private:
    juce::Component::SafePointer<GenerateContent> owner;
    juce::ChildProcess& proc;
};

juce::String GenerateContent::rcloneBinary() const {
    // A configured path wins, so a version in a pyenv or a non-standard prefix works.
    if (const auto stored = database.getSetting("rclone_path"))
        if (juce::File(juce::String(*stored)).existsAsFile()) return juce::String(*stored);

    // A GUI app does not inherit a login shell's PATH, so `which` is no use here -- the
    // usual install prefixes are. Named, not guessed at runtime.
    for (const char* candidate : { "/opt/homebrew/bin/rclone", "/usr/local/bin/rclone",
                                    "/usr/bin/rclone", "/opt/local/bin/rclone" })
        if (juce::File(candidate).existsAsFile()) return candidate;
    return {};
}

juce::String GenerateContent::rcloneRemote() const {
    if (const auto stored = database.getSetting("rclone_remote"))
        return juce::String(*stored).trim();
    return {};
}

void GenerateContent::promptRcloneRemote() {
    const auto binary = rcloneBinary();
    if (binary.isEmpty()) {
        statusLabel.setText("rclone is not installed - brew install rclone, then set a remote",
                            juce::dontSendNotification);
        log("rclone not found in /opt/homebrew/bin, /usr/local/bin, /usr/bin or /opt/local/bin.");
        log("Install it, run `rclone config` once to add your service, then set the remote here.");
        return;
    }

    // What rclone itself says is configured, rather than asking the user to remember. If
    // the list is empty they have not run `rclone config` yet, and saying that is more
    // use than an empty text box.
    juce::String known;
    {
        juce::ChildProcess list;
        if (list.start(juce::StringArray { binary, "listremotes" },
                        juce::ChildProcess::wantStdOut))
            known = list.readAllProcessOutput().trim();
    }

    auto* window = new juce::AlertWindow("Upload destination",
        known.isEmpty()
            ? "No remotes are configured. Run `rclone config` in a terminal to add one, then come back."
            : "Configured remotes:\n" + known
              + "\n\nA destination is a remote plus an optional path, e.g. "
              + known.upToFirstOccurrenceOf("\n", false, false) + "deliveries/2026",
        juce::AlertWindow::NoIcon, this);
    window->addTextEditor("remote", rcloneRemote(), "Destination");
    window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true, juce::ModalCallbackFunction::create(
        [this, window](int result) {
            const auto value = window->getTextEditorContents("remote").trim();
            delete window;
            if (result != 1) return;
            database.setSetting("rclone_remote", value.isEmpty() ? std::optional<std::string>()
                                                                 : std::optional<std::string>(value.toStdString()));
            statusLabel.setText(value.isEmpty() ? juce::String("upload destination cleared")
                                                : "upload destination: " + value,
                                juce::dontSendNotification);
        }), false);
}

void GenerateContent::uploadExport(const juce::String& cueOrEmpty) {
    if (uploadProcess != nullptr && uploadProcess->isRunning()) {
        statusLabel.setText("an upload is already running", juce::dontSendNotification);
        return;
    }
    const auto binary = rcloneBinary();
    const auto remote = rcloneRemote();
    if (binary.isEmpty() || remote.isEmpty()) { promptRcloneRemote(); return; }

    const auto local = cueOrEmpty.isEmpty() ? exportRoot() : exportRoot().getChildFile(cueOrEmpty);
    if (!local.isDirectory()) {
        statusLabel.setText("nothing exported yet - export before uploading",
                            juce::dontSendNotification);
        return;
    }

    // <remote>/<project>/<cue>. The project name is part of the destination because a
    // shared drive holds more than one project, and "cue01" on its own says nothing.
    juce::String destination = remote;
    if (!destination.endsWithChar('/') && !destination.endsWithChar(':')) destination += "/";
    destination += projectFolder.getFileName();
    if (cueOrEmpty.isNotEmpty()) destination += "/" + cueOrEmpty;

    // copy, never sync: sync DELETES at the destination whatever is not in the source,
    // and pointing that at the wrong folder once is unrecoverable over a network.
    const juce::StringArray args { binary, "copy", local.getFullPathName(), destination,
                                    "--progress", "--stats=5s", "--stats-one-line" };

    uploadProcess = std::make_unique<juce::ChildProcess>();
    if (!uploadProcess->start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr)) {
        uploadProcess.reset();
        statusLabel.setText("could not start rclone", juce::dontSendNotification);
        return;
    }
    log("upload: " + args.joinIntoString(" "));
    statusLabel.setText("uploading to " + destination + "...", juce::dontSendNotification);
    uploadReader = std::make_unique<Uploader>(*this, *uploadProcess);
    uploadReader->startThread();
}

void GenerateContent::onUploadFinished(int exitCode) {
    if (uploadReader != nullptr) { uploadReader->stopThread(2000); uploadReader.reset(); }
    uploadProcess.reset();
    // rclone's own exit code, reported as-is. "Uploaded" over a non-zero exit is the one
    // thing this must never say (convention 6).
    statusLabel.setText(exitCode == 0 ? juce::String("upload finished")
                                      : "upload FAILED - rclone exit " + juce::String(exitCode)
                                        + " (see the console)",
                        juce::dontSendNotification);
    log(exitCode == 0 ? "upload finished" : "rclone exited " + juce::String(exitCode));
}

// ---- MIRA-GENERATE.md Phase 6: export ----------------------------------------------
//
// Renders go to <project>/export/<cue>/, not into the cue folder itself. The cue folder
// holds WORKING files -- shortfilm_cue01_v3.wav, the thing Keep made -- and the render is
// a second file of the same audio under a different name. Mixed together, the folder stops
// answering "which of these do I send?", which is the question the naming scheme exists to
// answer (§3.7).
juce::File GenerateContent::exportRoot() const {
    return projectFolder.getChildFile("export");
}

void GenerateContent::showExportMenu() {
    if (!projectFolder.isDirectory()) {
        statusLabel.setText("no project open -- export renders a project's cues",
                            juce::dontSendNotification);
        return;
    }
    const auto cues = listExistingCues();
    if (cues.isEmpty()) {
        statusLabel.setText("nothing to export -- Keep a take into a cue first",
                            juce::dontSendNotification);
        return;
    }

    juce::PopupMenu menu;
    menu.addSectionHeader("Export to " + exportRoot().getFileName() + "/");
    int id = 1;
    for (const auto& cue : cues) {
        const int n = projectFolder.getChildFile(cue)
                          .findChildFiles(juce::File::findFiles, false, "*.wav").size();
        menu.addItem(id++, cue + "  (" + juce::String(n) + ")", n > 0);
    }
    menu.addSeparator();
    menu.addItem(999, "Whole project  (" + juce::String(cues.size()) + " cues)");

    // Phase 7 lives in the same menu as Phase 6 because they are one errand: you export
    // in order to send. A separate button for "upload" would be a second thing to find.
    const auto remote = rcloneRemote();
    menu.addSeparator();
    menu.addSectionHeader("Share");
    menu.addItem(1000, remote.isEmpty() ? "Upload export... (set a destination)"
                                        : "Upload export to " + remote,
                  exportRoot().isDirectory() || remote.isEmpty());
    menu.addItem(1001, remote.isEmpty() ? "Set upload destination..."
                                        : "Change destination (" + remote + ")...");

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&exportButton),
                        [this, cues](int chosen) {
        if (chosen == 0) return;
        if (chosen == 1000) { uploadExport({}); return; }
        if (chosen == 1001) { promptRcloneRemote(); return; }
        if (chosen == 999) { exportProject(); return; }
        if (chosen >= 1 && chosen <= cues.size()) exportCue(cues[chosen - 1]);
    });
}

int GenerateContent::exportOneFolder(const juce::File& cueFolder, const juce::String& cueName,
                                     juce::StringArray& problems) {
    int written = 0;
    const auto destFolder = exportRoot().getChildFile(cueName);
    const auto projectName = projectFolder.getFileName();

    auto wavs = cueFolder.findChildFiles(juce::File::findFiles, false, "*.wav");
    wavs.sort();
    for (const auto& wav : wavs) {
        mira::ui::TakeEdit edit;
        std::optional<double> bpm;
        juce::String keyScale;

        // findByPath, never a string compare -- a project whose name carries an accent is
        // not hypothetical, and NFC/NFD is exactly how Phase 5b's bug hid (convention 9).
        const auto record = database.findByPath(wav.getFullPathName().toStdString());
        if (record.has_value()) {
            for (const auto& seg : database.findSegmentsForFile(record->id)) {
                const auto human = juce::JSON::parse(juce::String(seg.human));
                edit.startSeconds = seg.startSeconds;
                edit.endSeconds = seg.endSeconds;
                edit.fadeInSeconds = static_cast<double>(human.getProperty("fade_in", 0.0));
                edit.fadeOutSeconds = static_cast<double>(human.getProperty("fade_out", 0.0));
                edit.gainDb = static_cast<double>(human.getProperty("gain_db", 0.0));
                break;  // one segment per take IS the edit (Phase 5)
            }
            // BPM and key come from the caption layer, which already gates them: an
            // unsupported tempo is absent there and therefore absent from the filename.
            const auto fields = mira::extractCaptionFields(database, *record);
            bpm = fields.bpm;
            if (fields.keyScale.has_value()) keyScale = juce::String(*fields.keyScale);
        }

        const auto name = mira::ui::deliveryName(projectName, cueName, bpm, keyScale,
                                                  mira::ui::versionFromWorkingName(wav.getFileName()));
        const auto result = mira::ui::renderTake(wav, destFolder.getChildFile(name), edit);
        log((result.ok ? "export  " : "EXPORT FAILED  ") + cueName + "/" + result.message);
        if (result.ok) ++written;
        else problems.add(wav.getFileName() + ": " + result.message);
    }
    return written;
}

void GenerateContent::exportCue(const juce::String& cue) {
    juce::StringArray problems;
    const int n = exportOneFolder(projectFolder.getChildFile(cue), cue, problems);

    juce::String msg = juce::String(n) + (n == 1 ? " file" : " files") + " exported to export/" + cue;
    // Says what did not land, and how many, rather than reporting a clean success over a
    // partial one (convention 6).
    if (!problems.isEmpty()) msg += "  --  " + juce::String(problems.size()) + " failed: " + problems[0];
    statusLabel.setText(msg, juce::dontSendNotification);
    if (n > 0) exportRoot().getChildFile(cue).revealToUser();
}

void GenerateContent::exportProject() {
    juce::StringArray problems;
    int total = 0, cuesWithFiles = 0;
    for (const auto& cue : listExistingCues()) {
        const int n = exportOneFolder(projectFolder.getChildFile(cue), cue, problems);
        total += n;
        if (n > 0) ++cuesWithFiles;
    }

    juce::String msg = juce::String(total) + (total == 1 ? " file" : " files")
                     + " exported across " + juce::String(cuesWithFiles)
                     + (cuesWithFiles == 1 ? " cue" : " cues");
    if (!problems.isEmpty()) msg += "  --  " + juce::String(problems.size()) + " failed: " + problems[0];
    statusLabel.setText(msg, juce::dontSendNotification);
    if (total > 0) exportRoot().revealToUser();
}

juce::StringArray GenerateContent::listExistingCues() const {
    juce::StringArray cues;
    if (!projectFolder.isDirectory()) return cues;
    for (const auto& d : projectFolder.findChildFiles(juce::File::findDirectories, false)) {
        // "takes" is the scratch bin, not a cue -- it is where these files are coming
        // FROM (MIRA-GENERATE.md Phase 2a).
        if (d.getFileName() == "takes") continue;
        // Nor is "discarded" (the bin) or "export" (where Phase 6 puts the renders).
        // Both are project plumbing that happen to be directories; offering either as a
        // cue to Keep into is how a take ends up filed in the bin by accident.
        if (d.getFileName() == "discarded" || d.getFileName() == "export") continue;
        cues.add(d.getFileName());
    }
    cues.sort(true);
    return cues;
}

void GenerateContent::keepResult() {
    const auto wav = resultTile.getFile();
    if (!wav.existsAsFile()) return;

    // Phase 3: in a project, Keep is the moment a take stops being scratch and becomes a
    // cue. Without a project this is the SA3 Generate window and Keep means what it
    // always meant -- register where it lies, file it under "Generated".
    if (projectFolder.isDirectory()) {
        // One cue prompt for the whole selection: keeping four alts of the same cue is
        // the common case, and they are exactly the ones that get selected together.
        const auto selected = takeStack->getSelectedFiles();
        promptForCue(listExistingCues(), {}, this, [this, selected](juce::String cue) {
            cue = cue.trim();
            if (cue.isEmpty()) return; // no silent "untitled" cue
            for (const auto& f : selected) keepResultIntoCue(f, cue);
            takeStack->clearMultiSelection();
        });
        return;
    }

    const auto path = wav.getFullPathName().toStdString();
    const auto hash = mira::sha256File(path);
    database.upsertScannedFile(path, hash,
                                wav.getLastModificationTime().toMilliseconds() / 1000,
                                wav.getSize(),
                                juce::Time::getCurrentTime().toMilliseconds() / 1000);

    const auto record = database.findByPath(path);
    if (!record.has_value()) { log("could not register " + wav.getFileName()); return; }

    // Under one key, so it can never collide with a hand-edited human field and is
    // trivially recognisable later as "mira made this".
    if (!lastRecipe.isVoid())
        database.setHumanField(record->id, "$.generated",
                                juce::JSON::toString(lastRecipe, true).toStdString());

    auto collection = database.findCollectionByName("Generated");
    const auto id = collection.has_value() ? collection->id
                                           : database.createCollection("Generated");
    database.addFilesToCollection(id, { record->id });

    if (onLibraryChanged) onLibraryChanged();
    takeStack->markKept(wav, wav);
    statusLabel.setText(wav.getFileName() + " - kept in mira, collection \"Generated\"",
                        juce::dontSendNotification);
    log("kept: " + wav.getFileName());
}

// MIRA-GENERATE.md Phase 3. The take moves out of <project>/takes/ into
// <project>/<cue>/ and takes its working name (§3.7): {project}_{cue}_v{n}.
//
// A MOVE, and PRD §1's "no file ever moves" is not being broken. That rule protects the
// user's own library -- folders mira was pointed at. This is mira's own scratch output,
// and filing it is the entire point of the button.
//
// The version number comes from what is ALREADY IN THE CUE FOLDER, never a session
// counter (§5's fourth open question): reopening a project next week has to continue at
// v4, not restart at v1.

// ---- MIRA-GENERATE.md Phase 5: cut and fade ----------------------------------------
//
// One segment per take IS the edit. Not a list: a take is a single cue, and "which of
// these four segments is the one you meant?" is a question export should never have to
// ask. Re-trimming moves the same row, because its `human` holds the fades -- delete and
// re-create would drop the edit every time a handle moved.

void GenerateContent::loadEditFor(const juce::File& wav) {
    editFile = wav;
    editSegmentId = 0;
    if (!wav.existsAsFile()) { refreshEditControls(); return; }

    const auto record = database.findByPath(wav.getFullPathName().toStdString());
    if (!record.has_value()) { refreshEditControls(); return; } // not kept yet -- no row to hang an edit on

    for (const auto& seg : database.findSegmentsForFile(record->id)) {
        editSegmentId = seg.id;
        const auto human = juce::JSON::parse(juce::String(seg.human));
        fadeInSlider.setValue(static_cast<double>(human.getProperty("fade_in", 0.0)),
                               juce::dontSendNotification);
        fadeOutSlider.setValue(static_cast<double>(human.getProperty("fade_out", 0.0)),
                                juce::dontSendNotification);
        gainSlider.setValue(static_cast<double>(human.getProperty("gain_db", 0.0)),
                             juce::dontSendNotification);
        // The trim drawn on the waveform, so reopening a kept take SHOWS its edit rather
        // than only remembering it.
        preview.setSegments({ { seg.id, seg.startSeconds, seg.endSeconds, {} } });
        auditionStart = seg.startSeconds;
        auditionEnd = seg.endSeconds;
        preview.setFadeRange(seg.startSeconds, seg.endSeconds);
        preview.setFades(fadeInSlider.getValue(), fadeOutSlider.getValue());
        // The loop region IS the trim. Keeping the toggle's own state across takes is
        // deliberate: loop is a way of listening, not a property of the file, so turning
        // it on once should survive clicking through a stack of takes.
        preview.setLoop(preview.isLooping(), seg.startSeconds, seg.endSeconds);
        refreshEditControls();
        return;
    }

    fadeInSlider.setValue(0.0, juce::dontSendNotification);
    fadeOutSlider.setValue(0.0, juce::dontSendNotification);
    gainSlider.setValue(0.0, juce::dontSendNotification);
    preview.setSegments({});
    auditionStart = auditionEnd = 0.0;
    preview.setFadeRange(0.0, 0.0);
    preview.setFades(0.0, 0.0);
    // No trim: the whole take is the loop. getTotalLengthSeconds() is 0 until the
    // thumbnail has loaded, and a 0-length loop would wrap on every tick -- setLoop's
    // own guard (end > start) is what keeps that from being an audible machine-gun.
    preview.setLoop(preview.isLooping(), 0.0, preview.getTotalLengthSeconds());
    refreshEditControls();
}

void GenerateContent::writeEditFields() {
    if (editSegmentId == 0) {
        // Nothing to hang the values on yet. A full-length segment is the honest
        // representation of "the whole take, quieter": it declares the same range the
        // file already has, so export reads one code path whether or not a trim was made.
        const auto record = database.findByPath(editFile.getFullPathName().toStdString());
        const double length = preview.getTotalLengthSeconds();
        if (!record.has_value() || length <= 0.0) return;
        // Only once there is something to store -- an untouched take should not litter
        // the segments table just for being looked at.
        if (fadeInSlider.getValue() == 0.0 && fadeOutSlider.getValue() == 0.0
            && gainSlider.getValue() == 0.0) return;
        editSegmentId = database.createSegment(std::nullopt, record->id, 0.0, length, "{}");
        if (editSegmentId == 0) return;
    }
    // One key at a time into `human`, the same merge discipline setHumanField follows --
    // never a wholesale replace, so a tag someone put on this segment by hand survives a
    // fade being nudged.
    database.setSegmentHumanField(editSegmentId, "$.fade_in",
                                   juce::String(fadeInSlider.getValue(), 3).toStdString());
    database.setSegmentHumanField(editSegmentId, "$.fade_out",
                                   juce::String(fadeOutSlider.getValue(), 3).toStdString());
    database.setSegmentHumanField(editSegmentId, "$.gain_db",
                                   juce::String(gainSlider.getValue(), 2).toStdString());
}

void GenerateContent::applyTrimFromSelection() {
    auto selection = preview.getSelectionSeconds();
    if (!selection.has_value()) {
        statusLabel.setText("drag a selection on the waveform first", juce::dontSendNotification);
        return;
    }
    const auto record = database.findByPath(editFile.getFullPathName().toStdString());
    if (!record.has_value()) {
        // A take still sitting in takes/ has no library row, so there is nowhere to put a
        // non-destructive edit. Saying so beats writing it somewhere that will not be
        // read back (convention 6).
        statusLabel.setText("Keep this take first - an edit is stored against the library row",
                            juce::dontSendNotification);
        return;
    }

    const double a = juce::jmin(selection->first, selection->second);
    const double b = juce::jmax(selection->first, selection->second);
    if (b - a < 0.05) { statusLabel.setText("selection too short", juce::dontSendNotification); return; }

    if (editSegmentId != 0) database.updateSegmentRange(editSegmentId, a, b);
    else editSegmentId = database.createSegment(std::nullopt, record->id, a, b, "{}");

    writeEditFields();
    preview.setSegments({ { editSegmentId, a, b, {} } });
    auditionStart = a; auditionEnd = b;
    preview.setFadeRange(a, b);
    preview.setLoop(preview.isLooping(), a, b);
    preview.clearSelection();
    refreshEditControls();
    statusLabel.setText("trimmed to " + juce::String(a, 2) + "s - " + juce::String(b, 2) + "s"
                         + "  (nothing written to the file until export)",
                         juce::dontSendNotification);
    if (onLibraryChanged) onLibraryChanged();
}

void GenerateContent::clearTrim() {
    if (editSegmentId == 0) return;
    database.deleteSegment(editSegmentId);
    editSegmentId = 0;
    preview.setSegments({});
    auditionStart = auditionEnd = 0.0;
    preview.setLoop(preview.isLooping(), 0.0, preview.getTotalLengthSeconds());
    refreshEditControls();
    statusLabel.setText("trim removed - the take is full length again", juce::dontSendNotification);
    if (onLibraryChanged) onLibraryChanged();
}

void GenerateContent::auditionEdit() {
    if (auditioning) { stopAudition(); return; }
    if (!editFile.existsAsFile()) return;

    auditionStart = 0.0;
    auditionEnd = 0.0;
    if (editSegmentId != 0)
        if (auto seg = database.findSegmentById(editSegmentId)) {
            auditionStart = seg->startSeconds;
            auditionEnd = seg->endSeconds;
        }
    if (auditionEnd <= auditionStart) { auditionStart = 0.0; auditionEnd = 0.0; } // whole take

    auditioning = true;
    auditionButton.setButtonText("Stop");
    if (auditionEnd > auditionStart) preview.playRange(auditionStart, auditionEnd);
    else                             preview.playRange(0.0, 0.0);
}

void GenerateContent::stopAudition() {
    auditioning = false;
    auditionButton.setButtonText("Play edit");
    preview.setPlaybackGain(1.0f);
    preview.stopPlayback();
}

void GenerateContent::refreshEditControls() {
    const bool haveFile = editFile.existsAsFile();
    const bool haveTrim = editSegmentId != 0;
    trimButton.setEnabled(haveFile);
    clearTrimButton.setEnabled(haveTrim);

    // NOT gated on haveTrim. Gain and fades have nothing to do with trimming -- a take
    // you want 3 dB down, or faded out at the end, need not be cut at all -- but they
    // were dead until a trim existed, because the values are stored on a `segments` row
    // and no row existed yet. Reported as "what's the gain slider there, it doesn't
    // move": it was disabled, which at this size is indistinguishable from broken.
    // writeEditFields() now creates a full-length segment on first use instead.
    const bool haveRow = haveFile
                      && database.findByPath(editFile.getFullPathName().toStdString()).has_value();
    for (auto* sl : { &fadeInSlider, &fadeOutSlider, &gainSlider }) sl->setEnabled(haveRow);

    // Trim needs a selection, so it is only live when there is one -- and the label says
    // which of the two things is missing rather than leaving a dead button to explain
    // itself.
    const auto selection = preview.getSelectionSeconds();
    trimButton.setEnabled(haveFile && selection.has_value());

    if (!haveFile) {
        editLabel.setText("", juce::dontSendNotification);
    } else if (selection.has_value()) {
        const double a2 = juce::jmin(selection->first, selection->second);
        const double b2 = juce::jmax(selection->first, selection->second);
        editLabel.setText("selected " + juce::String(a2, 2) + "s - " + juce::String(b2, 2)
                           + "s  (" + juce::String(b2 - a2, 2) + "s)", juce::dontSendNotification);
    } else if (!haveTrim) {
        editLabel.setText("drag across the waveform to select a range", juce::dontSendNotification);
    } else if (auto seg = database.findSegmentById(editSegmentId)) {
        editLabel.setText("trimmed " + juce::String(seg->startSeconds, 2) + "s - "
                           + juce::String(seg->endSeconds, 2) + "s  ("
                           + juce::String(seg->endSeconds - seg->startSeconds, 2) + "s)",
                           juce::dontSendNotification);
    }
}

void GenerateContent::keepResultIntoCue(const juce::File& wav, const juce::String& cueName) {
    const auto cueSlug = slugify(cueName);
    const auto projectSlug = slugify(projectFolder.getFileName());
    if (cueSlug.isEmpty()) { log("cue name has no usable characters: " + cueName); return; }

    auto cueFolder = projectFolder.getChildFile(cueSlug);
    if (!cueFolder.isDirectory() && !cueFolder.createDirectory().wasOk()) {
        log("could not create cue folder: " + cueFolder.getFullPathName());
        return;
    }

    const auto base = projectSlug + "_" + cueSlug + "_v";
    int version = 1;
    for (const auto& f : cueFolder.findChildFiles(juce::File::findFiles, false, base + "*.wav")) {
        auto n = f.getFileNameWithoutExtension().fromLastOccurrenceOf("_v", false, false).getIntValue();
        if (n >= version) version = n + 1;
    }

    auto target = cueFolder.getChildFile(base + juce::String(version) + ".wav");
    const auto sourcePath = wav.getFullPathName().toStdString();

    // Stop reading the file before moving it -- the preview holds it open, exactly as
    // discardResult already has to do before moving one to the Trash.
    preview.setFile({});
    resultTile.setFile({});
    if (!wav.moveFileTo(target)) {
        log("could not file take into " + cueFolder.getFileName());
        preview.setFile(wav);
        resultTile.setFile(wav);
        return;
    }
    // The recipe sidecar travels with the take; a take whose .json was left behind in
    // takes/ would lose its settings the moment Clean up ran.
    if (auto sidecar = wav.withFileExtension("json"); sidecar.existsAsFile())
        sidecar.moveFileTo(target.withFileExtension("json"));

    const auto path = target.getFullPathName().toStdString();
    // If a scan had already indexed the take in takes/, follow the row rather than
    // leaving a stale path behind and inserting a second one.
    database.moveFilePath(sourcePath, path);
    database.upsertScannedFile(path, mira::sha256File(path),
                                target.getLastModificationTime().toMilliseconds() / 1000,
                                target.getSize(),
                                juce::Time::getCurrentTime().toMilliseconds() / 1000);

    const auto record = database.findByPath(path);
    if (!record.has_value()) { log("could not register " + target.getFileName()); return; }

    // The recipe of THIS take, read from the sidecar that travelled with it -- not
    // lastRecipe, which is whatever was generated most recently. With a stack of takes
    // (Phase 4) those are routinely different: keeping the third take after generating a
    // fourth would otherwise record the fourth one's settings. The sidecars beside the
    // audio ARE the record, as the code that writes them already says.
    auto recipe = recipeFor(target);
    if (!recipe.isVoid()) {
        // §3.6: a SIBLING of the analysis toolchain under `provenance`, merged in, so a
        // later analysis pass and this can both be true of the same file.
        database.setProvenanceField(record->id, "$.generation",
                                     juce::JSON::toString(recipe, true).toStdString());
        // Kept where it has always been kept too -- an existing take's `$.generated`
        // reader should not have to know about Phase 3 to keep working.
        database.setHumanField(record->id, "$.generated",
                                juce::JSON::toString(recipe, true).toStdString());
    }
    // §3.7: "Chosen is v1; alts number after it." Editable afterwards -- this is a
    // starting position, not a verdict, which is why it lives in `human`.
    database.setHumanField(record->id, "$.status", version == 1 ? "\"chosen\"" : "\"alt\"");
    database.setHumanField(record->id, "$.cue", "\"" + cueSlug.toStdString() + "\"");

    if (onLibraryChanged) onLibraryChanged();
    // The row FOLLOWS the file into the cue and moves up into KEPT, rather than
    // vanishing: what has been kept so far is the thing the session is steered by.
    takeStack->markKept(wav, target);
    statusLabel.setText(target.getFileName() + " - kept in " + cueSlug, juce::dontSendNotification);
    log("kept: " + cueSlug + "/" + target.getFileName());
}

// Trash, not delete: the result is still audible in the preview when this is pressed,
// and an accidental click on the wrong one should be recoverable.
// Where a discarded take goes. In a PROJECT it goes to <project>/discarded/, not to the
// Trash: the sidebar lists the project's folders, and a discard bin that is a real folder
// is a place like every other place -- listed, countable, and recoverable by dragging a
// file back out. In the Trash it is none of those, and the DISCARDED rows only survived
// until the window closed, so "what did I already try?" had no answer the next morning.
//
// Without a project (the SA3 Generate window) there is nowhere to put one, so the Trash
// remains the answer there.
bool GenerateContent::discardOne(const juce::File& wav) {
    const auto sidecar = wav.withFileExtension("json");
    if (projectFolder.isDirectory()) {
        auto bin = projectFolder.getChildFile("discarded");
        bin.createDirectory();
        auto target = bin.getChildFile(wav.getFileName());
        for (int i = 2; target.existsAsFile(); ++i)
            target = bin.getChildFile(wav.getFileNameWithoutExtension() + "-" + juce::String(i) + ".wav");
        const bool ok = wav.moveFileTo(target);
        // The recipe follows the audio. A take without its .json cannot be reproduced,
        // which is most of the reason to keep a discarded one at all.
        if (ok && sidecar.existsAsFile())
            sidecar.moveFileTo(target.withFileExtension("json"));
        if (ok) database.moveFilePath(wav.getFullPathName().toStdString(),
                                       target.getFullPathName().toStdString());
        return ok;
    }
    const bool ok = wav.moveToTrash();
    if (sidecar.existsAsFile()) sidecar.moveToTrash();
    return ok;
}

void GenerateContent::discardResult() {
    // Bulk discard, when rows have been Cmd-clicked. One confirmation for the set rather
    // than one per file: the whole point of selecting six is not to answer six questions.
    const auto selected = takeStack->getSelectedFiles();
    if (selected.size() > 1) {
        juce::NativeMessageBox::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon,
            "Discard " + juce::String(static_cast<int>(selected.size())) + " takes?",
            "They move to the project's discarded folder with their .json recipes, so "
            "the same idea is not generated twice -- and can be pulled back out.",
            this,
            juce::ModalCallbackFunction::create([this, selected](int result) {
                if (result == 0) return;
                int moved = 0;
                for (const auto& f : selected) {
                    if (f == resultTile.getFile()) { preview.setFile({}); resultTile.setFile({}); }
                    if (discardOne(f)) ++moved;
                    takeStack->markDiscarded(f);
                }
                takeStack->clearMultiSelection();
                if (sidebar != nullptr) sidebar->rebuild();
                statusLabel.setText(juce::String(moved) + " takes discarded",
                                     juce::dontSendNotification);
                log("discarded " + juce::String(moved) + " takes");
            }));
        return;
    }

    const auto wav = resultTile.getFile();
    if (!wav.existsAsFile()) return;

    preview.setFile({});                 // stop reading the file we are about to move
    resultTile.setFile({});
    const auto name = wav.getFileName();
    const bool ok = discardOne(wav);

    // Gone from the stack either way: if the move to Trash failed the file is still
    // there, but leaving a row whose buttons no longer do anything is worse than a log
    // line saying what happened.
    takeStack->markDiscarded(wav);
    revealButton.setEnabled(false);
    if (sidebar != nullptr) sidebar->rebuild();
    statusLabel.setText(ok ? name + (projectFolder.isDirectory() ? " - moved to discarded/"
                                                                 : juce::String(" - moved to Trash"))
                           : "could not discard " + name,
                        juce::dontSendNotification);
    log(ok ? "discarded: " + name : "could not discard " + name);
}

void GenerateContent::cleanupUnkept() {
    if (!outputFolder.isDirectory()) return;
    // "Kept" is defined by the library, not by a local list -- the DB row is the record,
    // so a file kept in an earlier session is still safe today.
    juce::Array<juce::File> doomed;
    juce::int64 bytes = 0;
    for (const auto& f : outputFolder.findChildFiles(juce::File::findFiles, false, "*.wav")) {
        if (database.findByPath(f.getFullPathName().toStdString()).has_value()) continue;
        doomed.add(f);
        bytes += f.getSize();
    }
    if (doomed.isEmpty()) {
        statusLabel.setText("nothing to clean up - every generation here is kept",
                            juce::dontSendNotification);
        return;
    }

    const auto mb = juce::String(bytes / (1024.0 * 1024.0), 0);
    juce::NativeMessageBox::showOkCancelBox(
        juce::MessageBoxIconType::WarningIcon,
        "Clean up " + juce::String(doomed.size()) + " unkept generation(s)?",
        "Moves " + juce::String(doomed.size()) + " file(s) (" + mb + " MB) from\n"
            + outputFolder.getFullPathName() + "\nto the Trash, with their .json recipes.\n\n"
            "Anything you pressed Keep on is left alone.",
        this,
        juce::ModalCallbackFunction::create([this, doomed](int result) {
            if (result == 0) return;                  // cancelled
            int moved = 0;
            for (const auto& f : doomed) {
                const auto sidecar = f.withFileExtension("json");
                // Out of the stack before the file goes, not after -- a row whose
                // file has been trashed underneath it would paint from a thumbnail of
                // something that is no longer there. Clean up FORGETS rather than marking
                // discarded: it is a bulk sweep of things never judged, not a decision.
                takeStack->forget(f);
                if (f.moveToTrash()) ++moved;
                if (sidecar.existsAsFile()) sidecar.moveToTrash();
            }
            statusLabel.setText(juce::String(moved) + " file(s) moved to Trash",
                                juce::dontSendNotification);
            log("cleaned up " + juce::String(moved) + " unkept generation(s)");
        }));
}

juce::var GenerateContent::buildLoraSpecs() {
    juce::Array<juce::var> specs;
    for (int i = 0; i < kLoraSlots; ++i) {
        const auto& sl = slots[static_cast<size_t>(i)];
        const int sel = sl.box.getSelectedId();
        if (sel <= 1 || sel - 1 > loraFiles.size()) continue;
        auto* spec = new juce::DynamicObject();
        spec->setProperty("path", loraFiles[sel - 2].getFullPathName());
        spec->setProperty("strength", sl.strength.getValue());
        // A PAIR, not a "1-8" string: resolve_steps unpacks `lo, hi = raw`. Same
        // convention as sa3_gradio -- min at 1 means "from the start", max at or beyond
        // the schedule length means "to the last step", so a maxed slider still means
        // "all steps" after Steps changes.
        const int lo = sl.stepLo;
        const int hi = sl.stepHi;
        const int nSteps = static_cast<int>(stepsSlider.getValue());
        if (lo > 1 || hi < nSteps) {
            juce::Array<juce::var> range { lo > 1 ? juce::var(lo) : juce::var(),
                                            hi < nSteps ? juce::var(hi) : juce::var() };
            spec->setProperty("steps", juce::var(range));
        }
        specs.add(juce::var(spec));
    }
    return specs.isEmpty() ? juce::var() : juce::var(specs);
}

void GenerateContent::chooseInitAudio() {
    auto chooser = std::make_shared<juce::FileChooser>("Init audio (audio2audio / inpaint source)",
                                                        juce::File(), "*.wav;*.aiff;*.flac;*.mp3");
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectFiles,
                         [this, chooser](const juce::FileChooser& fc) {
        const auto f = fc.getResult();
        if (!f.existsAsFile()) return;
        initAudio = f;
        initLabel.setText(f.getFileName(), juce::dontSendNotification);
        // One path in, whether it arrived by picker or by drop, so the strip can never
        // be showing a different file from the one that gets sent.
        if (inpaintStrip != nullptr) { inpaintStrip->setFile(f); resized(); }
    });
}

void GenerateContent::chooseOutputFolder() {
    auto chooser = std::make_shared<juce::FileChooser>("Where to write generated audio",
                                                        outputFolder, "");
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectDirectories,
                         [this, chooser](const juce::FileChooser& fc) {
        const auto d = fc.getResult();
        if (d.isDirectory()) { outputFolder = d; log("output folder: " + d.getFullPathName()); }
    });
}

void GenerateContent::startWorker() {
    modelLoaded = false;   // a fresh process has no weights in it
    // Attach to the shared worker rather than spawning one. Registering the callbacks
    // through the hub is the part that matters: Sa3Worker has ONE onLog and ONE onExit,
    // so with several windows attached directly, whoever assigned last would silently
    // own them and every other console would go quiet.
    if (hubToken == 0)
        hubToken = hub.addListener({
            [this](juce::String line) { log(line); },
            [this](int code) {
                statusLabel.setText("worker exited (" + juce::String(code) + ")",
                                     juce::dontSendNotification);
                setBusy(false, {});
            }});

    juce::String error;
    if (hub.get(error) == nullptr) {
        statusLabel.setText("worker failed to start", juce::dontSendNotification);
        log(error);
        generateButton.setEnabled(false);
        encodeButton.setEnabled(false);
        return;
    }
    statusLabel.setText("worker running - models load on first generate (~44 s)",
                        juce::dontSendNotification);
}

void GenerateContent::generate() {
    if (busy || !hub.isRunning()) return;

    outputFolder.createDirectory();
    // "mira-20260915-095014.wav" says nothing about what made it. By convention every
    // trained caption opens with its trigger(s) and only then starts "Key: value" pairs
    // -- so the leading colon-free tokens ARE the triggers, and they are the single most
    // useful thing to see in Finder or a DAW browser.
    juce::StringArray triggers;
    for (const auto& piece : juce::StringArray::fromTokens(promptEditor.getText(), ",", "")) {
        const auto p = piece.trim();
        if (p.isEmpty() || p.contains(":")) break;   // first Key: pair ends the triggers
        triggers.add(juce::File::createLegalFileName(p));
        if (triggers.size() >= 3) break;
    }
    juce::String autoName = (triggers.isEmpty() ? juce::String("mira") : triggers.joinIntoString("-"))
                          + "-s" + juce::String(static_cast<int>(seedSlider.getValue()));
    if (cfgSlider.getValue() != 1.0)                 // omitted when it is the default
        autoName += "-cfg" + juce::String(cfgSlider.getValue(), 1);
    autoName += "-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");

    const auto base = nameEditor.getText().trim().isNotEmpty()
                          ? juce::File::createLegalFileName(nameEditor.getText().trim())
                          : autoName;
    // Never silently overwrite a take someone might still want.
    auto wav = outputFolder.getChildFile(base + ".wav");
    for (int n = 2; wav.existsAsFile(); ++n)
        wav = outputFolder.getChildFile(base + "-" + juce::String(n) + ".wav");

    auto req = new juce::DynamicObject();
    req->setProperty("cmd", "generate");
    req->setProperty("prompt", promptEditor.getText());
    req->setProperty("cfg", cfgSlider.getValue());
    if (negativeEditor.getText().trim().isNotEmpty())
        req->setProperty("negative_prompt", negativeEditor.getText().trim());
    req->setProperty("seconds", secondsSlider.getValue());
    req->setProperty("steps", static_cast<int>(stepsSlider.getValue()));
    req->setProperty("seed", static_cast<int>(seedSlider.getValue()));
    req->setProperty("out", wav.getFullPathName());

    const auto specs = buildLoraSpecs();
    if (!specs.isVoid()) req->setProperty("loras", specs);

    {
        auto* r = new juce::DynamicObject();
        r->setProperty("prompt", promptEditor.getText().trim());
        if (negativeEditor.getText().trim().isNotEmpty())
            r->setProperty("negative_prompt", negativeEditor.getText().trim());
        r->setProperty("seed", static_cast<int>(seedSlider.getValue()));
        r->setProperty("cfg", cfgSlider.getValue());
        r->setProperty("steps", static_cast<int>(stepsSlider.getValue()));
        r->setProperty("seconds", secondsSlider.getValue());
        // LoRAs by NAME as well as path -- a path goes stale the moment the file moves,
        // and the name is what the dropdown showed when the choice was made.
        juce::Array<juce::var> used;
        for (int i = 0; i < kLoraSlots; ++i) {
            const auto& sl = slots[static_cast<size_t>(i)];
            const int sel = sl.box.getSelectedId();
            if (sel <= 1 || sel - 1 > loraFiles.size()) continue;
            auto* u = new juce::DynamicObject();
            u->setProperty("name", loraFiles[sel - 2].getFileNameWithoutExtension());
            u->setProperty("strength", sl.strength.getValue());
            u->setProperty("gate", juce::var(juce::Array<juce::var>{
                sl.stepLo, sl.stepHi }));
            used.add(juce::var(u));
        }
        if (!used.isEmpty()) r->setProperty("loras", juce::var(used));
        lastRecipe = juce::var(r);
    }

    if (initAudio.existsAsFile()) {
        if (inpaintToggle.getToggleState()) {
            // Inpainting keeps everything OUTSIDE the range bit-exact and regenerates
            // only what is inside it, so the source is passed as inpaint_audio.
            juce::Array<juce::var> range { inpaintStart.getValue(), inpaintEnd.getValue() };
            req->setProperty("inpaint_audio", initAudio.getFullPathName());
            req->setProperty("inpaint_range", juce::var(range));
        } else {
            req->setProperty("init_audio", initAudio.getFullPathName());
        }
    }

    busySteps = static_cast<int>(stepsSlider.getValue());
    busySeconds = secondsSlider.getValue();
    busyHadLoad = !modelLoaded;
    setBusy(true, "generating");
    genProgress.start(busySteps, busySeconds, busyHadLoad);
    resized();
    juce::String sendError;
    auto* w = hub.get(sendError);
    if (w == nullptr) { statusLabel.setText("worker is not running", juce::dontSendNotification); return; }
    w->send(req, [this, wav](bool ok, juce::var payload) {
        const double took = genProgress.elapsedSeconds();
        setBusy(false, {});
        if (!ok) {
            log("generate failed: " + payload.getProperty("error", "unknown").toString());
            statusLabel.setText("generation failed - see log", juce::dontSendNotification);
            return;
        }
        // The recipe beside the audio. It exists because this information otherwise
        // lives only in the log and dies with the session -- and a generation you cannot
        // reproduce is a generation you cannot learn from.
        if (auto* obj = lastRecipe.getDynamicObject()) {
            obj->setProperty("file", wav.getFileName());
            obj->setProperty("created", juce::Time::getCurrentTime().toISO8601(true));
            obj->setProperty("wall_ms", payload.getProperty("wall_ms", 0));
            wav.withFileExtension("json").replaceWithText(juce::JSON::toString(lastRecipe, false));
        }
        // addTake focuses it, and onFocused sets preview/resultTile and enables the
        // buttons -- one path, so a take opened by clicking an older row is in exactly
        // the same state as one that just finished rendering. (This comment described
        // what the code was MEANT to do; addTake was not actually firing onFocused, so a
        // new take showed the previous one's waveform until it was clicked.)
        // Do NOT steal the preview from something that is playing. A generation finishing
        // used to focus the new take, which loaded a different file into the one transport
        // and cut the audio off mid-bar -- while you were listening to decide whether the
        // previous one was any good, which is exactly when you are least willing to lose
        // it. The take still lands, and the NOW bay still shows it; it just does not take
        // the speaker away from you.
        const bool listening = preview.isPlaying();
        takeStack->addTake(wav, TakeStack::State::Pending, !listening);
        revealButton.setEnabled(true);
        if (onTakeGenerated) onTakeGenerated(wav);
        // Only a SUCCESSFUL run teaches the estimate. A failure stops early and would
        // drag k towards a number no real generation ever takes.
        modelLoaded = true;
        genProgress.learn(took, busySteps, busySeconds, busyHadLoad);
        database.setSetting("gen_calibration", juce::String(genProgress.getCalibration(), 6).toStdString());
        const auto ms = static_cast<int>(payload.getProperty("wall_ms", 0));
        statusLabel.setText(wav.getFileName() + " - done in "
                            + juce::String(ms / 1000.0, 1) + "s"
                            + (listening ? " - still playing the last one, click the new take to hear it"
                                         : " - drag the tile into your DAW"),
                            juce::dontSendNotification);
    });
}

int GenerateContent::writeSidecars(const juce::File& folder, const juce::String& trigger) {
    // Driven from the database, not a directory walk: only analysed files have anything
    // to caption, and the DB path is the same one `mira caption` uses.
    auto prefix = folder.getFullPathName();
    if (!prefix.endsWithChar('/')) prefix << '/';
    int written = 0;
    // Prefix-filtered in C++ rather than with a SQL LIKE: a folder name containing % or
    // _ would otherwise act as a wildcard and silently pull in unrelated files.
    for (const auto& record : database.queryFiles("analyzed_at IS NOT NULL")) {
        const juce::String path(record.path);
        if (!path.startsWith(prefix)) continue;
        const juce::File audio(path);
        if (!audio.existsAsFile()) continue;
        const auto fields = mira::extractCaptionFields(database, record);
        const auto json = mira::renderSa3SidecarJson(fields, trigger.toStdString());
        auto sidecar = audio.getSiblingFile(audio.getFileNameWithoutExtension() + ".json");
        if (sidecar.replaceWithText(juce::String(json))) ++written;
    }
    return written;
}

void GenerateContent::zipLatents(const juce::File& latentsDir, const juce::File& zipOut) {
    juce::ZipFile::Builder builder;
    for (const auto& f : latentsDir.findChildFiles(juce::File::findFiles, false))
        builder.addFile(f, 6, f.getFileName());   // flat: the trainer expects a flat dir
    zipOut.deleteFile();
    if (auto stream = std::unique_ptr<juce::FileOutputStream>(zipOut.createOutputStream())) {
        double progressOut = 0.0;
        builder.writeToStream(*stream, &progressOut);
    }
}

void GenerateContent::chooseEncodeFolder() {
    if (busy || !hub.isRunning()) return;
    const auto trigger = triggerEditor.getText().trim();
    if (trigger.isEmpty()) {
        statusLabel.setText("pick a trigger token first", juce::dontSendNotification);
        return;
    }
    auto chooser = std::make_shared<juce::FileChooser>("Folder of audio for this LoRA",
                                                        juce::File(), "");
    const auto flags = juce::FileBrowserComponent::openMode
                     | juce::FileBrowserComponent::canSelectDirectories;
    chooser->launchAsync(flags, [this, chooser, trigger](const juce::FileChooser& fc) {
        const auto dir = fc.getResult();
        if (!dir.isDirectory()) return;                     // cancelled

        // If this folder was prepared before, its sidecars already carry a trigger.
        // Re-using a different one silently retrains the same audio under a new token,
        // so say so rather than letting it pass.
        const auto existing = triggerOfFolder(dir);
        if (existing.isNotEmpty() && existing != trigger)
            log("note: this folder was previously prepared with trigger '" + existing
                + "', now using '" + trigger + "'");

        setBusy(true, "writing captions");
        const int n = writeSidecars(dir, trigger);
        log("captions: wrote " + juce::String(n) + " sidecar(s) with trigger '" + trigger + "'");
        if (n == 0) {
            setBusy(false, {});
            statusLabel.setText("no analysed files in that folder - scan and analyse it first",
                                juce::dontSendNotification);
            return;
        }

        const auto name = dir.getFileName().replaceCharacter(' ', '-').retainCharacters(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_");
        const auto outDir = studioRoot.getChildFile("latents").getChildFile(name);
        outDir.createDirectory();

        auto req = new juce::DynamicObject();
        req->setProperty("cmd", "pre_encode");
        req->setProperty("audio_dir", dir.getFullPathName());
        req->setProperty("output_dir", outDir.getFullPathName());
        setBusy(true, "encoding " + dir.getFileName());
        juce::String encodeError;
        auto* ew = hub.get(encodeError);
        if (ew == nullptr) { statusLabel.setText("worker is not running", juce::dontSendNotification); return; }
        ew->send(req, [this, outDir, name, trigger](bool ok, juce::var payload) {
            if (!ok) {
                setBusy(false, {});
                log("pre-encode failed: " + payload.getProperty("error", "unknown").toString());
                statusLabel.setText("encode failed - see log", juce::dontSendNotification);
                return;
            }
            const int enc = static_cast<int>(payload.getProperty("encoded", 0));
            const int err = static_cast<int>(payload.getProperty("errors", 0));

            // The check that matters: a latent sidecar with no trigger means the captions
            // did not reach the audio, and training would silently see the trigger alone.
            int tagged = 0, total = 0;
            for (const auto& j : outDir.findChildFiles(juce::File::findFiles, false, "*.json")) {
                if (j.getFileName() == "details.json") continue;
                ++total;
                if (juce::JSON::parse(j.loadFileAsString())
                        .getProperty("trigger", "").toString() == trigger) ++tagged;
            }
            log("latents: " + juce::String(tagged) + "/" + juce::String(total)
                + " carry trigger '" + trigger + "'");

            const auto zip = studioRoot.getChildFile(name + "-latents.zip");
            setBusy(true, "zipping");
            zipLatents(outDir, zip);
            setBusy(false, {});

            if (tagged == 0 && total > 0) {
                statusLabel.setText("WARNING: latents have NO tags - do not train on this",
                                    juce::dontSendNotification);
            } else {
                statusLabel.setText("ready: " + juce::String(enc) + " encoded, "
                                    + juce::String(err) + " error(s), " + juce::String(tagged)
                                    + " tagged -> " + zip.getFileName(),
                                    juce::dontSendNotification);
            }
            refreshDatasets();
            zip.revealToUser();
        });
    });
}

// One place that decides what is clickable. Generate needs BOTH a free worker and somewhere
// to put the result -- without the second test the canvas panel offered to generate into a
// folder no block owns.
void GenerateContent::updateBusyControls() {
    const bool haveTarget = outputFolder != juce::File();
    generateButton.setEnabled(!busy && haveTarget);
    encodeButton.setEnabled(!busy);
    stopButton.setEnabled(busy);
}

void GenerateContent::setBusy(bool nowBusy, const juce::String& what) {
    busy = nowBusy;
    updateBusyControls();
    if (busy) {
        busyStartMs = juce::Time::getMillisecondCounter();
        statusLabel.setText(what + "...", juce::dontSendNotification);
        startTimerHz(4);
    } else {
        genProgress.stop();
        startTimerHz(1);   // keep the pressure readout live while idle
    }
    resized();             // the strip takes height from the stack, or gives it back
}

void GenerateContent::stopGeneration() {
    // The worker is blocked inside MLX and will not read stdin until the current request
    // returns, so a polite "cancel" message would sit unread until the thing we want to
    // cancel has already finished. Killing the process is the only real stop.
    log("stopping worker...");
    // One worker serves every window, so this cancels anything another window had
    // queued too. Said out loud below rather than left as a surprise.
    hub.restart();
    setBusy(false, {});
    statusLabel.setText(hub.listenerCount() > 1
                            ? juce::String("stopped - worker restarted; any other window's run was cancelled too")
                            : juce::String("stopped - worker restarted, model will reload on next run"),
                        juce::dontSendNotification);
}

void GenerateContent::updatePressure() {
    const auto m = readMemory();
    juce::String text;
    // roundToInt, not String(double, 0): JUCE reads 0 decimal places as "unspecified"
    // and prints the full mantissa (63.7065%).
    text << "RAM " << juce::roundToInt(m.freePct) << "% free";
    if (m.swapUsedGb >= 0.1) text << "   swap " << juce::String(m.swapUsedGb, 1) << " GB";

    const bool warn = m.pressure >= 2 || m.freePct < 15.0;
    if (warn) text << "   MEMORY PRESSURE - close other SA3 processes";
    pressureLabel.setText(text, juce::dontSendNotification);
    pressureLabel.setColour(juce::Label::textColourId,
                            warn ? juce::Colours::orange : juce::Colours::white.withAlpha(0.45f));
}

void GenerateContent::timerCallback() {
    updatePressure();

    // Phase 5: the audition envelope. Stepped from here rather than rendered, which makes
    // it an APPROXIMATION of what export writes -- accurate enough to judge a fade by ear,
    // and deliberately not the thing that produces the file. The timer runs at a few Hz,
    // so a fade under about half a second will audibly step; that is a reason to render
    // the export properly, not a reason to trust this for the last word.
    // Applied to ORDINARY playback now, not to a separate "Play edit" mode. A second
    // play button for "the same audio, but as you just set it up" is a thing to remember
    // to press, and forgetting it means judging a fade you never heard. The trim bounds
    // come from the stored segment, so Play respects a trim whether or not it was made
    // this session.
    if (preview.isPlaying()) {
        const double pos = preview.getPlayPositionSeconds();
        const double end = auditionEnd > auditionStart ? auditionEnd : pos + 1.0;
        const double fadeIn = fadeInSlider.getValue();
        const double fadeOut = fadeOutSlider.getValue();
        float gain = static_cast<float>(std::pow(10.0, gainSlider.getValue() / 20.0));

        if (fadeIn > 0.0 && pos < auditionStart + fadeIn)
            gain *= static_cast<float>(juce::jlimit(0.0, 1.0, (pos - auditionStart) / fadeIn));
        if (fadeOut > 0.0 && pos > end - fadeOut)
            gain *= static_cast<float>(juce::jlimit(0.0, 1.0, (end - pos) / fadeOut));

        preview.setPlaybackGain(gain);
        // Looping owns the out point when it is on; stopping here would end the take on
        // its first pass and the loop would never come round.
        if (!preview.isLooping() && auditionEnd > auditionStart && pos >= auditionEnd - 0.01)
            preview.stopPlayback();
    } else if (auditioning) {
        stopAudition();
    }
    if (!busy) return;
    const auto secs = (juce::Time::getMillisecondCounter() - busyStartMs) / 1000;
    statusLabel.setText(statusLabel.getText().upToFirstOccurrenceOf(" [", false, false)
                            + " [" + juce::String(secs) + "s]", juce::dontSendNotification);
}

void GenerateContent::log(const juce::String& line) {
    workerLog.append(LogStore::Source::app, line.trimEnd());
}

void GenerateContent::paint(juce::Graphics& g) {
    // In the canvas the panel is part of ONE surface with the arrangement beside it, so
    // it takes the canvas's colour. The hardcoded near-black below is the generate
    // window's own, where it is the whole window and has nothing to match.
    g.fillAll(panelOnly ? MiraLookAndFeel::surface2 : juce::Colour(0xff1a1a1a));
    // The panel-mode divider, drawn as a grip rather than a gap -- an invisible drag
    // target is one nobody discovers.
    if (panelOnly && !panelDivider.isEmpty())
    {
        g.setColour(MiraLookAndFeel::border);
        const int cx = panelDivider.getCentreX(), cy = panelDivider.getCentreY();
        for (int i = -1; i <= 1; ++i)
            g.fillRect(cx + i * 10 - 6, cy - 1, 12, 2);
    }
}

// Everything in the right pane, laid out once. Called twice per resize: once to MEASURE
// (applyBounds=false) so the pane can be given a height tall enough for all of it, and
// once to place. One function rather than two that must agree -- a measure pass that
// drifts from the layout pass is how a control ends up half off the bottom of a viewport.
// How tall the prompt box has to be to show the whole prompt at this width. Measured with
// the editor's own font through a TextLayout rather than asking the editor, because the
// measure pass runs BEFORE any bounds are set -- getTextHeight() would be answering about
// whatever width the editor happened to have last time.
int GenerateContent::promptHeightFor(int width) const {
    const float inner = static_cast<float>(juce::jmax(40, width - 14));  // editor's own inset
    juce::AttributedString as;
    as.append(promptEditor.getText().isEmpty() ? juce::String("M") : promptEditor.getText(),
              laf.sansRegular(13.0f));
    juce::TextLayout layout;
    layout.createLayout(as, inner);
    // A floor so an empty prompt is still a box you can aim at, and a ceiling so pasting
    // an essay cannot push Generate off the bottom of the pane.
    return juce::jlimit(56, 220, juce::roundToInt(layout.getHeight()) + 14);
}

int GenerateContent::layoutRightPane(int width, bool applyBounds) {
    juce::Rectangle<int> r { 0, 0, width, 100000 };
    r = r.reduced(10, 8);
    int startY = r.getY();

    auto row = [&r, applyBounds](int h, int gap = 6) {
        auto x = r.removeFromTop(h);
        r.removeFromTop(gap);
        return applyBounds ? x : juce::Rectangle<int>();
    };
    auto place = [applyBounds](juce::Component& c, juce::Rectangle<int> b) {
        if (applyBounds) c.setBounds(b);
    };
    // "the title in the generate section can be better - better fonts, bold and nicer."
    // Letter-spaced small caps in the accent, with a rule running out to the edge --
    // a section marker that reads as structure rather than as another dim label among
    // the dim labels.
    auto heading = [&](juce::Label& l, const char* text) {
        auto line = row(22, 4);
        if (applyBounds) {
            juce::String spaced;
            for (auto c : juce::String(text)) { spaced += juce::String::charToString(c); spaced += " "; }
            l.setText(spaced.trim(), juce::dontSendNotification);
            l.setFont(juce::Font(juce::FontOptions(MiraLookAndFeel::textSize(10.5f), juce::Font::bold)));
            l.setColour(juce::Label::textColourId, MiraLookAndFeel::accent.withAlpha(0.85f));
            l.setBounds(line);
        }
    };

    // --- the prompt, first, because it is what the window is for
    // Grows with the prompt instead of clipping it. A fixed 72px box cut a seven-field
    // caption mid-line and left you scrolling a four-line editor to read one sentence.
    // The height is measured for THIS width with the editor's own font, so the measure
    // pass and the layout pass cannot disagree about how many lines there are.
    place(promptEditor, row(promptHeightFor(r.getWidth())));
    {
        auto line = row(26);
        place(buildPromptButton, line.removeFromLeft(130));
        line.removeFromLeft(6);
        place(negativeLabel, line.removeFromLeft(38));
        place(negativeEditor, line);
    }

    heading(loraHeading, "LORA");
    for (int i = 0; i < kLoraSlots; ++i) {
        auto& sl = slots[static_cast<size_t>(i)];
        // TWO rows per slot, not one. "the lora slider issue is moving from 1-8, the
        // steps are very small to move and slider is actually very difficult to move" --
        // the step sliders were about 40px wide for a range of 1 to 50, so one pixel was
        // more than one step and the grab area was narrower than the pointer. Width is
        // the fix, and width means giving each slot a second line.
        auto head = row(24, 2);
        place(sl.label, head.removeFromLeft(48));
        place(sl.box, head);

        const int labelW = 52;
        auto line = row(26, 4);
        place(sl.blendLabel, line.removeFromLeft(labelW).withTrimmedRight(4));
        place(sl.strength, line);

    }

    // All three step windows in ONE picture, under the three slots rather than split
    // across them: whether two LoRAs divide the run or fight over it is a question about
    // the SET of them, and it cannot be asked of three separate rows.
    place(loraLanes, row(LoraLanes::idealHeight(), 8));

    heading(settingsHeading, "SETTINGS");
    {
        // Every settings slider gets its name in the same column, so the four read as one
        // group rather than as a stack of anonymous bars with numbers on the end.
        const int labelW = 60;
        auto labelled = [&](juce::Label& l, juce::Slider& sl) {
            auto line = row(26, 4);
            place(l, line.removeFromLeft(labelW).withTrimmedRight(6));
            place(sl, line);
        };
        labelled(secondsLabel, secondsSlider);
        labelled(stepsLabel, stepsSlider);
        labelled(seedLabel, seedSlider);
        labelled(cfgLabel, cfgSlider);
    }
    // --- audio in: init audio and inpainting, one clearly-bounded section instead of a
    // row of controls that never said they belonged together.
    //
    // NOT IN A BLOCK. On the canvas the block IS the audio in: its take is the source and
    // its empty tail is the range, so a second place to load a file and drag a range is a
    // second, contradictory answer to a question the geometry already settles.
    if (panelOnly)
    {
        if (applyBounds)
        {
            inpaintHeading.setBounds({});  audioInCollapse.setBounds({});
            initAudioButton.setBounds({}); clearInitButton.setBounds({});
            inpaintToggle.setBounds({});   initLabel.setBounds({});
            inpaintHelp.setBounds({});     inpaintStart.setBounds({}); inpaintEnd.setBounds({});
            if (inpaintStrip != nullptr) inpaintStrip->setBounds({});
        }
    }
    else
    {
    heading(inpaintHeading, "AUDIO IN");
    if (applyBounds) {
        // The arrow sits on the heading's own line, at the right, where the rule ends.
        auto h = inpaintHeading.getBounds();
        audioInCollapse.setButtonText(audioInCollapsed ? juce::String(juce::CharPointer_UTF8("\xe2\x96\xb8"))
                                                        : juce::String(juce::CharPointer_UTF8("\xe2\x96\xbe")));
        audioInCollapse.setBounds(h.removeFromRight(22).withSizeKeepingCentre(22, 18));
    }
    if (audioInCollapsed) {
        if (applyBounds) {
            initAudioButton.setBounds({}); clearInitButton.setBounds({});
            inpaintToggle.setBounds({});   initLabel.setBounds({});
            inpaintHelp.setBounds({});     inpaintStart.setBounds({}); inpaintEnd.setBounds({});
            if (inpaintStrip != nullptr) inpaintStrip->setBounds({});
        }
    } else {
        {
            auto line = row(26);
            place(initAudioButton, line.removeFromLeft(100));
            line.removeFromLeft(4);
            place(clearInitButton, line.removeFromLeft(26));
            line.removeFromLeft(6);
            place(inpaintToggle, line.removeFromRight(120));
            line.removeFromRight(6);
            place(initLabel, line);
        }
        if (inpaintStrip != nullptr) place(*inpaintStrip, row(62, 4));
        if (inpaintToggle.getToggleState()) {
            place(inpaintHelp, row(30, 4));
            // The numbers stay, beside the picture: a range dragged by eye still has to
            // be typeable when a cue has to start at exactly 12.0s.
            auto line = row(26);
            place(inpaintStart, line.removeFromLeft(line.getWidth() / 2 - 3));
            line.removeFromLeft(6);
            place(inpaintEnd, line);
        } else if (applyBounds) {
            inpaintHelp.setBounds({});
            inpaintStart.setBounds({}); inpaintEnd.setBounds({});
        }
    }
    }


    // --- where it lands. Grouped together and labelled, instead of the output folder
    // button sitting between "Add LoRA file..." and "Build prompt..." with nothing to say
    // they were unrelated ("clean - output folder - show in finder are confusing the way
    // it is placed").
    // In the canvas the BLOCK owns where its audio goes, so pointing the output somewhere
    // else would break the one thing that makes a block a block. The row is the generate
    // window's, where there is no block to own it.
    if (!panelOnly)
    {
        auto line = row(26);
        place(outFolderButton, line.removeFromLeft(120));
        line.removeFromLeft(6);
        place(nameEditor, line);
    }
    else if (applyBounds)
    {
        outFolderButton.setBounds({});
        nameEditor.setBounds({});
    }

    // --- the training bench, only on the SA3 Generate face (§3.4)
    if (trainingBenchVisible) {
        heading(loraHeading, "LORA"); // reuse is fine: the project face never draws it
        auto line = row(26);
        place(triggerLabel, line.removeFromLeft(46));
        place(triggerEditor, line.removeFromLeft(70));
        line.removeFromLeft(6);
        place(encodeButton, line.removeFromLeft(170));
        place(datasetsLabel, row(19, 4));
    } else if (applyBounds) {
        triggerLabel.setBounds({}); triggerEditor.setBounds({});
        encodeButton.setBounds({}); datasetsLabel.setBounds({});
    }

    // Two lines, not one. Four fixed-width buttons came to 432px against a pane that is
    // 360px wide once it is the slim column it was asked to be, so "Console" was drawn
    // as an ellipsis and the row ran under the edge. Widths are shares of the line now,
    // so the row fits whatever the pane is: Generate and Stop stay together because they
    // are the same control in two states, and the two navigational buttons go below.
    {
        auto line = row(32, 6);
        const int stopW = juce::jmin(90, line.getWidth() / 3);
        place(generateButton, line.removeFromLeft(line.getWidth() - stopW - 6));
        line.removeFromLeft(6);
        place(stopButton, line);
    }

    // EXTEND and REMIX, under Generate, because they are the same verb aimed at audio this
    // block already has. On the canvas toolbar they read as things the CANVAS does, which
    // is wrong twice: they act on one block, and they act through this generator's prompt.
    if (showBlockActions)
    {
        auto line = row(28, 2);
        const int half = (line.getWidth() - 6) / 2;
        place(extendButton, line.removeFromLeft(half));
        line.removeFromLeft(6);
        place(remixButton, line);
    }
    else if (applyBounds)
    {
        extendButton.setBounds({});
        remixButton.setBounds({});
    }
    // Show in Finder and the worker console are window furniture, not part of making a
    // block: in the canvas the block's folder is one click away on the block itself, and
    // the console belongs to the app rather than to this panel.
    if (!panelOnly)
    {
        auto line = row(26);
        const int half = (line.getWidth() - 6) / 2;
        place(revealButton, line.removeFromLeft(half));
        line.removeFromLeft(6);
        place(consoleButton, line);
    }
    else if (applyBounds)
    {
        revealButton.setBounds({});
        consoleButton.setBounds({});
    }
    return r.getY() - startY + 8;
}

void GenerateContent::resized() {
    auto r = getLocalBounds().reduced(10);

    auto top = r.removeFromTop(22);
    generatePaneToggle.setButtonText(generatePaneCollapsed ? "Generate >" : "Generate <");
    generatePaneToggle.setBounds(top.removeFromRight(96).withSizeKeepingCentre(96, 20));
    top.removeFromRight(6);
    pressureLabel.setBounds(top.removeFromRight(230));
    statusLabel.setBounds(top);
    // Directly under the status line, full width, and only while it is running. Anywhere
    // inside a pane costs that pane height permanently; here it costs 22px of the one
    // strip in the window that has nothing else to do.
    if (genProgress.isVisible()) {
        r.removeFromTop(2);
        genProgress.setBounds(r.removeFromTop(GenerateProgress::kHeight));
    }
    r.removeFromTop(6);


    // --- column mode: everything in one narrow column, controls over takes.
    if (panelOnly)
    {
        // THE PANEL IS THE GENERATOR AND NOTHING ELSE.
        //
        // It used to carry the take stack too, because it is the same GenerateContent the
        // generate window uses and I brought the whole thing across. That was wrong, and
        // it made the canvas incoherent: a block IS a generator with one result, so NOW,
        // TAKES, KEPT, DISCARDED and the keep/discard thumbs are four states and two
        // verbs the canvas has no use for. Generate, and the file lands on the block.
        // Want to keep the old one? Duplicate the block -- that is what Duplicate is for.
        //
        // The files are all still on disk in the block's folder; they are simply not
        // presented as a second hierarchy inside a view that already has one.
        rightView.setVisible(true);
        if (sidebar != nullptr) sidebar->setBounds({});
        takesView.setBounds({});
        takesLabel.setBounds({});
        cleanupButton.setBounds({});
        exportButton.setBounds({});
        panelDivider = {};

        const int innerWidth = r.getWidth() - (rightView.isVerticalScrollBarShown() ? 10 : 0);
        const int needed = layoutRightPane(innerWidth, false);
        rightView.setBounds(r);
        rightPane.setSize(innerWidth, juce::jmax(needed, r.getHeight()));
        layoutRightPane(innerWidth, true);
        return;
    }

    // Two containers. The right one is sized to its content and scrolls; the left takes
    // whatever is left, with a floor so the takes never disappear on a narrow window.
    // Folded, the right pane takes no width at all and the takes column gets the window.
    rightView.setVisible(!generatePaneCollapsed);
    juce::Rectangle<int> rightArea;
    if (!generatePaneCollapsed)
    {
        // Slimmer than half. The right pane is a column of labelled controls with a
        // fixed comfortable width; past that it is just a wide pane, while the waveform
        // is the one thing in this window that always wants more room.
        const int rightWidth = juce::jlimit(300, 390, r.getWidth() / 3);
        rightArea = r.removeFromRight(rightWidth);
        r.removeFromRight(8);
    }
    auto leftArea = r;

    rightView.setBounds(rightArea);
    if (!generatePaneCollapsed)
    {
        const int innerWidth = rightArea.getWidth() - (rightView.isVerticalScrollBarShown() ? 10 : 0);
        const int needed = layoutRightPane(innerWidth, false);
        rightPane.setSize(innerWidth, juce::jmax(needed, rightArea.getHeight()));
        layoutRightPane(innerWidth, true);
    }

    // --- far left: the project's own folders, so this window never has to send you to
    // the library to find out what is in the project.
    if (sidebar != nullptr)
    {
        const int w = juce::jlimit(140, 220, leftArea.getWidth() / 4);
        sidebar->setBounds(leftArea.removeFromLeft(w));
        leftArea.removeFromLeft(8);
    }

    // --- left: the takes
    {
        auto header = leftArea.removeFromTop(26);
        cleanupButton.setBounds(header.removeFromRight(92).withSizeKeepingCentre(92, 22));
        header.removeFromRight(6);
        exportButton.setBounds(header.removeFromRight(84).withSizeKeepingCentre(84, 22));
        header.removeFromRight(6);
        takesLabel.setBounds(header);
        leftArea.removeFromTop(4);
        takesView.setBounds(leftArea);
    }

    layoutTakeStack();
}

// The inside of the takes column, shared by the ordinary layout and the canvas's column
// mode -- two copies of this would be two places for the waveform's bounds to drift.
juce::Rectangle<int> GenerateContent::panelDividerArea() const { return panelDivider; }

void GenerateContent::mouseMove(const juce::MouseEvent& e) {
    setMouseCursor(panelOnly && panelDivider.contains(e.getPosition())
                       ? juce::MouseCursor::UpDownResizeCursor
                       : juce::MouseCursor::NormalCursor);
}

void GenerateContent::mouseDown(const juce::MouseEvent& e) {
    draggingSplit = panelOnly && panelDivider.expanded(0, 3).contains(e.getPosition());
}

void GenerateContent::mouseDrag(const juce::MouseEvent& e) {
    if (!draggingSplit) return;
    const int usable = juce::jmax(200, getHeight() - 40);
    panelSplit = juce::jlimit(0.15, 0.85, (double) (e.y - 40) / usable);
    resized();
}

void GenerateContent::mouseUp(const juce::MouseEvent&) { draggingSplit = false; }

void GenerateContent::layoutTakeStack() {
    if (takeStack != nullptr) {
        const int innerWidth = takesView.getWidth() - (takesView.isVerticalScrollBarShown() ? 10 : 0);
        takeStack->setSize(innerWidth, juce::jmax(takeStack->getIdealHeight(), takesView.getHeight()));

        // Inside the expanded row: the big waveform, then the drag tile with Keep and
        // Discard beside it. These are children of the STACK, so the bounds below are in
        // the stack's coordinate space -- which is exactly why nothing else may
        // addAndMakeVisible them (see the constructor).
        // Keep and Discard sit on the take's own row, at the right of its filename --
        // "discard and keep be on top with the title of wav". They are about the take,
        // so they belong on the take, not on a bar below a waveform that may be two
        // hundred pixels away from the name of the thing it is about.
        if (auto rowArea = takeStack->getFocusedRowArea(); !rowArea.isEmpty()) {
            auto r = rowArea.withTrimmedRight(8);
            discardButton.setBounds(r.removeFromRight(26).withSizeKeepingCentre(26, 22));
            r.removeFromRight(4);
            keepButton.setBounds(r.removeFromRight(26).withSizeKeepingCentre(26, 22));
        } else {
            keepButton.setBounds({}); discardButton.setBounds({});
        }

        auto slot = takeStack->getFocusedContentArea();
        if (!slot.isEmpty()) {
            // The drag tile is gone; the row is the drag handle now.
            resultTile.setBounds({});
            // ONE line, not two. The edit controls live INSIDE the waveform's transport
            // row, in space it holds back for them, so the scissors sit beside the
            // speaker instead of on a strip of their own underneath -- "keep all icons
            // in line of the speakers, not a row on the bottom".
            //
            // The fades are gone from here entirely: they are handles on the waveform
            // now. Two sliders and two labels cost most of a row and still never said
            // where the fade fell against the audio, which is the only thing you want to
            // know about a fade.
            const int reserve = 26 + 4 + 26 + 10 + 30 + 104;  // scissors, full, gain
            preview.setTransportReserve(reserve);
            preview.setBounds(slot);

            auto edit = preview.getTransportReserveArea();
            edit.translate(preview.getX(), preview.getY());
            trimButton.setBounds(edit.removeFromLeft(26).withSizeKeepingCentre(26, 22));
            edit.removeFromLeft(4);
            clearTrimButton.setBounds(edit.removeFromLeft(26).withSizeKeepingCentre(26, 22));
            edit.removeFromLeft(10);
            gainLabel.setBounds(edit.removeFromLeft(30));
            gainSlider.setBounds(edit.withSizeKeepingCentre(edit.getWidth(), 20));

            // Fades and the hint have no widgets any more. The hint said "drag across
            // the waveform to select a range", which is what a waveform does everywhere.
            fadeInSlider.setBounds({});  fadeInLabel.setBounds({});
            fadeOutSlider.setBounds({}); fadeOutLabel.setBounds({});
            editLabel.setBounds({});

        }
    }
}
