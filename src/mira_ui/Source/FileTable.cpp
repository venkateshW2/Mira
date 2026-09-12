#include "FileTable.h"

#include "mira/scan/Scanner.h"

#include <algorithm>
#include <regex>

namespace {
juce::String formatDash(const std::optional<double>& v, const char* fmt)
{
    if (!v || *v == 0.0) return juce::CharPointer_UTF8("\xe2\x80\x94"); // em dash, mockup's .dash
    return juce::String::formatted(fmt, *v);
}

using Ranked = std::vector<std::pair<std::string, double>>;

// "its percussion but showing as guitars" -- the stem-tuned classifier (IRMAS, 11
// classes: cel/cla/flu/gac/gel/org/pia/sax/tru/vio/voi) has NO drums/percussion class at
// all -- structurally, not probabilistically, it can never say "drums", so a percussion
// stem is guaranteed to be force-fit into one of those 11 melodic/harmonic families. The
// full-mix model (mtg_jamendo_instrument, taxonomy/instrument-labels.yaml) DOES include
// "drums"/"percussion", and stays reasonably reliable at recognizing drums specifically
// even on isolated audio (it's a very distinctive timbre) despite being full-mix-trained
// overall. So: prefer the stem-tuned reading in general (isolated-audio-tuned beats
// full-mix-trained for everything else), EXCEPT when the full-mix model's own top guess
// is drums/percussion with real confidence -- that's the one case where deferring to the
// full-mix opinion is actually more correct, not less.
Ranked pickPrimaryInstrumentEntries(const Ranked& fullMixEntries, const Ranked& stemEntries, bool isStem)
{
    if (!isStem) return fullMixEntries;
    bool fullMixSaysPercussion = !fullMixEntries.empty()
                                  && (fullMixEntries.front().first == "drums" || fullMixEntries.front().first == "percussion")
                                  && fullMixEntries.front().second >= 0.3;
    if (!stemEntries.empty() && !fullMixSaysPercussion) return stemEntries;
    return fullMixEntries;
}

// The narrow percussion-only version of this lived here until review round 5, when
// mira::instrumentFromFilename (Scanner.h) generalised it to every instrument a stem's
// filename can name -- shared by the table, the details panel and the caption so all
// three answer the same way.

juce::String formatDuration(double seconds)
{
    int totalSeconds = static_cast<int>(seconds + 0.5);
    int h = totalSeconds / 3600;
    int m = (totalSeconds % 3600) / 60;
    int s = totalSeconds % 60;
    if (h > 0) return juce::String::formatted("%d:%02d:%02d", h, m, s);
    return juce::String::formatted("%d:%02d", m, s);
}

// Sample-library filenames (and, just as often, the folder that holds them — a pack's
// "_(WAVs)_Hi-Hat_Loops_(125BPM)" subfolder, or a "100BPM"/"120BPM" tempo-split folder)
// routinely carry their own metadata as text, well before any real analysis has touched
// the file. Deliberately conservative: only fires on an explicit "NNNbpm" token — a bare
// number elsewhere is too ambiguous (track numbers, sample counts) to guess at safely
// here. False negatives (a real tempo written in a format this doesn't recognize) are
// fine; a false positive is not.
std::optional<int> parseExplicitBpmToken(const juce::String& text)
{
    static const std::regex re(R"((\d{2,3})\s?bpm)", std::regex::icase);
    std::smatch m;
    auto s = text.toStdString();
    if (std::regex_search(s, m, re))
    {
        int bpm = std::stoi(m[1].str());
        if (bpm >= 40 && bpm <= 300) return bpm;
    }
    return std::nullopt;
}

// Looser fallback for packs that fold the tempo into a bare numeric prefix with no
// "bpm" unit at all — "DKP_70_drum_percussion_hottitude.wav" (Drum & Bass convention:
// 70 is a half-time reference for a 140 track). Genuinely ambiguous — could just as
// easily be a catalogue or velocity number — so this only fires as a last resort when
// nothing more explicit matched, and the caller must mark it as a low-confidence guess
// distinct from parseExplicitBpmToken's result ("its not the correct thing bu twecan
// try" — the user's own framing: worth showing, not worth trusting).
std::optional<int> parseLeadingNumberBpm(const juce::String& stem)
{
    // Checks only the first two tokens — the bare number itself ("70_drum...") or one
    // pack-prefix code ahead of it ("DKP_100_drum...") — not the whole filename, so a
    // length or velocity number buried later never wins. Whichever of the two is the
    // plain 2-3 digit number is the candidate; if neither is, this is too ambiguous.
    auto tokens = juce::StringArray::fromTokens(stem, " _-.", "");
    static const std::regex re(R"(^\d{2,3}$)");
    for (int i = 0; i < juce::jmin(2, tokens.size()); ++i)
    {
        auto s = tokens[i].toStdString();
        if (std::regex_match(s, re))
        {
            int bpm = std::stoi(s);
            if (bpm >= 60 && bpm <= 200) return bpm;
        }
    }
    return std::nullopt;
}

// Walks up from the file's own folder looking for an explicit "NNNbpm" token in an
// ancestor folder's name — capped depth, same "brute force is fine, don't let a
// pathological tree hang" judgment as folderHasAudioInSubtree in FolderTreeView.cpp.
std::optional<int> parseExplicitBpmFromAncestors(const juce::File& file, int maxDepth = 4)
{
    juce::File folder = file.getParentDirectory();
    for (int i = 0; i < maxDepth && folder.exists() && folder.getFileName().isNotEmpty(); ++i)
    {
        if (auto bpm = parseExplicitBpmToken(folder.getFileName())) return bpm;
        folder = folder.getParentDirectory();
    }
    return std::nullopt;
}

std::optional<juce::String> parseKeyFromFilename(const juce::String& stem)
{
    static const std::regex re(R"(^([A-Ga-g])([#b]?)(maj|min|m)$)", std::regex::icase);
    for (const auto& token : juce::StringArray::fromTokens(stem, " _-.", ""))
    {
        std::smatch m;
        auto s = token.toStdString();
        if (std::regex_match(s, m, re))
        {
            juce::String note = juce::String(m[1].str()).toUpperCase();
            juce::String accidental(m[2].str());
            juce::String quality = juce::String(m[3].str()).toLowerCase();
            juce::String scale = (quality == "min" || quality == "m") ? "min" : "maj";
            return note + accidental + " " + scale;
        }
    }
    return std::nullopt;
}
} // namespace

FileTableModel::FileTableModel(mira::Database& databaseIn, const MiraLookAndFeel& lafIn)
    : database(databaseIn), laf(lafIn)
{
    formatManager.registerBasicFormats(); // WAV/AIFF/FLAC/MP3/OGG readers — header-only reads below, no decoding
    rebuild();
}

void FileTableModel::setScope(const juce::String& folderPathOrEmpty)
{
    scopePath = folderPathOrEmpty;
    rebuild();
}

void FileTableModel::setScopeDeferred(const juce::String& folderPathOrEmpty)
{
    scopePath = folderPathOrEmpty;
    allRows.clear();
    applyFilter(); // empties rows + display; paint() shows "Loading..." meanwhile
}

