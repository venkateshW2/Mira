#include "GenerateWindow.h"

#include "mira/scan/Scanner.h"

#include "mira/caption/CaptionFields.h"
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
    g.setFont(juce::FontOptions(13.0f));
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
                                  mira::Database& databaseIn)
    : laf(lafIn), database(databaseIn), studioRoot(std::move(studioRootIn)),
      resultTile(lafIn) {

    statusLabel.setText("starting worker...", juce::dontSendNotification);
    addAndMakeVisible(statusLabel);

    promptEditor.setMultiLine(true, true);
    promptEditor.setReturnKeyStartsNewLine(true);
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
        rightPane.addAndMakeVisible(sl.box);

        sl.strength.setRange(0.0, 2.0, 0.05);
        sl.strength.setValue(1.0, juce::dontSendNotification);
        sl.strength.setSliderStyle(juce::Slider::LinearHorizontal);
        sl.strength.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 18);
        tip(sl.strength, "0 = base model (bypass), 1 = as trained, above 1 = overdriven.");
        rightPane.addAndMakeVisible(sl.strength);

        // Step gating: apply the LoRA only during part of the diffusion run. Early steps
        // shape structure and arrangement, late steps shape timbre and texture -- so this
        // separates "did it change the music?" from "did it just recolour the surface?".
        for (auto* st : { &sl.minStep, &sl.maxStep }) {
            st->setRange(1, 50, 1);
            st->setSliderStyle(juce::Slider::LinearHorizontal);
            st->setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 18);
            rightPane.addAndMakeVisible(*st);
        }
        sl.minStep.setValue(1, juce::dontSendNotification);
        sl.maxStep.setValue(8, juce::dontSendNotification);
        for (auto* pair : { &sl.blendLabel, &sl.structureLabel, &sl.timbreLabel }) {
            pair->setFont(juce::Font(juce::FontOptions(10.0f)));
            pair->setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
            pair->setJustificationType(juce::Justification::centredRight);
            rightPane.addAndMakeVisible(*pair);
        }
        sl.blendLabel.setText("blend", juce::dontSendNotification);
        sl.structureLabel.setText("structure", juce::dontSendNotification);
        sl.timbreLabel.setText("timbre", juce::dontSendNotification);
        tip(sl.minStep, "First step this LoRA applies to. Early steps shape structure.");
        tip(sl.maxStep, "Last step this LoRA applies to. Late steps shape timbre.");
    }

    keepButton.setEnabled(false);
    tip(keepButton, "Add this result to mira's library, under the \"Generated\" collection, with its full recipe.");
    keepButton.onClick = [this] { keepResult(); };
    addAndMakeVisible(keepButton);

    discardButton.setEnabled(false);
    tip(discardButton, "Move this result and its recipe to the Trash.");
    discardButton.onClick = [this] { discardResult(); };
    addAndMakeVisible(discardButton);

    tip(cleanupButton, "Trash every generation in the output folder you never pressed Keep on.");
    cleanupButton.onClick = [this] { cleanupUnkept(); };
    addAndMakeVisible(cleanupButton);

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
        tip(s, tipText);
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
    rightPane.addAndMakeVisible(generateButton);

    triggerLabel.setText("trigger", juce::dontSendNotification);
    rightPane.addAndMakeVisible(triggerLabel);
    triggerEditor.setText("xyr");
    tip(triggerEditor, "Rare token this LoRA is keyed to. One per film (Dune used zvq).");
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
        takesLabel.setText(takeStack->getTotalCount() == 0
                               ? juce::String("Takes")
                               : "Takes (" + juce::String(takeStack->getTotalCount()) + ")",
                           juce::dontSendNotification);
        resized();
    };
    rightPane.addAndMakeVisible(loraHeading);
    rightPane.addAndMakeVisible(settingsHeading);
    rightPane.addAndMakeVisible(inpaintHeading);
    {
        struct Named { juce::Label* label; const char* text; };
        for (auto n : { Named{ &secondsLabel, "duration" }, Named{ &stepsLabel, "steps" },
                         Named{ &seedLabel, "seed" }, Named{ &cfgLabel, "cfg" } }) {
            n.label->setText(n.text, juce::dontSendNotification);
            n.label->setFont(juce::Font(juce::FontOptions(11.5f, juce::Font::bold)));
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
    inpaintHelp.setFont(juce::Font(juce::FontOptions(11.0f)));
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

    takesLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
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

    syncLoraStepRanges();
    if (inpaintStrip != nullptr) inpaintStrip->setTimeline(secondsSlider.getValue());
    syncInpaintSliderRanges();

    // ---- Phase 5 edit controls. Hosted in the focused take row beside Keep/Discard,
    // because an edit belongs to ONE take and there is no sense in which the window has a
    // current trim independent of which take is open.
    tip(trimButton, "Drag a selection on the waveform, then trim to it. Non-destructive: "
                     "the file is untouched until export.");
    trimButton.onClick = [this] { applyTrimFromSelection(); };
    tip(clearTrimButton, "Remove the trim; the take goes back to full length.");
    clearTrimButton.onClick = [this] { clearTrim(); };
    tip(auditionButton, "Play the edit -- trimmed, faded and gained -- rather than the raw take.");
    auditionButton.onClick = [this] { auditionEdit(); };

    editLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
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
            f.slider->setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
            f.slider->setTextValueSuffix(f.suffix);
            f.slider->onValueChange = [this] { writeEditFields(); };
            tip(*f.slider, f.help);
            f.label->setText(f.name, juce::dontSendNotification);
            f.label->setFont(juce::Font(juce::FontOptions(11.0f)));
            f.label->setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
            f.label->setJustificationType(juce::Justification::centredRight);
        }
    }
    // A selection is the input to Trim, so the edit row has to react to one existing.
    // Without this the only hint that a drag on the waveform does anything was a tooltip
    // on a button -- which is why the gesture read as missing rather than as invisible.
    preview.onSelectionChanged = [this] { refreshEditControls(); };
    refreshEditControls();

    // Last, so nothing added above can take these back (see the note beside preview).
    takeStack->setHostedComponents({ &preview, &resultTile, &keepButton, &discardButton,
                                      &trimButton, &clearTrimButton, &auditionButton, &editLabel,
                                      &fadeInLabel, &fadeInSlider, &fadeOutLabel, &fadeOutSlider,
                                      &gainLabel, &gainSlider });

    tip(stopButton, "Kill and restart the worker. Next run reloads the model (~44s).");
    stopButton.onClick = [this] { stopGeneration(); };
    stopButton.setEnabled(false);
    rightPane.addAndMakeVisible(stopButton);

    tip(pressureLabel, "Free RAM / swap. Once swap fills, every step pages to disk.");
    addAndMakeVisible(pressureLabel);
    updatePressure();
    startTimerHz(1);   // pressure keeps updating even when idle

    logView.setMultiLine(true, false);
    logView.setReadOnly(true);
    logView.setCaretVisible(false);
    logView.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 11.0f, 0));
    tip(logView, "Worker output: model loads, LoRA plans, tracebacks.");
    addAndMakeVisible(logView);

    progressBar.setVisible(false);
    rightPane.addAndMakeVisible(progressBar);

    datasetsLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.45f));
    datasetsLabel.setFont(juce::FontOptions(11.0f));
    tip(datasetsLabel, "Prepared datasets: folder -> trigger (latent count).");
    rightPane.addAndMakeVisible(datasetsLabel);
    refreshDatasets();
    refreshLoras();
    startWorker();
    setSize(720, 640);
}

