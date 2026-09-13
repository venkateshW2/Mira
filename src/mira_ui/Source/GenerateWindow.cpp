#include "GenerateWindow.h"

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
    addAndMakeVisible(promptEditor);

    for (int i = 0; i < kLoraSlots; ++i) {
        auto& sl = slots[static_cast<size_t>(i)];
        sl.label.setText("LoRA " + juce::String(i + 1), juce::dontSendNotification);
        addAndMakeVisible(sl.label);
        tip(sl.box, "LoRA for this slot. Fill two slots to blend styles.");
        addAndMakeVisible(sl.box);

        sl.strength.setRange(0.0, 2.0, 0.05);
        sl.strength.setValue(1.0, juce::dontSendNotification);
        sl.strength.setSliderStyle(juce::Slider::LinearHorizontal);
        sl.strength.setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 18);
        tip(sl.strength, "0 = base model (bypass), 1 = as trained, above 1 = overdriven.");
        addAndMakeVisible(sl.strength);

        // Step gating: apply the LoRA only during part of the diffusion run. Early steps
        // shape structure and arrangement, late steps shape timbre and texture -- so this
        // separates "did it change the music?" from "did it just recolour the surface?".
        for (auto* st : { &sl.minStep, &sl.maxStep }) {
            st->setRange(1, 50, 1);
            st->setSliderStyle(juce::Slider::LinearHorizontal);
            st->setTextBoxStyle(juce::Slider::TextBoxRight, false, 40, 18);
            addAndMakeVisible(*st);
        }
        sl.minStep.setValue(1, juce::dontSendNotification);
        sl.maxStep.setValue(8, juce::dontSendNotification);
        tip(sl.minStep, "First step this LoRA applies to. Early steps shape structure.");
        tip(sl.maxStep, "Last step this LoRA applies to. Late steps shape timbre.");
    }

    tip(addLoraButton, "Copy a .safetensors into loras/sa3-medium/.");
    addLoraButton.onClick = [this] { addLoraFile(); };
    addAndMakeVisible(addLoraButton);

    auto setupNumber = [this](juce::Slider& s, double lo, double hi, double interval,
                              double value, const juce::String& tipText) {
        s.setRange(lo, hi, interval);
        s.setValue(value, juce::dontSendNotification);
        s.setSliderStyle(juce::Slider::LinearHorizontal);
        s.setTextBoxStyle(juce::Slider::TextBoxRight, false, 56, 20);
        tip(s, tipText);
        addAndMakeVisible(s);
    };
    setupNumber(secondsSlider, 10, 380, 1, 30,
                "Output length. CHANGING THIS RELOADS THE MODEL (~44s). RAM grows with it.");
    setupNumber(stepsSlider, 4, 50, 1, 8,
                "Diffusion steps. 8 is the tuned default.");
    setupNumber(seedSlider, 0, 100000, 1, 26,
                "Seed. Fix it when comparing anything.");

    tip(generateButton, "Generate with the settings above.");
    generateButton.onClick = [this] { generate(); };
    addAndMakeVisible(generateButton);

    triggerLabel.setText("trigger", juce::dontSendNotification);
    addAndMakeVisible(triggerLabel);
    triggerEditor.setText("xyr");
    tip(triggerEditor, "Rare token this LoRA is keyed to. One per film (Dune used zvq).");
    addAndMakeVisible(triggerEditor);

    tip(encodeButton, "Captions -> encode -> zip, for one film folder. Analysed files only.");
    encodeButton.onClick = [this] { chooseEncodeFolder(); };
    addAndMakeVisible(encodeButton);

    outputFolder = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                       .getChildFile("mira-generated");
    outputFolder.createDirectory();
    tip(outFolderButton, "Where generated WAVs go.");
    outFolderButton.onClick = [this] { chooseOutputFolder(); };
    addAndMakeVisible(outFolderButton);

    nameEditor.setTextToShowWhenEmpty("filename (blank = timestamp)",
                                       juce::Colours::white.withAlpha(0.35f));
    tip(nameEditor, "Filename for the next take. Blank = timestamp. Never overwrites.");
    addAndMakeVisible(nameEditor);

    tip(initAudioButton, "audio2audio: start from this audio instead of noise.");
    initAudioButton.onClick = [this] { chooseInitAudio(); };
    addAndMakeVisible(initAudioButton);
    clearInitButton.onClick = [this] {
        initAudio = juce::File();
        initLabel.setText("no init audio", juce::dontSendNotification);
    };
    tip(clearInitButton, "Clear init audio.");
    addAndMakeVisible(clearInitButton);
    initLabel.setText("no init audio", juce::dontSendNotification);
    addAndMakeVisible(initLabel);

    tip(inpaintToggle, "Regenerate only the range below, keep the rest bit-exact.");
    inpaintToggle.onClick = [this] { resized(); };
    addAndMakeVisible(inpaintToggle);
    for (auto* sl : { &inpaintStart, &inpaintEnd }) {
        sl->setRange(0.0, 380.0, 0.5);
        sl->setSliderStyle(juce::Slider::LinearHorizontal);
        sl->setTextBoxStyle(juce::Slider::TextBoxRight, false, 48, 18);
        addAndMakeVisible(*sl);
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
    addAndMakeVisible(revealButton);

    tip(preview, "Play and scrub. Drag the strip below to get the file into your DAW.");
    addAndMakeVisible(preview);
    addAndMakeVisible(resultTile);

    tip(stopButton, "Kill and restart the worker. Next run reloads the model (~44s).");
    stopButton.onClick = [this] { stopGeneration(); };
    stopButton.setEnabled(false);
    addAndMakeVisible(stopButton);

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
    addAndMakeVisible(progressBar);

    datasetsLabel.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.45f));
    datasetsLabel.setFont(juce::FontOptions(11.0f));
    tip(datasetsLabel, "Prepared datasets: folder -> trigger (latent count).");
    addAndMakeVisible(datasetsLabel);
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

