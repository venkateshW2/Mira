#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <set>

#include "mira/db/Database.h"
#include "MiraLookAndFeel.h"

// TASKS.md Phase 5 — file list, second design. First design queried the DB directly
// (`database.queryFiles(...)`), which meant clicking a folder that had never been
// scanned showed nothing at all — confirmed wrong in discussion: "clicking on the
// folder should show all files of the folder or subfolder even if it's not analysed...
// we show all the files... then we allow to analyse". This is a real filesystem
// listing instead (mira::hasSupportedAudioExtension, mira_core/Scanner.h's own
// audio-extension check, not a second list to keep in sync) merged with whatever DB
// row exists for each path via Database::findByPath — a file with no DB row still
// shows up, just with a "not scanned" Status and dashes everywhere else, exactly the
// same "unmeasured, not zero" discipline the CLI's machine JSON already uses.
enum FileTableColumnId
{
    ColFile = 1,
    ColStatus,
    ColBpm,
    ColKey,
    ColDuration,
    ColSampleRate,
    ColLoudness,
    ColActive,
    ColType,
    ColGenre,
    ColInstrument,
    ColMood,
};

// The split filter bar's state (review round 2: "filters should have division, not one
// box"). An empty string or nullopt means "any" for that field; every set field must
// match (AND).
struct FilterCriteria
{
    juce::String text; // free text over the name and every field, space-separated terms
    juce::String key;
    std::optional<double> bpmMin, bpmMax;
    juce::String genre, instrument, mood;

    bool isEmpty() const
    {
        return text.trim().isEmpty() && key.isEmpty() && !bpmMin && !bpmMax && genre.isEmpty()
               && instrument.isEmpty() && mood.isEmpty();
    }
};

// Distinct values actually present in the current scope, files and segments both --
// what the filter bar's dropdowns offer (FilterBar::setFacetOptions).
struct FacetOptions
{
    juce::StringArray keys, genres, instruments, moods;
};

// A file-list scope that is a mira-side collection rather than a folder on disk. The
// scope string stays one opaque token everywhere else (tabs, row-build generations, the
// status bar's folder readout), so only the two places that actually resolve it to files
// need to know the difference.
inline constexpr const char* kCollectionScopePrefix = "mira:collection:";

class FileTableModel : public juce::TableListBoxModel
{
public:
    FileTableModel(mira::Database& databaseIn, const MiraLookAndFeel& lafIn);

    // Empty path == nothing selected yet, list stays empty — no "Show All" mode
    // ("show all is confusing and dont need it"). A non-empty path scopes to that one
    // folder and its subfolders, recursively, walking the real filesystem, not the DB.
    // Caller must also call the owning TableListBox's updateContent() afterwards,
    // matching this class's existing rebuild() contract.
    void setScope(const juce::String& folderPathOrEmpty);
    void rebuild(); // re-runs the current scope (e.g. after a scan completes)
    // Rebuilds just one file's row (e.g. one file of an analyze batch just finished).
    // False when that path isn't in the current scope, so nothing on screen changed.
    bool refreshPath(const juce::String& path);