GenerateContent::~GenerateContent() {
    stopTimer();
    worker.reset();   // blocks until the child exits cleanly
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
        for (auto* st : { &sl.minStep, &sl.maxStep }) {
            const bool wasAtEnd = st->getValue() >= st->getMaximum();
            st->setRange(1, nSteps, 1);
            // A slider parked at the old maximum meant "all the way to the end", so it
            // follows the new end rather than being left stranded mid-range by a change
            // it had no opinion about.
            if (wasAtEnd) st->setValue(nSteps, juce::dontSendNotification);
        }
        if (sl.minStep.getValue() > sl.maxStep.getValue())
            sl.minStep.setValue(sl.maxStep.getValue(), juce::dontSendNotification);
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

juce::StringArray GenerateContent::listExistingCues() const {
    juce::StringArray cues;
    if (!projectFolder.isDirectory()) return cues;
    for (const auto& d : projectFolder.findChildFiles(juce::File::findDirectories, false)) {
        // "takes" is the scratch bin, not a cue -- it is where these files are coming
        // FROM (MIRA-GENERATE.md Phase 2a).
        if (d.getFileName() == "takes") continue;
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
        promptForCue(listExistingCues(), {}, this, [this, wav](juce::String cue) {
            cue = cue.trim();
            if (cue.isEmpty()) return; // no silent "untitled" cue
            keepResultIntoCue(wav, cue);
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
        refreshEditControls();
        return;
    }

    fadeInSlider.setValue(0.0, juce::dontSendNotification);
    fadeOutSlider.setValue(0.0, juce::dontSendNotification);
    gainSlider.setValue(0.0, juce::dontSendNotification);
    preview.setSegments({});
    refreshEditControls();
}

void GenerateContent::writeEditFields() {
    if (editSegmentId == 0) return;
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
    auditionButton.setEnabled(haveFile);
    for (auto* sl : { &fadeInSlider, &fadeOutSlider, &gainSlider }) sl->setEnabled(haveTrim);

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
void GenerateContent::discardResult() {
    const auto wav = resultTile.getFile();
    if (!wav.existsAsFile()) return;
    const auto sidecar = wav.withFileExtension("json");

    preview.setFile({});                 // stop reading the file we are about to move
    resultTile.setFile({});
    const auto name = wav.getFileName();
    bool ok = wav.moveToTrash();
    if (sidecar.existsAsFile()) sidecar.moveToTrash();

    // Gone from the stack either way: if the move to Trash failed the file is still
    // there, but leaving a row whose buttons no longer do anything is worse than a log
    // line saying what happened.
    takeStack->markDiscarded(wav);
    revealButton.setEnabled(false);
    statusLabel.setText(ok ? name + " - moved to Trash"
                           : "could not move " + name + " to Trash",
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
        const int lo = static_cast<int>(sl.minStep.getValue());
        const int hi = static_cast<int>(sl.maxStep.getValue());
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
    worker = std::make_unique<mira::Sa3Worker>();
    worker->onLog = [this](juce::String line) { log(line); };
    worker->onExit = [this](int code) {
        statusLabel.setText("worker exited (" + juce::String(code) + ")", juce::dontSendNotification);
        setBusy(false, {});
    };
    juce::String error;
    const auto script = studioRoot.getChildFile("sa3_worker.py");
    if (!worker->start(pythonFor(studioRoot), script, "medium", "same-l", error)) {
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
    if (busy || worker == nullptr || !worker->isRunning()) return;

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
                static_cast<int>(sl.minStep.getValue()), static_cast<int>(sl.maxStep.getValue()) }));
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

    setBusy(true, "generating");
    worker->send(req, [this, wav](bool ok, juce::var payload) {
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
        // addTake selects it, and the selection callback sets preview/resultTile and
        // enables the buttons -- one path, so a take opened by clicking an older row is
        // in exactly the same state as one that just finished rendering.
        takeStack->addTake(wav);
        revealButton.setEnabled(true);
        const auto ms = static_cast<int>(payload.getProperty("wall_ms", 0));
        statusLabel.setText(wav.getFileName() + " - done in "
                            + juce::String(ms / 1000.0, 1) + "s - drag the tile into your DAW",
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
    if (busy || worker == nullptr || !worker->isRunning()) return;
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
        worker->send(req, [this, outDir, name, trigger](bool ok, juce::var payload) {
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

void GenerateContent::setBusy(bool nowBusy, const juce::String& what) {
    busy = nowBusy;
    generateButton.setEnabled(!busy);
    encodeButton.setEnabled(!busy);
    stopButton.setEnabled(busy);
    progressBar.setVisible(busy);
    if (busy) {
        busyStartMs = juce::Time::getMillisecondCounter();
        progress = -1.0;                      // indeterminate: the worker reports no %
        statusLabel.setText(what + "...", juce::dontSendNotification);
        startTimerHz(4);
    } else {
        startTimerHz(1);   // keep the pressure readout live while idle
    }
}

void GenerateContent::stopGeneration() {
    // The worker is blocked inside MLX and will not read stdin until the current request
    // returns, so a polite "cancel" message would sit unread until the thing we want to
    // cancel has already finished. Killing the process is the only real stop.
    log("stopping worker...");
    worker.reset();
    setBusy(false, {});
    startWorker();
    statusLabel.setText("stopped - worker restarted, model will reload on next run",
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
    if (auditioning) {
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
        if (auditionEnd > auditionStart && pos >= auditionEnd - 0.01) stopAudition();
    }
    if (!busy) return;
    const auto secs = (juce::Time::getMillisecondCounter() - busyStartMs) / 1000;
    statusLabel.setText(statusLabel.getText().upToFirstOccurrenceOf(" [", false, false)
                            + " [" + juce::String(secs) + "s]", juce::dontSendNotification);
}

void GenerateContent::log(const juce::String& line) {
    logView.moveCaretToEnd();
    logView.insertTextAtCaret(line.trimEnd() + "\n");
}

void GenerateContent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff1a1a1a));
}

// Everything in the right pane, laid out once. Called twice per resize: once to MEASURE
// (applyBounds=false) so the pane can be given a height tall enough for all of it, and
// once to place. One function rather than two that must agree -- a measure pass that
// drifts from the layout pass is how a control ends up half off the bottom of a viewport.
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
            l.setFont(juce::Font(juce::FontOptions(10.5f, juce::Font::bold)));
            l.setColour(juce::Label::textColourId, MiraLookAndFeel::accent.withAlpha(0.85f));
            l.setBounds(line);
        }
    };

    // --- the prompt, first, because it is what the window is for
    place(promptEditor, row(72));
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

        auto line = row(26, 10);
        const int labelW = 52;
        const int cell = juce::jmax(90, (line.getWidth() - 3 * labelW - 12) / 3);
        place(sl.blendLabel, line.removeFromLeft(labelW).withTrimmedRight(4));
        place(sl.strength, line.removeFromLeft(cell));
        line.removeFromLeft(6);
        place(sl.structureLabel, line.removeFromLeft(labelW).withTrimmedRight(4));
        place(sl.minStep, line.removeFromLeft(cell));
        line.removeFromLeft(6);
        place(sl.timbreLabel, line.removeFromLeft(labelW).withTrimmedRight(4));
        place(sl.maxStep, line);
    }

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
    heading(inpaintHeading, "AUDIO IN");
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
        // The numbers stay, beside the picture: a range dragged by eye still has to be
        // typeable when a cue has to start at exactly 12.0s.
        auto line = row(26);
        place(inpaintStart, line.removeFromLeft(line.getWidth() / 2 - 3));
        line.removeFromLeft(6);
        place(inpaintEnd, line);
    } else if (applyBounds) {
        inpaintHelp.setBounds({});
        inpaintStart.setBounds({}); inpaintEnd.setBounds({});
    }

    // --- where it lands. Grouped together and labelled, instead of the output folder
    // button sitting between "Add LoRA file..." and "Build prompt..." with nothing to say
    // they were unrelated ("clean - output folder - show in finder are confusing the way
    // it is placed").
    {
        auto line = row(26);
        place(outFolderButton, line.removeFromLeft(120));
        line.removeFromLeft(6);
        place(nameEditor, line);
    }

    // --- the training bench, only on the SA3 Generate face (§3.4)
    if (trainingBenchVisible) {
        heading(loraHeading, "LORA"); // reuse is fine: the project face never draws it
        auto line = row(26);
        place(triggerLabel, line.removeFromLeft(46));
        place(triggerEditor, line.removeFromLeft(70));
        line.removeFromLeft(6);
        place(encodeButton, line.removeFromLeft(170));
        place(datasetsLabel, row(16, 4));
    } else if (applyBounds) {
        triggerLabel.setBounds({}); triggerEditor.setBounds({});
        encodeButton.setBounds({}); datasetsLabel.setBounds({});
    }

    {
        auto line = row(32);
        place(generateButton, line.removeFromLeft(120));
        line.removeFromLeft(6);
        place(stopButton, line.removeFromLeft(80));
        line.removeFromLeft(6);
        place(revealButton, line.removeFromLeft(130));
    }
    place(progressBar, row(12));

    return r.getY() - startY + 8;
}

void GenerateContent::resized() {
    auto r = getLocalBounds().reduced(10);

    auto top = r.removeFromTop(22);
    pressureLabel.setBounds(top.removeFromRight(260));
    statusLabel.setBounds(top);
    r.removeFromTop(6);

    // The log is a diagnostic and lives under both panes, full width.
    auto logHeight = juce::jlimit(70, 160, r.getHeight() / 5);
    logView.setBounds(r.removeFromBottom(logHeight));
    r.removeFromBottom(8);

    // Two containers. The right one is sized to its content and scrolls; the left takes
    // whatever is left, with a floor so the takes never disappear on a narrow window.
    const int rightWidth = juce::jlimit(320, 520, r.getWidth() / 2);
    auto rightArea = r.removeFromRight(rightWidth);
    r.removeFromRight(8);
    auto leftArea = r;

    rightView.setBounds(rightArea);
    {
        const int innerWidth = rightArea.getWidth() - (rightView.isVerticalScrollBarShown() ? 10 : 0);
        const int needed = layoutRightPane(innerWidth, false);
        rightPane.setSize(innerWidth, juce::jmax(needed, rightArea.getHeight()));
        layoutRightPane(innerWidth, true);
    }

    // --- left: the takes
    {
        auto header = leftArea.removeFromTop(26);
        cleanupButton.setBounds(header.removeFromRight(92).withSizeKeepingCentre(92, 22));
        header.removeFromRight(6);
        takesLabel.setBounds(header);
        leftArea.removeFromTop(4);
        takesView.setBounds(leftArea);
    }

    if (takeStack != nullptr) {
        const int innerWidth = takesView.getWidth() - (takesView.isVerticalScrollBarShown() ? 10 : 0);
        takeStack->setSize(innerWidth, juce::jmax(takeStack->getIdealHeight(), takesView.getHeight()));

        // Inside the expanded row: the big waveform, then the drag tile with Keep and
        // Discard beside it. These are children of the STACK, so the bounds below are in
        // the stack's coordinate space -- which is exactly why nothing else may
        // addAndMakeVisible them (see the constructor).
        auto slot = takeStack->getFocusedContentArea();
        if (!slot.isEmpty()) {
            auto buttons = slot.removeFromBottom(30);
            // Phase 5's edit row, between the waveform and the keep/discard row: the
            // order of the strip is the order of the decisions -- hear it, cut it, keep it.
            auto edit = slot.removeFromBottom(28);
            slot.removeFromBottom(4);
            preview.setBounds(slot.withTrimmedBottom(4));

            auditionButton.setBounds(edit.removeFromLeft(86).withSizeKeepingCentre(86, 22));
            edit.removeFromLeft(5);
            trimButton.setBounds(edit.removeFromLeft(120).withSizeKeepingCentre(120, 22));
            edit.removeFromLeft(4);
            clearTrimButton.setBounds(edit.removeFromLeft(92).withSizeKeepingCentre(92, 22));
            edit.removeFromLeft(8);
            gainSlider.setBounds(edit.removeFromRight(juce::jmax(90, edit.getWidth() / 4)));
            gainLabel.setBounds(edit.removeFromRight(36));
            edit.removeFromRight(6);
            fadeOutSlider.setBounds(edit.removeFromRight(juce::jmax(90, edit.getWidth() / 3)));
            fadeOutLabel.setBounds(edit.removeFromRight(52));
            edit.removeFromRight(6);
            fadeInSlider.setBounds(edit.removeFromRight(juce::jmax(90, edit.getWidth() / 2)));
            fadeInLabel.setBounds(edit.removeFromRight(46));
            edit.removeFromRight(6);
            editLabel.setBounds(edit);

            keepButton.setBounds(buttons.removeFromRight(64).withSizeKeepingCentre(64, 24));
            buttons.removeFromRight(5);
            discardButton.setBounds(buttons.removeFromRight(80).withSizeKeepingCentre(80, 24));
            buttons.removeFromRight(8);
            resultTile.setBounds(buttons);
        }
    }
}
