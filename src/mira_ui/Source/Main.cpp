// TASKS.md Phase 5. Build-order step 1 promoted spike/03_dragout into this real app
// target, reading mira_core's real Database instead of one hardcoded fixture file. Step
// 2 applied the real palette/type tokens (MiraLookAndFeel). This file now also carries
// step 4: FileTableComponent below replaces step 1's plain Viewport + stacked
// Components with the real paintCell-only TableListBox (FileTable.h/.cpp) — the
// virtualization that actually matters at drive scale. Progress queue and the
// waveform/detail panel are still open build-order steps.

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "mira/analyze/ActiveSpanMap.h"
#include "mira/analyze/CueDetection.h"
#include "mira/db/Database.h"
#include "mira/scan/Scanner.h"
#include "MiraLookAndFeel.h"
#include "FileTable.h"
#include "FileDetailsWindow.h"
#include "PlaceholderPanels.h"
#include "FolderTreeView.h"
#include "ThumbnailStore.h"
#include "NativeWindowChrome.h"
#include "FilterBar.h"
#include "CueEditor.h"
#include "LogView.h"

#include <cstdlib>
#include <deque>
#include <iostream> // runScopeBenchmark's stderr readout
#include <set>
#include <typeinfo> // StallWatchdog's click-target names

// Same convention as the CLI's defaultDbPath() (main.cpp) — duplicated rather than
// shared, since mira_ui doesn't link main.cpp (that would pull in the whole CLI's
// argument-parsing/command surface for one function).
static juce::File defaultDbFile()
{
    auto home = std::getenv("HOME");
    juce::File dir = home != nullptr ? juce::File(juce::String(home)).getChildFile(".mira")
                                      : juce::File::getCurrentWorkingDirectory().getChildFile(".mira");
    dir.createDirectory();
    return dir.getChildFile("library.db");
}

// The real analysis pipeline (Essentia/ONNX/beat_this_cpp/libKeyFinder/...) lives only
// in the CLI's `mira` target -- deliberately never linked into mira_ui (mira_core is the
// lightweight Database+Scanner-only split precisely so the UI doesn't pull all that in).
// "Analyze" (AnalyzeJob below) shells out to that CLI binary instead of duplicating any
// of it -- same reasoning as the earlier decision to reuse mira::scan() rather than
// reimplement scanning, just across a process boundary instead of a shared static lib.
// This walks up from mira_ui's own executable looking for a sibling `src/mira` -- true
// only in this dev build tree (build/src/mira_ui/.../mira.app/... and build/src/mira as
// siblings under build/), not a packaged install layout; that's a real limitation to
// revisit before this ships to anyone who isn't building from source.
static juce::File findMiraCliExecutable()
{
    auto dir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
    for (int i = 0; i < 12 && dir.exists() && dir.getParentDirectory() != dir; ++i)
    {
        auto candidate = dir.getChildFile("src").getChildFile("mira");
        if (candidate.existsAsFile()) return candidate;
        dir = dir.getParentDirectory();
    }
    return {};
}

// Runs mira::scan() (mira_core/Scanner.h — real, the CLI's own scan logic, not a second
// implementation) off the message thread so a large folder doesn't freeze the UI. Opens
// its own Database connection to the same file rather than sharing the UI's — SQLite
// supports multiple connections to one file (SQLITE_THREADSAFE=1, src/CMakeLists.txt),
// and this sidesteps any question of whether the UI's own Database object is safe to
// touch from a background thread at all.
class ScanJob : public juce::Thread
{
public:
    // declareAsStemIn mirrors the CLI's own `mira scan --as stem` flag (ScanOptions::
    // declareAsStem, Scanner.h) -- "so the instrument algorithm shifts to the correct
    // algorithm": a root categorized Score/Music Stems (MainComponent::
    // rootWantsStemDeclaration) sets this automatically, so every file under it gets
    // content_type='stem' at scan time and the stem-tuned instrument reading (FileTable.
    // cpp/FileDetailsWindow.cpp) kicks in without the router having to guess from sibling
    // grouping alone.
    ScanJob(juce::String dbPathIn, std::vector<juce::String> rootsIn, bool declareAsStemIn,
             std::function<void(bool)> onDoneIn, std::function<void(int64_t, juce::String)> onProgressIn = nullptr)
        : juce::Thread("mira scan"), dbPath(std::move(dbPathIn)), roots(std::move(rootsIn)),
          declareAsStem(declareAsStemIn), onDone(std::move(onDoneIn)), onProgress(std::move(onProgressIn))
    {
    }

    void run() override
    {
        // "if scanning has an error ask them to scan again" — mira::scan() itself
        // already swallows and logs per-file errors (Scanner.cpp), but opening the
        // database connection on this thread could still throw (e.g. a locked/corrupt
        // file); caught here so that's reported as a real failure instead of silently
        // killing the thread (an uncaught exception in juce::Thread::run() calls
        // std::terminate, taking the whole app down with it).
        bool success = true;
        try
        {
            mira::Database db(dbPath.toStdString());
            mira::ScanOptions opts;
            opts.declareAsStem = declareAsStem;
            for (const auto& r : roots) opts.roots.push_back(r.toStdString());
            if (onProgress)
            {
                // scan() calls this on this same background thread for every file
                // (Scanner.cpp — no throttling on its end any more, "always at 0 files...
                // feels like its hung" with the old every-25th-file throttle, since one
                // huge stem file alone could take a while to hash). Throttled here
                // instead, by wall-clock time rather than file count, so a folder of a
                // few enormous files still updates regularly even though each one on its
                // own would never hit a fixed file-count threshold.
                auto lastUpdateMs = std::make_shared<juce::int64>(0);
                opts.onProgress = [this, lastUpdateMs](const mira::ScanStats& stats, const std::string& path) {
                    auto now = juce::Time::getMillisecondCounter();
                    if (now - *lastUpdateMs < 150 && !path.empty()) return;
                    *lastUpdateMs = now;
                    juce::MessageManager::callAsync(
                        [this, count = stats.filesSeen, name = juce::File(path).getFileName()] {
                            onProgress(count, name);
                        });
                };
            }
            mira::scan(db, opts);
        }
        catch (const std::exception&)
        {
            success = false;
        }
        juce::MessageManager::callAsync([this, success] { onDone(success); });
    }

private:
    juce::String dbPath;
    std::vector<juce::String> roots;
    bool declareAsStem;
    std::function<void(bool)> onDone;
    std::function<void(int64_t, juce::String)> onProgress;
};

// TASKS.md Phase 5 leftovers: "pre-generated waveform previews during Scan". Runs right
// after a scan finishes, over the files that scan just recorded, writing each one's
// peaks into the on-disk ThumbnailStore so clicking a row shows a complete waveform
// immediately instead of watching one fill in.
//
// Deliberately *after* the scan rather than inside it: scanning is hashing and database
// writes with no audio decoding at all (Scanner.cpp is in mira_core precisely because it
// has no ML or audio-format dependency), and decoding every file to generate peaks would
// roughly be a second, much slower scan hidden inside the first one. Lowest thread
// priority, and every file is skipped the moment it already has a cache entry, so a
// rescan of an unchanged library costs almost nothing.
class ThumbnailPrecacheJob : public juce::Thread
{
public:
    ThumbnailPrecacheJob(std::vector<juce::String> pathsIn, std::function<void()> onDoneIn)
        : juce::Thread("mira thumbnail precache"), paths(std::move(pathsIn)), onDone(std::move(onDoneIn))
    {
        formatManager.registerBasicFormats();
    }

    void run() override
    {
        for (const auto& path : paths)
        {
            if (threadShouldExit()) break;
            mira_ui::thumbnails::generateAndSave(formatManager, juce::File(path),
                                                  [this] { return threadShouldExit(); });
        }
        // No progress reporting and no error surfacing on purpose: this is pure cache
        // warming. Every failure mode (unreadable file, aborted mid-pass) simply leaves
        // that file on the original lazy path, which is what it was on before this
        // existed -- nothing to tell the user about.
        juce::MessageManager::callAsync([this] { if (onDone) onDone(); });
    }

private:
    juce::AudioFormatManager formatManager;
    std::vector<juce::String> paths;
    std::function<void()> onDone;
};

// Builds a folder's list rows off the UI thread (review round 3: the stall logger's only
// real stalls were cold folder opens at 370-667 ms, and "folder clicks horribly slow" was
// the report). It has its own Database connection and its own AudioFormatManager, the
// same reasoning as ScanJob: SQLite serves several connections to one file, and this
// thread never touches the UI's connection or objects. FileTableModel::collectRows is
// const and reads no model state, so calling it here is safe while the UI keeps using
// the model.
class RowBuildJob : public juce::Thread
{
public:
    RowBuildJob(const FileTableModel& modelIn, juce::String dbPathIn, juce::String scopeIn,
                std::function<void(juce::String, FileTableModel::RowList)> onDoneIn)
        : juce::Thread("mira row build"), model(modelIn), dbPath(std::move(dbPathIn)), scope(std::move(scopeIn)),
          onDone(std::move(onDoneIn))
    {
        formatManager.registerBasicFormats();
    }

    void run() override
    {
        FileTableModel::RowList rows;
        try
        {
            mira::Database db(dbPath.toStdString());
            rows = model.collectRows(scope, db, formatManager, [this] { return threadShouldExit(); });
        }
        catch (const std::exception&)
        {
            return; // the list keeps what it had; the next refresh or folder click retries
        }
        if (threadShouldExit()) return; // superseded by a newer folder click
        onDone(scope, std::move(rows)); // onDone gets itself back onto the message thread
    }

private:
    const FileTableModel& model;
    juce::AudioFormatManager formatManager;
    juce::String dbPath, scope;
    std::function<void(juce::String, FileTableModel::RowList)> onDone;
};

// Always-on UI-thread stall logger (review round 3: "clicks are very slow and jerky, every
// 6-7 clicks it just goes into a hang"). Three synthetic benches (MIRA_BENCH=scope/click,
// with playback and with an analyze batch running) all measured smooth, so this measures
// real use instead. A 10 ms timer on the message thread notes every time it fires more
// than 100 ms late -- the UI thread was blocked for that long -- and appends one line to
// ~/.mira/ui-stalls.log: how long, when, and what the last mouse click landed on.
//
// Known false positive: while a macOS menu-bar menu is open, AppKit runs its own event
// loop and JUCE timers pause, so that shows up as a "stall" too. Those lines' last click
// isn't a JUCE component, which is how to tell them apart.
class StallWatchdog : private juce::Timer, private juce::MouseListener
{
public:
    StallWatchdog()
    {
        juce::Desktop::getInstance().addGlobalMouseListener(this);
        lastTick = now();
        startTimer(10);
    }
    ~StallWatchdog() override { juce::Desktop::getInstance().removeGlobalMouseListener(this); }

private:
    static double now() { return juce::Time::getMillisecondCounterHiRes(); }

    void timerCallback() override
    {
        auto t = now();
        auto gap = t - lastTick;
        lastTick = t;
        if (gap > kStallThresholdMs) logStall(gap);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        // Innermost three components, e.g. "TableListBox < FileTableComponent <
        // MainComponent" -- enough to say which pane without logging every class up
        // to the window. typeid names are mangled but readable ("18FileTableComponent").
        juce::StringArray chain;
        for (auto* c = e.eventComponent; c != nullptr && chain.size() < 3; c = c->getParentComponent())
            chain.add(typeid(*c).name());
        lastClickTarget = chain.joinIntoString(" < ");
        lastClickAt = now();
    }

    void logStall(double gapMs)
    {
        auto home = std::getenv("HOME");
        if (home == nullptr) return;
        auto file = juce::File(juce::String(home)).getChildFile(".mira").getChildFile("ui-stalls.log");
        auto sinceClick = lastClickAt > 0.0 ? juce::String(juce::roundToInt(now() - lastClickAt - gapMs)) + " ms after"
                                            : juce::String("no click yet,");
        // "[bench]" on lines written while MIRA_BENCH is driving the app -- the benches'
        // own blocking loops stall on purpose, and those lines mustn't be mistaken for
        // stalls from real use (it happened once: the first log was all bench runs).
        static const bool benchRun = std::getenv("MIRA_BENCH") != nullptr;
        file.appendText(juce::Time::getCurrentTime().toString(true, true, true, true)
                        + (benchRun ? "  [bench]" : "") + "  stall "
                        + juce::String(juce::roundToInt(gapMs)) + " ms  started " + sinceClick + " last click on "
                        + (lastClickTarget.isEmpty() ? juce::String("(nothing)") : lastClickTarget) + "\n");
    }

    static constexpr double kStallThresholdMs = 100.0;
    double lastTick = 0.0;
    double lastClickAt = 0.0;
    juce::String lastClickTarget;
};

// The CLI's opt-in analyze stages (main.cpp's `mira analyze --chords/--transcribe/
// --recheck-tempo`), exposed in the UI (TASKS.md Phase 5 leftovers). They are opt-in on
// the CLI for cost reasons, not correctness ones — measured 15.0s (chords) and 3.7s
// (transcription) on a 5:08 song against 0.4s for key alone — so the UI keeps them off
// by default and off the fast path, as sticky app-wide toggles under the Analyze menu
// rather than a dialog in front of every analyze run.
//
// Session-scoped on purpose: there is no settings file in mira_ui yet, and a hidden
// persisted toggle that silently triples analysis time across launches is worse than
// re-ticking it. Each queued batch captures the options it was queued with, so changing
// a toggle never retroactively rewrites what's already waiting in the queue.
struct AnalyzeOptions
{
    bool chords = false;
    bool transcribe = false;
    bool recheckTempo = false;

    juce::StringArray toCliFlags() const
    {
        juce::StringArray flags;
        if (chords) flags.add("--chords");
        if (transcribe) flags.add("--transcribe");
        if (recheckTempo) flags.add("--recheck-tempo");
        return flags;
    }

    // "Analyze 5 Selected (+chords, +transcribe)" — whatever is about to run, said out
    // loud on the button that runs it, so an expensive extra stage can't be on without
    // it being visible at the point of use.
    juce::String suffixForMenuLabel() const
    {
        auto flags = toCliFlags();
        if (flags.isEmpty()) return {};
        juce::StringArray words;
        for (const auto& f : flags) words.add("+" + f.substring(2));
        return " (" + words.joinIntoString(", ") + ")";
    }
};

// "lets get on with analysis -- analyse button for selected files or the folder -- the
// progress for it". Shells out to the CLI's `mira analyze --paths-from <tmp file>`
// (main.cpp's runAnalyze) rather than linking the ML pipeline into mira_ui -- see
// findMiraCliExecutable's own comment above for why. Runs on its own background thread,
// same reason ScanJob does: a real analysis pass is genuinely slow (measured ~38s for one
// full track, TASKS.md) and must never block the message thread.
class AnalyzeJob : public juce::Thread
{
public:
    AnalyzeJob(juce::File miraCliIn, juce::String dbPathIn, std::vector<juce::String> pathsIn,
               AnalyzeOptions optionsIn, std::function<void(bool)> onDoneIn,
               std::function<void(int, int, juce::String)> onProgressIn,
               std::function<void(juce::String)> onRawLineIn,
               std::function<void(int, int, juce::String)> onFileStartedIn,
               std::function<void(juce::String)> onStageIn)
        : juce::Thread("mira analyze"), miraCli(std::move(miraCliIn)), dbPath(std::move(dbPathIn)),
          paths(std::move(pathsIn)), options(optionsIn), onDone(std::move(onDoneIn)),
          onProgress(std::move(onProgressIn)), onRawLine(std::move(onRawLineIn)),
          onFileStarted(std::move(onFileStartedIn)), onStage(std::move(onStageIn))
    {
    }

