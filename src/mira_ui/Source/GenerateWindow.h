#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include <array>
#include <memory>

#include "mira/db/Database.h"
#include "MiraLookAndFeel.h"
#include "TakeStack.h"
#include "LoraLibraryWindow.h"
#include "NativeWindowChrome.h"
#include "InpaintStrip.h"
#include "LoraLanes.h"
#include "PromptBuilderWindow.h"
#include "Sa3Worker.h"
#include "WaveformView.h"
#include "LogView.h"
#include "GenerateProgress.h"
#include "ProjectSidebar.h"
#include "Sa3WorkerHub.h"

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
                     mira::Database& databaseIn, Sa3WorkerHub& hubIn);
    ~GenerateContent() override;

    void resized() override;
    void paint(juce::Graphics&) override;

    // Fired after Keep registers a file, so the library sidebar can pick up the new
    // "Generated" collection without a restart.
    std::function<void()> onLibraryChanged;

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

    bool discardOne(const juce::File& wav);
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
    // Borrowed, never owned: one worker serves every window (Sa3WorkerHub.h).
    Sa3WorkerHub& hub;
    int hubToken = 0;
    juce::Array<juce::File> loraFiles;

    juce::Label statusLabel;
    juce::TextEditor promptEditor;
    // Three LoRA slots, matching what the sampler supports: the worker takes a `loras`
    // list and lora_merge builds one combined step plan over them, so two trained styles
    // can be blended -- the thing separate single-LoRA runs can never do.
    struct LoraSlot {
        juce::ComboBox box;
        juce::Slider strength;
        // ONE two-thumb slider, not two single-value ones. The pair was never two axes:
        // buildLoraSpecs sends `steps: [lo, hi]`, a WINDOW of sampler steps during which
        // this LoRA is allowed to act. Early steps shape structure and arrangement, late
        // steps shape timbre and texture -- so the window says when it acts and for how
        // much of the run. Two separate sliders made that read as two independent
        // settings that might trade off against each other; they never did, and the only
        // relationship between them is min <= max.
        //
        // Not collapsed to a SINGLE value, which was the other option considered: a
        // single number can carry the position or the width but not both, and losing the
        // width would remove "apply this LoRA for the whole run", the default.
        // Plain values, not a widget: LoraLanes is the editor now, and a hidden slider
        // holding the same numbers would be a second source of truth for them.
        int stepLo = 1, stepHi = 8;
        juce::Label label;
        // "have slider with heading like Lorablend - timbre - structure". The three
        // numbers are a blend amount and a step window, and without words on them they
        // read as three anonymous sliders. Early diffusion steps shape structure and
        // arrangement, late steps shape timbre and texture -- which is what makes the
        // step window worth exposing at all, and the labels now say so.
        juce::Label blendLabel;
    };
    static constexpr int kLoraSlots = 3;
    std::array<LoraSlot, kLoraSlots> slots;

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
    // Three of the four settings sliders had no label at all -- only a number box, so
    // "30 / 8 / 5374" sat in a column with nothing saying which was which.
    juce::Label secondsLabel, stepsLabel, seedLabel;
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
    // Keeping is deliberate, not automatic: most generations are throwaways, and a
    // library that fills with rejects is worse than one that does not know about them.
    // Pressing this registers the file the way the scanner would, stores the recipe in
    // `human`, and files it under a "Generated" collection.
    GlyphButton keepButton { GlyphButton::Glyph::ThumbUp };
    void keepResult();
    // The counterpart to Keep. Output accumulates fast -- 40 files / 945 MB before this
    // existed -- and pruning it by hand in Finder means opening each one to find out
    // what it was. Both of these move to the TRASH, never unlink: a generation you
    // cannot get back is a bad thing to make one click away.
    GlyphButton discardButton { GlyphButton::Glyph::ThumbDown };
    void discardResult();
    // Sweeps the output folder of everything never Kept, so the routine case (generate
    // ten, keep one) does not require ten decisions later.
    juce::TextButton cleanupButton { "Clean up..." };
    void cleanupUnkept();
    // Everything that produced the current result, captured at request time so Keep
    // cannot record a recipe that drifted from what was actually rendered.
    juce::var lastRecipe;
    juce::TextButton outFolderButton { "Output folder..." };
    juce::TextEditor nameEditor;                 // base filename, blank = timestamp
    juce::File outputFolder;
    bool trainingBenchVisible = true; // the SA3 Generate window keeps its bench
    juce::File projectFolder;         // invalid = no project, Keep behaves as it always did
    juce::File scopeFolder;           // sidebar selection; invalid = show everything
    // The generate controls fold away. Organising a project -- auditioning takes,
    // trimming, filing them into cues -- needs none of prompt, LoRA or steps, and that
    // pane is half the window. Folded, the waveform gets the width.
    bool generatePaneCollapsed = false;
    juce::TextButton generatePaneToggle;
    std::unique_ptr<ProjectSidebar> sidebar;  // project window only

    // A second look-and-feel instance, identical to the app's except that its popup menus
    // are compact -- attached ONLY to the three LoRA pickers, which are the one place a
    // 21-item list with section headings has to fit on screen.
    MiraLookAndFeel compactMenuLaf;
    // ---- MIRA-GENERATE.md Phase 5: cut and fade -------------------------------------
    //
    // Non-destructive, per §3.5: the trim is a `segments` row on the take and the fades
    // and gain are JSON in `segments.human`. Nothing is written to audio until export, so
    // a cue can be re-cut next week without regenerating and the source take is never
    // damaged. That is also what lets a kept take reopen with its edit intact.
    GlyphButton trimButton { GlyphButton::Glyph::Scissors };
    GlyphButton clearTrimButton { GlyphButton::Glyph::FullLength };
    // "Play edit not needed": Play now plays the edit. The trim range and the fade/gain
    // envelope are applied to ordinary playback, so there is no second play button and no
    // second thing to remember to press to hear what you just set.
    juce::TextButton auditionButton { "Play edit" };
    juce::Label editLabel;
    juce::Slider fadeInSlider, fadeOutSlider, gainSlider;
    juce::Label fadeInLabel, fadeOutLabel, gainLabel;

    // The segment of the take currently in the preview, or 0 when it has none yet.
    int64_t editSegmentId = 0;
    juce::File editFile;

    void loadEditFor(const juce::File& wav);
    void writeEditFields();
    void applyTrimFromSelection();
    void clearTrim();
    void auditionEdit();
    void stopAudition();
    void refreshEditControls();
    // The audition envelope, stepped from the timer. An approximation of what export
    // renders; see WaveformView::setPlaybackGain.
    bool auditioning = false;
    double auditionStart = 0.0, auditionEnd = 0.0;

    void keepResultIntoCue(const juce::File& wav, const juce::String& cueName);
    juce::StringArray listExistingCues() const;
    juce::var recipeFor(const juce::File& wav) const;
    // The LoRA step window is measured in SAMPLER steps and compared against the Steps
    // value (buildLoraSpecs: `hi < nSteps` is what "to the last step" means). A slider
    // running to 50 while Steps is 8 was therefore offering 42 positions that do not
    // exist -- dragging through them changed nothing, which is most of why it felt like
    // the slider was broken. The range follows Steps instead.
    void syncLoraStepRanges();
    int previousStepCount = 8; // to tell "window reached the end" from "window happens to sit there"
    void syncLoraLanes();
    // The inpaint range is bounded by the DURATION, not by the source file's length, so
    // the end handle can reach past where the audio stops -- which is what an extension
    // is. sa3_mlx.py zero-pads the init audio up to the requested duration, so the region
    // beyond the file is real, addressable timeline.
    void syncInpaintSliderRanges();

