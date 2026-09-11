#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "mira/db/Database.h"
#include "MiraLookAndFeel.h"

// TASKS.md Phase 5 build-order step 4: the real paintCell-only TableListBox file list,
// replacing step 1's plain Viewport + stacked Components (fine for a 15-file test
// library, not for drive scale — TableListBox only ever creates row components for what's
// on screen, painting the rest via paintCell instead of one juce::Component per file).
// Columns match the reference mockup's table (File/BPM/Key/Loudness/Active/Type); see
// FileTable.cpp's buildRow() for exactly which machine JSON path backs each one and the
// mockup's own "—" convention for values that were never measured, not measured-as-zero.
enum FileTableColumnId
{
    ColFile = 1,
    ColBpm,
    ColKey,
    ColLoudness,
    ColActive,
    ColType,
};

class FileTableModel : public juce::TableListBoxModel
{
public:
    FileTableModel(mira::Database& databaseIn, const MiraLookAndFeel& lafIn);

    // Reloads rows from the DB. Caller must also call the owning TableListBox's
    // updateContent() afterwards — this class doesn't hold a reference to the table
    // itself, matching TableListBoxModel's usual ownership direction (table owns model).
    void rebuild();

    int getNumRows() override;
    void paintRowBackground(juce::Graphics&, int rowNumber, int width, int height, bool rowIsSelected) override;
    void paintCell(juce::Graphics&, int rowNumber, int columnId, int width, int height, bool rowIsSelected) override;

    // Drag-out (PRD §2d, spike/03_dragout, generalized in build-order step 1): the file
    // path travels as the drag description itself, not via SourceDetails::sourceComponent
    // — TableListBox's internal per-row component is JUCE-owned, not one of ours, so
    // there's nothing to dynamic_cast the way step 1's plain DraggableFileRow allowed.
    juce::var getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows) override;

    static void setupColumns(juce::TableHeaderComponent& header);

private:
    struct Row
    {
        mira::FileRecord record;
        juce::String bpmText, keyText, loudnessText, activeText;
    };
    Row buildRow(const mira::FileRecord& record) const;

    mira::Database& database;
    const MiraLookAndFeel& laf;
    std::vector<Row> rows;
};