FileTableModel::ScopeSummary FileTableModel::getScopeSummary() const
{
    // Over allRows, not rows: this describes the folder, not the filter. The filter bar
    // has its own "3 of 412" readout for the other question.
    ScopeSummary summary;
    summary.fileCount = static_cast<int>(allRows.size());
    for (const auto& row : allRows)
    {
        if (row.durationSeconds > 0.0) summary.totalSeconds += row.durationSeconds;
        else ++summary.unknownDurations;
    }
    return summary;
}

bool FileTableModel::setRows(const juce::String& scope, RowList newRows)
{
    // A build that finished after the user moved to another folder is stale -- the
    // component's generation check normally catches this first; this is the backstop.
    if (scope != scopePath) return false;
    allRows = std::move(newRows);
    applyFilter();
    return true;
}

// The synchronous path, on the UI thread's own connection -- only used where the scope is
// empty or a caller genuinely needs the rows immediately (construction). Folder clicks
// and refreshes go through RowBuildJob -> collectRows -> setRows instead (review round 3).
void FileTableModel::rebuild()
{
    allRows = collectRows(scopePath, database, formatManager, nullptr);
    applyFilter();
}

FileTableModel::RowList FileTableModel::collectRows(const juce::String& scope, mira::Database& db,
                                                     juce::AudioFormatManager& fm,
                                                     const std::function<bool()>& shouldAbort) const
{
    RowList collected;

    // No "Show All" any more — "show all is confusing and dont need it". An empty scope
    // means nothing's been picked in the sidebar yet, so the list stays empty rather
    // than unioning every added root; clicking a root folder already shows everything
    // under it (TASKS.md Phase 5 discussion: the file list is a filesystem view, the DB
    // is just a status overlay on top of it), which is all Show All ever added.
    if (scope.isEmpty()) return collected;
    std::vector<juce::File> roots { juce::File(scope) };

    for (const auto& root : roots)
    {
        if (!root.isDirectory()) continue;
        for (const auto& entry : juce::RangedDirectoryIterator(root, true, "*", juce::File::findFiles))
        {
            auto file = entry.getFile();
            auto pathStd = file.getFullPathName().toStdString();
            if (!mira::hasSupportedAudioExtension(pathStd)) continue;
            if (mira::isAppleDoubleSidecar(pathStd)) continue;
            // Checked per file: a superseded build (the user already clicked another
            // folder) stops within one file's header read, not after the whole folder.
            if (shouldAbort && shouldAbort()) return {};
            collected.push_back(buildRow(file, db, fm));
        }
    }

    std::sort(collected.begin(), collected.end(), [](const Row& a, const Row& b) {
        return juce::File(a.record.path).getFileName().compareIgnoreCase(juce::File(b.record.path).getFileName())
             < 0;
    });
    return collected;
}

bool FileTableModel::refreshPath(const juce::String& path)
{
    // Review round 3: every file an analyze batch finished used to trigger rebuild() --
    // re-walking the folder and re-reading every file's header and DB row, ~165 ms on a
    // 420-file folder, once per finished file, on the UI thread. One row is all that
    // actually changed.
    for (auto& row : allRows)
    {
        if (juce::String(row.record.path) != path) continue;
        row = buildRow(juce::File(path));
        applyFilter();
        return true;
    }
    return false;
}

void FileTableModel::setFilterCriteria(const FilterCriteria& newCriteria)
{
    criteria = newCriteria;
    applyFilter();
}

FacetOptions FileTableModel::getFacetOptions() const
{
    FacetOptions options;
    for (const auto& row : allRows)
    {
        if (row.keyValue.isNotEmpty()) options.keys.addIfNotAlreadyThere(row.keyValue, true);
        options.genres.mergeArray(row.genres, true);
        options.instruments.mergeArray(row.instruments, true);
        options.moods.mergeArray(row.moods, true);
        for (const auto& segment : row.segments)
        {
            options.genres.mergeArray(segment.genres, true);
            options.instruments.mergeArray(segment.instruments, true);
            options.moods.mergeArray(segment.moods, true);
        }
    }
    for (auto* values : { &options.keys, &options.genres, &options.instruments, &options.moods })
    {
        values->removeEmptyStrings(); // a ComboBox item can't have empty text
        values->sortNatural();
    }
    return options;
}

void FileTableModel::applyFilter()
{
    if (criteria.isEmpty())
    {
        rows = allRows;
        for (auto& row : rows)
        {
            row.visibleSegments.clear();
            for (int s = 0; s < static_cast<int>(row.segments.size()); ++s) row.visibleSegments.push_back(s);
        }
        rebuildDisplay();
        return;
    }

    // Space-separated terms, all of which must match somewhere -- "kick 140" narrows to
    // kicks at 140bpm rather than matching either one. Substring, not prefix: the useful
    // query on a sample library is usually a fragment in the middle of a name ("snare"
    // in "BOS_FT_snare_01.wav").
    auto terms = juce::StringArray::fromTokens(criteria.text.trim().toLowerCase(), " ", "");
    terms.removeEmptyStrings();

    // Segments only count as their own matches when a criterion can actually tell them
    // apart from their file: text, genre, instrument or mood. BPM and key are inherited,
    // so for a BPM-only filter every segment of a matching stem would "match" -- a count
    // that says nothing.
    bool segmentSpecific = !terms.isEmpty() || criteria.genre.isNotEmpty() || criteria.instrument.isNotEmpty()
                           || criteria.mood.isNotEmpty();

    // A file is listed if it matches itself *or* through any of its segments ("if I
    // search a bpm this segment should also show", review round 2). A 37-minute score
    // stem whose one averaged label is wrong still turns up when a segment of it is
    // what's being searched for.
    rows.clear();
    for (const auto& row : allRows)
    {
        bool fileMatches = matchesCriteria(row, terms, row.genres, row.instruments, row.moods, row.keywords);
        std::vector<int> matching;
        if (segmentSpecific)
            for (int s = 0; s < static_cast<int>(row.segments.size()); ++s)
            {
                const auto& segment = row.segments[static_cast<size_t>(s)];
                if (matchesCriteria(row, terms, segment.genres, segment.instruments, segment.moods, segment.keywords))
                    matching.push_back(s);
            }

        if (fileMatches || !matching.empty())
        {
            rows.push_back(row);
            auto& listed = rows.back();
            listed.matchedSegments = static_cast<int>(matching.size());
            // Matching segments only while a segment-specific filter is on (those rows
            // are *why* the file is listed); every segment otherwise.
            if (!matching.empty()) listed.visibleSegments = matching;
            else
            {
                listed.visibleSegments.clear();
                for (int s = 0; s < static_cast<int>(listed.segments.size()); ++s) listed.visibleSegments.push_back(s);
            }
        }
    }
    rebuildDisplay();
}

void FileTableModel::rebuildDisplay()
{
    display.clear();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        display.push_back({ i, -1 });
        const auto& row = rows[i];
        // A file whose segments matched the filter opens itself -- those child rows are
        // the actual result; hiding them behind a closed triangle would hide the answer.
        bool expanded = expandedPaths.count(juce::String(row.record.path)) > 0 || row.matchedSegments > 0;
        if (!expanded) continue;
        for (int s : row.visibleSegments) display.push_back({ i, s });
    }
}