public:
    // MIRA-GENERATE.md Phase 1: "switching project switches the output folder". Same
    // effect as picking one with the Output folder... button, minus the picker. An
    // invalid folder is ignored rather than clearing the current one -- losing the
    // output folder mid-session would strand the next generation.
    // MIRA-GENERATE.md §3.4: "Two faces, one engine." The Project window is this same
    // component with the training bench hidden -- the trigger field, Prepare LoRA
    // dataset... and the prepared-datasets line. Deliberately a flag on ONE component
    // rather than a second 995-line window: a fork would be two things to keep in step,
    // and every fix to generation would have to be made twice. The plan's "do not strip
    // the existing window to make the new one" is exactly what this preserves -- nothing
    // is removed, one face declines to show it.
    void setTrainingBenchVisible(bool shouldBeVisible)
    {
        trainingBenchVisible = shouldBeVisible;
        // The sidebar belongs to a PROJECT window, which is exactly the window without
        // the training bench. The SA3 Generate window has no project to list.
        if (!shouldBeVisible && sidebar == nullptr)
        {
            sidebar = std::make_unique<ProjectSidebar>(laf);
            sidebar->onFolderSelected = [this](juce::File f) {
                scopeFolder = f;
                loadExistingTakes();
                // Clicking a cue means "show me what is in it". A collapsed section
                // header would answer that with a closed triangle.
                if (takeStack != nullptr) takeStack->expandAllSections();
                resized();
            };
            sidebar->setProject(projectFolder);
            addAndMakeVisible(*sidebar);
        }
        triggerLabel.setVisible(shouldBeVisible);
        triggerEditor.setVisible(shouldBeVisible);
        encodeButton.setVisible(shouldBeVisible);
        datasetsLabel.setVisible(shouldBeVisible);
        resized();
    }

    // MIRA-GENERATE.md Phase 3. When set, Keep asks for a cue and files the take into
    // <project>/<cue>/ under its working name. Unset (the SA3 Generate window) leaves
    // Keep exactly as it was: register in place, add to the "Generated" collection.
    void setProject(const juce::File& folder)
    {
        projectFolder = folder;
        if (sidebar != nullptr) sidebar->setProject(folder);
    }

    // The LoRA Library window calls this after a checkpoint is added or renamed, so the
    // dropdowns update without reopening the generate window.
    void reloadLoras() { refreshLoras(); }

    // Join the app's single audio device, so this window plays through whatever Audio
    // Settings last chose rather than through a device of its own that nothing can reach.
    void useSharedAudioDevice(juce::AudioDeviceManager& shared)
    {
        preview.useSharedDeviceManager(shared);
    }

    void setOutputFolder(const juce::File& folder)
    {
        if (folder.getFullPathName().isEmpty()) return;
        // CREATE it. A brand-new project has no takes/ yet, and returning early here
        // left the window on its default -- ~/Music/mira-generated, which holds every
        // previous session's takes. So a new project opened showing another project's
        // files and wrote its own takes there too, silently. Convention 6: never fall
        // back to something that looks like an answer.
        if (!folder.isDirectory() && !folder.createDirectory().wasOk())
        {
            log("could not create output folder: " + folder.getFullPathName());
            statusLabel.setText("could not create " + folder.getFullPathName(),
                                 juce::dontSendNotification);
            return;
        }
        outputFolder = folder;
        log("output folder: " + folder.getFullPathName());
        loadExistingTakes();
    }

    // "the old takes are not showing up." The stack was session-only, so reopening a
    // project showed an empty list beside a folder full of takes. Everything already in
    // the takes folder comes back as PENDING, and everything already filed into a cue
    // comes back as KEPT -- the disk is the record, which is the same reason Clean up
    // asks the library rather than keeping a list of its own.
    //
    // Collapsed, and nothing is focused: reopening a project should not start playing
    // something, and forty open rows would be useless.
    void loadExistingTakes()
    {
        if (takeStack == nullptr) return;
        takeStack->clear();

        // Scoped by the sidebar. An empty scope means "all takes", which is what the
        // window showed before there was a sidebar at all.
        const bool scoped = scopeFolder.isDirectory();
        const auto inScope = [this, scoped](const juce::File& d) {
            return !scoped || d == scopeFolder;
        };

        if (projectFolder.isDirectory())
        {
            for (const auto& cue : projectFolder.findChildFiles(juce::File::findDirectories, false))
            {
                const auto name = cue.getFileName();
                if (name == "takes") continue;
                // A folder's NAME is its state: kept cues, or the discard bin. That is
                // the whole point of the sidebar -- one way to think about where a file
                // is, rather than a section and a folder that can disagree.
                const auto state = name == "discarded" ? TakeStack::State::Discarded
                                                        : TakeStack::State::Kept;
                if (inScope(cue))
                    for (const auto& f : cue.findChildFiles(juce::File::findFiles, false, "*.wav"))
                        takeStack->addTake(f, state, false);
                for (const auto& sub : cue.findChildFiles(juce::File::findDirectories, false))
                    if (inScope(sub))
                        for (const auto& f : sub.findChildFiles(juce::File::findFiles, false, "*.wav"))
                            takeStack->addTake(f, state, false);
            }
        }

        if (outputFolder.isDirectory() && inScope(outputFolder))
            for (const auto& f : outputFolder.findChildFiles(juce::File::findFiles, false, "*.wav"))
                takeStack->addTake(f, TakeStack::State::Pending, false);
    }

