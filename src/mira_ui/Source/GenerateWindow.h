#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <memory>

#include "mira/db/Database.h"
#include "MiraLookAndFeel.h"
#include "PromptBuilderWindow.h"
#include "Sa3Worker.h"
#include "WaveformView.h"

// SA3 generation and pre-encoding, inside mira.
//
// The reason this exists rather than the gradio page it replaces: a browser cannot drag a
// file into a DAW. Gradio's best offer is a download button, so the path is always
// generate -> download -> find in Finder -> drag. That is structural, not styling, and no
// amount of restyling the web UI fixes it. JUCE can hand the DAW a real file (proven in
// spike/03_dragout), so that is the capability this window is actually for.
//
// The second reason is the loop: a generation lands next to mira's library rather than in
// a browser temp dir, so it can be analysed, captioned and fed back into the next LoRA.
//
// Everything runs through one persistent Sa3Worker (sa3-studio/sa3_worker.py). See
// Sa3Worker.h for why the process is kept alive: the DiT load is ~44 s and must be paid
// once, not per click.
//
// Every control carries a tooltip. The gradio page it replaces has ZERO `info=` strings,
// which is precisely why it is unreadable to anyone who did not build it -- an absence to
// fix here, not to reproduce.
class GenerateContent : public juce::Component,
                         public juce::DragAndDropContainer,
                         private juce::Timer
{
public:
    GenerateContent(const MiraLookAndFeel& lafIn, juce::File studioRootIn,
                     mira::Database& databaseIn);
    ~GenerateContent() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    // Replaces the prompt box contents (used by the file table's "Use Caption in SA3
    // Generate"). A trigger is NOT prepended here -- which LoRA you are about to use is
    // the slot picker's business, and the caption itself is trigger-free.
    void setPrompt(const juce::String& text) { promptEditor.setText(text); }

private:
    // A finished WAV, draggable straight into a DAW. Mirrors spike/03_dragout: the drag
    // only starts past a 5px threshold so a click-to-select never becomes an accidental
    // drag, and startDragging's last argument is what makes macOS treat it as a real file
    // drag (NSDraggingItem) rather than an in-app one.
    class ResultTile : public juce::Component,
                        public juce::SettableTooltipClient
    {
    public:
        explicit ResultTile(const MiraLookAndFeel& lafIn);
        void setFile(juce::File f);
        juce::File getFile() const { return file; }
        void paint(juce::Graphics&) override;
        void mouseDrag(const juce::MouseEvent&) override;
        void mouseDoubleClick(const juce::MouseEvent&) override;
    private:
        const MiraLookAndFeel& laf;
        juce::File file;
    };

    void startWorker();
    void refreshLoras();
    void addLoraFile();
    juce::var buildLoraSpecs();
    void chooseInitAudio();
    void chooseOutputFolder();
    void generate();
    void chooseEncodeFolder();
    // Writes one SA3 sidecar per analysed file under `folder`, in-process via
    // mira_core's CaptionFields + Sa3Renderer -- the same code `mira caption
    // --emit-sidecar` uses, so the UI and CLI can never disagree. Returns how many
    // were written. MUST run before pre-encoding: pre_encode_mlx.py reads
    // <stem>.json beside the audio and returns {} when there is none, which trains a
    // LoRA that only ever saw the trigger, silently.
    int writeSidecars(const juce::File& folder, const juce::String& trigger);
    void zipLatents(const juce::File& latentsDir, const juce::File& zipOut);
    void setBusy(bool busy, const juce::String& what);
    void log(const juce::String& line);
    void timerCallback() override;
    void updatePressure();
    void stopGeneration();

    const MiraLookAndFeel& laf;
    mira::Database& database;
    juce::File studioRoot;                 // sa3-studio/
    std::unique_ptr<mira::Sa3Worker> worker;
    juce::Array<juce::File> loraFiles;

    juce::Label statusLabel;
    juce::TextEditor promptEditor;
    // Three LoRA slots, matching what the sampler supports: the worker takes a `loras`
    // list and lora_merge builds one combined step plan over them, so two trained styles
    // can be blended -- the thing separate single-LoRA runs can never do.
    struct LoraSlot {
        juce::ComboBox box;
        juce::Slider strength, minStep, maxStep;
        juce::Label label;
    };
    static constexpr int kLoraSlots = 3;
    std::array<LoraSlot, kLoraSlots> slots;
    juce::TextButton addLoraButton { "Add LoRA file..." };
    // Opens the format helper. A LoRA is trained on "Key: value, Key: value" captions,
    // so a hand-typed bare word list is off distribution -- this builds the shape.
    juce::TextButton buildPromptButton { "Build prompt..." };
    // The CONTENT outlives the window on purpose -- Construct closes the window, and
    // reopening should resume with every field as it was rather than blank.
    std::unique_ptr<PromptBuilderContent> promptBuilderContent;
    std::unique_ptr<PromptBuilderWindow> promptBuilder;
    juce::Slider secondsSlider, stepsSlider, seedSlider;
    // CFG + negative prompt. sa3_gradio guards the negative branch with `if cfg != 1.0`
    // and takes a conditional-only fast path at exactly 1.0, so the Avoid box does
    // NOTHING until this is raised -- they ship together or not at all.
    // cfg defaults to 1.0, the value every generation before this used, so adding the
    // control changes nothing until it is deliberately moved.
    juce::Slider cfgSlider;
    juce::Label cfgLabel, negativeLabel;
    juce::TextEditor negativeEditor;
    juce::TextButton generateButton { "Generate" };
    juce::TextButton encodeButton { "Prepare LoRA dataset..." };
    juce::TextEditor triggerEditor;
    juce::Label triggerLabel;
    // Which trigger each prepared dataset used. Read back off disk rather than
    // remembered in a config: the sidecars beside the audio ARE the record, so this
    // cannot drift from what was actually encoded.
    juce::Label datasetsLabel;
    void refreshDatasets();
    juce::String triggerOfFolder(const juce::File& folder) const;
    juce::TextButton revealButton { "Show in Finder" };
    juce::TextButton outFolderButton { "Output folder..." };
    juce::TextEditor nameEditor;                 // base filename, blank = timestamp
    juce::File outputFolder;

    // audio2audio / inpainting. Both are already wired through the worker
    // (init_audio, inpaint_audio, inpaint_range) -- these are the missing controls.
    juce::TextButton initAudioButton { "Init audio..." };
    juce::TextButton clearInitButton { "x" };
    juce::File initAudio;
    juce::Label initLabel;
    juce::ToggleButton inpaintToggle { "inpaint range" };
    juce::Slider inpaintStart, inpaintEnd;
    // Preview + scrub, reusing the app's own transport (it owns its AudioDeviceManager,
    // so it needs nothing wired in). The small tile below it stays as the DRAG handle:
    // the waveform's own click-drag is scrubbing, and overloading it with an external
    // file drag would make both feel broken.
    WaveformView preview;
    ResultTile resultTile;
    juce::TextButton stopButton { "Stop" };
    // Memory readout. Generation RAM scales with clip length (peak was 11 GB at 30 s on
    // a 16 GB machine), and a second SA3 process -- a forgotten gradio, say -- is enough
    // to push the whole thing into swap, where every diffusion step pages to disk. That
    // failure looks exactly like "the model got slower", so it needs to be visible.
    juce::Label pressureLabel;
    juce::TextEditor logView;
    juce::ProgressBar progressBar { progress };
    // NO TooltipWindow here on purpose: MainComponent already owns the app's single one
    // (Main.cpp). A second instance renders every tooltip twice, overlapping.
    double progress = 0.0;
    bool busy = false;
    juce::int64 busyStartMs = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(GenerateContent)
};