    // The filter bar's criteria (split into key / BPM range / genre / instrument / mood /
    // free text in review round 2). Filtering is a view over the already-built rows, not
    // a re-scan -- the expensive part of showing a folder is walking it and reading every
    // file's header, and changing a filter must not repeat that. Same updateContent()
    // contract as setScope.
    void setFilterCriteria(const FilterCriteria& newCriteria);
    FacetOptions getFacetOptions() const;
    int getUnfilteredRowCount() const { return static_cast<int>(allRows.size()); }

    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int rowNumber, int width, int height, bool rowIsSelected) override;
    void paintCell(juce::Graphics&, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;

    // Drag-out (PRD §2d, spike/03_dragout, generalized in build-order step 1): the file
    // path travels as the drag description itself, not via SourceDetails::sourceComponent
    // — TableListBox's internal per-row component is JUCE-owned, not one of ours, so
    // there's nothing to dynamic_cast the way step 1's plain DraggableFileRow allowed.
    juce::var getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows) override;

    // Drives the bottom combined waveform/detail panel's visibility (nil when nothing's
    // selected — hand-sketched layout, TASKS.md Phase 5: the panel isn't a permanent
    // fixed strip, it only takes up space once there's an actual file to show).
    void selectedRowsChanged(int lastRowSelected) override;
    std::function<void(const mira::FileRecord*)> onSelectionChanged;

    // Every currently *selected* row's real filesystem path — what "Analyze" (single or
    // multiple, FileTableComponent's toolbar) acts on.
    std::vector<juce::String> getSelectedPaths(const juce::SparseSet<int>& selectedRows) const;

    // Every row currently listed, selected or not — "Analyze" falls back to this (the
    // whole current tab's folder) when nothing's selected, same fallback Scan used to have.
    std::vector<juce::String> getAllPaths() const;

    // Display-row index for a file id, or -1 if that file isn't currently listed. Needed to
    // drive the table's selection from somewhere other than a click on it -- the cue view's
    // stem rows (review round 7: "i cant select a track /file so idont know the segemetn
    // names"). Returns the FILE row, never one of its segment children.
    int displayRowForFileId(int64_t fileId) const;

    // "status column should have analyse button not a separate button" + "so if i
    // analyse different files in a different folder then i dono which file is
    // analysing" — analysis state is global (not per-tab/scope), driven by
    // MainComponent's analyze queue, and consulted live by statusText/statusColour so
    // any row for a path currently queued or being analyzed reads that way regardless of
    // which folder/tab happens to be showing it right now. repaint() only (no rebuild) —
    // the underlying row data (bpm/key/etc.) hasn't changed, just how Status paints.
    void setAnalysisState(std::set<juce::String> analyzingPaths, std::set<juce::String> queuedPaths);

    // Right-click on any row — FileTableComponent builds the actual context menu
    // (Analyze Selected/Folder); the model only reports the click and preserves whatever
    // multi-selection was already active (TableListBox's own default behaviour).
    // Hover help for the child rows: "auto cue" and "auto segment" are two different kinds
    // of object in one list, and the words alone do not carry that.
    juce::String getCellTooltip(int rowNumber, int columnId) override;

    void cellClicked(int rowNumber, int columnId, const juce::MouseEvent& e) override;
    std::function<void(const juce::MouseEvent&)> onRightClicked;

    // "we could also do a clik on the file pop up dialog open which shows the details" —
    // double-click opens FileDetailsWindow for that one row; only fires when the file
    // actually has a DB id to look up (inDatabase) — a not-yet-scanned file has nothing
    // to show or edit yet.
    void cellDoubleClicked(int rowNumber, int columnId, const juce::MouseEvent& e) override;
    std::function<void(int64_t)> onRowDoubleClicked;

    // A segment child row was selected: the parent file's record plus the segment's time
    // range, so the bottom panel can load the file and select exactly that range.
    std::function<void(const mira::FileRecord&, int64_t segmentId, double, double)> onSegmentSelected;

    // Expands or collapses one file row's segment children -- the File column's
    // disclosure triangle, or the right-click menu. No-op on a row without segments.
    void toggleExpanded(int displayRow);
    bool isFileRowWithSegments(int displayRow) const;
    bool isExpanded(int displayRow) const;
    // Fired after the visible rows change without a rebuild (expand/collapse), with the
    // display row to keep selected -- the owner calls updateContent(), since the model
    // has no handle on its own TableListBox.
    std::function<void(int)> onDisplayChanged;
    // The table's header, so the triangle hit-test in cellClicked can find the File
    // column's current x (columns are user-draggable, File isn't guaranteed first).
    void setHeader(juce::TableHeaderComponent* h) { header = h; }

    static void setupColumns(juce::TableHeaderComponent& header);