private:

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
    // No longer drawn. It was a large permanent rectangle whose only jobs were "show the
    // filename" and "be draggable", and the row above the waveform already shows the
    // filename and is now itself draggable (TakeStack::mouseDrag). What survives is its
    // third, unadvertised job: it is the window's record of WHICH take is current, read
    // by Show in Finder, keepResult and the cleanup paths.
    ResultTile resultTile;
    // MIRA-GENERATE.md Phase 4. The window held ONE result until now -- each generation
    // replaced the last, so comparing two meant regenerating. The stack keeps every take
    // of the session until it is kept or discarded, and hosts preview/resultTile/Keep/
    // Discard inside whichever row is expanded (see TakeStack's note on why they are
    // hosted rather than duplicated per row).
    juce::AudioFormatManager takeFormatManager;
    juce::AudioThumbnailCache takeThumbnailCache { 64 };
    std::unique_ptr<TakeStack> takeStack;
    juce::Viewport takesView;
    juce::Label takesLabel; // "Takes (n)" -- the stack has no header of its own
    // Sits between the takes header and the stack, so the bar fills in the same
    // rectangle the finished take drops into. Replaces the indeterminate strip that used
    // to live at the bottom of the right pane, next to the prompt.
    GenerateProgress genProgress;
    // Model weights load once per worker PROCESS, not once per generation, so the first
    // run after a start or a Stop costs ~44 s more than the rest. The estimate has to
    // know which kind of run this is or it is wrong by 44 s in one direction or the other.
    bool modelLoaded = false;
    // AUDIO IN folds away. It is the tallest section in the right pane and the one least
    // often wanted, and with it open the Generate button fell below the fold on a normal
    // window -- "the generate button is actually hidden and need to scroll for it".
    bool audioInCollapsed = true;
    juce::TextButton audioInCollapse;
    int busySteps = 0;
    double busySeconds = 0.0;
    bool busyHadLoad = false;

    // Two containers, as asked for: takes and audio on the left, everything else on the
    // right. Both scroll. That is the fix for "resize destroys the ui - the prompt gets
    // hidden": the old layout was one top-down column that simply ran out of window, so
    // whatever fell off the bottom was laid out at zero height and vanished. A pane that
    // scrolls cannot lose a control, at any window size.
    struct Pane : juce::Component {
        void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface); }
    };
    Pane rightPane;
    juce::Viewport rightView;
    juce::Label loraHeading, settingsHeading, inpaintHeading, inpaintHelp;
    LoraLanes loraLanes;
    std::unique_ptr<InpaintStrip> inpaintStrip;
    int layoutRightPane(int width, bool applyBounds);
    int promptHeightFor(int width) const;
    juce::TextButton stopButton { "Stop" };
    // Memory readout. Generation RAM scales with clip length (peak was 11 GB at 30 s on
    // a 16 GB machine), and a second SA3 process -- a forgotten gradio, say -- is enough
    // to push the whole thing into swap, where every diffusion step pages to disk. That
    // failure looks exactly like "the model got slower", so it needs to be visible.
    juce::Label pressureLabel;
    // The worker console is a WINDOW now, not a strip glued to the bottom of this one.
    // It was ~140px of monospace permanently occupying the full width of the generate
    // window to show four lines nobody reads until something breaks -- and when
    // something does break, four lines is not enough anyway. A window can be opened,
    // made tall, and left on a second display.
    LogStore workerLog;
    std::unique_ptr<LogWindow> consoleWindow;
    juce::TextButton consoleButton { "Console" };
    // NO TooltipWindow here on purpose: MainComponent already owns the app's single one
    // (Main.cpp). A second instance renders every tooltip twice, overlapping.
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
    // Floats above the main window. This is a tool panel used ALONGSIDE the library
    // -- you pick a file there, build a prompt here, and drag the result out to a DAW --
    // so ordinary sibling behaviour (drop behind on every click in the main window, then
    // hunt for it in the Window menu) is wrong for it. Same reason a plugin's editor
    // floats. ProjectWindow is the one that does NOT float; see its note.
    GenerateWindow(const MiraLookAndFeel& laf, juce::File studioRoot, mira::Database& db,
                    Sa3WorkerHub& hub)
        : GenerateWindow(laf, std::move(studioRoot), db, hub, "SA3 Generate", true, true)
    {
    }

    std::function<void()> onClosed;
    GenerateContent* content = nullptr;
    // Which project this window is for. Set by the owner; used to raise an already-open
    // project instead of opening a second window onto the same folder.
    juce::File projectFolder;

    void setPrompt(const juce::String& text) {
        if (auto* c = dynamic_cast<GenerateContent*>(getContentComponent())) c->setPrompt(text);
    }

    // Closing detaches from the shared worker but does NOT stop it -- another window may
    // be mid-generation. (It used to own its worker and tear ~5 GB down on close; with
    // one worker for the app, the model stays warm until the app quits or Stop is
    // pressed, which is also what makes opening a second project cheap.)
    void closeButtonPressed() override { if (onClosed) onClosed(); }