    void run() override
    {
        bool success = false;
        // A real temp file, not argv entries -- an arbitrarily large selection (a whole
        // folder with no rows selected) could otherwise exceed a sane command-line
        // length; main.cpp's --paths-from reads it back one path per line, so no
        // escaping is needed even for paths containing spaces.
        auto pathsFile = juce::File::createTempFile("mira_analyze_paths");
        {
            juce::FileOutputStream out(pathsFile);
            for (const auto& p : paths) out << p << "\n";
        }

        if (miraCli.existsAsFile())
        {
            juce::ChildProcess proc;
            juce::StringArray args { miraCli.getFullPathName(), "analyze", "--db", dbPath, "--paths-from",
                                      pathsFile.getFullPathName(),
                                      // Review round 5: without this the CLI says nothing
                                      // between starting a file and finishing it, which on a
                                      // 41-minute stem is many minutes of apparent hang.
                                      "--progress-stages" };
            args.addArray(options.toCliFlags());
            if (proc.start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
            {
                juce::String pending;
                char chunk[4096];
                for (;;)
                {
                    int n = proc.readProcessOutput(chunk, sizeof(chunk));
                    if (n > 0)
                    {
                        pending += juce::String::fromUTF8(chunk, n);
                        int nl;
                        while ((nl = pending.indexOfChar('\n')) >= 0)
                        {
                            handleLine(pending.substring(0, nl));
                            pending = pending.substring(nl + 1);
                        }
                    }
                    else if (!proc.isRunning())
                    {
                        break;
                    }
                }
                proc.waitForProcessToFinish(10000);
                success = proc.getExitCode() == 0;
            }
        }

        pathsFile.deleteFile();
        juce::MessageManager::callAsync([this, success] { onDone(success); });
    }

private:
    // The CLI's machine-readable progress protocol, all on plain stdout (--verbose is a
    // separate, human-readable stderr stream with timings and is not what this reads):
    //   decoding: <path>        -- a file is being decoded, before any analysis runs
    //   starting: N/M <path>    -- that file's analysis has begun
    //   stage: <name>           -- the current file just finished that stage
    //   progress: N/M <path>    -- that file is analyzed and written to the database
    // Every line, recognised or not, also goes to the log window verbatim -- the whole
    // point of a log is the lines nobody thought to parse.
    void handleLine(const juce::String& line)
    {
        if (onRawLine)
        {
            juce::MessageManager::callAsync([cb = onRawLine, line] { cb(line); });
        }

        if (line.startsWith("stage: "))
        {
            auto stage = line.substring(7).trim();
            if (onStage) juce::MessageManager::callAsync([cb = onStage, stage] { cb(stage); });
            return;
        }
        if (line.startsWith("decoding: "))
        {
            auto path = line.substring(10).trim();
            if (onFileStarted)
                juce::MessageManager::callAsync([cb = onFileStarted, path] { cb(0, 0, path); });
            return;
        }

        bool starting = line.startsWith("starting: ");
        if (!starting && !line.startsWith("progress: ")) return;
        auto rest = line.substring(10);
        auto slash = rest.indexOfChar('/');
        auto space = rest.indexOfChar(' ');
        if (slash <= 0 || space <= slash) return;
        int n = rest.substring(0, slash).getIntValue();
        int m = rest.substring(slash + 1, space).getIntValue();
        auto path = rest.substring(space + 1);
        if (starting)
        {
            if (onFileStarted)
                juce::MessageManager::callAsync([cb = onFileStarted, n, m, path] { cb(n, m, path); });
            return;
        }
        juce::MessageManager::callAsync([this, n, m, path] { onProgress(n, m, path); });
    }

    juce::File miraCli;
    juce::String dbPath;
    std::vector<juce::String> paths;
    AnalyzeOptions options;
    std::function<void(bool)> onDone;
    std::function<void(int, int, juce::String)> onProgress;
    std::function<void(juce::String)> onRawLine;
    std::function<void(int, int, juce::String)> onFileStarted;
    std::function<void(juce::String)> onStage;
};

// "so on the tab there is search and tab... an empty tab show the current folder we are
// in and suppose i add a new tab the older tab stays with that folder" — browser-style
// tabs, one per file-list "view": each tab just remembers which folder it's scoped to
// (FileTableComponent::tabScopes), so switching tabs re-scopes the one shared file list
// rather than each tab owning its own full table/model. A close ("x") only appears once
// there's more than one tab -- closing the last one would leave nothing to click.
class TabChip : public juce::Component
{
public:
    TabChip(juce::String labelIn, bool activeIn, bool closableIn)
        : label(std::move(labelIn)), active(activeIn), closable(closableIn)
    {
    }

    std::function<void()> onSelect;
    std::function<void()> onClose;

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour(active ? MiraLookAndFeel::surface3 : MiraLookAndFeel::surface2);
        g.fillRoundedRectangle(bounds.reduced(1.0f, 2.0f), 5.0f);
        if (active)
        {
            g.setColour(MiraLookAndFeel::accent);
            g.fillRoundedRectangle(bounds.removeFromBottom(2.0f).reduced(6.0f, 0.0f), 1.0f);
        }

        auto textBounds = getLocalBounds().reduced(10, 0);
        if (closable) textBounds.removeFromRight(18);
        g.setColour(active ? MiraLookAndFeel::text : MiraLookAndFeel::textDim);
        g.setFont(juce::Font(juce::FontOptions(12.5f)));
        g.drawText(label, textBounds, juce::Justification::centredLeft, true);

        if (closable)
        {
            auto closeBounds = getLocalBounds().removeFromRight(20).toFloat().reduced(8.0f);
            g.setColour(MiraLookAndFeel::textFaint);
            g.drawLine(closeBounds.getX(), closeBounds.getY(), closeBounds.getRight(), closeBounds.getBottom(), 1.3f);
            g.drawLine(closeBounds.getX(), closeBounds.getBottom(), closeBounds.getRight(), closeBounds.getY(), 1.3f);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (closable && getLocalBounds().removeFromRight(20).contains(e.getPosition()))
        {
            if (onClose) onClose();
        }
        else if (onSelect)
        {
            onSelect();
        }
    }

private:
    juce::String label;
    bool active;
    bool closable;
};

class TabBarComponent : public juce::Component
{
public:
    static constexpr int kNewTabButtonWidth = 26;

    TabBarComponent()
    {
        newTabButton.setButtonText("+");
        newTabButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface2);
        newTabButton.setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
        addAndMakeVisible(newTabButton);

        // Absorbs what used to be FileTableComponent's own separate toolbar row below
        // the tabs -- that row was almost always empty (only populated during an active
        // scan) and just read as unexplained dead space between the tabs and the table
        // ("why is there a padding between tabs and the list").
        statusLabel.setFont(juce::Font(juce::FontOptions(12.0f)));
        statusLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
        statusLabel.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(statusLabel);
    }

    // "status column should have analyse button not a separate button" -- Analyze moved
    // to the table's own right-click menu (FileTableComponent's cellClicked wiring); this
    // status text is still the one shared readout for it (and for Scan), now doing
    // double duty since only one of the two is ever actually running at a time in
    // practice.
    void setStatusText(const juce::String& text) { statusLabel.setText(text, juce::dontSendNotification); }

    std::function<void(int)> onTabSelected;
    std::function<void()> onNewTab;
    std::function<void(int)> onCloseTab;

    void setTabs(const std::vector<juce::String>& labels, int activeIndex)
    {
        chips.clear();
        for (int i = 0; i < static_cast<int>(labels.size()); ++i)
        {
            auto* chip = chips.add(new TabChip(labels[static_cast<size_t>(i)], i == activeIndex, labels.size() > 1));
            addAndMakeVisible(chip);
            chip->onSelect = [this, i] { if (onTabSelected) onTabSelected(i); };
            chip->onClose = [this, i] { if (onCloseTab) onCloseTab(i); };
        }
        newTabButton.onClick = [this] { if (onNewTab) onNewTab(); };
        resized();
    }

    void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface); }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(4, 3);
        newTabButton.setBounds(bounds.removeFromLeft(kNewTabButtonWidth));
        bounds.removeFromLeft(4);
        statusLabel.setBounds(bounds.removeFromRight(220));
        bounds.removeFromRight(8);
        int perChip = chips.isEmpty() ? 0 : juce::jlimit(80, 170, bounds.getWidth() / chips.size());
        for (auto* chip : chips)
        {
            chip->setBounds(bounds.removeFromLeft(perChip));
            bounds.removeFromLeft(2);
        }
    }

private:
    juce::OwnedArray<TabChip> chips;
    juce::TextButton newTabButton;
    juce::Label statusLabel;
};

// The drag target: the dragged file's path travels as TableListBox's own drag
// description (FileTableModel::getDragSourceDescription), not via SourceDetails::
// sourceComponent — TableListBox owns its row components, so there's no app-defined row
// type to dynamic_cast back to the way step 1's DraggableFileRow allowed.
class FileTableComponent : public juce::Component, public juce::DragAndDropContainer
{
public:
    static constexpr int kTabBarHeight = 30;

    FileTableComponent(mira::Database& databaseIn, const MiraLookAndFeel& lafIn)
        : laf(lafIn), database(databaseIn), model(databaseIn, lafIn)
    {
        // Hand-sketched layout: "FILES LIST DETAILS — ANALYSE BUTTON — SINGLE OR
        // MULTIPLE" — this used to be a separate toolbar row of its own; folded into the
        // tab bar's own right-aligned status readout instead ("why is there a padding
        // between tabs and the list" — that empty toolbar row, almost always blank
        // except during an active scan, was exactly that padding). No manual Scan button
        // any more either — "scanning is mira's job not the user's job": indexing runs
        // automatically the moment a folder's added (MainComponent's scan orchestrator).
        // "status column should have analyse button not a separate button... select
        // multiple right click analyse" — Analyze itself lives on the table's own
        // right-click menu below, not a toolbar button; the Status column is the readout
        // (not scanned/queued/analyzing/analyzed).
        tabBar.onTabSelected = [this](int index) { switchTab(index); };
        tabBar.onNewTab = [this] { newTab(); };
        tabBar.onCloseTab = [this](int index) { closeTab(index); };
        addAndMakeVisible(tabBar);
        refreshTabBar();

        table.setModel(&model);
        table.setRowHeight(32); // was 28 — bumped alongside the row text going 12.5->14pt
        table.setHeaderHeight(28);
        table.setMultipleSelectionEnabled(true);
        FileTableModel::setupColumns(table.getHeader());
        table.getHeader().setPopupMenuActive(true); // right-click on the HEADER -> JUCE's built-in column chooser
        addAndMakeVisible(table);
        table.updateContent();
        table.setVisible(model.getNumRows() > 0); // starts empty: no folder selected yet

        model.onRightClicked = [this](const juce::MouseEvent&) { showRowContextMenu(); };
        model.onRowDoubleClicked = [this](int64_t fileId) { openFileDetails({ fileId }); };
        // Segment child rows (review round 3): expand/collapse changes which rows exist
        // without a rebuild, so the table has to be told; the toggled file row stays
        // selected rather than the selection landing on whatever child moved into its
        // old index.
        model.setHeader(&table.getHeader());
        model.onDisplayChanged = [this](int keepSelected) {
            table.updateContent();
            if (keepSelected >= 0) table.selectRow(keepSelected);
            repaint();
        };
    }

    // "we could also do a clik on the file pop up dialog open which chose the deatils
    // and be editable and save" — one details window at a time; opening a second file
    // replaces whatever's already showing rather than stacking windows.
    // Review round 2: "can the file details not be a popout, be like a right side bar".
    // The file list only asks for details now; MainComponent owns the sidebar, since it
    // sits beside everything below the filter bar, not inside this component.
    void openFileDetails(std::vector<int64_t> fileIds)
    {
        if (!fileIds.empty() && onDetailsRequested) onDetailsRequested(std::move(fileIds));
    }

    // Every selected row that has a DB id -- what the details sidebar follows. A row
    // that isn't scanned yet has nothing to show or edit, so it's simply not included.
    ~FileTableComponent() override { stopRowBuilds(); }

    // Needed before the first background row build; MainComponent sets it right after
    // construction. Until then, builds fall back to the synchronous path.
    void setDatabasePath(juce::String path) { dbPath = std::move(path); }

    std::function<void()> onRowsLoaded; // each time a background row build lands (the scope bench waits on it)

    int getRowCount() { return model.getNumRows(); } // MainComponent::runScopeBenchmark's readout
    void selectRowForBench(int row) { table.selectRow(row); } // same path as a real click (MIRA_BENCH=click)
    std::vector<juce::String> getAllPathsForBench() const { return model.getAllPaths(); }

    std::vector<int64_t> getSelectedFileIds()
    {
        std::vector<int64_t> ids;
        for (const auto& path : model.getSelectedPaths(table.getSelectedRows()))
            if (auto record = database.findByPath(path.toStdString())) ids.push_back(record->id);
        return ids;
    }

    // Right-click on any row (header right-click is the separate column-chooser above).
    // JUCE's TableListBox preserves an existing multi-selection on right-click as long as
    // the clicked row is already part of it (native list-view convention) -- "select
    // multiple right click analyse" relies on exactly that.
    void showRowContextMenu()
    {
        auto selected = model.getSelectedPaths(table.getSelectedRows());
        auto paths = selected.empty() ? model.getAllPaths() : selected;
        if (paths.empty()) return;

        juce::PopupMenu menu;
        juce::String label = selected.empty()
                                  ? "Analyze Folder (" + juce::String(paths.size()) + " files)"
                                  : "Analyze " + juce::String(paths.size()) + " Selected";
        if (getAnalyzeOptionsSuffix) label += getAnalyzeOptionsSuffix();
        menu.addItem(label, [this, paths] {
            if (onAnalyzeRequested) onAnalyzeRequested(paths);
        });
        // "we could also do a clik on the file pop up dialog open" -- right-click is the
        // discoverable path to the same thing double-click does. With more than one row
        // selected it opens the same window in batch mode (TASKS.md Phase 5 leftovers,
        // "multi-file batch editing"): one set of field editors writing through to the
        // whole selection, rather than N windows or a refusal.
        std::vector<int64_t> selectedIds;
        for (const auto& path : selected)
            if (auto record = database.findByPath(path.toStdString())) selectedIds.push_back(record->id);

        if (!selectedIds.empty())
        {
            juce::String detailsLabel = selectedIds.size() == 1
                                             ? juce::String("Details...")
                                             : "Edit " + juce::String(static_cast<int>(selectedIds.size()))
                                                   + " Selected...";
            menu.addItem(detailsLabel, [this, selectedIds] { openFileDetails(selectedIds); });
        }
        menu.showMenuAsync(juce::PopupMenu::Options());
    }

    void paint(juce::Graphics& g) override
    {
        // No "Show All" starting state any more — nothing picked in the sidebar means
        // an empty list, so say why rather than leaving a blank pane that reads as broken.
        if (model.getNumRows() == 0)
        {
            g.setColour(MiraLookAndFeel::textFaint);
            g.setFont(laf.sansRegular(13.0f));
            g.drawText(loadingRows ? juce::String(juce::CharPointer_UTF8("Loading\xe2\x80\xa6"))
                                   : juce::String("Select a folder to see its files"),
                       getLocalBounds().withTrimmedTop(kTabBarHeight),
                       juce::Justification::centred, 1);
        }
    }

    void resized() override
    {
        auto bounds = getLocalBounds();
        tabBar.setBounds(bounds.removeFromTop(kTabBarHeight));
        table.setBounds(bounds);
    }

    // Forwards TableListBoxModel's selection callback — MainComponent doesn't reach
    // into the model directly (FileTableComponent owns it), same encapsulation the
    // drag-out path already uses.
    void setOnSelectionChanged(std::function<void(const mira::FileRecord*)> cb)
    {
        model.onSelectionChanged = std::move(cb);
    }

    void setOnSegmentSelected(std::function<void(const mira::FileRecord&, int64_t, double, double)> cb)
    {
        model.onSegmentSelected = std::move(cb);
    }

    // Sidebar click-to-scope (TASKS.md Phase 5 discussion) — an empty path now means
    // "nothing selected yet, show nothing" (FileTable.h's setScope comment): there's no
    // "Show All" mode any more — "show all is confusing and dont need it" — clicking a
    // root already shows everything under it, which was the only thing Show All ever
    // added on top of that.
    //
    // "an empty tab show the current folder we are in and suppose i add a new tab the
    // older tab stays with that folder" — a sidebar click always re-scopes the currently
    // *active* tab, never creates one; a new tab only appears via the tab bar's own "+".
    void setScope(const juce::String& folderPathOrEmpty)
    {
        tabScopes[static_cast<size_t>(activeTab)] = folderPathOrEmpty;
        applyScope(folderPathOrEmpty);
        refreshTabBar();
    }

    const juce::String& getScope() const { return currentScope; }

    void newTab()
    {
        tabScopes.push_back({});
        activeTab = static_cast<int>(tabScopes.size()) - 1;
        applyScope({});
        refreshTabBar();
    }

    void switchTab(int index)
    {
        if (index < 0 || index >= static_cast<int>(tabScopes.size())) return;
        activeTab = index;
        applyScope(tabScopes[static_cast<size_t>(activeTab)]);
        refreshTabBar();
    }

    void closeTab(int index)
    {
        if (index < 0 || index >= static_cast<int>(tabScopes.size())) return;
        if (tabScopes.size() == 1) return; // always at least one tab -- nothing to fall back to otherwise
        tabScopes.erase(tabScopes.begin() + index);
        if (activeTab >= static_cast<int>(tabScopes.size())) activeTab = static_cast<int>(tabScopes.size()) - 1;
        else if (index < activeTab) activeTab--;
        applyScope(tabScopes[static_cast<size_t>(activeTab)]);
        refreshTabBar();
    }

    // Re-walks the filesystem for whatever's currently scoped — MainComponent calls this
    // when the scan orchestrator finishes a root the current scope falls under, so newly
    // scanned/analyzed status shows up without the user having to reselect the folder.
    void refresh()
    {
        startRowBuild(); // background; the current rows stay on screen until the new ones land
    }