private:
    // inDatabase is false for a file that exists on disk but hasn't been indexed yet
    // (mira's automatic scan hasn't reached it) — still shown, just with dashes
    // everywhere. Status only ever shows "analyzed" once record.analyzedAt is set; every
    // other state (not yet indexed, or indexed but not yet analyzed) reads the same to
    // the user — "scanned in list also not needed its should be analysed" — scanning
    // itself is invisible plumbing now (FileTableModel::statusText/statusColour).
    struct Row
    {
        mira::FileRecord record;
        bool inDatabase = false;
        juce::String bpmText, keyText, loudnessText, activeText;
        juce::String genreText, instrumentText, moodText;
        // Read straight off the file's own header via AudioFormatManager — no analysis,
        // no scan needed, so these are populated the instant a folder's clicked, same as
        // the filename-derived BPM/key guesses below. "we are not showing the time of
        // the file? the sample rate of the file? we should show all the metadata fields."
        juce::String formatText, durationText, sampleRateText;
        // The same header read as durationText, kept as a number so a folder's total
        // running time can be summed without re-parsing "4:05" back out of the string.
        // 0 means the header couldn't be read (durationText is an em dash) -- a real
        // "unknown", counted separately by getScopeSummary rather than added in as zero.
        double durationSeconds = 0.0;
        // "so now files have key mentioned in it so lets use that also... this list even
        // without scanning is sortable" — a filename-derived guess (e.g. "Break_140bpm_
        // Fmin.wav"), shown only when there's no real analyzed value yet. Never conflated
        // with real machine data — paintCell dims these, same source-honesty discipline
        // CaptionFields.cpp's human/machine distinction already follows.
        bool bpmFromFilename = false; // explicit "NNNbpm" token, in the file's own name or an ancestor folder's
        bool bpmGuessLoose = false;   // bare leading number ("DKP_70_...") — plausible but unconfirmed, dimmed further
        bool keyFromFilename = false;
        // "once i edit the details page the edit should show in the list" — a human
        // override (FileDetailsWindow's Save) beats the analyzed value; these flag which
        // ones are, so paintCell can colour them distinctly from a plain analyzed value.
        bool genreFromHuman = false;
        bool instrumentFromHuman = false;
        bool moodFromHuman = false;
        // "still the same kick and drums as electric guitars" — a last-resort filename
        // keyword override (FileTable.cpp's filenameSaysPercussion) when both instrument
        // models missed an obvious percussion file; dimmed like the BPM/key filename
        // guesses, never conflated with real model output.
        bool instrumentFromFilename = false;

        // Structured values the split filters match on (review round 2). These are
        // not the display text above: that's top-1 only, "+N"-suffixed, and sometimes a
        // dimmed guess. Filtering "Instrument: flute" has to find a stem where flute is
        // a real second candidate, not only one where it won. See buildRow for exactly
        // which values count.
        std::optional<double> bpmValue;
        juce::String keyValue;
        juce::StringArray genres, instruments, moods, keywords;

        // One per segment (auto or manual). Each has its own labels from its own
        // segment_analysis plus its own human tags; BPM and key come from the file,
        // since per-segment analysis doesn't re-run rhythm or key (main.cpp's
        // buildSegmentMachineJson).
        //
        // Also everything a segment's own child row displays (review round 3: "the list
        // should show parent file, child as segment details") -- computed once in
        // buildRow alongside the filter facets, so painting a child row never touches
        // the database.
        struct SegmentFacets
        {
            juce::StringArray genres, instruments, moods, keywords;
            int64_t id = 0;
            double startSeconds = 0.0, endSeconds = 0.0;
            bool autoCreated = false; // source = 'auto' (analysis) vs a person's segment
            bool humanTagged = false; // has its own human tags -- shown in accent, like files
            // Which KIND of thing this child row is. A segment is file-scoped -- a sample
            // inside this one stem, cut from its own silence. A cue is group-scoped -- one
            // piece of music across the whole synced set.
            //
            // Child rows are now SEGMENTS ONLY; this stays false in practice and is kept
            // because the painters and the tooltip still branch on it. Cues were listed here
            // as siblings of segments and it did not work: the user, after a fresh analysis,
            // "i really dont get it what it means - and why i need it". They were right. A
            // cue belongs to the SET, so listing it under a file meant the same cue appeared
            // once under each of fifteen stems, and every one of those rows was blank
            // (cues have no `segment_analysis`). Fifteen copies of a row with no data in it.
            bool groupScoped = false;

            // Which cue this segment falls inside -- "it should actually have which cue the
            // segment is in - like cue 1 or 12". THIS is the relationship worth showing: a
            // segment is a phrase in one stem, a cue is the section of the reel it happens
            // during, and knowing "this flute phrase is in cue 12" is what connects the file
            // list to the cue pass. 0 means no cue covers it (or none are detected yet).
            int cueNumber = 0;
            juce::String cueType; // the cue's authored type, when it has one
            juce::String loudnessText, genreText, instrumentText, moodText;
        };
        std::vector<SegmentFacets> segments;
        int matchedSegments = 0; // set by applyFilter; the File column shows it when > 0
        // Which segments this row's children show when expanded: all of them normally,
        // only the matching ones while a segment-specific filter is active. Set by applyFilter.
        std::vector<int> visibleSegments;
    };