protected:
    // ProjectWindow below is the same window with a different title, no bench and no
    // always-on-top. Everything else -- the worker, its 5 GB teardown, setPrompt -- is
    // inherited rather than copied.
    GenerateWindow(const MiraLookAndFeel& laf, juce::File studioRoot, mira::Database& db,
                    Sa3WorkerHub& hub,
                    const juce::String& windowTitle, bool showTrainingBench, bool floatAbove)
        : juce::DocumentWindow(windowTitle, MiraLookAndFeel::surface, juce::DocumentWindow::allButtons)
    {
        // mira's own title bar, not the OS one -- "the title bar and the stuff can be
        // like mira". MainWindow made this choice first and for the same reason: the
        // native bar is plain OS gray and cannot take MiraLookAndFeel's colours, so it
        // sits visibly disconnected from the window under it.
        setUsingNativeTitleBar(false);
        setTitleBarHeight(34);
        content = new GenerateContent(laf, std::move(studioRoot), db, hub);
        setContentOwned(content, false);
        setResizable(true, false);
        centreWithSize(1180, 820); // two panes need the width; it was cramped at 720
        // Below this the right pane's controls would be narrower than their own labels
        // and the takes column would vanish. It scrolls rather than clipping either way,
        // but a window smaller than this is not usable, only survivable.
        setResizeLimits(820, 520, 10000, 10000);
        content->setTrainingBenchVisible(showTrainingBench);
        setAlwaysOnTop(floatAbove);
        setVisible(true);
        toFront(true);
        // After setVisible -- that is what creates the peer the corner mask needs.
        mira_ui::chrome::applyRoundedCorners(*this, 10.0f);
    }
};

// MIRA-GENERATE.md Phase 2. Inference only: prompt builder, LoRA slots and settings,
// preview, keep/discard. No trigger field, no encode button, no datasets list (§3.4).
//
// NOT always-on-top, unlike GenerateWindow. That window floats because it is a tool panel
// used alongside the library -- pick a file there, prompt here. A project window is where
// the work happens, so it behaves as an ordinary sibling of the browser and the two sit
// side by side (§3.3).
class ProjectWindow : public GenerateWindow
{
public:
    ProjectWindow(const MiraLookAndFeel& laf, juce::File studioRoot, mira::Database& db,
                   Sa3WorkerHub& hub, const juce::String& projectName)
        : GenerateWindow(laf, std::move(studioRoot), db, hub,
                          projectName.isNotEmpty() ? projectName : juce::String("Project"),
                          false, false)
    {
    }
};