    // The filter bar's criteria, applied to the list. A rebuild isn't needed (and would
    // be wasteful) -- the rows are already built, this only changes which of them show.
    void setFilterCriteria(const FilterCriteria& criteria)
    {
        model.setFilterCriteria(criteria);
        table.updateContent();
        table.setVisible(model.getNumRows() > 0);
        if (onRowCountsChanged) onRowCountsChanged(model.getNumRows(), model.getUnfilteredRowCount());
        repaint();
    }

    // One file's row only -- what each finished file of an analyze batch uses instead of
    // refresh() (see FileTableModel::refreshPath for the measured cost it avoids).
    void refreshPath(const juce::String& path)
    {
        if (!model.refreshPath(path)) return; // not in this scope -- nothing on screen changed
        if (onFacetsChanged) onFacetsChanged(model.getFacetOptions());
        table.updateContent();
        table.setVisible(model.getNumRows() > 0);
        if (onRowCountsChanged) onRowCountsChanged(model.getNumRows(), model.getUnfilteredRowCount());
        repaint();
    }

    std::function<void(int, int)> onRowCountsChanged; // shown, total -- drives the filter bar's readout
    // New scope's distinct keys/genres/instruments/moods -- refills the filter dropdowns.
    // Fired on rebuild (scope change, refresh), not on filter changes: the options are
    // what the folder has, not what's currently passing the filter.
    std::function<void(FacetOptions)> onFacetsChanged;

    // Tab bar's own right-aligned readout — no click target, driven entirely by
    // MainComponent's scan orchestrator (see this class's ctor comment).
    void setScanStatusText(const juce::String& text) { tabBar.setStatusText(text); }

    std::function<void(std::vector<juce::String>)> onAnalyzeRequested;
    // Owned by MainComponent (the Analyze menu's sticky toggles) -- the file list just
    // reads it so its own Analyze item says what it's actually about to run.
    std::function<juce::String()> getAnalyzeOptionsSuffix;
    // Double-click / "Details..." -- MainComponent opens its details sidebar for these.
    std::function<void(std::vector<int64_t>)> onDetailsRequested;

    // Global (not per-tab) analysis state, forwarded straight through to the model —
    // see FileTableModel::setAnalysisState's own comment for why this has to be global.
    void setAnalysisState(std::set<juce::String> analyzing, std::set<juce::String> queued)
    {
        model.setAnalysisState(std::move(analyzing), std::move(queued));
        table.repaint();
    }

    bool shouldDropFilesWhenDraggedExternally(const juce::DragAndDropTarget::SourceDetails& details,
                                               juce::StringArray& files, bool& canMoveFiles) override
    {
        auto path = details.description.toString();
        if (path.isEmpty()) return false;
        files.add(path);
        canMoveFiles = false; // copy, never move — PRD §1: "no file ever moves"
        return true;
    }

private:
    // The actual model/table update -- setScope/newTab/switchTab/closeTab all funnel
    // through here after updating tabScopes/activeTab themselves, so there's exactly one
    // place that talks to FileTableModel.
    void applyScope(const juce::String& folderPathOrEmpty)
    {
        currentScope = folderPathOrEmpty;
        // Switch immediately (empty list, "Loading..."); the rows are built off the UI
        // thread and land via rowsBuilt (review round 3).
        model.setScopeDeferred(folderPathOrEmpty);
        startRowBuild();
        if (onFacetsChanged) onFacetsChanged(model.getFacetOptions());
        table.updateContent();
        // table.setColour paints its own opaque background over the whole row area even
        // with zero rows, which would hide the "Select a folder..." message paint() below
        // draws underneath it — hide the table itself so that message can show through.
        table.setVisible(model.getNumRows() > 0);
        if (onRowCountsChanged) onRowCountsChanged(model.getNumRows(), model.getUnfilteredRowCount());
        repaint();
    }

    void refreshTabBar()
    {
        std::vector<juce::String> labels;
        labels.reserve(tabScopes.size());
        for (const auto& scope : tabScopes)
            labels.push_back(scope.isEmpty() ? "New Tab" : juce::File(scope).getFileName());
        tabBar.setTabs(labels, activeTab);
    }

    const MiraLookAndFeel& laf;
    mira::Database& database;
    juce::String currentScope;
    FileTableModel model;
    juce::TableListBox table;
    TabBarComponent tabBar;
    std::vector<juce::String> tabScopes { juce::String() }; // always at least one tab

    // Background row building. Declared after `model` on purpose, so they're destroyed
    // before it (the jobs hold a reference to the model); the destructor also stops them
    // explicitly. A superseded job is signalled and parked in retiredRowBuilds rather
    // than waited on, so a quick second folder click never blocks the UI on the first.
    juce::String dbPath;
    std::unique_ptr<RowBuildJob> rowBuildJob;
    std::vector<std::unique_ptr<RowBuildJob>> retiredRowBuilds;
    uint64_t rowBuildGeneration = 0; // bumped per request; a result from an older one is dropped
    bool loadingRows = false;        // paint() says "Loading..." instead of "Select a folder"

    void startRowBuild()
    {
        if (dbPath.isEmpty())
        {
            // Only during construction, before setDatabasePath -- the scope is empty
            // then, so this synchronous path costs nothing.
            model.rebuild();
            rowsInstalled();
            return;
        }
        auto generation = ++rowBuildGeneration;
        // "Loading..." only when there's nothing else to show. On a refresh of the same
        // folder the current rows stay up until the new ones replace them.
        loadingRows = model.getNumRows() == 0 && currentScope.isNotEmpty();
        retireRowBuild();

        juce::Component::SafePointer<FileTableComponent> safeThis(this);
        rowBuildJob = std::make_unique<RowBuildJob>(
            model, dbPath, currentScope,
            [safeThis, generation](juce::String builtScope, FileTableModel::RowList builtRows) {
                juce::MessageManager::callAsync(
                    [safeThis, generation, scope = std::move(builtScope), rows = std::move(builtRows)]() mutable {
                        if (safeThis != nullptr) safeThis->rowsBuilt(generation, scope, std::move(rows));
                    });
            });
        rowBuildJob->startThread(juce::Thread::Priority::normal); // user-initiated, not background work
        repaint();
    }

    void rowsBuilt(uint64_t generation, const juce::String& scope, FileTableModel::RowList rows)
    {
        if (generation != rowBuildGeneration) return; // a newer folder click superseded this one
        loadingRows = false;
        model.setRows(scope, std::move(rows));
        rowsInstalled();
        if (onRowsLoaded) onRowsLoaded();
    }

    void rowsInstalled()
    {
        if (onFacetsChanged) onFacetsChanged(model.getFacetOptions());
        table.updateContent();
        table.setVisible(model.getNumRows() > 0);
        if (onRowCountsChanged) onRowCountsChanged(model.getNumRows(), model.getUnfilteredRowCount());
        repaint();
    }

    void retireRowBuild()
    {
        if (rowBuildJob != nullptr)
        {
            rowBuildJob->signalThreadShouldExit();
            retiredRowBuilds.push_back(std::move(rowBuildJob));
        }
        retiredRowBuilds.erase(std::remove_if(retiredRowBuilds.begin(), retiredRowBuilds.end(),
                                              [](const auto& job) { return !job->isThreadRunning(); }),
                               retiredRowBuilds.end());
    }

    void stopRowBuilds()
    {
        if (rowBuildJob != nullptr) rowBuildJob->stopThread(2000);
        for (auto& job : retiredRowBuilds) job->stopThread(2000);
    }
    int activeTab = 0;
};

// TASKS.md Phase 5 skeleton pass: every region the reference mockup has, laid out with
// real (FlexBox) proportional sizing so the window is genuinely resizable — not the
// hand-written fixed-pixel math step 1/2/4 used, which only ever had 2-3 regions to
// place. Panels without a real implementation yet are honestly marked as such
// (PlaceholderPanels.h) rather than faked; each becomes its own build-order step.
// Hand-sketched layout (TASKS.md Phase 5, superseding the first skeleton pass's fixed
// two-panel bottom strip): sidebar spans the full middle-row height by default; the
// bottom combined panel only claims height once a file is selected, and the sidebar
// shrinks to match rather than the two coexisting as permanent fixed strips.
class MainComponent : public juce::Component
{
public:
    static constexpr int kFilterBarHeight = 40;
    static constexpr int kDefaultBottomPanelHeight = 240;
    static constexpr int kMinBottomPanelHeight = 140;
    static constexpr int kMaxBottomPanelHeight = 480;
    static constexpr int kStatusBarHeight = 26;
    static constexpr int kSplitterWidth = 6;
    static constexpr int kFolderTreeMinWidth = 120;
    static constexpr int kFolderTreeMaxWidth = 400;

    explicit MainComponent(const MiraLookAndFeel& lafIn) : laf(lafIn)
    {
        auto dbFile = defaultDbFile();
        dbPath = dbFile.getFullPathName();
        database = std::make_unique<mira::Database>(dbPath.toStdString());
        miraCliPath = findMiraCliExecutable();

        filterBar = std::make_unique<FilterBar>(laf);
        addAndMakeVisible(*filterBar);

        folderTree = std::make_unique<FolderTreeView>(*database, laf);
        // fileList doesn't exist yet at this line (created below) — safe regardless,
        // these lambdas only run later, on an actual click, well after construction.
        folderTree->onFolderSelected = [this](const juce::File& f) { fileList->setScope(f.getFullPathName()); };
        // "scanning is mira's job not the user's job" — a newly added root gets scanned
        // automatically, no manual Scan click required.
        folderTree->onFolderAdded = [this](const juce::File& f) { enqueueScan(f.getFullPathName()); };
        // "right click folder and choose to analyse the folder so all the files go into
        // analysis -- there is already groups made use that if need be" -- a real
        // recursive walk of the real files on disk, same audio-extension filtering
        // FileTableModel::rebuild already uses, not a DB query (a never-scanned file
        // still gets analyzed here the same way Scan already tolerates it).
        folderTree->onAnalyzeFolderRequested = [this](const juce::File& folder) {
            std::vector<juce::String> paths;
            for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findFiles))
            {
                auto file = entry.getFile();
                auto pathStd = file.getFullPathName().toStdString();
                if (!mira::hasSupportedAudioExtension(pathStd)) continue;
                if (mira::isAppleDoubleSidecar(pathStd)) continue;
                paths.push_back(file.getFullPathName());
            }
            enqueueAnalyze(std::move(paths));
        };
        addAndMakeVisible(*folderTree);

        splitter = std::make_unique<DragBar>(true);
        splitter->onDelta = [this](int dx) {
            folderTreeWidth = juce::jlimit(kFolderTreeMinWidth, kFolderTreeMaxWidth, folderTreeWidth + dx);
            resized();
        };
        addAndMakeVisible(*splitter);

        fileList = std::make_unique<FileTableComponent>(*database, laf);
        fileList->setDatabasePath(dbPath); // enables background row builds (review round 3)
        fileList->setOnSelectionChanged([this](const mira::FileRecord* record) {
            if (record != nullptr)
            {
                bottomPanel->setSelectedFileName(juce::File(record->path).getFileName());
                bottomPanel->setSelectedFile(juce::File(record->path));
                bottomPanel->setAnalysisSummary(buildAnalysisSummary(*record));
                statusBar->setRightText("selected: " + juce::File(record->path).getFileName()
                                         + " " + juce::String(juce::CharPointer_UTF8("\xc2\xb7")) + " id "
                                         + juce::String(record->id));
            }
            else
            {
                statusBar->setRightText("no file selected");
            }
            fileSelected = (record != nullptr);
            selectedFileId = record != nullptr ? record->id : 0;
            // Cleared on every selection change; a segment row re-sets it immediately
            // afterwards (FileTableModel::selectedRowsChanged calls this first).
            selectedSegmentId = 0;
            reloadSegmentsForSelection();
            refreshDetailsSidebar(); // follows the selection while open, single or batch
            refreshMenuState();
            resized();
        });
        fileList->onAnalyzeRequested = [this](std::vector<juce::String> paths) { enqueueAnalyze(std::move(paths)); };
        // The selection handler above has already loaded the parent file by the time
        // this runs (FileTableModel::selectedRowsChanged calls it first).
        fileList->setOnSegmentSelected([this](const mira::FileRecord&, int64_t segmentId, double start, double end) {
            bottomPanel->selectWaveformRange(start, end);
            // The file-selection handler above already ran and cleared this, so setting
            // it here (and forcing) is what makes the sidebar show the segment rather
            // than its parent file (review round 4).
            selectedSegmentId = segmentId;
            refreshDetailsSidebar(true);
        });
        fileList->getAnalyzeOptionsSuffix = [this] { return analyzeOptions.suffixForMenuLabel(); };
        fileList->onRowCountsChanged = [this](int shown, int total) { filterBar->setCounts(shown, total); };
        filterBar->onCriteriaChanged = [this](FilterCriteria c) { fileList->setFilterCriteria(c); };
        fileList->onFacetsChanged = [this](FacetOptions options) { filterBar->setFacetOptions(options); };
        fileList->onDetailsRequested = [this](std::vector<int64_t> ids) { openDetailsSidebar(std::move(ids)); };
        addAndMakeVisible(*fileList);

        bottomPanel = std::make_unique<BottomPanel>(laf);
        bottomPanel->buildTagsMenu = [this](juce::PopupMenu& menu) { buildTagsMenu(menu); };
        bottomPanel->buildSegmentsMenu = [this](juce::PopupMenu& menu) { buildSegmentsMenu(menu); };
        bottomPanel->onMenuAction = [this](int actionId) { performMenuAction(actionId); };
        bottomPanel->getActivityView().onCueBoundaryMoved = [this](int64_t id, double seconds) {
            moveCueBoundary(id, seconds);
        };
        bottomPanel->getActivityView().onCueClicked = [this](int64_t id) {
            if (auto cue = database->findSegmentById(id))
                bottomPanel->selectWaveformRange(cue->startSeconds, cue->endSeconds);
        };
        bottomPanel->getActivityView().onCueRightClicked = [this](int64_t id) { showCueMenu(id); };
        bottomPanel->onAddSegment = [this](double start, double end) { addSegment(start, end); };
        bottomPanel->onSegmentsMenu = [this] { showSegmentsList(); };
        bottomPanel->onSegmentClicked = [this](int64_t id) { showSegmentMenu(id); };
        addAndMakeVisible(*bottomPanel);
        bottomPanel->setVisible(false);

        // "the waveform section also needs to be resizable" — drags the whole bottom
        // panel's height (the waveform inside it just fills whatever room that leaves,
        // PlaceholderPanels.h's BottomPanel::resized), same DragBar pattern
        // the sidebar's own width splitter already uses.
        // Details sidebar's own width handle -- same DragBar the folder tree uses on the
        // left, mirrored: dragging it left (negative dx) makes the sidebar wider.
        detailsSplitter = std::make_unique<DragBar>(true);
        detailsSplitter->onDelta = [this](int dx) {
            detailsSidebarWidth -= dx;
            resized();
        };
        addChildComponent(*detailsSplitter);

        bottomSplitter = std::make_unique<DragBar>(false);
        bottomSplitter->onDelta = [this](int dy) {
            bottomPanelHeight = juce::jlimit(kMinBottomPanelHeight, kMaxBottomPanelHeight, bottomPanelHeight - dy);
            resized();
        };
        addAndMakeVisible(*bottomSplitter);
        bottomSplitter->setVisible(false);

        statusBar = std::make_unique<StatusBarComponent>(laf);
        addAndMakeVisible(*statusBar);
        refreshLibraryCount();

        // Mockup's 1180px-wide table plus a folder-tree sidebar; tall enough that the
        // bottom panel (once a file's selected) has real room instead of being squeezed.
        setSize(920, 720);
        setWantsKeyboardFocus(true); // Space-bar play/pause, see keyPressed override below

        // Resume anything left incomplete from a previous run — a root added but never
        // finished scanning, or one interrupted by an app quit or an error. "if we
        // stopped it... reports that rescan is needed" — this is that resume, done
        // automatically rather than waiting for the user to notice and ask for it.
        for (const auto& p : database->listIncompleteFolderRoots()) enqueueScan(juce::String(p));

        // Waveform-cache catch-up (review round 2: "did we sort the waveform thumbnail
        // while scanning?" -- only for scans run after the cache existed; every folder
        // scanned before that had never been warmed). One pass per root at launch.
        // Already-cached files are skipped without decoding, so after the first launch
        // this costs a directory walk per root and nothing more. Background priority,
        // and cancelled on quit.
        // MIRA_NO_PRECACHE: dev switch, so a timing run can rule the catch-up in or out.
        if (std::getenv("MIRA_NO_PRECACHE") == nullptr)
            for (const auto& p : database->listFolderRoots()) enqueueThumbnailPrecache(juce::String(p));

        // MIRA_BENCH=1: dev-only timing harness (review round 2: "every click is slow,
        // folder clicks horribly slow"). Times exactly what a sidebar folder click does
        // -- FileTableComponent::setScope -- for every added root, two passes (cold disk,
        // then warm), prints to stderr, and quits. Real numbers for every perf change,
        // rather than judging "smooth" by eye.
        if (auto* bench = std::getenv("MIRA_BENCH"))
        {
            if (juce::String(bench) == "click") juce::MessageManager::callAsync([this] { startClickBenchmark(); });
            else juce::MessageManager::callAsync([this] { runScopeBenchmark(); });
        }
    }

    ~MainComponent() override
    {
        if (scanJob) scanJob->stopThread(4000);
        if (analyzeJob) analyzeJob->stopThread(4000);
        // Checks threadShouldExit() between files and inside its peak-generation poll,
        // so it drops whatever it was warming rather than holding up quit.
        if (thumbnailPrecacheJob) thumbnailPrecacheJob->stopThread(4000);
    }

    void paint(juce::Graphics& g) override
    {
        MiraLookAndFeel::paintGlassPanel(g, getLocalBounds(), 0.0f, MiraLookAndFeel::surface);
    }

    // "play stop with space bar" -- JUCE routes an unhandled key event up through
    // ancestors' keyPressed() from whatever's currently focused, so this fires as long
    // as no focused child (a real text field, say) already claimed Space for itself.
    // setWantsKeyboardFocus(true) below plus this override is the whole mechanism; no
    // explicit focus-grabbing needed elsewhere.
    bool keyPressed(const juce::KeyPress& key) override
    {
        // Cmd+F to the filter box -- the one shortcut every browser-shaped app has, and
        // the filter bar is otherwise only reachable by aiming at it.
        if (key == juce::KeyPress('f', juce::ModifierKeys::commandModifier, 0))
        {
            filterBar->focusEditor();
            return true;
        }
        // Cmd+I toggles the details sidebar -- Finder's own Get Info shortcut, and the
        // only way to open it that doesn't involve aiming at a specific row.
        if (key == juce::KeyPress('i', juce::ModifierKeys::commandModifier, 0))
        {
            if (detailsSidebarOpen) closeDetailsSidebar();
            else openDetailsSidebar(fileList->getSelectedFileIds());
            return true;
        }
        if (key == juce::KeyPress::spaceKey && fileSelected)
        {
            bottomPanel->togglePlayback();
            return true;
        }
        return false;
    }

    juce::AudioDeviceManager& getAudioDeviceManager() { return bottomPanel->getAudioDeviceManager(); }

    void resized() override
    {
        auto bounds = getLocalBounds();
        filterBar->setBounds(bounds.removeFromTop(kFilterBarHeight));
        statusBar->setBounds(bounds.removeFromBottom(kStatusBarHeight));

        // Details sidebar: the full height below the filter bar, beside both the file
        // list and the bottom panel. The details content is tall (five field rows plus
        // the caption block), and giving it only the middle row would make it scroll
        // constantly. Width is capped at half the window so it can't swallow the list.
        bool showSidebar = detailsSidebar != nullptr;
        detailsSplitter->setVisible(showSidebar);
        if (showSidebar)
        {
            detailsSidebarWidth = juce::jlimit(kMinDetailsSidebarWidth,
                                               juce::jmax(kMinDetailsSidebarWidth, bounds.getWidth() / 2),
                                               detailsSidebarWidth);
            detailsSidebar->setBounds(bounds.removeFromRight(detailsSidebarWidth));
            detailsSplitter->setBounds(bounds.removeFromRight(kSplitterWidth));
        }

        // Bottom combined panel spans the *full* remaining width (under the sidebar
        // too) and only claims height when a file is actually selected — at height 0
        // there's nothing to lay out inside it, so skip touching it further.
        bottomPanel->setVisible(fileSelected);
        bottomSplitter->setVisible(fileSelected);
        if (fileSelected)
        {
            bottomPanel->setBounds(bounds.removeFromBottom(bottomPanelHeight));
            bottomSplitter->setBounds(bounds.removeFromBottom(kSplitterWidth));
        }

        // Middle row: resizable folder tree + splitter + file table, sharing whatever
        // height is left after the chrome above — this is what "the sidebar wraps
        // itself to that height" means: it's never laid out taller than this remainder.
        folderTree->setBounds(bounds.removeFromLeft(folderTreeWidth));
        splitter->setBounds(bounds.removeFromLeft(kSplitterWidth));
        fileList->setBounds(bounds);
    }

    // Real macOS menu bar's File > Add Folder... (Main.cpp's MiraMenuBarModel) reaches
    // through this rather than duplicating FolderTreeView's picker flow.
    FolderTreeView& getFolderTree() { return *folderTree; }

    // Real macOS menu bar's File > Rescan (MiraMenuBarModel below) — "rescan can be in
    // the osx toolbar... not in the ui". Rescans whatever folder's currently scoped in
    // the file list, or every added root if nothing's scoped yet.
    void rescanCurrentOrAll()
    {
        auto scope = fileList->getScope();
        if (scope.isNotEmpty())
        {
            enqueueScan(scope);
            return;
        }
        for (const auto& p : database->listFolderRoots()) enqueueScan(juce::String(p));
    }