void GenerateContent::refreshLoras() {
    loraFiles.clear();
    const auto dir = loraDirFor(studioRoot);
    if (dir.isDirectory())
        for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.safetensors"))
            loraFiles.add(f);

    for (int i = 0; i < kLoraSlots; ++i) {
        auto& box = slots[static_cast<size_t>(i)].box;
        const int previous = box.getSelectedId();
        box.clear(juce::dontSendNotification);
        box.addItem("(empty)", 1);
        for (int k = 0; k < loraFiles.size(); ++k)
            box.addItem(loraFiles[k].getFileNameWithoutExtension(), k + 2);
        // Slot 1 preselects the newest checkpoint; the others stay empty so a single-LoRA
        // generation is still one click.
        const int wanted = previous > 0 ? previous
                          : (i == 0 && !loraFiles.isEmpty() ? loraFiles.size() + 1 : 1);
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
    const auto base = nameEditor.getText().trim().isNotEmpty()
                          ? juce::File::createLegalFileName(nameEditor.getText().trim())
                          : "mira-" + juce::Time::getCurrentTime().formatted("%Y%m%d-%H%M%S");
    // Never silently overwrite a take someone might still want.
    auto wav = outputFolder.getChildFile(base + ".wav");
    for (int n = 2; wav.existsAsFile(); ++n)
        wav = outputFolder.getChildFile(base + "-" + juce::String(n) + ".wav");

    auto req = new juce::DynamicObject();
    req->setProperty("cmd", "generate");
    req->setProperty("prompt", promptEditor.getText());
    req->setProperty("seconds", secondsSlider.getValue());
    req->setProperty("steps", static_cast<int>(stepsSlider.getValue()));
    req->setProperty("seed", static_cast<int>(seedSlider.getValue()));
    req->setProperty("out", wav.getFullPathName());

    const auto specs = buildLoraSpecs();
    if (!specs.isVoid()) req->setProperty("loras", specs);

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
        resultTile.setFile(wav);
        preview.setFile(wav);
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

void GenerateContent::resized() {
    auto r = getLocalBounds().reduced(12);
    auto top = r.removeFromTop(22);
    pressureLabel.setBounds(top.removeFromRight(260));
    statusLabel.setBounds(top);
    r.removeFromTop(4);
    promptEditor.setBounds(r.removeFromTop(64));
    r.removeFromTop(6);

    auto row = [&r](int h, int gap = 5) { auto x = r.removeFromTop(h); r.removeFromTop(gap); return x; };

    for (int i = 0; i < kLoraSlots; ++i) {
        auto& sl = slots[static_cast<size_t>(i)];
        auto line = row(22, 2);
        sl.label.setBounds(line.removeFromLeft(48));
        sl.box.setBounds(line.removeFromLeft(200));
        line.removeFromLeft(4);
        sl.strength.setBounds(line.removeFromLeft(150));
        line.removeFromLeft(4);
        sl.minStep.setBounds(line.removeFromLeft(juce::jmax(80, line.getWidth() / 2 - 2)));
        line.removeFromLeft(4);
        sl.maxStep.setBounds(line);
    }
    r.removeFromTop(4);

    secondsSlider.setBounds(row(22));
    stepsSlider.setBounds(row(22));
    seedSlider.setBounds(row(22));

    auto a2a = row(24);
    initAudioButton.setBounds(a2a.removeFromLeft(100));
    a2a.removeFromLeft(4);
    clearInitButton.setBounds(a2a.removeFromLeft(24));
    a2a.removeFromLeft(6);
    initLabel.setBounds(a2a.removeFromLeft(juce::jmax(120, a2a.getWidth() - 130)));
    inpaintToggle.setBounds(a2a);

    if (inpaintToggle.getToggleState()) {
        auto rng = row(22);
        inpaintStart.setBounds(rng.removeFromLeft(rng.getWidth() / 2 - 3));
        rng.removeFromLeft(6);
        inpaintEnd.setBounds(rng);
    } else {
        inpaintStart.setBounds({}); inpaintEnd.setBounds({});
    }

    auto out = row(24);
    outFolderButton.setBounds(out.removeFromLeft(120));
    out.removeFromLeft(6);
    nameEditor.setBounds(out.removeFromLeft(200));
    out.removeFromLeft(6);
    addLoraButton.setBounds(out.removeFromLeft(120));

    auto buttons = row(30);
    generateButton.setBounds(buttons.removeFromLeft(110));
    buttons.removeFromLeft(6);
    triggerLabel.setBounds(buttons.removeFromLeft(44));
    triggerEditor.setBounds(buttons.removeFromLeft(56).reduced(0, 3));
    buttons.removeFromLeft(4);
    encodeButton.setBounds(buttons.removeFromLeft(165));
    buttons.removeFromLeft(4);
    revealButton.setBounds(buttons.removeFromLeft(115));
    buttons.removeFromLeft(4);
    stopButton.setBounds(buttons.removeFromLeft(70));

    datasetsLabel.setBounds(row(16, 3));
    progressBar.setBounds(row(12));
    preview.setBounds(row(110));
    resultTile.setBounds(row(38));
    logView.setBounds(r);
}