int FileTableModel::displayRowForFileId(int64_t fileId) const
{
    for (size_t i = 0; i < display.size(); ++i)
    {
        // Segment children carry their parent's rowIndex, so skipping them is what keeps
        // this from selecting a child row that happens to belong to the right file.
        if (display[i].segmentIndex >= 0) continue;
        if (rows[display[i].rowIndex].record.id == fileId) return static_cast<int>(i);
    }
    return -1;
}

juce::String FileTableModel::getCellTooltip(int rowNumber, int columnId)
{
    juce::ignoreUnused(columnId);
    const auto* segment = segmentAt(rowNumber);
    if (segment == nullptr) return {};

    juce::String text;
    text << "SEGMENT - a range inside this one file, cut from its own silences.\n"
         << (segment->autoCreated ? "\"auto\" = made by analysis, so a re-run may replace it.\n"
                                  : "Made or edited by you, so a re-run never touches it.\n");

    if (segment->cueNumber > 0)
        text << "\nIt plays during CUE " << segment->cueNumber
             << (segment->cueType.isNotEmpty() ? " (" + segment->cueType + ")" : juce::String())
             << " - the section of the whole synced set that covers this moment.\n"
             << "Cues span every stem; this segment is only in this file.";
    else
        text << "\nNot inside any cue - either none are detected for this set yet, "
                "or it falls in a gap between them.";

    return text;
}

const FileTableModel::Row* FileTableModel::fileRowAt(int displayRow) const
{
    if (displayRow < 0 || displayRow >= static_cast<int>(display.size())) return nullptr;
    return &rows[display[static_cast<size_t>(displayRow)].rowIndex];
}

const FileTableModel::Row::SegmentFacets* FileTableModel::segmentAt(int displayRow) const
{
    if (displayRow < 0 || displayRow >= static_cast<int>(display.size())) return nullptr;
    const auto& d = display[static_cast<size_t>(displayRow)];
    if (d.segmentIndex < 0) return nullptr;
    return &rows[d.rowIndex].segments[static_cast<size_t>(d.segmentIndex)];
}

bool FileTableModel::isFileRowWithSegments(int displayRow) const
{
    const auto* row = fileRowAt(displayRow);
    return row != nullptr && segmentAt(displayRow) == nullptr && !row->segments.empty();
}

bool FileTableModel::isExpanded(int displayRow) const
{
    const auto* row = fileRowAt(displayRow);
    return row != nullptr
           && (expandedPaths.count(juce::String(row->record.path)) > 0 || row->matchedSegments > 0);
}

void FileTableModel::toggleExpanded(int displayRow)
{
    if (!isFileRowWithSegments(displayRow)) return;
    auto path = juce::String(fileRowAt(displayRow)->record.path);
    if (expandedPaths.count(path) > 0) expandedPaths.erase(path);
    else expandedPaths.insert(path);
    rebuildDisplay();
    // The toggled file row's index doesn't move (only rows *below* it change), so
    // keeping it selected is just keeping this same index.
    if (onDisplayChanged) onDisplayChanged(displayRow);
}

bool FileTableModel::matchesCriteria(const Row& row, const juce::StringArray& terms, const juce::StringArray& genres,
                                     const juce::StringArray& instruments, const juce::StringArray& moods,
                                     const juce::StringArray& keywords) const
{
    if (criteria.bpmMin || criteria.bpmMax)
    {
        // No usable BPM means "unknown", which can't be shown to be inside a range. It
        // doesn't pass: a range filter that let every unanalyzed file through would be
        // useless on a mostly-unanalyzed library.
        if (!row.bpmValue) return false;
        if (criteria.bpmMin && *row.bpmValue < *criteria.bpmMin) return false;
        if (criteria.bpmMax && *row.bpmValue > *criteria.bpmMax) return false;
    }
    if (criteria.key.isNotEmpty() && !row.keyValue.equalsIgnoreCase(criteria.key)) return false;
    if (criteria.genre.isNotEmpty() && !genres.contains(criteria.genre, true)) return false;
    if (criteria.instrument.isNotEmpty() && !instruments.contains(criteria.instrument, true)) return false;
    if (criteria.mood.isNotEmpty() && !moods.contains(criteria.mood, true)) return false;

    if (!terms.isEmpty())
    {
        // Full label sets, not the columns' top-1 "+N" text, and keywords too (the one
        // field with no column at all), so free text finds anything the row really has.
        juce::String haystack = juce::File(row.record.path).getFileName();
        haystack << " " << row.bpmText << " " << row.keyText << " " << genres.joinIntoString(" ") << " "
                 << instruments.joinIntoString(" ") << " " << moods.joinIntoString(" ") << " "
                 << keywords.joinIntoString(" ") << " " << row.formatText << " "
                 << juce::String(row.record.contentType) << " " << statusText(row);
        haystack = haystack.toLowerCase();
        for (const auto& term : terms)
            if (!haystack.contains(term)) return false;
    }
    return true;
}

namespace {
// Same bar CaptionFields.cpp's confidence gates use (kCaptionInstrumentThreshold etc.)
// -- a label counts as something this file "has" for filtering at the same confidence
// at which mira would put it in a caption.
constexpr double kFacetMinScore = 0.10;

// Width reserved at the left of the File column for the segment disclosure triangle --
// on every row, with or without segments, so file names stay aligned down the list.
constexpr int kDisclosureWidth = 14;

// A segment's instruments, with the same drums carve-out the file rows get
// (pickPrimaryInstrumentEntries). Without it a drum stem's segments read "electric
// guitar" under a file that correctly reads "drums": IRMAS has no drums or percussion
// class at all, so it can only answer with a wrong melodic instrument. Measured on
// DRUMS_1.wav (id 566): its segments scored electric guitar 0.27/0.30 on the stem model
// while the full-mix model said drums 0.37/0.38.
juce::StringArray segmentInstrumentLabels(mira::Database& db, const std::string& machineJson, bool isStem,
                                           const std::string& path)
{
    auto fullMix = db.jsonObjectEntries(machineJson, "$.instrument_normalized", 8);
    if (fullMix.empty()) fullMix = db.jsonObjectEntries(machineJson, "$.instrument", 8);
    Ranked stem;
    if (isStem)
    {
        stem = db.jsonObjectEntries(machineJson, "$.stem_instrument_normalized", 8);
        if (stem.empty()) stem = db.jsonObjectEntries(machineJson, "$.stem_instrument.scores", 8);
    }
    auto primary = pickPrimaryInstrumentEntries(fullMix, stem, isStem);

    // A stem should read as essentially one instrument, so it needs real confidence
    // before a second label joins; a mix/track lists everything it found. The parent
    // file's name leads here too -- a segment of "BRASS_1.wav" is still brass.
    double floor = isStem ? 0.30 : kFacetMinScore;
    juce::StringArray labels;
    if (isStem)
        if (auto hint = mira::instrumentFromFilename(path)) labels.add(juce::String(*hint));
    for (size_t i = 0; i < primary.size(); ++i)
        if (primary[i].second >= floor || labels.isEmpty())
            labels.addIfNotAlreadyThere(juce::String(primary[i].first), true);
    return labels;
}

// "top +N", the same compact form the file columns use for a multi-label field.
juce::String topPlusCount(const juce::StringArray& labels)
{
    if (labels.isEmpty()) return juce::String(juce::CharPointer_UTF8("\xe2\x80\x94"));
    return labels[0] + (labels.size() > 1 ? " +" + juce::String(labels.size() - 1) : juce::String());
}

// Human tags win outright, same rule as every column. Otherwise the first machine path
// that has data, keeping every label at or above kFacetMinScore -- plus always the top
// one, so a file whose best guess sits under the bar still has *something* to filter by,
// the same label its column already shows.
juce::StringArray facetLabels(mira::Database& db, const std::vector<std::string>& human,
                               const std::string& machineJson, std::initializer_list<const char*> paths)
{
    juce::StringArray out;
    if (!human.empty())
    {
        for (const auto& value : human) out.addIfNotAlreadyThere(juce::String(value), true);
        return out;
    }
    for (const char* path : paths)
    {
        auto entries = db.jsonObjectEntries(machineJson, path, 12);
        if (entries.empty()) continue;
        for (size_t i = 0; i < entries.size(); ++i)
            if (i == 0 || entries[i].second >= kFacetMinScore)
                out.addIfNotAlreadyThere(juce::String(entries[i].first), true);
        break;
    }
    return out;
}
} // namespace