private:
    // Queues one root for scanning (deduped against both the queue and whatever's
    // currently running) and starts it immediately if nothing else is scanning.
    // "scanning is mira's job not the user's job" — every entry point into scanning
    // (Add Folder, an interrupted-scan resume at startup, or the native Rescan menu
    // item) funnels through here rather than each spinning up its own ScanJob.
    void enqueueScan(const juce::String& root)
    {
        if (root == scanningRoot) return;
        for (const auto& queued : scanQueue)
            if (queued == root) return;
        scanQueue.push_back(root);
        database->setFolderRootScanComplete(root.toStdString(), false);
        // Queued, not Scanning yet — "can there be like a que and stuff", so a root
        // waiting behind another one reads differently in the sidebar (dim "queued")
        // from the one actually being worked on right now (bold accent "scanning • N").
        folderTree->setRootScanState(root, FolderRootScanState::Queued, 0);
        startNextScanIfIdle();
    }

    // "so the folder needs a mark of the type... so the instrument algorithm shifts to
    // the correct algorithm" -- a root categorized as either stem kind (Score or Music
    // Stems) gets scanned with the same declare-as-stem treatment the CLI's own
    // `mira scan --as stem` flag provides, so content_type='stem' is set from the
    // category the user actually chose rather than left to the router's own sibling-set
    // guessing. This is what makes the stem-tuned instrument reading apply reliably.
    bool rootWantsStemDeclaration(const juce::String& path) const
    {
        for (const auto& info : database->listFolderRootInfos())
        {
            if (juce::String(info.path) != path || !info.groupId) continue;
            for (const auto& group : database->listFolderGroups())
                if (group.id == *info.groupId)
                    return group.category && group.category->rfind("stems", 0) == 0;
        }
        return false;
    }

    // TASKS.md Phase 5 leftovers, "segment-marker tagging workflow for Score Stems": the
    // Score/Music Stems split existed but only drove the instrument algorithm. This is
    // the other half of what it was for -- long-form score stems get markers, music
    // stems (short per-track submixes) don't, so the marker controls appear only under a
    // root filed in a "stems_score" group.
    //
    // Widened after review: any `stems*` category, not just "stems_score". Two reasons.
    // Markers were asked for on Music Stems too. And a library created before the
    // Score/Music split still has its SCORE STEMS group stored with the legacy category
    // "stems" -- an exact "stems_score" match silently hid the whole workflow from the
    // very folders it was built for. rootWantsStemDeclaration already uses this same
    // prefix test.
    bool isStemPath(const juce::String& filePath) const
    {
        for (const auto& info : database->listFolderRootInfos())
        {
            if (!info.groupId || !filePath.startsWith(juce::String(info.path))) continue;
            for (const auto& group : database->listFolderGroups())
                if (group.id == *info.groupId && group.category && group.category->rfind("stems", 0) == 0)
                    return true;
        }
        return false;
    }

    // A segment's `human` object is CaptionFields.cpp's convention (keywords/moods/
    // genre/instruments); the band in the waveform has room for a few words at most, so
    // this collapses whichever of those are set into one short line.
    juce::String segmentLabel(const mira::SegmentRecord& segment) const
    {
        juce::StringArray parts;
        for (const char* path : { "$.keywords", "$.moods", "$.genre", "$.instruments" })
        {
            for (const auto& value : database->jsonStringArray(segment.human, path))
                parts.add(juce::String(value));
            if (!parts.isEmpty()) break; // first populated field wins -- one line, not all four
        }
        // An untouched auto-segment has no human tags yet, but it does have its own
        // analysis (createAutoSegments runs it immediately) -- show that rather than a
        // blank band, so 30-odd auto segments on a stem are distinguishable at a glance.
        if (parts.isEmpty())
        {
            auto fileId = segment.fileId ? *segment.fileId : selectedFileId;
            if (auto machine = database->getSegmentMachine(segment.id, fileId))
                if (auto top = database->jsonObjectTopKey(*machine, "$.instrument_normalized"))
                    parts.add(juce::String(*top));
        }
        return parts.joinIntoString(", ");
    }

    // "Bb" -> 10, "F#m" -> 6, "N" (no chord) -> -1. Root only: the chord lane colours by
    // root and prints the label verbatim, so the quality ("maj7", "aug", "/D") never
    // needs interpreting here. An accidental is only ever '#' or 'b', which is what keeps
    // "Bm" (B minor) apart from "Bb" (B flat).
    static int chordRootPitchClass(const juce::String& label)
    {
        if (label.isEmpty()) return -1;
        static const int naturals[] = { 9, 11, 0, 2, 4, 5, 7 }; // A B C D E F G
        auto root = label[0];
        if (root < 'A' || root > 'G') return -1;
        int pc = naturals[root - 'A'];
        if (label.length() > 1)
        {
            if (label[1] == '#') pc += 1;
            else if (label[1] == 'b') pc -= 1;
        }
        return (pc + 12) % 12;
    }

    // The lanes that draw what `mira analyze` already stored (TASKS.md Phase 5, "data
    // already exists, nothing draws it"). All the reads are cheap enough to do on the
    // message thread -- one json_each statement each over a `machine` blob that tops out
    // around 65 KB across the whole library -- so they stay in the selection handler
    // rather than becoming another RowBuildJob.
    //
    // The one non-obvious job here is the TIME BASE. PRD §5 makes the analyzers run on
    // the spliced active audio, so a row analyzed before 2026-09-12 has chord/note/beat
    // timestamps that are offsets into that splice, not positions in the file -- the bug
    // that drew BASS_1.wav's chords across the left half of a waveform whose audio sits
    // between 35% and 85%. `machine.$.timebase == "file"` marks a row the analyzer has
    // already converted; anything else is legacy and gets converted here, so the existing
    // library is right without re-analyzing 108 files. See ActiveSpanMap.h.
    //
    // Both paths then CLIP to the active spans, so the drawn result is identical either
    // way: a chord or note block only ever sits over audio that actually produced it,
    // never over silence no analyzer ever saw.
    void reloadTimelineLanes(const std::optional<mira::FileRecord>& record)
    {
        if (!record)
        {
            bottomPanel->setActiveSpans({});
            bottomPanel->setChords({});
            bottomPanel->setNotes({});
            bottomPanel->setBeats({}, {});
            return;
        }

        auto spans = database->parseActiveSpans(record->activeSpans);
        bottomPanel->setActiveSpans(spans);

        const bool legacyTimebase =
            database->jsonExtractString(record->machine, "$.timebase").value_or("") != "file";
        auto toFileIntervals = [&spans, legacyTimebase](double start, double end) {
            return legacyTimebase ? mira::activeIntervalToFileIntervals(spans, start, end)
                                   : mira::clipFileIntervalToSpans(spans, start, end);
        };

        auto duration = database->jsonExtractDouble(record->machine, "$.duration_seconds");
        auto fileDuration = duration.value_or(0.0);

        // A chord change is an instant; the lane draws intervals, so each one runs until
        // the next change. What the LAST one runs to differs by time base: in legacy
        // (spliced) time the audio ends at the total active duration, in file time it
        // ends at the last span's end -- using the file's own duration for either would
        // stretch the final chord across trailing silence.
        auto changes = database->parseChords(record->machine);
        double lastEnd = spans.empty() ? fileDuration
                                        : (legacyTimebase ? mira::totalActiveSeconds(spans)
                                                          : spans.back().second);
        std::vector<WaveformView::ChordMark> chordMarks;
        for (size_t i = 0; i < changes.size(); ++i)
        {
            auto label = juce::String(changes[i].chord);
            double start = changes[i].t;
            double end = i + 1 < changes.size() ? changes[i + 1].t : lastEnd;
            int root = chordRootPitchClass(label);
            // One change can yield several blocks when it straddles a splice -- that is
            // two stretches of the file with unanalyzed silence between them, not one
            // long chord, so it is drawn as two.
            for (const auto& [blockStart, blockEnd] : toFileIntervals(start, end))
                chordMarks.push_back({ blockStart, blockEnd, label, root });
        }
        bottomPanel->setChords(std::move(chordMarks));

        std::vector<WaveformView::NoteBlock> noteBlocks;
        for (const auto& note : database->parseNotes(record->machine))
            for (const auto& [blockStart, blockEnd] : toFileIntervals(note.startSeconds, note.endSeconds))
                noteBlocks.push_back({ blockStart, blockEnd, note.pitch,
                                        static_cast<float>(note.amplitude) });
        bottomPanel->setNotes(std::move(noteBlocks));

        // Beats and downbeats are instants, so they map rather than clip -- a beat lands
        // inside an active span by construction, since that is the only audio that was
        // analyzed. Real detected downbeats, not a grid laid out from the BPM scalar:
        // a synthetic grid drifts away from the audio on anything that isn't metronomic,
        // which is exactly the material a bar ruler is most needed for.
        auto beats = database->jsonDoubleArray(record->machine, "$.rhythm.beat_this_beats");
        auto downbeats = database->jsonDoubleArray(record->machine, "$.rhythm.beat_this_downbeats");
        if (legacyTimebase)
        {
            for (auto& beat : beats) beat = mira::activeTimeToFileTime(spans, beat);
            for (auto& downbeat : downbeats) downbeat = mira::activeTimeToFileTime(spans, downbeat);
        }
        bottomPanel->setBeats(std::move(beats), std::move(downbeats));
    }

    // The stem activity matrix and the cues drawn across it (review round 6). Only a file
    // that is part of a synced set has anything to show here -- a lone file has nothing to
    // be compared against, and the matrix hides itself.
    void reloadGroupActivity(const std::optional<mira::FileRecord>& record)
    {
        if (!record || !record->groupId)
        {
            bottomPanel->setGroupActivity({}, 0.0);
            bottomPanel->setCues({});
            return;
        }

        std::vector<GroupActivityView::StemRow> rows;
        double reel = 0.0;
        for (const auto& sibling : database->findFilesByGroupId(*record->groupId))
        {
            GroupActivityView::StemRow row;
            row.label = juce::File(sibling.path).getFileNameWithoutExtension();
            row.spans = database->parseActiveSpans(sibling.activeSpans);
            row.isMix = mira::filenameSuggestsFullMix(sibling.path);
            row.isCurrent = sibling.id == record->id;
            if (auto duration = database->jsonExtractDouble(sibling.machine, "$.duration_seconds"))
                reel = juce::jmax(reel, *duration);
            rows.push_back(std::move(row));
        }
        bottomPanel->setGroupActivity(std::move(rows), reel);

        std::vector<GroupActivityView::CueMark> cues;
        for (const auto& cue : database->findSegmentsForGroup(*record->groupId))
            cues.push_back({ cue.id, cue.startSeconds, cue.endSeconds, segmentLabel(cue),
                              cue.human != "{}" });
        bottomPanel->setCues(std::move(cues));
        refreshCueEditor();
    }

    // Cue detection runs entirely on stored active spans -- no audio, no models -- so it is
    // a direct call here rather than another child process, and finishes in milliseconds
    // even on a 41-minute fifteen-stem reel.
    void detectCuesForSelection()
    {
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record || !record->groupId) return;

        std::vector<mira::CueStem> stems;
        double reel = 0.0;
        for (const auto& sibling : database->findFilesByGroupId(*record->groupId))
        {
            mira::CueStem stem;
            stem.path = sibling.path;
            stem.activeSpans = database->parseActiveSpans(sibling.activeSpans);
            stem.isMix = mira::filenameSuggestsFullMix(sibling.path);
            if (auto duration = database->jsonExtractDouble(sibling.machine, "$.duration_seconds"))
                reel = juce::jmax(reel, *duration);
            stems.push_back(std::move(stem));
        }

        auto result = mira::detectCues(stems, reel);
        // Untouched proposals are replaced; a cue someone tagged or dragged is never
        // overwritten (the user's own rule). Existing edited cues stay exactly where they
        // are, and new proposals land around them.
        database->deleteUntouchedAutoSegmentsForGroup(*record->groupId);
        auto kept = database->findSegmentsForGroup(*record->groupId);

        int created = 0;
        for (const auto& cue : result.cues)
        {
            // Don't propose a cue on top of one that was kept -- the human's boundary wins.
            bool overlapsKept = false;
            for (const auto& existing : kept)
                if (cue.startSeconds < existing.endSeconds && cue.endSeconds > existing.startSeconds)
                { overlapsKept = true; break; }
            if (overlapsKept) continue;
            database->createSegment(*record->groupId, std::nullopt, cue.startSeconds, cue.endSeconds,
                                     "{}", "auto");
            ++created;
        }

        logStore.append(LogStore::Source::app,
                         "detect cues: " + juce::String(result.cues.size()) + " proposed, "
                             + juce::String(created) + " created, " + juce::String(kept.size())
                             + " kept (" + juce::String(result.silenceBoundaries) + " silence, "
                             + juce::String(result.instrumentationBoundaries) + " instrumentation"
                             + (result.usedMix ? ", mix used" : "") + ")");

        reloadSegmentsForSelection();
        fileList->refresh();
    }

    // Dragging a boundary moves TWO cues: the one that starts there and the one that ends
    // there. That is what makes a reel a partition rather than a pile of islands, and doing
    // it in one write pair is what keeps gaps and overlaps from ever existing.
    void moveCueBoundary(int64_t cueId, double newStartSeconds)
    {
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record || !record->groupId) return;
        auto cues = database->findSegmentsForGroup(*record->groupId);
        std::sort(cues.begin(), cues.end(),
                   [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });

        for (size_t i = 0; i < cues.size(); ++i)
        {
            if (cues[i].id != cueId) continue;
            database->setSegmentBounds(cueId, newStartSeconds, cues[i].endSeconds);
            database->markSegmentEdited(cueId);
            // The cue before it now ends where this one starts -- but only if they were
            // actually adjacent. A boundary between two cues separated by real silence is
            // this cue's start alone, and dragging it must not stretch the previous cue
            // across the gap.
            if (i > 0 && std::abs(cues[i - 1].endSeconds - cues[i].startSeconds) < 1.0)
            {
                database->setSegmentBounds(cues[i - 1].id, cues[i - 1].startSeconds, newStartSeconds);
                database->markSegmentEdited(cues[i - 1].id);
            }
            break;
        }
        reloadSegmentsForSelection();
    }

    // "type timecode will also be helpful" -- dragging is fast but imprecise (at fit zoom on
    // a 41-minute reel one pixel is about 1.7 seconds), so the same edit is reachable by
    // typing m:ss or m:ss.ms.
    void promptCueTimecode(int64_t cueId)
    {
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record || !record->groupId) return;
        auto segment = database->findSegmentById(cueId);
        if (!segment) return;

        auto* window = new juce::AlertWindow("Cue start", "Start time as m:ss or m:ss.mmm",
                                              juce::MessageBoxIconType::NoIcon);
        window->addTextEditor("time", formatSegmentTime(segment->startSeconds));
        window->addButton("Set", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        window->enterModalState(true, juce::ModalCallbackFunction::create([this, window, cueId](int result) {
            if (result == 1)
            {
                auto text = window->getTextEditorContents("time").trim();
                auto colon = text.indexOfChar(':');
                double seconds = colon >= 0
                    ? text.substring(0, colon).getDoubleValue() * 60.0 + text.substring(colon + 1).getDoubleValue()
                    : text.getDoubleValue();
                if (seconds >= 0.0) moveCueBoundary(cueId, seconds);
            }
            delete window;
        }), false);
    }

    void reloadSegmentsForSelection()
    {
        if (selectedFileId == 0)
        {
            bottomPanel->setSegmentWorkflowEnabled(false);
            bottomPanel->setSegments({});
            reloadTimelineLanes(std::nullopt);
            return;
        }
        auto record = database->findById(selectedFileId);
        if (!record)
        {
            bottomPanel->setSegmentWorkflowEnabled(false);
            bottomPanel->setSegments({});
            reloadTimelineLanes(std::nullopt);
            return;
        }

        // Lanes are not gated on isStemPath the way the segment controls below are: a
        // sample or a music track can have chords and a transcription too, and drawing
        // what was analyzed is never the wrong thing to do. Only the *authoring* workflow
        // (declaring caption segments) is stem-specific.
        reloadTimelineLanes(record);
        reloadGroupActivity(record);

        bottomPanel->setSegmentWorkflowEnabled(isStemPath(juce::String(record->path)));

        // Both scopes show on the same timeline: a group-scoped boundary genuinely
        // applies to this file too (it covers every sibling stem at identical
        // timestamps), it just isn't *only* about this file -- hence the separate colour
        // rather than a separate list.
        std::vector<WaveformView::SegmentSpan> spans;
        for (const auto& segment : database->findSegmentsForFile(record->id))
            spans.push_back({ segment.id, segment.startSeconds, segment.endSeconds, segmentLabel(segment), false });
        if (record->groupId)
            for (const auto& segment : database->findSegmentsForGroup(*record->groupId))
                spans.push_back({ segment.id, segment.startSeconds, segment.endSeconds, segmentLabel(segment), true });

        std::sort(spans.begin(), spans.end(),
                   [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
        bottomPanel->setSegments(std::move(spans));
    }

    static juce::String formatSegmentTime(double seconds)
    {
        auto total = juce::roundToInt(seconds);
        return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
    }

    void addSegment(double startSeconds, double endSeconds)
    {
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record) return;

        auto create = [this, startSeconds, endSeconds](std::optional<std::string> groupId,
                                                        std::optional<int64_t> fileId) {
            database->createSegment(std::move(groupId), fileId, startSeconds, endSeconds, "{}");
            bottomPanel->clearWaveformSelection();
            reloadSegmentsForSelection();
        };

        // A file in a synced stem set gets the choice explicitly rather than a guess:
        // cutting the whole set at identical boundaries is the reason group-scoped
        // segments exist (main.cpp's export-segments comment), but a boundary that
        // genuinely only applies to one stem is a real case too.
        if (record->groupId)
        {
            auto siblingCount = static_cast<int>(database->findFilesByGroupId(*record->groupId).size());
            juce::PopupMenu menu;
            menu.addSectionHeader(formatSegmentTime(startSeconds) + juce::String(juce::CharPointer_UTF8("\xe2\x80\x93"))
                                   + formatSegmentTime(endSeconds));
            menu.addItem("All " + juce::String(siblingCount) + " synced stems",
                          [create, groupId = *record->groupId] { create(groupId, std::nullopt); });
            menu.addItem("This file only", [create, id = record->id] { create(std::nullopt, id); });
            menu.showMenuAsync(juce::PopupMenu::Options());
            return;
        }
        create(std::nullopt, record->id);
    }

    // One segment's own actions, reached either from the Segments (N) list or by
    // clicking its band in the waveform.
public:
    // Review round 5: the waveform's title bar becomes a menu surface, and the same menus
    // appear in the right-click and in the macOS menu bar. Built here rather than at each
    // call site so all three stay identical by construction -- the failure mode with three
    // hand-written copies is the one that only shows up when someone adds an item to two
    // of them.
    //
    // Ids are a fixed range per menu, distinct from WaveformView::kViewAction* so a single
    // dispatcher can route a result from any surface without ambiguity.
    enum MenuAction {
        kEditFileTags = 700,
        kClearFileTags,
        kAnalyzeSelected,
        kEditSegmentTags,
        kAddSegmentFromSelection,
        kSegmentsList,
        kExportSegments,
        kDeleteSegment,
        kShowLog, // referenced by MiraMenuBarModel's Window menu
        kDetectCues,
        kToggleActivityMatrix,
        kClearCues,
        kOpenCueEditor,
    };

    void buildTagsMenu(juce::PopupMenu& menu)
    {
        // "Edit Tags..." opens the details sidebar rather than a second dialog: that
        // sidebar already IS mira's tag editor (review round 2 replaced the pop-out window
        // with it), and a parallel dialog editing the same fields is how two editors start
        // disagreeing about what was saved.
        bool haveFile = selectedFileId != 0;
        menu.addItem(kEditFileTags, "Edit Tags...", haveFile);
        menu.addItem(kClearFileTags, "Clear Human Tags", haveFile);
        menu.addSeparator();
        // Only meaningful with a segment actually selected -- greyed rather than hidden so
        // the menu keeps the same shape and "how do I tag a segment" has a visible answer.
        menu.addItem(kEditSegmentTags, "Edit Segment Tags...", selectedSegmentId != 0);
        menu.addSeparator();
        menu.addItem(kAnalyzeSelected, "Analyze This File", haveFile);
    }

    void buildSegmentsMenu(juce::PopupMenu& menu)
    {
        // The macOS menu bar can ask for a menu at moments this component doesn't control
        // (it rebuilds on demand, including during teardown), so every one of these
        // builders tolerates a panel that isn't there rather than assuming it is.
        if (bottomPanel == nullptr) return;
        bool haveSelection = bottomPanel->hasWaveformSelection();
        menu.addItem(kAddSegmentFromSelection,
                      haveSelection ? "+ Segment from Selection" : "+ Segment (drag to select first)",
                      haveSelection);
        menu.addItem(kSegmentsList, "Segments (" + juce::String(bottomPanel->getSegmentCount()) + ")...",
                      bottomPanel->getSegmentCount() > 0);
        menu.addSeparator();
        menu.addItem(kDeleteSegment, "Delete Selected Segment", selectedSegmentId != 0);
        menu.addItem(kExportSegments, "Export Segments...", bottomPanel->getSegmentCount() > 0);

        // Cues are the group-scoped half of the same feature: a segment that covers every
        // stem in the synced set at once. Only offered when there IS a set.
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        bool inGroup = record && record->groupId.has_value();
        menu.addSeparator();
        menu.addSectionHeader("Cues (synced set)");
        menu.addItem(kOpenCueEditor, "Cue Editor...", inGroup);
        menu.addItem(kDetectCues, "Detect Cues", inGroup);
        menu.addItem(kClearCues, "Clear Untagged Cues", inGroup);
        menu.addItem(kToggleActivityMatrix, "Stem Activity Matrix", inGroup,
                      bottomPanel->isActivityShown());
    }

    void performMenuAction(int actionId)
    {
        switch (actionId)
        {
            case kEditFileTags:
                if (selectedFileId != 0) openDetailsSidebar({ selectedFileId });
                return;
            case kClearFileTags:
                if (selectedFileId != 0)
                {
                    database->clearHumanFields(selectedFileId);
                    fileList->refresh();
                    refreshSelectedFilePanel();
                }
                return;
            case kAnalyzeSelected:
                if (auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt)
                    enqueueAnalyze({ juce::String(record->path) });
                return;
            case kEditSegmentTags:
                if (selectedSegmentId != 0) editSegmentTags(selectedSegmentId);
                return;
            case kAddSegmentFromSelection:
                if (bottomPanel != nullptr)
                    if (auto range = bottomPanel->getWaveformSelection())
                        addSegment(range->first, range->second);
                return;
            case kSegmentsList: showSegmentsList(); return;
            case kExportSegments: exportSegments(); return;
            case kDeleteSegment:
                if (selectedSegmentId != 0)
                {
                    database->deleteSegment(selectedSegmentId);
                    selectedSegmentId = 0;
                    reloadSegmentsForSelection();
                    fileList->refresh();
                }
                return;
            case kShowLog: showLogWindow(); return;
            case kOpenCueEditor: showCueEditor(); return;
            case kDetectCues: detectCuesForSelection(); return;
            case kToggleActivityMatrix:
                bottomPanel->setActivityShown(!bottomPanel->isActivityShown());
                return;
            case kClearCues:
                if (auto rec = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt)
                    if (rec->groupId)
                    {
                        database->deleteUntouchedAutoSegmentsForGroup(*rec->groupId);
                        reloadSegmentsForSelection();
                        fileList->refresh();
                    }
                return;
            default:
                // A View id from the macOS menu bar: WaveformView owns those, and the
                // panel's own dispatcher knows how to route them.
                if (bottomPanel != nullptr) bottomPanel->performMenuAction(actionId);
                return;
        }
    }

    // View lives in WaveformView (it owns zoom and lane state); this just forwards, so the
    // macOS View menu is the identical menu the header's View button shows.
    void buildViewMenu(juce::PopupMenu& menu) const
    {
        if (bottomPanel != nullptr) bottomPanel->buildViewMenu(menu);
    }

    // One cue's own actions. Tagging is the point of a cue (the user's framing: a cue is a
    // tag identifier -- "funny", "action") and tagging it also makes it permanent, since an
    // edited cue is never regenerated.
    void showCueMenu(int64_t cueId)
    {
        auto cue = database->findSegmentById(cueId);
        if (!cue) return;
        juce::PopupMenu menu;
        menu.addSectionHeader(formatSegmentTime(cue->startSeconds)
                               + juce::String(juce::CharPointer_UTF8("\xe2\x80\x93"))
                               + formatSegmentTime(cue->endSeconds) + "  (all stems)");
        menu.addItem("Edit Cue Tags...", [this, cueId] { editSegmentTags(cueId); });
        menu.addItem("Set Start Timecode...", [this, cueId] { promptCueTimecode(cueId); });
        menu.addSeparator();
        menu.addItem("Play From Here", [this, cueId] {
            if (auto c = database->findSegmentById(cueId))
                bottomPanel->selectWaveformRange(c->startSeconds, c->endSeconds);
        });
        menu.addSeparator();
        menu.addItem("Delete Cue", [this, cueId] {
            database->deleteSegment(cueId);
            reloadSegmentsForSelection();
        });
        menu.showMenuAsync(juce::PopupMenu::Options());
    }

    void showCueEditor()
    {
        if (cueEditor == nullptr)
        {
            cueEditor = std::make_unique<CueEditorWindow>(laf);
            auto& page = cueEditor->getContent();
            page.onDetectCues = [this] { detectCuesForSelection(); };
            page.onClearUntagged = [this] { performMenuAction(kClearCues); };
            page.onMarkCue = [this](double start, double end) { addCueFromRange(start, end); };
            page.onCueSelected = [this](int64_t id) {
                if (auto cue = database->findSegmentById(id))
                    bottomPanel->selectWaveformRange(cue->startSeconds, cue->endSeconds);
            };
            page.onCueRightClicked = [this](int64_t id) { showCueMenu(id); };
            page.getMatrix().onCueBoundaryMoved = [this](int64_t id, double seconds) {
                moveCueBoundary(id, seconds);
            };
            page.getMatrix().onCueRightClicked = [this](int64_t id) { showCueMenu(id); };
            cueEditor->onClosed = [this] {
                juce::MessageManager::callAsync([this] { cueEditor.reset(); });
            };
        }
        else
        {
            cueEditor->toFront(true);
        }
        refreshCueEditor();
    }

    // The cue window shows the SET, not the selected file, so it is fed from the same reload
    // the bottom panel's matrix uses and simply ignores which sibling happens to be selected.
    void refreshCueEditor()
    {
        if (cueEditor == nullptr) return;
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record || !record->groupId)
        {
            cueEditor->getContent().setContent({}, {}, 0.0, "no synced stem set selected");
            return;
        }

        std::vector<GroupActivityView::StemRow> rows;
        double reel = 0.0;
        for (const auto& sibling : database->findFilesByGroupId(*record->groupId))
        {
            GroupActivityView::StemRow row;
            row.label = juce::File(sibling.path).getFileNameWithoutExtension();
            row.spans = database->parseActiveSpans(sibling.activeSpans);
            row.isMix = mira::filenameSuggestsFullMix(sibling.path);
            row.isCurrent = sibling.id == record->id;
            if (auto duration = database->jsonExtractDouble(sibling.machine, "$.duration_seconds"))
                reel = juce::jmax(reel, *duration);
            rows.push_back(std::move(row));
        }
        std::vector<GroupActivityView::CueMark> cues;
        for (const auto& cue : database->findSegmentsForGroup(*record->groupId))
            cues.push_back({ cue.id, cue.startSeconds, cue.endSeconds, segmentLabel(cue), cue.human != "{}" });

        cueEditor->getContent().setContent(std::move(rows), std::move(cues), reel,
                                            juce::File(record->path).getParentDirectory().getFileName());
    }

    // A cue marked by hand is edited from birth: nobody swept a range across fifteen stems by
    // accident, and a re-detect must never take it away again.
    void addCueFromRange(double startSeconds, double endSeconds)
    {
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record || !record->groupId || endSeconds <= startSeconds) return;
        auto id = database->createSegment(*record->groupId, std::nullopt, startSeconds, endSeconds, "{}",
                                           "manual");
        database->markSegmentEdited(id);
        reloadSegmentsForSelection();
        fileList->refresh();
    }

    void showLogWindow()
    {
        if (logWindow != nullptr)
        {
            logWindow->toFront(true);
            return;
        }
        logWindow = std::make_unique<LogWindow>(logStore, laf);
        logWindow->onClosed = [this] {
            // Deferred: this is called from inside the window's own close button handler,
            // and destroying it synchronously from there deletes the component mid-click.
            juce::MessageManager::callAsync([this] { logWindow.reset(); });
        };
    }