public:
    // Building a folder's rows happens off the UI thread (review round 3: cold folder
    // opens blocked the UI 370-667 ms). RowBuildJob (Main.cpp) calls collectRows with its
    // *own* Database connection and AudioFormatManager -- collectRows and buildRow read
    // no model state at all, so that's safe while the UI keeps using the model -- then
    // hands the list back on the message thread via setRows. Row stays private; only
    // this list type travels.
    // What the current scope holds, for the status bar's folder readout. Summed from the
    // rows the table already built (each one read its own header for the Duration column),
    // so this costs nothing extra and needs no scan and no analysis -- a folder shows its
    // size and running time the moment it is clicked.
    struct ScopeSummary
    {
        int fileCount = 0;        // files in scope, before the filter bar
        double totalSeconds = 0.0;
        int unknownDurations = 0; // headers that wouldn't read -- reported, never hidden
    };
    ScopeSummary getScopeSummary() const;

    using RowList = std::vector<Row>;
    RowList collectRows(const juce::String& scope, mira::Database& db, juce::AudioFormatManager& fm,
                        const std::function<bool()>& shouldAbort) const;
    // Switches scope immediately with an empty list (the rows follow via setRows).
    void setScopeDeferred(const juce::String& folderPathOrEmpty);
    // Installs rows built for `scope`; ignored (false) if the scope has changed since.
    bool setRows(const juce::String& scope, RowList newRows);

private:
    // Thread-agnostic: everything it reads comes in through db/fm (see collectRows).
    Row buildRow(const juce::File& file, mira::Database& db, juce::AudioFormatManager& fm) const;
    // The UI thread's own convenience form -- one row, the model's own connection
    // (refreshPath uses this).
    Row buildRow(const juce::File& file) const { return buildRow(file, database, formatManager); }
    juce::String statusText(const Row& row) const;
    juce::Colour statusColour(const Row& row) const;
    void applyFilter();
    // Whether one set of labels (the file's own, or one segment's) passes every set
    // criterion. BPM and key always come from `row`, since a segment inherits both.
    bool matchesCriteria(const Row& row, const juce::StringArray& terms, const juce::StringArray& genres,
                         const juce::StringArray& instruments, const juce::StringArray& moods,
                         const juce::StringArray& keywords) const;

    mira::Database& database;
    const MiraLookAndFeel& laf;
    mutable juce::AudioFormatManager formatManager; // header-only reads for Format/Duration/Sample Rate columns (buildRow is const)
    std::vector<Row> allRows; // everything in the current scope, before the filter bar
    std::vector<Row> rows;    // what's actually listed -- allRows with the filter applied

    // What the table actually shows: each listed file, then (when expanded) its segment
    // children. Every row-number-taking method (getNumRows, paintCell, selection, drag,
    // clicks) goes through this, never through `rows` directly -- a table row number is
    // no longer a file index once children are interleaved.
    struct DisplayRow
    {
        size_t rowIndex;  // into `rows`
        int segmentIndex; // into that row's `segments`; -1 = the file row itself
    };
    std::vector<DisplayRow> display;
    std::set<juce::String> expandedPaths; // keyed by path, so expansion survives rebuilds
    juce::TableHeaderComponent* header = nullptr;
    void paintSegmentCell(juce::Graphics& g, const Row& row, const Row::SegmentFacets& segment, int columnId,
                          juce::Rectangle<int> bounds, bool rowIsSelected);
    void rebuildDisplay();
    const Row* fileRowAt(int displayRow) const;               // the file, for a file *or* segment row
    const Row::SegmentFacets* segmentAt(int displayRow) const; // null on a file row
    juce::String scopePath;
    FilterCriteria criteria;
    std::set<juce::String> analyzingPaths;
    std::set<juce::String> queuedForAnalysisPaths;
};
