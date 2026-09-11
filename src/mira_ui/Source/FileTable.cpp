#include "FileTable.h"

namespace {
juce::String formatDash(const std::optional<double>& v, const char* fmt)
{
    if (!v || *v == 0.0) return juce::CharPointer_UTF8("\xe2\x80\x94"); // em dash, mockup's .dash
    return juce::String::formatted(fmt, *v);
}
} // namespace

FileTableModel::FileTableModel(mira::Database& databaseIn, const MiraLookAndFeel& lafIn)
    : database(databaseIn), laf(lafIn)
{
    rebuild();
}

void FileTableModel::rebuild()
{
    rows.clear();
    for (const auto& f : database.queryFiles("1=1 ORDER BY scanned_at DESC"))
        rows.push_back(buildRow(f));
}

FileTableModel::Row FileTableModel::buildRow(const mira::FileRecord& record) const
{
    Row row;
    row.record = record;

    auto bpm = database.jsonExtractDouble(record.machine, "$.rhythm.beat_this_bpm");
    row.bpmText = formatDash(bpm, "%.0f");

    auto key = database.jsonExtractString(record.machine, "$.key.key");
    row.keyText = key ? juce::String(*key) : juce::CharPointer_UTF8("\xe2\x80\x94");

    auto lufs = database.jsonExtractDouble(record.machine, "$.dsp.integrated_loudness_lufs");
    row.loudnessText = lufs ? juce::String::formatted("%.1f", *lufs) : juce::CharPointer_UTF8("\xe2\x80\x94");

    // activeRatio is 0.0-1.0 (Phase 1 active-region detection — only runs for stems/
    // declared stems/files over 5 minutes, PRD §5); nullopt here genuinely means "never
    // ran", not "0% active" — same dash treatment as an unmeasured bpm/key above.
    row.activeText = record.activeRatio ? juce::String(static_cast<int>(*record.activeRatio * 100)) + "%"
                                         : juce::CharPointer_UTF8("\xe2\x80\x94");

    return row;
}

int FileTableModel::getNumRows() { return static_cast<int>(rows.size()); }

void FileTableModel::paintRowBackground(juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected)
{
    if (rowIsSelected)
        g.setColour(MiraLookAndFeel::accentSoft);
    else if (rowNumber % 2 == 1)
        g.setColour(MiraLookAndFeel::surface);
    else
        g.setColour(MiraLookAndFeel::surface.brighter(0.01f));
    g.fillRect(0, 0, width, height);
    g.setColour(MiraLookAndFeel::borderSoft);
    g.drawLine(0.0f, static_cast<float>(height - 1), static_cast<float>(width), static_cast<float>(height - 1), 1.0f);
}

void FileTableModel::paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool rowIsSelected)
{
    if (rowNumber < 0 || rowNumber >= static_cast<int>(rows.size())) return;
    const auto& row = rows[static_cast<size_t>(rowNumber)];
    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(8, 0);

    switch (columnId)
    {
        case ColFile:
            // Mockup's tr.sel td.name { color: var(--accent); } — selected rows tint
            // both the row background (paintRowBackground above) and the filename text.
            g.setColour(rowIsSelected ? MiraLookAndFeel::accent : MiraLookAndFeel::text);
            g.setFont(laf.sansRegular(12.5f));
            g.drawFittedText(juce::File(row.record.path).getFileName(), bounds, juce::Justification::centredLeft, 1);
            break;
        case ColBpm:
        case ColLoudness:
        case ColActive:
            // Mockup's .num convention: mono, right-justified, tabular-nums.
            g.setColour(MiraLookAndFeel::text);
            g.setFont(laf.monoRegular(12.0f));
            g.drawFittedText(columnId == ColBpm ? row.bpmText : columnId == ColLoudness ? row.loudnessText : row.activeText,
                              bounds, juce::Justification::centredRight, 1);
            break;
        case ColKey:
            g.setColour(MiraLookAndFeel::text);
            g.setFont(laf.sansRegular(12.0f));
            g.drawFittedText(row.keyText, bounds, juce::Justification::centredLeft, 1);
            break;
        case ColType:
            // Mockup's .ctype: mono, dim, small.
            g.setColour(MiraLookAndFeel::textDim);
            g.setFont(laf.monoRegular(11.0f));
            g.drawFittedText(row.record.contentType, bounds, juce::Justification::centredLeft, 1);
            break;
        default:
            break;
    }
}

juce::var FileTableModel::getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows)
{
    if (currentlySelectedRows.isEmpty()) return {};
    int rowNumber = currentlySelectedRows[0];
    if (rowNumber < 0 || rowNumber >= static_cast<int>(rows.size())) return {};
    return juce::String(rows[static_cast<size_t>(rowNumber)].record.path);
}

void FileTableModel::setupColumns(juce::TableHeaderComponent& header)
{
    constexpr int kNotSortable = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable
                                | juce::TableHeaderComponent::draggable | juce::TableHeaderComponent::appearsOnColumnMenu;
    header.addColumn("File", ColFile, 200, 100, -1, kNotSortable);
    header.addColumn("BPM", ColBpm, 55, 40, -1, kNotSortable);
    header.addColumn("Key", ColKey, 80, 50, -1, kNotSortable);
    header.addColumn("Loudness", ColLoudness, 70, 50, -1, kNotSortable);
    header.addColumn("Active", ColActive, 60, 40, -1, kNotSortable);
    header.addColumn("Type", ColType, 70, 50, -1, kNotSortable);
}