// Pop-out window wrapper. Same shape as LogWindow: native title bar, explicit toFront
// (a fresh DocumentWindow can otherwise appear BEHIND the main window on macOS, which
// looks exactly like the menu item having done nothing).
class GenerateWindow : public juce::DocumentWindow
{
public:
    GenerateWindow(const MiraLookAndFeel& laf, juce::File studioRoot, mira::Database& db)
        : juce::DocumentWindow("SA3 Generate", MiraLookAndFeel::surface,
                                juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new GenerateContent(laf, std::move(studioRoot), db), false);
        setResizable(true, false);
        centreWithSize(720, 700);
        // Floats above the main window. This is a tool panel used ALONGSIDE the library
        // -- you pick a file there, build a prompt here, and drag the result out to a
        // DAW -- so ordinary sibling behaviour (drop behind on every click in the main
        // window, then hunt for it in the Window menu) is wrong for it. Same reason a
        // plugin's editor floats.
        setAlwaysOnTop(true);
        setVisible(true);
        toFront(true);
    }

    std::function<void()> onClosed;

    void setPrompt(const juce::String& text) {
        if (auto* c = dynamic_cast<GenerateContent*>(getContentComponent())) c->setPrompt(text);
    }

    // Closing shuts the worker down with it (GenerateContent's destructor), releasing
    // ~5 GB of resident model. That is deliberate: leaving a warm worker alive behind a
    // closed window would quietly hold half the RAM on a 16 GB machine.
    void closeButtonPressed() override { if (onClosed) onClosed(); }
};