private:
    void showSegmentMenu(int64_t segmentId)
    {
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record) return;
        auto segment = findSegmentById(*record, segmentId);
        if (!segment) return;

        juce::PopupMenu menu;
        menu.addSectionHeader(formatSegmentTime(segment->startSeconds)
                               + juce::String(juce::CharPointer_UTF8("\xe2\x80\x93"))
                               + formatSegmentTime(segment->endSeconds)
                               + (segment->groupId ? juce::String("  (synced set)") : juce::String()));
        menu.addItem("Edit Tags...", [this, segmentId] { editSegmentTags(segmentId); });
        menu.addItem("Export Segments...", [this] { exportSegments(); });
        menu.addSeparator();
        menu.addItem("Delete Segment", [this, segmentId] {
            database->deleteSegment(segmentId);
            reloadSegmentsForSelection();
        });
        menu.showMenuAsync(juce::PopupMenu::Options());
    }

    std::optional<mira::SegmentRecord> findSegmentById(const mira::FileRecord& record, int64_t segmentId) const
    {
        for (const auto& segment : database->findSegmentsForFile(record.id))
            if (segment.id == segmentId) return segment;
        if (record.groupId)
            for (const auto& segment : database->findSegmentsForGroup(*record.groupId))
                if (segment.id == segmentId) return segment;
        return std::nullopt;
    }

    void showSegmentsList()
    {
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record) return;

        std::vector<mira::SegmentRecord> all = database->findSegmentsForFile(record->id);
        if (record->groupId)
        {
            auto groupSegments = database->findSegmentsForGroup(*record->groupId);
            all.insert(all.end(), groupSegments.begin(), groupSegments.end());
        }
        std::sort(all.begin(), all.end(),
                   [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });

        juce::PopupMenu menu;
        for (const auto& segment : all)
        {
            auto label = formatSegmentTime(segment.startSeconds)
                          + juce::String(juce::CharPointer_UTF8("\xe2\x80\x93"))
                          + formatSegmentTime(segment.endSeconds);
            if (segment.groupId) label += "  (synced set)";
            auto tags = segmentLabel(segment);
            if (tags.isNotEmpty()) label += "  " + juce::String(juce::CharPointer_UTF8("\xc2\xb7")) + "  " + tags;
            menu.addItem(label, [this, id = segment.id] { showSegmentMenu(id); });
        }
        menu.addSeparator();
        menu.addItem("Export Segments...", [this] { exportSegments(); });
        menu.showMenuAsync(juce::PopupMenu::Options());
    }

    // Writes through Database::setSegmentHumanField, the same `human` convention
    // CaptionFields.cpp reads for a segment's caption -- these tags are the entire point
    // of declaring the boundary, since `export-segments` renders them into each cut
    // clip's SA3 sidecar.
    void editSegmentTags(int64_t segmentId)
    {
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record) return;
        auto segment = findSegmentById(*record, segmentId);
        if (!segment) return;

        auto* window = new juce::AlertWindow("Segment Tags",
                                              formatSegmentTime(segment->startSeconds) + " to "
                                                  + formatSegmentTime(segment->endSeconds),
                                              juce::MessageBoxIconType::NoIcon);
        struct Field { const char* name; const char* jsonPath; };
        static constexpr Field kFields[] = {
            // keywords first: it's the one field with no machine equivalent at all (the
            // scene/vibe words mira has no analyzer for), so it's the main reason to be
            // in this dialog.
            { "Keywords", "$.keywords" },
            { "Moods", "$.moods" },
            { "Genre", "$.genre" },
            { "Instruments", "$.instruments" },
        };
        for (const auto& field : kFields)
        {
            juce::StringArray existing;
            for (const auto& value : database->jsonStringArray(segment->human, field.jsonPath))
                existing.add(juce::String(value));
            window->addTextEditor(field.name, existing.joinIntoString(", "), juce::String(field.name) + " (comma-separated)");
        }
        window->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
        window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

        window->enterModalState(true, juce::ModalCallbackFunction::create([this, window, segmentId](int result) {
            if (result == 1)
            {
                for (const auto& field : kFields)
                {
                    juce::var array = juce::Array<juce::var>();
                    for (auto& token : juce::StringArray::fromTokens(window->getTextEditorContents(field.name), ",", ""))
                        if (token.trim().isNotEmpty()) array.append(token.trim());
                    database->setSegmentHumanField(
                        segmentId, field.jsonPath,
                        juce::JSON::toString(array, juce::JSON::FormatOptions {}.withSpacing(juce::JSON::Spacing::none))
                            .toStdString());
                }
                reloadSegmentsForSelection();
            }
            delete window;
        }), false);
    }

    // Shells out to the CLI's own `mira export-segments`, same reasoning AnalyzeJob has
    // for `mira analyze`: the cutting/sidecar-rendering code lives there and is not
    // duplicated here. A group-scoped target cuts every sibling stem at identical
    // boundaries, which is why the group id is preferred over the file path when there
    // is one.
    void exportSegments()
    {
        auto record = selectedFileId != 0 ? database->findById(selectedFileId) : std::nullopt;
        if (!record) return;
        if (!miraCliPath.existsAsFile()) return;

        auto target = record->groupId ? juce::String(*record->groupId) : juce::String(record->path);
        auto chooser = std::make_shared<juce::FileChooser>("Export segments to...", juce::File(), "");
        auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
        chooser->launchAsync(flags, [this, chooser, target](const juce::FileChooser& fc) {
                                  auto dir = fc.getResult();
                                  if (!dir.isDirectory()) return; // cancelled
                                  juce::ChildProcess proc;
                                  juce::StringArray args { miraCliPath.getFullPathName(), "export-segments", target,
                                                            "--out-dir", dir.getFullPathName(), "--db", dbPath };
                                  // Fire-and-forget with a real result check: cutting is
                                  // fast (no models involved, just decode + write), so
                                  // this doesn't need AnalyzeJob's whole thread/progress
                                  // apparatus, but a silent failure would be worse than
                                  // no button.
                                  bool started = proc.start(args, juce::ChildProcess::wantStdOut
                                                                       | juce::ChildProcess::wantStdErr);
                                  auto output = started ? proc.readAllProcessOutput() : juce::String();
                                  bool ok = started && proc.waitForProcessToFinish(60000) && proc.getExitCode() == 0;
                                  juce::AlertWindow::showAsync(
                                      juce::MessageBoxOptions()
                                          .withIconType(ok ? juce::MessageBoxIconType::InfoIcon
                                                            : juce::MessageBoxIconType::WarningIcon)
                                          .withTitle(ok ? "Segments exported" : "Export failed")
                                          .withMessage(output.isNotEmpty() ? output : juce::String("No output.")),
                                      static_cast<juce::ModalComponentManager::Callback*>(nullptr));
                              });
    }

    // One precache pass at a time, and never one per queued root: the passes are pure
    // cache warming, so a later root's pass simply waits rather than competing with the
    // current one for disk and CPU the scan queue is already using.
    void enqueueThumbnailPrecache(const juce::String& root)
    {
        juce::File rootDir(root);
        if (!rootDir.isDirectory()) return;

        // Walked from the filesystem with the scanner's own predicates, not queried from
        // the database -- the same pair FileTableModel's listing uses, so the precache
        // covers exactly the files the file list will actually offer to play.
        std::vector<juce::String> paths;
        for (const auto& entry : juce::RangedDirectoryIterator(rootDir, true, "*", juce::File::findFiles))
        {
            auto pathStd = entry.getFile().getFullPathName().toStdString();
            if (!mira::hasSupportedAudioExtension(pathStd) || mira::isAppleDoubleSidecar(pathStd)) continue;
            paths.push_back(entry.getFile().getFullPathName());
        }
        if (paths.empty()) return;

        thumbnailPrecacheQueue.push_back(std::move(paths));
        startNextThumbnailPrecacheIfIdle();
    }

    void startNextThumbnailPrecacheIfIdle()
    {
        if (thumbnailPrecacheJob != nullptr || thumbnailPrecacheQueue.empty()) return;
        auto paths = std::move(thumbnailPrecacheQueue.front());
        thumbnailPrecacheQueue.pop_front();
        thumbnailPrecacheJob = std::make_unique<ThumbnailPrecacheJob>(std::move(paths), [this] {
            // Deferred destruction -- this callback runs while the thread's own run() is
            // still on the stack (same reasoning FileDetailsWindow's close path has).
            juce::MessageManager::callAsync([this] {
                thumbnailPrecacheJob.reset();
                startNextThumbnailPrecacheIfIdle();
            });
        });
        // Lowest priority of any of mira's background work: a waveform that isn't
        // instant is a small annoyance, a UI that stutters while it's being warmed is a
        // bigger one.
        thumbnailPrecacheJob->startThread(juce::Thread::Priority::background);
    }

    // MIRA_BENCH=click: what a *row* click costs, and whether clicks stall periodically
    // ("every 6-7 clicks just goes into a hang", review round 3). Rows are selected one
    // at a time, 150 ms apart like a person clicking down a list, through TableListBox::
    // selectRow, so the whole real selection path runs (waveform load, segments,
    // summary, sidebar). Two numbers per click: the synchronous handler time, and how
    // late the click fired. The second catches stalls from deferred work (thumbnail
    // loads, deferred sidebar rebuilds, repaints) that handler time alone would miss.
    struct ClickBenchPhase
    {
        juce::String scope;
        int clicks;
        bool sidebar;
        bool playback = false;             // start playing after every click, like play-then-click-next
        bool analyzeWhileClicking = false; // queue a real 10-file analyze batch first
    };
    std::vector<ClickBenchPhase> clickBenchPhases;

    void startClickBenchmark()
    {
        auto roots = database->listFolderRoots();
        auto rootContaining = [&roots](const char* needle) {
            for (const auto& r : roots)
                if (juce::String(r).contains(needle)) return juce::String(r);
            return juce::String();
        };
        // Plain clicks, and clicks with the details sidebar open, measured smooth (1-23 ms
        // handler, <= 9 ms late). These phases add the two conditions that run could
        // not produce: audio playing off the drive during clicks, and an analyze batch
        // finishing files (each one triggers a full list rebuild) during clicks.
        clickBenchPhases = {
            { rootContaining("EP9_STEMS"), 15, false },                // control: plain clicks
            { rootContaining("EP9_STEMS"), 20, false, true },          // playing between clicks
            { rootContaining("Dark Techno"), 60, false, false, true }, // analyze batch running
        };
        runClickBenchPhase(0);
    }

    void runClickBenchPhase(size_t phase)
    {
        if (phase >= clickBenchPhases.size())
        {
            juce::JUCEApplication::getInstance()->systemRequestedQuit();
            return;
        }
        const auto& p = clickBenchPhases[phase];
        if (p.scope.isEmpty())
        {
            runClickBenchPhase(phase + 1);
            return;
        }
        // Rows load off the UI thread now, so clicking starts only once they've landed --
        // on a cold folder the first clicks would otherwise find an empty list.
        onceRowsLoaded([this, phase] { scheduleBenchClick(phase, 0); });
        fileList->setScope(p.scope);
        if (p.sidebar) openDetailsSidebar({});
        else closeDetailsSidebar();
        if (p.analyzeWhileClicking)
        {
            auto paths = fileList->getAllPathsForBench();
            if (paths.size() > 10) paths.resize(10);
            enqueueAnalyze(paths);
        }
        std::cerr << "bench click phase " << phase << (p.sidebar ? " (sidebar open)" : "")
                  << (p.playback ? " (playing)" : "") << (p.analyzeWhileClicking ? " (analyzing)" : "") << "  "
                  << p.scope << std::endl;
    }

    void scheduleBenchClick(size_t phase, int click)
    {
        constexpr int kIntervalMs = 150;
        auto due = juce::Time::getMillisecondCounterHiRes() + kIntervalMs;
        juce::Timer::callAfterDelay(kIntervalMs, [this, phase, click, due] {
            auto late = juce::Time::getMillisecondCounterHiRes() - due;
            int rows = fileList->getRowCount();
            if (rows == 0)
            {
                runClickBenchPhase(phase + 1);
                return;
            }
            auto t0 = juce::Time::getMillisecondCounterHiRes();
            fileList->selectRowForBench(click % rows);
            auto ms = juce::Time::getMillisecondCounterHiRes() - t0;
            // The *next* click's handler then has to stop this playback -- the exact
            // path that would wait on the audio callback lock if a disk read stalls.
            if (clickBenchPhases[phase].playback) bottomPanel->togglePlayback();
            std::cerr << "bench click " << click << "  handler " << juce::roundToInt(ms) << " ms  late "
                      << juce::roundToInt(late) << " ms" << std::endl;
            if (click + 1 < clickBenchPhases[phase].clicks) scheduleBenchClick(phase, click + 1);
            else runClickBenchPhase(phase + 1);
        });
    }

    // Runs `fn` the next time a background row build lands, exactly once. The callback is
    // cleared from a *later* message, never from inside itself: clearing a std::function
    // while its own body is running destroys its captures mid-call. The first version of
    // the scope bench did exactly that, read freed memory, and printed garbage timings.
    void onceRowsLoaded(std::function<void()> callback)
    {
        auto fired = std::make_shared<bool>(false);
        fileList->onRowsLoaded = [this, fired, fn = std::move(callback)] {
            if (*fired) return; // e.g. an analyze batch's end-of-batch refresh landing again
            *fired = true;
            juce::MessageManager::callAsync([this] { fileList->onRowsLoaded = nullptr; });
            fn();
        };
    }

    void runScopeBenchmark() { runScopeBenchmarkStep(1, 0); }

    // Rows are built off the UI thread now, so a folder "click" has two costs worth
    // keeping apart: how long setScope blocks the UI (the thing that felt like a hang --
    // should now be ~0), and how long until the rows actually appear. One root at a
    // time, each started only once the previous one's rows landed, so timings don't
    // overlap. Two passes: cold disk, then warm.
    void runScopeBenchmarkStep(int pass, size_t index)
    {
        auto roots = database->listFolderRoots();
        if (index >= roots.size())
        {
            benchPaintFolderTree(pass);
            if (pass < 2) runScopeBenchmarkStep(pass + 1, 0);
            else juce::JUCEApplication::getInstance()->systemRequestedQuit();
            return;
        }
        auto root = juce::String(roots[index]);
        auto t0 = juce::Time::getMillisecondCounterHiRes();
        onceRowsLoaded([this, pass, index, root, t0] {
            std::cerr << "bench pass " << pass << "  rows ready "
                      << juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - t0) << " ms  "
                      << fileList->getRowCount() << " rows  " << root << std::endl;
            juce::MessageManager::callAsync([this, pass, index] { runScopeBenchmarkStep(pass, index + 1); });
        });
        fileList->setScope(root);
        std::cerr << "bench pass " << pass << "  UI blocked "
                  << juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - t0) << " ms  " << root << std::endl;
    }

    // One full repaint of the folder tree, offscreen -- what "every click" pays whenever
    // the sidebar redraws (TreeView calls mightContainSubItems() per visible item while
    // painting, and that walks the disk). Measured 0-1 ms; kept as a regression check.
    void benchPaintFolderTree(int pass)
    {
        if (folderTree->getWidth() <= 0 || folderTree->getHeight() <= 0) return;
        juce::Image image(juce::Image::ARGB, folderTree->getWidth(), folderTree->getHeight(), true);
        juce::Graphics g(image);
        auto t0 = juce::Time::getMillisecondCounterHiRes();
        folderTree->paintEntireComponent(g, false);
        std::cerr << "bench pass " << pass << "  " << juce::roundToInt(juce::Time::getMillisecondCounterHiRes() - t0)
                  << " ms  folder tree repaint" << std::endl;
    }

    void startNextScanIfIdle()
    {
        if (scanningRoot.isNotEmpty()) return;
        if (scanQueue.empty())
        {
            // Truly idle: nothing running, nothing waiting — back to the plain dim
            // library-count readout instead of the bold "scanning" one.
            statusBar->setLeftTextScanning(false);
            // Analysis may still be running -- don't claim idle on its behalf.
            if (analyzeJob == nullptr) bottomPanel->setActivity(BottomPanel::Activity::idle, {});
            logStore.append(LogStore::Source::scan, "scan finished");
            refreshLibraryCount();
            return;
        }
        scanningRoot = scanQueue.front();
        scanQueue.pop_front();
        folderTree->setRootScanState(scanningRoot, FolderRootScanState::Scanning, 0);

        statusBar->setLeftTextScanning(true);
        bottomPanel->setActivity(BottomPanel::Activity::scanning, {});
        logStore.append(LogStore::Source::scan, "scan started");
        updateScanStatusText(0, {});

        scanJob = std::make_unique<ScanJob>(
            dbPath, std::vector<juce::String> { scanningRoot }, rootWantsStemDeclaration(scanningRoot),
            [this](bool success) { onScanFinished(success); },
            [this](int64_t filesSeen, juce::String currentFile) { onScanProgress(filesSeen, currentFile); });
        // Below normal priority: scanning is real disk + CPU work (SHA-256 over
        // potentially tens of GB) that would otherwise compete with the message thread
        // for CPU time on a busy scan and make the whole UI feel laggy — "the app feels
        // junky and useless" while a big folder scans. Lower priority means the OS
        // scheduler favours the UI thread whenever it actually needs to run; the scan
        // just takes a little longer in the background, which is the right trade here.
        scanJob->startThread(juce::Thread::Priority::low);
    }

    // Shared by startNextScanIfIdle (0 files, just started) and onScanProgress —
    // includes how many more roots are waiting behind this one, the actual "queue" the
    // user asked to be able to see.
    // currentFile is empty right at the very start (0 files, nothing examined yet) —
    // "always stuck at scanning 0 files... feels like its hung" was a real throttling
    // bug (Scanner.cpp used to only report every 25th file, so one folder of a few huge
    // stem files could sit at "0 files" for ages); now every file is reported and
    // throttled by wall-clock time instead (ScanJob), and the filename itself is shown
    // too so it's obvious mira is actually working through a slow file, not stuck.
    void updateScanStatusText(int64_t filesSeen, const juce::String& currentFile)
    {
        // juce::String(const char*) treats its input as plain ASCII, not UTF-8 (JUCE's
        // own doc comment on that constructor) — a raw "\xe2\x80\xa6" byte literal
        // passed straight to it renders as mojibake ("â€¦"), not an ellipsis. Every
        // non-ASCII literal here goes through CharPointer_UTF8 instead, same convention
        // FileTable.cpp's formatDash() already uses for its em dash.
        auto ellipsis = juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"));
        auto middleDot = juce::String(juce::CharPointer_UTF8("\xc2\xb7"));
        juce::String text = "Scanning \"" + juce::File(scanningRoot).getFileName() + "\"" + ellipsis + " "
                             + juce::String(filesSeen) + " files";
        if (currentFile.isNotEmpty()) text += " " + middleDot + " " + currentFile;
        if (!scanQueue.empty()) text += " (" + juce::String(scanQueue.size()) + " more queued)";
        statusBar->setLeftText(text);
        if (isScopeUnderScanningRoot())
        {
            juce::String toolbarText = "Scanning" + ellipsis + " " + juce::String(filesSeen) + " files";
            if (currentFile.isNotEmpty()) toolbarText += " " + middleDot + " " + currentFile;
            fileList->setScanStatusText(toolbarText);
        }
    }

    bool isScopeUnderScanningRoot() const
    {
        auto scope = fileList->getScope();
        if (scope.isEmpty() || scanningRoot.isEmpty()) return false;
        return scope == scanningRoot || juce::File(scope).isAChildOf(juce::File(scanningRoot));
    }

    void onScanProgress(int64_t filesSeen, const juce::String& currentFile)
    {
        folderTree->setRootScanState(scanningRoot, FolderRootScanState::Scanning, filesSeen);
        // No total-file count ahead of a filesystem walk, so this is a running count,
        // not a fabricated percentage.
        updateScanStatusText(filesSeen, currentFile);
    }

    void onScanFinished(bool success)
    {
        auto finishedRoot = scanningRoot;
        database->setFolderRootScanComplete(finishedRoot.toStdString(), success);
        folderTree->setRootScanState(finishedRoot, success ? FolderRootScanState::Complete
                                                             : FolderRootScanState::Error);

        if (isScopeUnderScanningRoot())
        {
            fileList->refresh();
            fileList->setScanStatusText({});
        }

        scanningRoot.clear();

        // Warm the on-disk waveform cache for everything under the root that just
        // finished (TASKS.md Phase 5 leftovers, "pre-generated waveform previews during
        // Scan"). Only on a successful scan: a failed one has an unknown, partial idea
        // of what's actually in the folder.
        if (success) enqueueThumbnailPrecache(finishedRoot);

        // "if scanning has an error ask them to scan again" — a real prompt, not a
        // silent failure; the sidebar's own "rescan needed" badge (FolderTreeView.cpp)
        // stays up either way until this (or the native Rescan menu item) is used.
        if (!success)
        {
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle("Scan failed")
                    .withMessage("mira couldn't finish scanning \"" + juce::File(finishedRoot).getFileName()
                                 + "\". Try scanning it again?")
                    .withButton("Scan Again")
                    .withButton("Not Now"),
                [this, finishedRoot](int result) {
                    if (result == 1) enqueueScan(finishedRoot);
                });
        }

        startNextScanIfIdle();
    }

    // "can we get the analysis file details in the bottom" -- a compact one-line real
    // summary for the bottom panel's detail bar (PlaceholderPanels.h), same effective-
    // value logic (human override beats analyzed) FileTable.cpp's buildRow and
    // FileDetailsWindow both already use, just condensed to one line instead of a full
    // table row or a whole details window.
    juce::String buildAnalysisSummary(const mira::FileRecord& record)
    {
        if (!record.analyzedAt) return {};
        juce::StringArray parts;

        auto humanBpm = database->jsonExtractDouble(record.human, "$.bpm");
        auto bpm = humanBpm ? humanBpm : database->jsonExtractDouble(record.machine, "$.rhythm.beat_this_bpm");
        if (bpm) parts.add(juce::String(juce::roundToInt(*bpm)) + " bpm");

        auto humanKey = database->jsonExtractString(record.human, "$.key");
        auto key = humanKey ? humanKey : database->jsonExtractString(record.machine, "$.key.key");
        if (key) parts.add(juce::String(*key));

        auto humanGenre = database->jsonStringArray(record.human, "$.genre");
        if (!humanGenre.empty())
        {
            parts.add(juce::String(humanGenre.front()));
        }
        else if (auto genreTop = database->jsonObjectTopKey(record.machine, "$.genre_normalized"))
        {
            parts.add(juce::String(*genreTop));
        }

        auto humanInstrument = database->jsonStringArray(record.human, "$.instruments");
        if (!humanInstrument.empty())
        {
            parts.add(juce::String(humanInstrument.front()));
        }
        else
        {
            auto instrumentPath = record.contentType == "stem" ? "$.stem_instrument_normalized" : "$.instrument_normalized";
            auto instrumentTop = database->jsonObjectTopKey(record.machine, instrumentPath);
            if (!instrumentTop) instrumentTop = database->jsonObjectTopKey(record.machine, "$.instrument_normalized");
            if (instrumentTop) parts.add(juce::String(*instrumentTop));
        }

        auto humanMood = database->jsonStringArray(record.human, "$.moods");
        if (!humanMood.empty())
        {
            parts.add(juce::String(humanMood.front()));
        }
        else if (auto moodTop = database->jsonObjectTopKey(record.machine, "$.moodtheme"))
        {
            parts.add(juce::String(*moodTop));
        }

        return parts.joinIntoString(juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")));
    }

    // Re-pushes the selected file's analysis summary and segments to the bottom panel.
    // The selection callback only fires when the *selection* changes, so a file that
    // finishes analyzing while it's already selected kept showing its pre-analysis
    // "no analysis yet" line until it was clicked away from and back (review round 2).
    void refreshSelectedFilePanel()
    {
        if (selectedFileId == 0) return;
        if (auto record = database->findById(selectedFileId))
            bottomPanel->setAnalysisSummary(buildAnalysisSummary(*record));
        reloadSegmentsForSelection();
    }

    // --- Details sidebar (review round 2: replaces the pop-out FileDetailsWindow) ---
    //
    // Every rebuild is deferred through callAsync, and none happens when the selected
    // ids haven't changed. Both rules exist for the same crash: Save and Close are
    // buttons *inside* the sidebar, and their handlers lead back here (Save refreshes
    // the list, which can re-fire the selection callback; Close closes the sidebar).
    // Rebuilding synchronously would delete the button whose click is still running.
    void openDetailsSidebar(std::vector<int64_t> ids)
    {
        detailsSidebarOpen = true;
        juce::MessageManager::callAsync([this, ids] { rebuildDetailsSidebar(ids); });
    }

    void closeDetailsSidebar()
    {
        detailsSidebarOpen = false;
        detailsSidebar.reset();
        detailsSidebarIds.clear();
        resized();
    }

    // `force` is for when the data changed underneath the same selection (a selected
    // file finished analyzing). That discards unsaved edits in the sidebar, so it's only
    // used when the file being shown actually changed.
    void refreshDetailsSidebar(bool force = false)
    {
        if (!detailsSidebarOpen) return;
        auto ids = fileList->getSelectedFileIds();
        // The segment matters as much as the ids: selecting a file row after one of its
        // own segment rows leaves the ids identical, and skipping that rebuild would
        // leave the segment's view up for the file.
        if (!force && detailsSidebar != nullptr && ids == detailsSidebarIds
            && selectedSegmentId == detailsSidebarSegmentId)
            return;
        juce::MessageManager::callAsync([this, ids] { rebuildDetailsSidebar(ids); });
    }

    void rebuildDetailsSidebar(std::vector<int64_t> ids)
    {
        if (!detailsSidebarOpen) return; // closed again before this deferred call ran
        detailsSidebarIds = ids;
        // Segment view only for a single file -- a segment belongs to one file, so it
        // has no meaning across a multi-selection.
        detailsSidebarSegmentId = ids.size() == 1 ? selectedSegmentId : 0;
        detailsSidebar.reset();
        if (!ids.empty())
        {
            detailsSidebar = std::make_unique<FileDetailsRoot>(
                *database, laf, std::move(ids),
                // Saved: the list and bottom summary show the new effective values. The
                // sidebar itself isn't touched -- FileDetailsContent::save() already
                // reloads its own fields.
                [this] {
                    fileList->refresh();
                    refreshSelectedFilePanel();
                },
                [this] { juce::MessageManager::callAsync([this] { closeDetailsSidebar(); }); },
                detailsSidebarSegmentId);
            addAndMakeVisible(*detailsSidebar);
        }
        // Nothing selected leaves the sidebar open but empty (resized() gives it no
        // room), so selecting a row brings it straight back rather than needing Cmd+I.
        resized();
    }

    void refreshLibraryCount()
    {
        auto count = database->queryFiles("1=1").size();
        auto dash = juce::String(juce::CharPointer_UTF8("\xe2\x80\x94"));
        statusBar->setLeftText(juce::String(count) + " file(s) in " + dbPath
                                + (count == 0 ? "  " + dash + " add a folder to get started" : ""));
    }

    // "so if i analyse different files in a different folder then i dono which file is
    // analysing -- this needs to be worked out" -- a real queue, same shape as the scan
    // one: multiple batches (one right-click "Analyze..." each) queue up, only one
    // AnalyzeJob subprocess runs at a time, but every file across every queued batch is
    // visible as "queued" or "analyzing" in the Status column regardless of which
    // folder/tab is currently on screen (FileTableModel::setAnalysisState is global, not
    // scoped to the active tab).
    void enqueueAnalyze(std::vector<juce::String> paths)
    {
        if (paths.empty()) return;
        if (!miraCliPath.existsAsFile())
        {
            juce::AlertWindow::showAsync(
                juce::MessageBoxOptions()
                    .withIconType(juce::MessageBoxIconType::WarningIcon)
                    .withTitle("Can't find mira's analyzer")
                    .withMessage("The mira command-line analyzer couldn't be located next to this app. "
                                 "Analysis needs it -- try rebuilding both targets."),
                static_cast<juce::ModalComponentManager::Callback*>(nullptr));
            return;
        }
        // Options are snapshotted here, not read at dequeue time: a batch runs with the
        // toggles that were on when it was requested, so ticking Chords while a long
        // queue is draining doesn't silently change what those already-queued batches do.
        analyzeQueue.push_back({ std::move(paths), analyzeOptions });
        updateAnalysisState();
        startNextAnalyzeIfIdle();
    }

    void startNextAnalyzeIfIdle()
    {
        if (analyzeJob != nullptr) return;
        if (analyzeQueue.empty())
        {
            fileList->setScanStatusText({}); // shared with Scan's own readout -- blank means genuinely idle
            return;
        }
        auto batch = analyzeQueue.front();
        analyzeQueue.pop_front();
        activeAnalyzeBatch = std::set<juce::String>(batch.paths.begin(), batch.paths.end());
        updateAnalysisState();

        analyzeJob = std::make_unique<AnalyzeJob>(
            miraCliPath, dbPath, batch.paths, batch.options,
            [this](bool success) { onAnalyzeFinished(success); },
            [this](int n, int m, juce::String path) { onAnalyzeProgress(n, m, path); },
            [this](juce::String line) { logStore.append(LogStore::Source::analyze, line); },
            [this](int n, int m, juce::String path) { onAnalyzeFileStarted(n, m, path); },
            [this](juce::String stage) { onAnalyzeStage(stage); });
        analyzeJob->startThread(juce::Thread::Priority::low); // same reasoning as ScanJob -- don't compete with the UI thread
    }

    // Review round 5: "in the bottom bar - show which file is getting analysed or the
    // process that is going on - so we know its not hung it is process."
    //
    // Until the CLI grew `starting:`/`stage:`, the only event mira_ui ever saw was a file
    // *finishing*. With 12 score stems of 41 minutes each that meant one line every few
    // minutes and nothing in between -- there was genuinely no way to tell work from a
    // hang. These two handlers are the in-between.
    void onAnalyzeFileStarted(int n, int m, const juce::String& path)
    {
        // n == 0 is the decode pass, which runs over every candidate before any analysis
        // starts and is itself minutes of work on a 41-minute file -- worth naming rather
        // than showing as a stalled "0/12".
        analyzeCurrentPath = path;
        analyzeCurrentStage = n == 0 ? "decoding" : "starting";
        if (n > 0)
        {
            analyzeCurrentIndex = n;
            analyzeCurrentTotal = m;
        }
        updateAnalyzeReadout();
    }

    void onAnalyzeStage(const juce::String& stage)
    {
        analyzeCurrentStage = stage;
        updateAnalyzeReadout();
    }

    void updateAnalyzeReadout()
    {
        if (analyzeCurrentPath.isEmpty())
        {
            fileList->setScanStatusText({});
            bottomPanel->setActivity(BottomPanel::Activity::idle, {});
            return;
        }
        auto dot = juce::String(juce::CharPointer_UTF8("\xc2\xb7"));
        juce::String text = "Analyzing";
        if (analyzeCurrentTotal > 0)
            text += " " + juce::String(analyzeCurrentIndex) + "/" + juce::String(analyzeCurrentTotal);
        text += " " + dot + " " + juce::File(analyzeCurrentPath).getFileName();
        if (analyzeCurrentStage.isNotEmpty()) text += " " + dot + " " + analyzeCurrentStage;
        if (!analyzeQueue.empty()) text += " (" + juce::String(analyzeQueue.size()) + " more queued)";
        fileList->setScanStatusText(text);
        // The panel gets the short form: it has a dot and a strip of header, not a status
        // bar's full width.
        bottomPanel->setActivity(BottomPanel::Activity::analyzing,
                                  juce::File(analyzeCurrentPath).getFileName()
                                      + (analyzeCurrentStage.isNotEmpty() ? " " + dot + " " + analyzeCurrentStage
                                                                           : juce::String()));
    }

    void onAnalyzeProgress(int n, int m, const juce::String& finishedPath)
    {
        // The CLI only reports a file once it's *done* (main.cpp's "progress: N/M path"
        // prints after the database write) -- there's no separate "started" event, so
        // this is the only point where any given file's state actually changes from
        // "analyzing" to real "analyzed". Removing it from the active-batch set here and
        // refreshing immediately (not just at the very end of the whole batch) is what
        // makes each row flip to green as it completes, one at a time.
        activeAnalyzeBatch.erase(finishedPath);
        updateAnalysisState();
        fileList->refreshPath(finishedPath); // one row, not the whole folder (review round 3)
        refreshSelectedFilePanel();
        // Rebuild the sidebar only if it's showing the file that just finished;
        // rebuilding for every file in a long batch would keep throwing away edits.
        if (auto finished = database->findByPath(finishedPath.toStdString());
            finished && std::find(detailsSidebarIds.begin(), detailsSidebarIds.end(), finished->id)
                            != detailsSidebarIds.end())
            refreshDetailsSidebar(true);

        analyzeCurrentIndex = n;
        analyzeCurrentTotal = m;
        analyzeCurrentStage = "done";
        analyzeCurrentPath = finishedPath;
        updateAnalyzeReadout();
        logStore.append(LogStore::Source::app,
                         "finished " + juce::String(n) + "/" + juce::String(m) + " "
                             + juce::File(finishedPath).getFileName());
    }

    void onAnalyzeFinished(bool success)
    {
        activeAnalyzeBatch.clear(); // any left over after a failure mid-batch shouldn't stay stuck "analyzing"
        analyzeJob.reset();
        analyzeCurrentPath = {};
        analyzeCurrentStage = {};
        analyzeCurrentIndex = analyzeCurrentTotal = 0;
        updateAnalyzeReadout();
        logStore.append(LogStore::Source::app, success ? "analyze batch finished"
                                                        : "analyze batch FAILED (non-zero exit)");
        updateAnalysisState();
        fileList->refresh(); // "once scan is done then show the details in the list" -- same for analyze
        refreshSelectedFilePanel();

        if (!success)
            juce::AlertWindow::showAsync(juce::MessageBoxOptions()
                                              .withIconType(juce::MessageBoxIconType::WarningIcon)
                                              .withTitle("Analysis failed")
                                              .withMessage("mira's analyzer didn't finish cleanly. Check that the "
                                                           "files are readable and try again."),
                                          static_cast<juce::ModalComponentManager::Callback*>(nullptr));

        startNextAnalyzeIfIdle();
    }

public:
    // The Analyze menu's model -- see AnalyzeOptions for why these are sticky session
    // toggles rather than a per-run dialog or a persisted setting.
    AnalyzeOptions getAnalyzeOptions() const { return analyzeOptions; }
    void setAnalyzeOptions(AnalyzeOptions o) { analyzeOptions = o; }

private:
    void updateAnalysisState()
    {
        std::set<juce::String> queued;
        for (const auto& batch : analyzeQueue)
            for (const auto& p : batch.paths) queued.insert(p);
        fileList->setAnalysisState(activeAnalyzeBatch, queued);
    }

    const MiraLookAndFeel& laf;
    juce::String dbPath;
    juce::File miraCliPath;
    std::unique_ptr<mira::Database> database;
    std::unique_ptr<FilterBar> filterBar;
    std::unique_ptr<FolderTreeView> folderTree;
    std::unique_ptr<DragBar> splitter;
    std::unique_ptr<FileTableComponent> fileList;
    std::unique_ptr<BottomPanel> bottomPanel;
    std::unique_ptr<DragBar> bottomSplitter;

    std::unique_ptr<FileDetailsRoot> detailsSidebar; // null while closed, or open with nothing selected
    std::unique_ptr<DragBar> detailsSplitter;
    std::vector<int64_t> detailsSidebarIds;          // what it was last built for
    int64_t detailsSidebarSegmentId = 0;             // and which segment of it, 0 = the file itself
    bool detailsSidebarOpen = false;
    int detailsSidebarWidth = 440;
    static constexpr int kMinDetailsSidebarWidth = 360; // Save + Revert + Close still fit side by side
    std::unique_ptr<StatusBarComponent> statusBar;
    int folderTreeWidth = 200;
    int bottomPanelHeight = kDefaultBottomPanelHeight;
    bool fileSelected = false;
    int64_t selectedFileId = 0;    // 0 == nothing selected -- what the segment workflow acts on
    int64_t selectedSegmentId = 0; // non-zero when a segment child row is the selection

    std::deque<juce::String> scanQueue;
    juce::String scanningRoot; // empty when nothing's actively scanning
    std::unique_ptr<ScanJob> scanJob;

    std::deque<std::vector<juce::String>> thumbnailPrecacheQueue;
    std::unique_ptr<ThumbnailPrecacheJob> thumbnailPrecacheJob;

    struct QueuedAnalyzeBatch
    {
        std::vector<juce::String> paths;
        AnalyzeOptions options; // snapshotted at enqueue time -- see enqueueAnalyze
    };
    std::deque<QueuedAnalyzeBatch> analyzeQueue;
    std::set<juce::String> activeAnalyzeBatch; // shrinks as AnalyzeJob reports each file done
    std::unique_ptr<AnalyzeJob> analyzeJob;

    // What the CLI is doing right now, as reported by its own `starting:`/`stage:` lines.
    juce::String analyzeCurrentPath, analyzeCurrentStage;
    int analyzeCurrentIndex = 0, analyzeCurrentTotal = 0;

    // Every line the analyzer prints, kept for the Window > Log... window. Declared here
    // rather than as a global so it dies with the component that owns the jobs feeding it.
    LogStore logStore;
    std::unique_ptr<LogWindow> logWindow;
    std::unique_ptr<CueEditorWindow> cueEditor;

public:
    // The macOS menu bar bakes each item's enabled/ticked state in when the menu is BUILT,
    // and JUCE hands AppKit a finished PopupMenu -- so without an explicit rebuild the
    // Tags/Segments/View menus keep whatever state they had at launch, which is "nothing is
    // selected, everything disabled". Found the hard way: "Cue Editor..." was permanently
    // greyed out even with a grouped file selected, because the menu had been built once,
    // before any selection existed.
    std::function<void()> onMenuStateChanged;

private:
    void refreshMenuState() const
    {
        if (onMenuStateChanged) onMenuStateChanged();
    }
    AnalyzeOptions analyzeOptions; // sticky for the session, toggled from the Analyze menu
    StallWatchdog stallWatchdog;   // logs real UI-thread stalls to ~/.mira/ui-stalls.log
};

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow(juce::String name, const MiraLookAndFeel& lafIn)
        : DocumentWindow(name, MiraLookAndFeel::surface, DocumentWindow::allButtons)
    {
        // JUCE-drawn, not native — the native title bar is plain OS gray and can't take
        // MiraLookAndFeel's colours at all (confirmed visually: it sat disconnected from
        // the rest of the window). drawDocumentWindowTitleBar above is what actually
        // paints this now.
        setUsingNativeTitleBar(false);
        setTitleBarHeight(34); // mockup's .titlebar: 9px vertical padding either side of the text line
        // "cant we do curves" -- tried a transparent-peer window silhouette
        // (windowIsSemiTransparent + a rounded content fill) so the corners would be
        // genuinely cut away by the OS compositor. Reverted: "the frame is still
        // straight, there's a double window kind of thing" -- it produced a visible
        // ghost/duplicate window outline instead (likely the drop shadow or peer bounds
        // rendering square underneath the rounded paint), a real regression, not the
        // intended effect. Back to a fully opaque square peer; a correct rounded-window
        // implementation is a separate, harder task than this attempt turned out to be.
        auto* content = new MainComponent(lafIn);
        mainComponent = content;
        setContentOwned(content, true);
        // "default opening of the view can be bigger, it opens too small" (review round
        // 2). 85% of the display's usable area rather than a fixed size: the fixed
        // 920x720 was cramped on a 1920x1080 external screen and would be wrong again on
        // any other one. Capped so a large monitor doesn't open a wall-sized window.
        if (auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        {
            auto area = display->userBounds;
            setSize(juce::jmin(1680, juce::roundToInt(area.getWidth() * 0.85f)),
                    juce::jmin(1050, juce::roundToInt(area.getHeight() * 0.85f)));
        }
        centreWithSize(getWidth(), getHeight());
        setResizable(true, true);
        // Below this, MainComponent's fixed-height chrome rows (filter bar + status,
        // plus the bottom panel once a file's selected) alone exceed the window,
        // leaving zero or negative room for the folder tree / file table region.
        setResizeLimits(MainComponent::kFolderTreeMinWidth + 300,
                         MainComponent::kFilterBarHeight + MainComponent::kMinBottomPanelHeight
                             + MainComponent::kStatusBarHeight + 120,
                         10000, 10000);
        setVisible(true);

        // The retry the comment above asks for, done the other way round: not JUCE's
        // transparent-peer path (that's what ghosted), but a Core Animation corner
        // radius on the NSWindow's own content layer, with the window made non-opaque so
        // AppKit clips and shadows the rounded shape itself. Called after setVisible --
        // that's what creates the peer this needs.
        mira_ui::chrome::applyRoundedCorners(*this, 10.0f);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    MainComponent& getMainComponent() { return *mainComponent; }

private:
    MainComponent* mainComponent = nullptr; // owned by DocumentWindow via setContentOwned above
};

// "the osx toolbar should have setting for audio -- like output and buffer" -- a small
// utility window around JUCE's own AudioDeviceSelectorComponent, bound to WaveformView's
// real AudioDeviceManager (MainComponent::getAudioDeviceManager) rather than a second
// unconnected one. Native title bar here (unlike MainWindow's custom-painted one) --
// this is a secondary system-settings-style dialog, not primary app chrome, so the extra
// work to theme it isn't worth it the way it was for the main window.
class AudioSettingsWindow : public juce::DocumentWindow
{
public:
    explicit AudioSettingsWindow(juce::AudioDeviceManager& deviceManager)
        : DocumentWindow("Audio Settings", MiraLookAndFeel::surface, DocumentWindow::closeButton)
    {
        // 0 input channels (min and max) -- mira never records, only plays back, so no
        // input-device picker or microphone permission prompt. hideAdvancedOptionsWithButton
        // =false keeps the buffer size / sample rate controls directly visible rather than
        // behind an extra click -- "output and buffer and stuff" was the explicit ask.
        auto* selector = new juce::AudioDeviceSelectorComponent(deviceManager, 0, 0, 0, 2, false, false, true, false);
        selector->setSize(480, 420);
        setContentOwned(selector, true);
        setResizable(false, false);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
        // Same native-title-bar recolouring FileDetailsWindow gets -- both utility
        // windows kept the OS-gray bar for the rounded corners and centred title it
        // gives for free; this keeps those and matches mira's palette too.
        mira_ui::chrome::applyDarkTitleBar(*this, MiraLookAndFeel::surface2);
    }

    void closeButtonPressed() override { setVisible(false); } // kept alive, not destroyed -- reopens instantly
};

// TASKS.md Phase 5: a real macOS menu bar (MenuBarModel::setMacMainMenu), not just
// in-window buttons — "starts to feel more like a native app", matching Soundly's own
// File/Edit/... menu bar. First cut: one File menu with Add Folder..., calling the exact
// same FolderTreeView::promptAddFolder() flow the sidebar's own "+" button uses, not a
// second implementation of the folder picker.
class MiraMenuBarModel : public juce::MenuBarModel
{
public:
    std::function<void()> onAddFolder;
    // "rescan can be in the osx toolbar like rescan not in the ui, its confusing" —
    // scanning itself is automatic (MainComponent::enqueueScan, fired on Add Folder and
    // resumed automatically at launch for anything left incomplete); this menu item is
    // purely a manual "do it again" escape hatch, not scanning's normal entry point.
    std::function<void()> onRescan;
    std::function<void()> onAudioSettings;

    // The analyze pipeline's opt-in stages (TASKS.md Phase 5 leftovers: "--chords /
    // --transcribe / --recheck-tempo exist on the CLI but aren't exposed anywhere in
    // mira_ui"). A menu with real checkmarks, not a dialog: they're app-wide settings
    // that stay put between runs, and the right-click Analyze item spells out whichever
    // ones are on before it runs anything.
    std::function<AnalyzeOptions()> getAnalyzeOptions;
    std::function<void(AnalyzeOptions)> setAnalyzeOptions;

    // Review round 5: "also into proper menu so it available". The same Tags/Segments/View
    // menus the waveform's header and right-click show, in the macOS menu bar -- built by
    // the same functions, so they cannot drift apart, and dispatched through the same
    // single action id space.
    std::function<void(juce::PopupMenu&)> buildTagsMenu, buildSegmentsMenu, buildViewMenu;
    std::function<void(int)> onAction;

    juce::StringArray getMenuBarNames() override
    {
        return {"File", "Analyze", "Tags", "Segments", "View", "Window"};
    }

    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String&) override
    {
        juce::PopupMenu menu;
        if (topLevelMenuIndex == 0)
        {
            menu.addItem(1, "Add Folder...");
            menu.addItem(2, "Rescan");
        }
        else if (topLevelMenuIndex == 1)
        {
            auto options = getAnalyzeOptions ? getAnalyzeOptions() : AnalyzeOptions {};
            menu.addSectionHeader("Extra stages (slower)");
            menu.addItem(10, "Chords (Chordino)", true, options.chords);
            menu.addItem(11, "Transcription (MIDI notes)", true, options.transcribe);
            menu.addItem(12, "Recheck Tempo (Essentia cross-check)", true, options.recheckTempo);
            // The costs are the reason these are off by default, so they belong in the
            // menu rather than only in TASKS.md — measured on a 5:08 song.
            menu.addSeparator();
            menu.addItem(13, "Chords +15s/file, Transcription +4s/file", false, false);
        }
        else if (topLevelMenuIndex == 2)
        {
            if (buildTagsMenu) buildTagsMenu(menu);
        }
        else if (topLevelMenuIndex == 3)
        {
            if (buildSegmentsMenu) buildSegmentsMenu(menu);
        }
        else if (topLevelMenuIndex == 4)
        {
            if (buildViewMenu) buildViewMenu(menu);
        }
        else if (topLevelMenuIndex == 5)
        {
            // Audio Settings lives here rather than in its own one-item top-level menu now
            // that there is a Window menu to hold it and the log.
            menu.addItem(MainComponent::kShowLog, "Log...");
            menu.addSeparator();
            menu.addItem(3, "Audio Settings...");
        }
        return menu;
    }

    void menuItemSelected(int menuItemID, int) override
    {
        if (menuItemID == 1 && onAddFolder) onAddFolder();
        else if (menuItemID == 2 && onRescan) onRescan();
        else if (menuItemID == 3 && onAudioSettings) onAudioSettings();
        else if (menuItemID >= 700 && onAction) onAction(menuItemID); // Tags/Segments/View share one id space
        else if (menuItemID >= 10 && menuItemID <= 12 && getAnalyzeOptions && setAnalyzeOptions)
        {
            auto options = getAnalyzeOptions();
            if (menuItemID == 10) options.chords = !options.chords;
            else if (menuItemID == 11) options.transcribe = !options.transcribe;
            else options.recheckTempo = !options.recheckTempo;
            setAnalyzeOptions(options);
            menuItemsChanged(); // repaint the checkmarks
        }
    }
};

