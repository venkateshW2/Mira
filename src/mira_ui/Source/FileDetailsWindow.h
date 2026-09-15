#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "mira/db/Database.h"
#include "MiraLookAndFeel.h"

// "this is analysis software so we need to show the details properly without breaking
// the ui" -- a real inspect/edit view for one file: everything the analysis pipeline
// actually produced (not just the Instrument column's collapsed top-1 label), plus the
// provenance of that run (model versions, when it ran), and editable fields that write
// through Database::setHumanField into the `human` JSON column -- PRD §6/§11's existing
// machine/human split, never touching `machine` itself. Shown in a right-hand
// sidebar (double-click a row, "Details..." on the right-click menu, or Cmd+I) rather than
// crammed into the bottom panel, so a long ranked label list has real room without
// fighting the waveform for space. It was a pop-out window until review round 2.
//
// Also the batch editor: constructed with N file ids, not one. Everything below that
// says "the record" means "the one record when N == 1, or the collapsed view across all
// N otherwise" -- a field whose effective value is identical across the whole selection
// shows that value and edits it everywhere; a field that differs shows a "multiple
// values" placeholder and is left alone unless someone actually types into it. That
// last part is why FieldRow tracks `initialText`: Save writes only the fields whose text
// changed, so opening a 40-file selection and hitting Save doesn't stamp one file's
// analyzed values onto the other 39 as human overrides.
class FileDetailsContent : public juce::Component
{
public:
    // segmentIdIn != 0 puts this in segment mode: one segment of one file, showing that
    // segment's own analysis, tags and caption (review round 4: "the details should
    // change according to the segment"). BPM and key stay the file's -- per-segment
    // analysis doesn't re-measure them -- but can still be overridden for the segment.
    FileDetailsContent(mira::Database& databaseIn, const MiraLookAndFeel& lafIn, std::vector<int64_t> fileIdsIn,
                        int64_t segmentIdIn = 0);

    void resized() override;
    void paint(juce::Graphics&) override;

    void save();   // writes every changed editor back via setHumanField
    void revert(); // clearHumanFields then reload -- back to pure machine output

    int getPreferredHeight() const { return contentHeight; }

private:
    struct FieldRow
    {
        juce::Label sectionLabel;   // "Genre", "BPM", ...
        juce::Label referenceLabel; // read-only ranked machine output, small/dim
        juce::TextEditor editor;    // editable effective value (human override if set, else machine top)
        juce::String initialText;   // what reload() put in `editor` -- Save writes only if this changed
        // Closed-vocabulary rows only (groove/swing/low_end/motion). Picking from it
        // fills `editor`; the editor stays typeable, so the list is a reminder of the
        // words the model was actually trained on rather than a cage. Empty for the
        // open-ended rows (genre/instruments/moods), which have no fixed vocabulary.
        juce::ComboBox vocab;
        bool hasVocab = false;
    };

    // One file's effective (human-override-else-machine) value per editable field, plus
    // the read-only ranked "what analysis actually said" line that goes above it.
    // Computed per record so the batch path can collapse N of these into one view using
    // exactly the same derivation the single-file path shows.
    struct EffectiveFields
    {
        juce::String bpm, key, genre, instrument, mood;
        juce::String bpmRef, keyRef, genreRef, instrumentRef, moodRef;
        // The measured groove/sound-design fields. Their reference line shows the raw
        // NUMBER analysis produced (grid strength, swing percentage, sub-bass share,
        // flux) rather than re-deriving the bucket word here -- the thresholds live in
        // CaptionFields.h and must have exactly one definition, and seeing "4.60x" next
        // to "programmed" explains the word in a way repeating it cannot.
        juce::String groove, swing, lowEnd, motion;
        juce::String grooveRef, swingRef, lowEndRef, motionRef;
    };

    void reload();        // re-reads records + rebuilds every field from the DB
    void reloadCaption(); // re-renders the SA3 caption only, from the already-loaded record
    void addFieldRow(FieldRow& row, const juce::String& sectionName,
                      const juce::StringArray& vocabulary = {});
    EffectiveFields computeFields(const mira::FileRecord& r) const;
    void setRowValue(FieldRow& row, const juce::String& value, const juce::String& reference);

    bool isBatch() const { return fileIds.size() > 1; }
    bool isSegmentMode() const { return segmentId != 0 && segment.has_value(); }

    int64_t segmentId = 0;
    std::optional<mira::SegmentRecord> segment;

    mira::Database& database;
    const MiraLookAndFeel& laf;
    std::vector<int64_t> fileIds;
    std::vector<mira::FileRecord> records; // every id that still resolves, in selection order
    mira::FileRecord record;               // records.front() -- the single-file views' subject

    juce::Label titleLabel, pathLabel, metaLabel, statusLabel, provenanceLabel;

    // What mira itself made this file with. Read out of human.$.generated, which the
    // Generate window's Keep button writes -- a generated file has no analysis until
    // someone runs one, so without this the panel says "not analyzed yet" for every
    // field and shows nothing at all about a recipe that is sitting right there.
    // Read-only: it is a record of what happened, not a setting.
    juce::Label generatedLabel;
    juce::TextEditor generatedEditor;
    bool hasGenerated = false;
    void reloadGenerated();
    FieldRow bpmRow, keyRow, genreRow, instrumentRow, moodRow;
    // Editable for the same reason every row here is: a measurement is a proposal,
    // and `human` outranks it (PRD §11). Clearing one back to empty leaves the
    // measured value showing, exactly as BPM already behaves.
    FieldRow grooveRow, swingRow, lowEndRow, motionRow;

    // SA3 caption block — the rendered LoRA-training caption for this file, i.e. what the
    // whole analysis pipeline actually exists to produce. Read-only on purpose: it is a
    // *derivation* of the editable fields above (via CaptionFields' confidence gating),
    // so the way to change it is to edit those fields and Save, not to type over the
    // output. Trigger is a live render knob, deliberately not persisted — it belongs to
    // the LoRA being trained, not to the file (the CLI takes it as `--trigger` per run
    // for the same reason).
    juce::Label captionSectionLabel, captionHintLabel, triggerLabel, tagsLabel;
    juce::TextEditor triggerEditor, proseEditor, tagsEditor;
    juce::TextButton copyProseButton { "Copy Prose" }, copyJsonButton { "Copy JSON" };

    int contentHeight = 0; // computed in reload(), drives the parent Viewport's scroll range
    int headerHeight = 0;  // computed in resized(), used by paint() to tint the header block
};

// The details sidebar itself (MainComponent::detailsSidebar): a scrollable
// FileDetailsContent on top, a fixed Save/Revert/Close row pinned below it that never
// scrolls out of view. It was the content of a pop-out DocumentWindow until review
// round 2 ("the pop out is not nice"); Close now closes the sidebar.
class FileDetailsRoot : public juce::Component
{
public:
    FileDetailsRoot(mira::Database& databaseIn, const MiraLookAndFeel& lafIn, std::vector<int64_t> fileIdsIn,
                     std::function<void()> onSavedIn, std::function<void()> onCloseIn, int64_t segmentIdIn = 0);

    void resized() override;
    void paint(juce::Graphics&) override;

private:
    static constexpr int kButtonBarHeight = 44;

    juce::Viewport viewport;
    std::unique_ptr<FileDetailsContent> content;
    juce::TextButton saveButton { "Save" }, revertButton { "Revert to Analyzed" }, closeButton { "Close" };
};