FileTableModel::Row FileTableModel::buildRow(const juce::File& file, mira::Database& db, juce::AudioFormatManager& fm) const
{
    Row row;
    auto existing = db.findByPath(file.getFullPathName().toStdString());
    if (existing)
    {
        row.record = *existing;
        row.inDatabase = true;
    }
    else
    {
        row.record.path = file.getFullPathName().toStdString();
        row.record.contentType = "unknown";
        row.inDatabase = false;
    }

    auto stem = file.getFileNameWithoutExtension();

    // Format/Duration/Sample Rate: read straight off the file's own header, no decode,
    // no scan — this is what makes the list "sortable even without scanning" for these
    // three fields specifically (BPM/key/loudness/active genuinely need real analysis;
    // these don't, the container already carries them).
    row.formatText = file.getFileExtension().trimCharactersAtStart(".").toUpperCase();
    if (row.formatText.isEmpty()) row.formatText = juce::CharPointer_UTF8("\xe2\x80\x94");
    row.durationText = juce::CharPointer_UTF8("\xe2\x80\x94");
    row.sampleRateText = juce::CharPointer_UTF8("\xe2\x80\x94");
    if (std::unique_ptr<juce::AudioFormatReader> reader(fm.createReaderFor(file)); reader != nullptr)
    {
        if (reader->sampleRate > 0.0)
        {
            row.durationSeconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
            row.durationText = formatDuration(row.durationSeconds);
            row.sampleRateText = juce::String(reader->sampleRate / 1000.0, 1) + "k";
        }
    }

    // "once i edit the details page the edit should show in the list" — human override
    // wins over the analyzed value here too, same PRD §11 "human always wins" rule
    // FileDetailsWindow's own effective-value logic and CaptionFields.cpp both already
    // follow; this table just hadn't been taught to look at `human` at all until now.
    auto humanBpm = db.jsonExtractDouble(row.record.human, "$.bpm");
    auto bpm = humanBpm ? humanBpm : db.jsonExtractDouble(row.record.machine, "$.rhythm.beat_this_bpm");
    if (bpm)
    {
        row.bpmText = formatDash(bpm, "%.0f");
    }
    else if (auto explicitBpm = parseExplicitBpmToken(stem))
    {
        row.bpmText = juce::String(*explicitBpm);
        row.bpmFromFilename = true;
    }
    else if (auto folderBpm = parseExplicitBpmFromAncestors(file))
    {
        row.bpmText = juce::String(*folderBpm);
        row.bpmFromFilename = true;
    }
    else if (auto looseBpm = parseLeadingNumberBpm(stem))
    {
        row.bpmText = juce::String(*looseBpm) + "?";
        row.bpmGuessLoose = true;
    }
    else
    {
        row.bpmText = juce::CharPointer_UTF8("\xe2\x80\x94");
    }

    auto humanKey = db.jsonExtractString(row.record.human, "$.key");
    auto key = humanKey ? humanKey : db.jsonExtractString(row.record.machine, "$.key.key");
    if (key)
    {
        row.keyText = juce::String(*key);
    }
    else if (auto guessed = parseKeyFromFilename(stem))
    {
        row.keyText = *guessed;
        row.keyFromFilename = true;
    }
    else
    {
        row.keyText = juce::CharPointer_UTF8("\xe2\x80\x94");
    }

    auto lufs = db.jsonExtractDouble(row.record.machine, "$.dsp.integrated_loudness_lufs");
    row.loudnessText = lufs ? juce::String::formatted("%.1f", *lufs) : juce::CharPointer_UTF8("\xe2\x80\x94");

    // activeRatio is 0.0-1.0 (Phase 1 active-region detection — only runs for stems/
    // declared stems/files over 5 minutes, PRD §5); nullopt here genuinely means "never
    // ran" (or never analyzed at all), not "0% active" — same dash treatment as an
    // unmeasured bpm/key above.
    row.activeText = row.record.activeRatio ? juce::String(static_cast<int>(*row.record.activeRatio * 100)) + "%"
                                             : juce::CharPointer_UTF8("\xe2\x80\x94");

    // Human override (edited in FileDetailsWindow) beats the analyzed top label, same as
    // BPM/Key above; otherwise top-1 of the ranked distribution, with a "+N" suffix when
    // there's more than one real candidate — "the instrument should have the top name
    // and + or something showing there is more in it" (the full ranked list itself only
    // shows in the details window; the table just signals there's more to see).
    auto humanGenre = db.jsonStringArray(row.record.human, "$.genre");
    if (!humanGenre.empty())
    {
        row.genreText = juce::String(humanGenre.front());
        row.genreFromHuman = true;
    }
    else
    {
        auto genreEntries = db.jsonObjectEntries(row.record.machine, "$.genre_normalized", 6);
        if (genreEntries.empty()) genreEntries = db.jsonObjectEntries(row.record.machine, "$.genre", 6);
        row.genreText = genreEntries.empty() ? juce::CharPointer_UTF8("\xe2\x80\x94")
                                              : juce::String(genreEntries.front().first)
                                                    + (genreEntries.size() > 1
                                                           ? " +" + juce::String(genreEntries.size() - 1)
                                                           : juce::String());
    }

    // "there are two algorithms, one for individual files, that should kick in for
    // stems' single instruments" — the stem-tuned opinion ($.stem_instrument_normalized /
    // .stem_instrument.scores, NOT the raw "$.stem_instrument" object — that one nests
    // scores under a window_count wrapper) is generally preferred for a stem, with a
    // drums/percussion exception (pickPrimaryInstrumentEntries above).
    auto humanInstrument = db.jsonStringArray(row.record.human, "$.instruments");
    if (!humanInstrument.empty())
    {
        row.instrumentText = juce::String(humanInstrument.front());
        row.instrumentFromHuman = true;
    }
    else
    {
        auto fullMixEntries = db.jsonObjectEntries(row.record.machine, "$.instrument_normalized", 6);
        if (fullMixEntries.empty()) fullMixEntries = db.jsonObjectEntries(row.record.machine, "$.instrument", 6);
        Ranked stemEntries;
        bool isStem = row.record.contentType == "stem";
        if (isStem)
        {
            stemEntries = db.jsonObjectEntries(row.record.machine, "$.stem_instrument_normalized", 6);
            if (stemEntries.empty()) stemEntries = db.jsonObjectEntries(row.record.machine, "$.stem_instrument.scores", 6);
        }
        auto instrumentEntries = pickPrimaryInstrumentEntries(fullMixEntries, stemEntries, isStem);

        // A stem's filename leads (review round 5: "in case of stems the name of the file
        // defines a lot of facts"). Whoever bounced "BRASS_1.wav" knew what was in it,
        // while both models are unreliable on isolated audio -- the stem-tuned one has no
        // drums/bass/brass-section class at all, and the full-mix one scored
        // "synthesizer 0.44" on a vocal stem. Dimmed like the BPM/key filename guesses,
        // never presented as analysis; the models then add to it.
        auto filenameHint = row.record.contentType == "stem"
                                 ? mira::instrumentFromFilename(row.record.path)
                                 : std::optional<std::string> {};
        juce::StringArray labels;
        if (filenameHint)
        {
            labels.add(juce::String(*filenameHint));
            row.instrumentFromFilename = true;
        }
        // "for music tracks instrument field should have all the details" — a full mix
        // genuinely contains several real instruments at once, so a track lists every
        // candidate above 10%; a stem should read as essentially one, so a second label
        // there needs real confidence.
        double labelFloor = row.record.contentType == "track" ? 0.10 : 0.30;
        for (const auto& [label, score] : instrumentEntries)
            if (score >= labelFloor || labels.isEmpty())
                labels.addIfNotAlreadyThere(juce::String(label), true);
        row.instrumentText =
            labels.isEmpty() ? juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")) : labels.joinIntoString(", ");
    }

    auto humanMood = db.jsonStringArray(row.record.human, "$.moods");
    if (!humanMood.empty())
    {
        row.moodText = juce::String(humanMood.front());
        row.moodFromHuman = true;
    }
    else
    {
        auto moodEntries = db.jsonObjectEntries(row.record.machine, "$.moodtheme", 6);
        row.moodText = moodEntries.empty() ? juce::CharPointer_UTF8("\xe2\x80\x94")
                                            : juce::String(moodEntries.front().first)
                                                  + (moodEntries.size() > 1
                                                         ? " +" + juce::String(moodEntries.size() - 1)
                                                         : juce::String());
    }

    // --- Structured values for the split filters (review round 2) ---
    // BPM: analyzed/human, or an explicit "NNNbpm" token. The loose "70?" guess is left
    // out on purpose: a bare leading number is as often a track number ("5. HHB VOX")
    // as a tempo, so letting it pass a BPM range would put the wrong files in results.
    if (bpm) row.bpmValue = *bpm;
    else if (row.bpmFromFilename) row.bpmValue = row.bpmText.getDoubleValue();
    if (key) row.keyValue = juce::String(*key);
    else if (row.keyFromFilename) row.keyValue = row.keyText;

    row.genres = facetLabels(db, humanGenre, row.record.machine, { "$.genre_normalized", "$.genre" });
    row.moods = facetLabels(db, humanMood, row.record.machine, { "$.moodtheme" });
    row.instruments = facetLabels(db, humanInstrument, row.record.machine,
                                  { "$.instrument_normalized", "$.instrument" });
    if (humanInstrument.empty())
    {
        // Both models' opinions for a stem, not just the one the column picked: a
        // filter is a question about what's in the file, and the stem model and the
        // full-mix model disagreeing is exactly when the second opinion matters.
        if (row.record.contentType == "stem")
            row.instruments.mergeArray(facetLabels(db, std::vector<std::string> {}, row.record.machine,
                                                   { "$.stem_instrument_normalized", "$.stem_instrument.scores" }),
                                       true);
        // The filename's own label, so a stem is findable by the name it was delivered
        // under even when neither model agrees. This used to add a hard-coded "drums"
        // whenever *any* hint fired -- correct back when the hint was percussion-only,
        // but wrong since review round 5 generalised it: it made every named stem
        // (BRASSS, DBCELLO, STRINGS...) come back under an Instrument: drums filter.
        if (row.record.contentType == "stem")
            if (auto hint = mira::instrumentFromFilename(row.record.path))
                row.instruments.addIfNotAlreadyThere(juce::String(*hint), true);
    }
    for (const auto& keyword : db.jsonStringArray(row.record.human, "$.keywords"))
        row.keywords.add(juce::String(keyword));

    // Segments: file-scoped ones plus any covering this file's synced stem group.
    if (row.inDatabase)
    {
        // SEGMENTS ONLY. Cues used to be appended here as sibling child rows and it was a
        // mistake: a cue is group-scoped, so the same cue appeared under each of fifteen
        // stems, and every one of those rows was blank because cues carry no
        // `segment_analysis`. The relationship that IS worth showing is which cue each
        // segment falls in, which is what `cues` below is for.
        auto segments = db.findSegmentsForFile(row.record.id);

        std::vector<mira::SegmentRecord> cues;
        if (row.record.groupId)
        {
            cues = db.findSegmentsForGroup(*row.record.groupId);
            std::sort(cues.begin(), cues.end(),
                      [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
        }

        for (const auto& segment : segments)
        {
            Row::SegmentFacets facets;

            // Matched on the segment's MIDPOINT, not its start: a segment that begins a
            // moment before a cue boundary belongs to the cue it spends its length in, not
            // to the one it clips the last second of.
            double midpoint = (segment.startSeconds + segment.endSeconds) * 0.5;
            for (size_t c = 0; c < cues.size(); ++c)
                if (midpoint >= cues[c].startSeconds && midpoint < cues[c].endSeconds)
                {
                    facets.cueNumber = static_cast<int>(c) + 1;
                    if (auto type = db.jsonExtractString(cues[c].human, "$.cue_type"))
                        facets.cueType = juce::String(*type);
                    break;
                }
            auto machine = db.getSegmentMachine(segment.id, row.record.id).value_or("{}");
            facets.genres = facetLabels(db, db.jsonStringArray(segment.human, "$.genre"), machine,
                                        { "$.genre_normalized", "$.genre" });
            // Human tags win; otherwise the same two-model pick the file row makes,
            // including the drums carve-out (segmentInstrumentLabels).
            auto humanSegmentInstruments = db.jsonStringArray(segment.human, "$.instruments");
            if (!humanSegmentInstruments.empty())
                for (const auto& value : humanSegmentInstruments)
                    facets.instruments.addIfNotAlreadyThere(juce::String(value), true);
            else
                facets.instruments =
                    segmentInstrumentLabels(db, machine, row.record.contentType == "stem", row.record.path);
            facets.moods = facetLabels(db, db.jsonStringArray(segment.human, "$.moods"), machine,
                                       { "$.moodtheme" });
            for (const auto& keyword : db.jsonStringArray(segment.human, "$.keywords"))
                facets.keywords.add(juce::String(keyword));
            // What the segment's child row shows. Human tags win per field, same as
            // the file columns (facetLabels already applies that, so the arrays above
            // are the effective values).
            facets.id = segment.id;
            facets.startSeconds = segment.startSeconds;
            facets.endSeconds = segment.endSeconds;
            facets.autoCreated = segment.source == "auto";
            facets.humanTagged = segment.human != "{}";
            auto segmentLufs = db.jsonExtractDouble(machine, "$.dsp.integrated_loudness_lufs");
            facets.loudnessText = segmentLufs ? juce::String::formatted("%.1f", *segmentLufs)
                                              : juce::String(juce::CharPointer_UTF8("\xe2\x80\x94"));
            facets.genreText = topPlusCount(facets.genres);
            facets.instrumentText = topPlusCount(facets.instruments);
            facets.moodText = topPlusCount(facets.moods);
            row.segments.push_back(std::move(facets));
        }
        // Timeline order -- file-scoped and group-scoped segments arrive as two
        // separately ordered lists.
        std::sort(row.segments.begin(), row.segments.end(),
                  [](const auto& a, const auto& b) { return a.startSeconds < b.startSeconds; });
    }

    return row;
}

// "scanned in list also not needed its should be analysed" — scanning is now automatic
// plumbing (MainComponent's scan orchestrator), not a user-facing concept, so the only
// state worth showing here is whether the real analysis pipeline has actually run.
// "not scanned yet" and "scanned but not analyzed" collapse into the same dash — the
// distinction was only ever meaningful back when Scan was a manual, visible step.
//
// "so if i analyse different files in a different folder then i dono which file is
// analysing" — analyzing/queuedForAnalysisPaths (set externally, see setAnalysisState)
// take priority over the plain analyzed/not-analyzed read: a file mid-analysis or
// waiting behind another folder's batch shows that state here, live, regardless of
// which folder happens to be the currently displayed scope.
juce::String FileTableModel::statusText(const Row& row) const
{
    auto path = juce::String(row.record.path);
    if (analyzingPaths.count(path)) return juce::String(juce::CharPointer_UTF8("analyzing\xe2\x80\xa6"));
    if (queuedForAnalysisPaths.count(path)) return "queued";
    if (row.inDatabase && row.record.analyzedAt) return "analyzed";
    return juce::CharPointer_UTF8("\xe2\x80\x94");
}

juce::Colour FileTableModel::statusColour(const Row& row) const
{
    auto path = juce::String(row.record.path);
    if (analyzingPaths.count(path)) return MiraLookAndFeel::accent;
    if (queuedForAnalysisPaths.count(path)) return MiraLookAndFeel::textDim;
    return (row.inDatabase && row.record.analyzedAt) ? MiraLookAndFeel::good : MiraLookAndFeel::textFaint;
}

void FileTableModel::setAnalysisState(std::set<juce::String> newAnalyzing, std::set<juce::String> newQueued)
{
    analyzingPaths = std::move(newAnalyzing);
    queuedForAnalysisPaths = std::move(newQueued);
}

void FileTableModel::cellClicked(int rowNumber, int columnId, const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        if (onRightClicked) onRightClicked(e);
        return;
    }
    // Disclosure-triangle hit-test. JUCE delivers this event relative to the whole row
    // (TableListBox's RowComp), so the File column's own x comes from the header.
    // Columns are draggable, so File isn't guaranteed to be first.
    if (columnId == ColFile && header != nullptr && isFileRowWithSegments(rowNumber))
    {
        auto column = header->getColumnPosition(header->getIndexOfColumnId(ColFile, true));
        auto x = e.x - column.getX();
        constexpr int kCellMargin = 8; // paintCell's bounds.reduced(8, 0)
        if (x >= 0 && x < kCellMargin + kDisclosureWidth + 4) toggleExpanded(rowNumber);
    }
}

void FileTableModel::cellDoubleClicked(int rowNumber, int /*columnId*/, const juce::MouseEvent&)
{
    // A segment row opens its parent file's details -- segment tags are edited from the
    // waveform band's own menu, not the file details sidebar.
    const auto* row = fileRowAt(rowNumber);
    if (row != nullptr && row->inDatabase && onRowDoubleClicked) onRowDoubleClicked(row->record.id);
}

int FileTableModel::getNumRows() { return static_cast<int>(display.size()); }

void FileTableModel::paintRowBackground(juce::Graphics& g, int rowNumber, int width, int height, bool rowIsSelected)
{
    bool isChild = segmentAt(rowNumber) != nullptr;
    if (rowIsSelected)
        g.setColour(MiraLookAndFeel::accentSoft);
    else if (isChild)
        // Children sit on the recessed surface (the chrome shade), so a block of segment
        // rows reads as belonging to the file above it rather than as more files.
        g.setColour(MiraLookAndFeel::surface2);
    else if (rowNumber % 2 == 1)
        g.setColour(MiraLookAndFeel::surface);
    else
        g.setColour(MiraLookAndFeel::surface.brighter(0.01f));
    g.fillRect(0, 0, width, height);
    g.setColour(MiraLookAndFeel::borderSoft);
    g.drawLine(0.0f, static_cast<float>(height - 1), static_cast<float>(width), static_cast<float>(height - 1), 1.0f);
}

// A segment child row (review round 3: "the list should show parent file, child as
// segment details"). Its own values where it has them -- time range, length, loudness,
// genre/instrument/mood from its own analysis or tags -- and the file's BPM/key/sample
// rate, dimmed, since per-segment analysis doesn't re-measure those (main.cpp's
// buildSegmentMachineJson).
void FileTableModel::paintSegmentCell(juce::Graphics& g, const Row& row, const Row::SegmentFacets& segment,
                                      int columnId, juce::Rectangle<int> bounds, bool rowIsSelected)
{
    const auto dash = juce::String(juce::CharPointer_UTF8("\xe2\x80\x94"));
    auto labelColour = [&](bool human) {
        return rowIsSelected ? MiraLookAndFeel::accent : human ? MiraLookAndFeel::accent : MiraLookAndFeel::text;
    };

    switch (columnId)
    {
        case ColFile:
        {
            // Indented past the parent's triangle, with a return arrow, so the time
            // range reads as belonging to the file above.
            auto rangeBounds = bounds.withTrimmedLeft(kDisclosureWidth + 12);
            auto range = juce::String(juce::CharPointer_UTF8("\xe2\x86\xb3 "))
                          + formatDuration(segment.startSeconds)
                          + juce::String(juce::CharPointer_UTF8(" \xe2\x80\x93 "))
                          + formatDuration(segment.endSeconds);
            g.setColour(rowIsSelected ? MiraLookAndFeel::accent : MiraLookAndFeel::textDim);
            g.setFont(laf.monoRegular(12.5f));
            g.drawText(range, rangeBounds, juce::Justification::centredLeft, 1);

            // "it should actually have which cue the segment is in - like cue 1 or 12".
            // Drawn after the range, in the cue colour, so it reads as a reference to
            // something else rather than as part of this row's own identity.
            if (segment.cueNumber > 0)
            {
                int offset = juce::roundToInt(
                                  juce::GlyphArrangement::getStringWidth(laf.monoRegular(12.5f), range))
                              + 14;
                auto text = "cue " + juce::String(segment.cueNumber)
                             + (segment.cueType.isNotEmpty() ? "  " + segment.cueType : juce::String());
                g.setColour(MiraLookAndFeel::good);
                g.setFont(laf.sansRegular(11.5f));
                g.drawText(text, rangeBounds.withTrimmedLeft(offset), juce::Justification::centredLeft, 1);
            }
            break;
        }
        case ColStatus:
            // Names the KIND, not just the provenance (review round 7, item 1). "cue" and
            // "segment" are different objects -- group-scoped vs file-scoped -- and this
            // column said "segment" for both, which is where the confusion started.
            // Colour follows the same language: green for a cue, amber for a segment. See
            // WaveformView's band painting for the other half of it, and the note there on
            // why teal is not available for either.
            g.setColour(segment.groupScoped ? MiraLookAndFeel::good : MiraLookAndFeel::accent);
            g.setFont(laf.monoRegular(12.5f));
            g.drawText(segment.groupScoped ? (segment.autoCreated ? "auto cue" : "cue")
                                           : (segment.autoCreated ? "auto segment" : "segment"),
                       bounds, juce::Justification::centredLeft, 1);
            break;
        case ColBpm:
        case ColSampleRate:
            g.setColour(MiraLookAndFeel::textFaint); // inherited from the file
            g.setFont(laf.monoRegular(13.5f));
            g.drawText(columnId == ColBpm ? row.bpmText : row.sampleRateText, bounds,
                       juce::Justification::centredRight, 1);
            break;
        case ColKey:
            g.setColour(MiraLookAndFeel::textFaint); // inherited from the file
            g.setFont(laf.sansRegular(13.5f));
            g.drawText(row.keyText, bounds, juce::Justification::centredLeft, 1);
            break;
        case ColDuration:
            g.setColour(MiraLookAndFeel::text);
            g.setFont(laf.monoRegular(13.5f));
            g.drawText(formatDuration(segment.endSeconds - segment.startSeconds), bounds,
                       juce::Justification::centredRight, 1);
            break;
        case ColLoudness:
            g.setColour(MiraLookAndFeel::text);
            g.setFont(laf.monoRegular(13.5f));
            g.drawText(segment.loudnessText, bounds, juce::Justification::centredRight, 1);
            break;
        case ColActive:
            g.setColour(MiraLookAndFeel::textFaint);
            g.setFont(laf.monoRegular(13.5f));
            g.drawText(dash, bounds, juce::Justification::centredRight, 1);
            break;
        case ColType:
            // Was hardcoded "SEG" for every child row, so a cue sat there labelled a
            // segment -- in the one column whose entire job is saying what a row IS. Found
            // by the user asking, for the second time, what the two words mean: the Status
            // column had been taught the difference and this one was still contradicting it.
            g.setColour(segment.groupScoped ? MiraLookAndFeel::good : MiraLookAndFeel::accent);
            g.setFont(laf.monoRegular(12.5f));
            g.drawText(segment.groupScoped ? "CUE" : "SEG", bounds, juce::Justification::centredLeft, 1);
            break;
        case ColGenre:
        case ColInstrument:
        case ColMood:
            g.setColour(labelColour(segment.humanTagged));
            g.setFont(laf.sansRegular(13.5f));
            g.drawText(columnId == ColGenre        ? segment.genreText
                       : columnId == ColInstrument ? segment.instrumentText
                                                   : segment.moodText,
                       bounds, juce::Justification::centredLeft, 1);
            break;
        default:
            break;
    }
}

void FileTableModel::paintCell(juce::Graphics& g, int rowNumber, int columnId, int width, int height, bool rowIsSelected)
{
    const auto* rowPtr = fileRowAt(rowNumber);
    if (rowPtr == nullptr) return;
    const auto& row = *rowPtr;
    auto bounds = juce::Rectangle<int>(0, 0, width, height).reduced(8, 0);

    if (const auto* segment = segmentAt(rowNumber))
    {
        paintSegmentCell(g, row, *segment, columnId, bounds, rowIsSelected);
        return;
    }

    switch (columnId)
    {
        case ColFile:
        {
            auto nameBounds = bounds;
            // Disclosure triangle: open (pointing down) when this file's segment rows
            // are showing. The slot is reserved on every row so names line up.
            auto triangleBounds = nameBounds.removeFromLeft(kDisclosureWidth).toFloat();
            nameBounds.removeFromLeft(4);
            if (!row.segments.empty())
            {
                auto c = triangleBounds.getCentre();
                juce::Path triangle;
                if (isExpanded(rowNumber)) triangle.addTriangle(c.x - 4.0f, c.y - 2.0f, c.x + 4.0f, c.y - 2.0f, c.x, c.y + 3.0f);
                else triangle.addTriangle(c.x - 2.0f, c.y - 4.0f, c.x - 2.0f, c.y + 4.0f, c.x + 3.0f, c.y);
                g.setColour(MiraLookAndFeel::textDim);
                g.fillPath(triangle);

                // Segment count on the right: green while a filter matched inside them
                // (the file is listed *because* of those segments), faint otherwise.
                bool matched = row.matchedSegments > 0;
                int count = matched ? row.matchedSegments : static_cast<int>(row.segments.size());
                auto hintBounds = nameBounds.removeFromRight(juce::jmin(96, nameBounds.getWidth() / 3));
                g.setColour(matched ? MiraLookAndFeel::good : MiraLookAndFeel::textFaint);
                g.setFont(laf.monoRegular(11.0f));
                g.drawText(juce::String(count) + (count == 1 ? " segment" : " segments"), hintBounds,
                           juce::Justification::centredRight, 1);
            }
            // Mockup's tr.sel td.name { color: var(--accent); } — selected rows tint
            // both the row background (paintRowBackground above) and the filename text.
            g.setColour(rowIsSelected ? MiraLookAndFeel::accent : MiraLookAndFeel::text);
            g.setFont(laf.sansRegular(14.0f));
            g.drawText(juce::File(row.record.path).getFileName(), nameBounds, juce::Justification::centredLeft, 1);
            break;
        }
        case ColStatus:
            g.setColour(statusColour(row));
            g.setFont(laf.monoRegular(12.5f));
            g.drawText(statusText(row), bounds, juce::Justification::centredLeft, 1);
            break;
        case ColBpm:
        case ColLoudness:
        case ColActive:
            // Mockup's .num convention: mono, right-justified, tabular-nums. Three
            // tiers, three shades: a real analyzed value is full text; an explicit
            // "NNNbpm" token (filename or folder) is dimmed (textDim); the loose bare-
            // number guess ("70?") is dimmer still (textFaint) — never conflated with
            // real data, and the two guesses distinguishable from each other too.
            g.setColour(columnId != ColBpm            ? MiraLookAndFeel::text
                         : row.bpmGuessLoose           ? MiraLookAndFeel::textFaint
                         : row.bpmFromFilename         ? MiraLookAndFeel::textDim
                                                        : MiraLookAndFeel::text);
            g.setFont(laf.monoRegular(13.5f));
            g.drawText(columnId == ColBpm ? row.bpmText : columnId == ColLoudness ? row.loudnessText : row.activeText,
                              bounds, juce::Justification::centredRight, 1);
            break;
        case ColKey:
            // Same dimming for a filename-derived key guess.
            g.setColour(row.keyFromFilename ? MiraLookAndFeel::textDim : MiraLookAndFeel::text);
            g.setFont(laf.sansRegular(13.5f));
            g.drawText(row.keyText, bounds, juce::Justification::centredLeft, 1);
            break;
        case ColType:
            // Mockup's .ctype: mono, dim, small. File format (WAV/MP3/M4A/...) read off
            // the header, not the analysis-derived content type (loop/oneshot/stem) —
            // "if we know its wav then show it as wav" was the ask.
            g.setColour(MiraLookAndFeel::textDim);
            g.setFont(laf.monoRegular(12.5f));
            g.drawText(row.formatText, bounds, juce::Justification::centredLeft, 1);
            break;
        case ColDuration:
            g.setColour(MiraLookAndFeel::text);
            g.setFont(laf.monoRegular(13.5f));
            g.drawText(row.durationText, bounds, juce::Justification::centredRight, 1);
            break;
        case ColSampleRate:
            g.setColour(MiraLookAndFeel::text);
            g.setFont(laf.monoRegular(13.5f));
            g.drawText(row.sampleRateText, bounds, juce::Justification::centredRight, 1);
            break;
        case ColGenre:
            // Edited (human override) shows in accent, same "this was a deliberate
            // choice, not just what the model said" signal the rename/display-name
            // fields use elsewhere in the sidebar.
            g.setColour(row.genreFromHuman ? MiraLookAndFeel::accent : MiraLookAndFeel::text);
            g.setFont(laf.sansRegular(13.5f));
            g.drawText(row.genreText, bounds, juce::Justification::centredLeft, 1);
            break;
        case ColInstrument:
            g.setColour(row.instrumentFromHuman  ? MiraLookAndFeel::accent
                        : row.instrumentFromFilename ? MiraLookAndFeel::textDim
                                                      : MiraLookAndFeel::text);
            g.setFont(laf.sansRegular(13.5f));
            g.drawText(row.instrumentText, bounds, juce::Justification::centredLeft, 1);
            break;
        case ColMood:
            g.setColour(row.moodFromHuman ? MiraLookAndFeel::accent : MiraLookAndFeel::text);
            g.setFont(laf.sansRegular(13.5f));
            g.drawText(row.moodText, bounds, juce::Justification::centredLeft, 1);
            break;
        default:
            break;
    }
}

juce::var FileTableModel::getDragSourceDescription(const juce::SparseSet<int>& currentlySelectedRows)
{
    // A segment row drags its whole parent file for now -- cutting the segment's own
    // audio at drag time is export work (mira export-segments), not a drag handler's.
    if (currentlySelectedRows.isEmpty()) return {};
    const auto* row = fileRowAt(currentlySelectedRows[0]);
    if (row == nullptr) return {};
    return juce::String(row->record.path);
}

void FileTableModel::selectedRowsChanged(int lastRowSelected)
{
    const auto* row = fileRowAt(lastRowSelected);
    if (onSelectionChanged) onSelectionChanged(row != nullptr ? &row->record : nullptr);
    // A segment row: its file is loaded by the call above first, then the waveform
    // selects just that segment's range.
    if (const auto* segment = segmentAt(lastRowSelected); segment != nullptr && onSegmentSelected)
        onSegmentSelected(row->record, segment->id, segment->startSeconds, segment->endSeconds);
}

std::vector<juce::String> FileTableModel::getSelectedPaths(const juce::SparseSet<int>& selectedRows) const
{
    // Parent files, deduplicated: selecting a file and some of its segments means that
    // one file to Analyze / Details, not the same path several times.
    std::vector<juce::String> paths;
    std::set<juce::String> seen;
    for (int i = 0; i < selectedRows.size(); ++i)
        if (const auto* row = fileRowAt(selectedRows[i]))
            if (auto path = juce::String(row->record.path); seen.insert(path).second) paths.push_back(path);
    return paths;
}

std::vector<juce::String> FileTableModel::getAllPaths() const
{
    std::vector<juce::String> paths;
    paths.reserve(rows.size());
    for (const auto& row : rows) paths.push_back(juce::String(row.record.path));
    return paths;
}

void FileTableModel::setupColumns(juce::TableHeaderComponent& header)
{
    constexpr int kNotSortable = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable
                                | juce::TableHeaderComponent::draggable | juce::TableHeaderComponent::appearsOnColumnMenu;
    // Genre/Instrument/Mood start hidden (no `visible` flag) — an "extensive" column
    // set with everything shown by default crowds the common case; right-click the
    // header for JUCE's own built-in column-chooser popup (enabled in
    // FileTableComponent's ctor) to turn any of these on, same convention a real file
    // browser's customizable columns uses.
    constexpr int kHiddenButChoosable = juce::TableHeaderComponent::resizable
                                       | juce::TableHeaderComponent::draggable
                                       | juce::TableHeaderComponent::appearsOnColumnMenu;
    header.addColumn("File", ColFile, 220, 100, -1, kNotSortable);
    header.addColumn("Status", ColStatus, 100, 65, -1, kNotSortable);
    header.addColumn("BPM", ColBpm, 60, 40, -1, kNotSortable);
    header.addColumn("Key", ColKey, 90, 50, -1, kNotSortable);
    header.addColumn("Duration", ColDuration, 70, 50, -1, kNotSortable);
    header.addColumn("Sample Rate", ColSampleRate, 90, 60, -1, kHiddenButChoosable);
    header.addColumn("Loudness", ColLoudness, 80, 50, -1, kNotSortable);
    // "the active title is misleading" -- it's active_ratio (Phase 1's active-region
    // detector: % of the file that's musically live, not silence), only measured for
    // stems/long files, not a general-purpose column. "Active %" reads less like a
    // yes/no flag and more like the percentage it actually is.
    header.addColumn("Active %", ColActive, 70, 45, -1, kNotSortable);
    header.addColumn("Type", ColType, 75, 50, -1, kNotSortable);
    header.addColumn("Genre", ColGenre, 160, 80, -1, kHiddenButChoosable);
    header.addColumn("Instrument", ColInstrument, 140, 80, -1, kHiddenButChoosable);
    header.addColumn("Mood", ColMood, 120, 80, -1, kHiddenButChoosable);
}