class MiraUiApp : public juce::JUCEApplication
{
public:
    // Uppercase per review round 2 ("should be Capital and bold even in the osx
    // toolbar"). This is the main window's title; the macOS menu-bar app name comes from
    // the bundle's PRODUCT_NAME (src/mira_ui/CMakeLists.txt), changed to match. macOS
    // already draws the app-menu name bold, so capitals are the only part mira controls.
    const juce::String getApplicationName() override { return "MIRA"; }
    const juce::String getApplicationVersion() override { return "0.1"; }

    void initialise(const juce::String&) override
    {
        // Declared before mainWindow (member order below) so it outlives every component
        // that reads its colours/fonts; shutdown() below still clears the *default* LAF
        // pointer explicitly before either is torn down, rather than relying on
        // destruction order alone for that part.
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        mainWindow = std::make_unique<MainWindow>(getApplicationName(), lookAndFeel);

        menuModel.onAddFolder = [this] { mainWindow->getMainComponent().getFolderTree().promptAddFolder(); };
        menuModel.onRescan = [this] { mainWindow->getMainComponent().rescanCurrentOrAll(); };
        menuModel.buildTagsMenu = [this](juce::PopupMenu& menu) {
            mainWindow->getMainComponent().buildTagsMenu(menu);
        };
        menuModel.buildSegmentsMenu = [this](juce::PopupMenu& menu) {
            mainWindow->getMainComponent().buildSegmentsMenu(menu);
        };
        menuModel.buildViewMenu = [this](juce::PopupMenu& menu) {
            mainWindow->getMainComponent().buildViewMenu(menu);
        };
        menuModel.onAction = [this](int actionId) { mainWindow->getMainComponent().performMenuAction(actionId); };
        mainWindow->getMainComponent().onMenuStateChanged = [this] { menuModel.menuItemsChanged(); };
        menuModel.getAnalyzeOptions = [this] { return mainWindow->getMainComponent().getAnalyzeOptions(); };
        menuModel.setAnalyzeOptions = [this](AnalyzeOptions o) {
            mainWindow->getMainComponent().setAnalyzeOptions(o);
        };
        menuModel.onAudioSettings = [this] {
            if (audioSettingsWindow == nullptr)
                audioSettingsWindow =
                    std::make_unique<AudioSettingsWindow>(mainWindow->getMainComponent().getAudioDeviceManager());
            audioSettingsWindow->setVisible(true);
            audioSettingsWindow->toFront(true);
        };
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(&menuModel);
#endif
    }

    void shutdown() override
    {
#if JUCE_MAC
        juce::MenuBarModel::setMacMainMenu(nullptr);
#endif
        audioSettingsWindow = nullptr;
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

private:
    MiraLookAndFeel lookAndFeel;
    MiraMenuBarModel menuModel;
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<AudioSettingsWindow> audioSettingsWindow;
};

START_JUCE_APPLICATION(MiraUiApp)
