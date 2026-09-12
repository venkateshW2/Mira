#include "FileDetailsWindow.h"

#include "mira/caption/CaptionFields.h"
#include "mira/scan/Scanner.h" // instrumentFromFilename -- a stem's name names its instrument
#include "mira/caption/Sa3Renderer.h"

namespace {
juce::String dash() { return juce::String(juce::CharPointer_UTF8("\xe2\x80\x94")); }

juce::String formatPercent(double v) { return juce::String(juce::roundToInt(v * 100.0)) + "%"; }

// Builds a compact "Label 42% \xb7 Label2 18%" reference string from a ranked
// (label, score) list -- the "full details of the analysis" read-out for a multi-label
// field, since the table's own column only ever shows the single top label.
juce::String formatRanked(const std::vector<std::pair<std::string, double>>& entries)
{
    if (entries.empty()) return juce::String(juce::CharPointer_UTF8("no analysis data \xe2\x80\x94 run Analyze first"));
    juce::StringArray parts;
    for (const auto& [label, score] : entries)
        parts.add(juce::String(label) + " " + formatPercent(score));
    return parts.joinIntoString(juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")));
}

// std::vector<std::string> -> "a, b, c" -- Database::jsonStringArray returns std
// vectors, but juce::StringArray has no iterator-pair constructor to build one from,
// hence the manual join here.
juce::String joinCommaSeparated(const std::vector<std::string>& items)
{
    juce::StringArray arr;
    for (const auto& s : items) arr.add(juce::String(s));
    return arr.joinIntoString(", ");
}

// Builds a JSON array literal from comma-separated free text, trimming empties -- what
// every multi-label editor (Genre/Instrument/Mood) writes into `human` on Save.
juce::String jsonStringArrayLiteral(const juce::String& commaSeparated)
{
    juce::var arr = juce::Array<juce::var>();
    for (auto& token : juce::StringArray::fromTokens(commaSeparated, ",", ""))
    {
        auto trimmed = token.trim();
        if (trimmed.isNotEmpty()) arr.append(trimmed);
    }
    return juce::JSON::toString(arr, juce::JSON::FormatOptions {}.withSpacing(juce::JSON::Spacing::none));
}

// Shown in a batch editor's field when the selection doesn't agree on a value. Typing
// over it is what makes Save write that field; leaving it alone leaves every file's own
// value untouched -- the standard "mixed value" convention from every other inspector.
juce::String formatClock(double seconds)
{
    auto total = juce::roundToInt(seconds);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}

juce::String multipleValuesPlaceholder()
{
    return juce::String(juce::CharPointer_UTF8("\xe2\x80\x94 multiple values \xe2\x80\x94"));
}
} // namespace

FileDetailsContent::FileDetailsContent(mira::Database& databaseIn, const MiraLookAndFeel& lafIn,
                                        std::vector<int64_t> fileIdsIn, int64_t segmentIdIn)
    // Order matches the header's declaration order, which is what actually runs.
    : segmentId(segmentIdIn), database(databaseIn), laf(lafIn), fileIds(std::move(fileIdsIn))
{
    for (auto* l : { &titleLabel, &pathLabel, &metaLabel, &statusLabel, &provenanceLabel })
    {
        l->setColour(juce::Label::textColourId, MiraLookAndFeel::text);
        addAndMakeVisible(l);
    }
    pathLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
    metaLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
    statusLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
    provenanceLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);

    addFieldRow(bpmRow, "BPM");
    addFieldRow(keyRow, "Key");
    addFieldRow(genreRow, "Genre");
    addFieldRow(instrumentRow, "Instrument");
    addFieldRow(moodRow, "Mood");

    captionSectionLabel.setText("SA3 Caption", juce::dontSendNotification);
    captionSectionLabel.setFont(laf.sansSemiBold(14.5f));
    captionSectionLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::text);
    addAndMakeVisible(captionSectionLabel);

    captionHintLabel.setText(
        juce::String(juce::CharPointer_UTF8("rendered from the gated fields above \xe2\x80\x94 edit those and Save to change it")),
        juce::dontSendNotification);
    captionHintLabel.setFont(laf.sansRegular(11.5f));
    captionHintLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
    addAndMakeVisible(captionHintLabel);

    triggerLabel.setText("Trigger", juce::dontSendNotification);
    triggerLabel.setFont(laf.sansRegular(11.5f));
    triggerLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
    addAndMakeVisible(triggerLabel);

    triggerEditor.setFont(laf.monoRegular(12.0f));
    triggerEditor.setTextToShowWhenEmpty("none", MiraLookAndFeel::textFaint);
    triggerEditor.setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::surface3);
    triggerEditor.setColour(juce::TextEditor::textColourId, MiraLookAndFeel::text);
    triggerEditor.setColour(juce::TextEditor::outlineColourId, MiraLookAndFeel::border);
    triggerEditor.setColour(juce::TextEditor::focusedOutlineColourId, MiraLookAndFeel::accent);
    // Live re-render on every keystroke: the whole point of showing the caption here is
    // seeing what a trigger token does to the sentence before committing to one.
    triggerEditor.onTextChange = [this] { reloadCaption(); };
    addAndMakeVisible(triggerEditor);

    tagsLabel.setText("Sidecar tags", juce::dontSendNotification);
    tagsLabel.setFont(laf.sansRegular(11.5f));
    tagsLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
    addAndMakeVisible(tagsLabel);

    for (auto* e : { &proseEditor, &tagsEditor })
    {
        e->setMultiLine(true, true);
        e->setReadOnly(true);
        e->setScrollbarsShown(true);
        e->setCaretVisible(false);
        e->setFont(laf.monoRegular(12.0f));
        e->setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::surface3);
        e->setColour(juce::TextEditor::textColourId, MiraLookAndFeel::text);
        e->setColour(juce::TextEditor::outlineColourId, MiraLookAndFeel::border);
        addAndMakeVisible(e);
    }
    tagsEditor.setColour(juce::TextEditor::textColourId, MiraLookAndFeel::textDim);

    for (auto* b : { &copyProseButton, &copyJsonButton })
    {
        b->setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
        b->setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
        addAndMakeVisible(b);
    }
    copyProseButton.onClick = [this] {
        juce::SystemClipboard::copyTextToClipboard(proseEditor.getText());
    };
    // The JSON sidecar, not the "key: value" display text — this is the thing that
    // actually goes next to a .wav in a training set (`mira caption --emit-sidecar`).
    copyJsonButton.onClick = [this] {
        auto fields = mira::extractCaptionFields(database, record);
        juce::SystemClipboard::copyTextToClipboard(
            juce::String(mira::renderSa3SidecarJson(fields, triggerEditor.getText().trim().toStdString())));
    };

    reload();
}

void FileDetailsContent::addFieldRow(FieldRow& row, const juce::String& sectionName)
{
    row.sectionLabel.setText(sectionName, juce::dontSendNotification);
    row.sectionLabel.setFont(laf.sansSemiBold(14.5f));
    row.sectionLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::text);
    addAndMakeVisible(row.sectionLabel);

    row.referenceLabel.setFont(laf.sansRegular(11.5f));
    row.referenceLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
    addAndMakeVisible(row.referenceLabel);

    row.editor.setFont(laf.sansRegular(13.5f));
    row.editor.setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::surface3);
    row.editor.setColour(juce::TextEditor::textColourId, MiraLookAndFeel::text);
    row.editor.setColour(juce::TextEditor::outlineColourId, MiraLookAndFeel::border);
    row.editor.setColour(juce::TextEditor::focusedOutlineColourId, MiraLookAndFeel::accent);
    addAndMakeVisible(row.editor);
}

FileDetailsContent::EffectiveFields FileDetailsContent::computeFields(const mira::FileRecord& rec) const
{
    EffectiveFields out;
    auto file = juce::File(rec.path);

    // BPM/Key: single-value fields -- reference shows exactly what analysis produced (or
    // that none has run yet); the editor always starts from the *effective* value
    // (human override if set, else the machine one) so Save is a no-op unless something
    // actually changed.
    auto machineBpm = database.jsonExtractDouble(rec.machine, "$.rhythm.beat_this_bpm");
    auto humanBpm = database.jsonExtractDouble(rec.human, "$.bpm");
    out.bpmRef = machineBpm ? "analyzed: " + juce::String(juce::roundToInt(*machineBpm)) + " bpm"
                            : juce::String(juce::CharPointer_UTF8("not analyzed yet"));
    out.bpm = humanBpm ? juce::String(*humanBpm)
                       : (machineBpm ? juce::String(juce::roundToInt(*machineBpm)) : juce::String());

    auto machineKey = database.jsonExtractString(rec.machine, "$.key.key");
    auto humanKey = database.jsonExtractString(rec.human, "$.key");
    out.keyRef = machineKey ? "analyzed: " + juce::String(*machineKey)
                            : juce::String(juce::CharPointer_UTF8("not analyzed yet"));
    out.key = humanKey ? juce::String(*humanKey) : (machineKey ? juce::String(*machineKey) : juce::String());

    // Genre/Instrument/Mood: multi-label -- "instruments are not right... need full
    // details" is exactly this: the reference line shows every candidate label the model
    // actually produced with its score, not just the winner, so a wrong top-1 is visibly
    // a close call (or genuinely wrong) instead of an unexplained single word.
    auto genreEntries = database.jsonObjectEntries(rec.machine, "$.genre_normalized", 6);
    if (genreEntries.empty()) genreEntries = database.jsonObjectEntries(rec.machine, "$.genre", 6);
    out.genreRef = formatRanked(genreEntries);
    auto humanGenre = database.jsonStringArray(rec.human, "$.genre");
    out.genre = humanGenre.empty()
                     ? (genreEntries.empty() ? juce::String() : juce::String(genreEntries.front().first))
                     : joinCommaSeparated(humanGenre);

    // Stems get a second, isolated-audio-tuned opinion -- "$.stem_instrument" is NOT a
    // flat label->score map like "$.instrument_normalized" (it's {"window_count":N,
    // "scores":{...}}), reading it directly produced garbage ("scores 0% ·
    // window_count..."). The real flat maps are "$.stem_instrument_normalized" (taxonomy-
    // normalized, preferred) and "$.stem_instrument.scores" (raw IRMAS 3-letter codes,
    // fallback).
    //
    // "its percussion but showing as guitars" -- the stem-tuned model (IRMAS, 11 classes:
    // cel/cla/flu/gac/gel/org/pia/sax/tru/vio/voi) has NO drums/percussion class at all,
    // so a percussion stem is *structurally* forced into one of those 11 melodic/
    // harmonic families -- not a probability issue, a coverage gap. The full-mix model
    // (mtg_jamendo_instrument) does include drums/percussion and stays fairly reliable at
    // recognizing them even on isolated audio (a distinctive timbre), so it's preferred
    // specifically when it confidently says drums/percussion; otherwise the stem-tuned
    // opinion wins as the more isolated-audio-appropriate one, same as before.
    bool isStem = rec.contentType == "stem";
    auto fullMixEntries = database.jsonObjectEntries(rec.machine, "$.instrument_normalized", 6);
    if (fullMixEntries.empty()) fullMixEntries = database.jsonObjectEntries(rec.machine, "$.instrument", 6);
    auto stemEntries = database.jsonObjectEntries(rec.machine, "$.stem_instrument_normalized", 6);
    if (stemEntries.empty()) stemEntries = database.jsonObjectEntries(rec.machine, "$.stem_instrument.scores", 6);

    bool fullMixSaysPercussion = !fullMixEntries.empty()
                                  && (fullMixEntries.front().first == "drums" || fullMixEntries.front().first == "percussion")
                                  && fullMixEntries.front().second >= 0.3;
    bool preferStem = isStem && !stemEntries.empty() && !fullMixSaysPercussion;
    const auto& primaryEntries = preferStem ? stemEntries : fullMixEntries;
    const auto& secondaryEntries = preferStem ? fullMixEntries : stemEntries;
    juce::String secondaryLabel = preferStem ? "full-mix opinion" : "stem opinion";

    juce::String instrumentRef = formatRanked(primaryEntries);
    if (!secondaryEntries.empty())
        instrumentRef += "  |  " + secondaryLabel + ": " + formatRanked(secondaryEntries);
    auto humanInstrument = database.jsonStringArray(rec.human, "$.instruments");

    // "for music tracks instrument field should have all the details" -- a full mix
    // genuinely has several real instruments sounding at once; a stem/sample/one-shot
    // should just be the one.
    juce::String defaultInstrumentText;
    {
        // A stem's filename leads (review round 5) -- see FileTable.cpp's matching
        // comment for why it beats both models on isolated audio.
        auto filenameHint = rec.contentType == "stem" ? mira::instrumentFromFilename(rec.path)
                                                      : std::optional<std::string> {};
        if (filenameHint)
            instrumentRef +=
                juce::String(juce::CharPointer_UTF8("  |  filename says: ")) + juce::String(*filenameHint);
        // Everything the analysis actually found, not one label (review round 4: "the
        // instrument field needs all the details"). Same rule as CaptionFields'
        // allInstruments, so this field and the caption below it can't disagree:
        //   - the preferred model's labels above the caption threshold;
        //   - for a stem, any full-mix label at 0.30+ as well -- IRMAS has no drums or
        //     percussion class at all, so that's the only way a drum stem gets named,
        //     while the high bar keeps full-mix noise ("computer", "bass" on a vocal
        //     stem) out;
        //   - plus "voice" when the voice head is confident, since that's the head
        //     actually trained to answer it (85% and 98% on the two review-round-4
        //     files, where both instrument heads said "synthesizer").
        //
        // `rec`, not the `record` member: computeFields runs per row in batch mode, and
        // reading the member here meant every row used the *loaded* file's content type.
        // Stems stay essentially one instrument (a high bar for anything joining the top
        // label); mixes and tracks list everything above the caption threshold.
        double labelFloor = rec.contentType == "stem" ? 0.30 : 0.10;
        juce::StringArray labels;
        if (filenameHint) labels.add(juce::String(*filenameHint));
        for (const auto& [label, score] : primaryEntries)
            if (score >= labelFloor || labels.isEmpty()) labels.addIfNotAlreadyThere(juce::String(label), true);
        if (rec.contentType == "stem")
            for (const auto& [label, score] : secondaryEntries)
                if (score >= 0.30) labels.addIfNotAlreadyThere(juce::String(label), true);
        if (auto voiceProb = database.jsonExtractDouble(rec.machine, "$.voice_instrumental.voice_probability"))
            if (*voiceProb >= 0.5) labels.addIfNotAlreadyThere("voice", true);
        if (labels.isEmpty() && !primaryEntries.empty()) labels.add(primaryEntries.front().first);
        defaultInstrumentText = labels.joinIntoString(", ");
    }
    out.instrumentRef = instrumentRef;
    out.instrument = humanInstrument.empty() ? defaultInstrumentText : joinCommaSeparated(humanInstrument);

    auto moodEntries = database.jsonObjectEntries(record.machine, "$.moodtheme", 6);
    out.moodRef = formatRanked(moodEntries);
    auto humanMood = database.jsonStringArray(record.human, "$.moods");
    out.mood = humanMood.empty() ? (moodEntries.empty() ? juce::String() : juce::String(moodEntries.front().first))
                                 : joinCommaSeparated(humanMood);

    return out;
}

void FileDetailsContent::setRowValue(FieldRow& row, const juce::String& value, const juce::String& reference)
{
    row.referenceLabel.setText(reference, juce::dontSendNotification);
    row.editor.setText(value, juce::dontSendNotification);
    row.initialText = value;
}

void FileDetailsContent::reload()
{
    records.clear();
    for (auto id : fileIds)
        if (auto existing = database.findById(id)) records.push_back(*existing);

    // Every id gone from the DB out from under this window -- leave the last-known state
    // showing rather than blanking the view into something that reads as broken.
    if (records.empty()) return;
    record = records.front();

    for (auto* row : { &bpmRow, &keyRow, &genreRow, &instrumentRow, &moodRow })
        row->editor.setTextToShowWhenEmpty({}, MiraLookAndFeel::textFaint);

    if (segmentId != 0) segment = database.findSegmentById(segmentId);

    if (isSegmentMode())
    {
        // One segment's own view. Its analysis is a subset of a file's (no rhythm/key --
        // main.cpp's buildSegmentMachineJson), so BPM and key are shown as the file's,
        // dimmed, while genre/instrument/mood come from this segment.
        auto file = juce::File(record.path);
        auto machine = database.getSegmentMachine(segmentId, record.id).value_or("{}");
        auto length = segment->endSeconds - segment->startSeconds;

        titleLabel.setText(file.getFileName() + "   " + formatClock(segment->startSeconds)
                               + juce::String(juce::CharPointer_UTF8(" \xe2\x80\x93 ")) + formatClock(segment->endSeconds),
                           juce::dontSendNotification);
        pathLabel.setText(file.getFullPathName(), juce::dontSendNotification);

        auto loudness = database.jsonExtractDouble(machine, "$.dsp.integrated_loudness_lufs");
        juce::StringArray metaParts;
        metaParts.add(juce::String(juce::roundToInt(length)) + "s segment");
        metaParts.add(segment->source == "auto" ? "found by analysis" : "made by hand");
        metaParts.add(loudness ? juce::String(*loudness, 1) + " LUFS" : dash());
        metaLabel.setText(metaParts.joinIntoString(juce::String(juce::CharPointer_UTF8(" \xc2\xb7 "))),
                           juce::dontSendNotification);

        statusLabel.setText(machine == "{}" ? juce::String("This segment has no analysis of its own yet")
                                            : juce::String("Segment of ") + file.getFileName(),
                             juce::dontSendNotification);
        provenanceLabel.setText(juce::String(juce::CharPointer_UTF8(
                                    "edits here apply to this segment only \xe2\x80\x94 the file keeps its own tags")),
                                 juce::dontSendNotification);

        // BPM/key: the file's values, since a segment inherits them. Still editable --
        // a segment's own `human` can override either (CaptionFields applies it).
        auto fileFields = computeFields(record);
        auto humanBpm = database.jsonExtractDouble(segment->human, "$.bpm");
        setRowValue(bpmRow, humanBpm ? juce::String(*humanBpm) : fileFields.bpm,
                    juce::String("file: ") + (fileFields.bpm.isEmpty() ? dash() : fileFields.bpm));
        auto humanKey = database.jsonExtractString(segment->human, "$.key");
        setRowValue(keyRow, humanKey ? juce::String(*humanKey) : fileFields.key,
                    juce::String("file: ") + (fileFields.key.isEmpty() ? dash() : fileFields.key));

        // std::vector, not std::initializer_list: an initializer_list's backing array is
        // a temporary that dies at the end of the statement that made it, so storing one
        // here and reading it in the loop below would be reading freed memory.
        struct SegmentField
        {
            FieldRow* row;
            const char* humanPath;
            std::vector<const char*> machinePaths;
        };
        bool isStem = record.contentType == "stem";
        const std::vector<SegmentField> fields = {
            { &genreRow, "$.genre", { "$.genre_normalized", "$.genre" } },
            { &moodRow, "$.moods", { "$.moodtheme" } },
        };

        // Instrument is picked separately, because which model leads depends on what the
        // full-mix one says: IRMAS has no drums/percussion class, so without this a drum
        // stem's segment reads "electric guitar" under a file that reads "drums"
        // (measured on DRUMS_1.wav id 566: stem model 0.27 electric guitar vs full-mix
        // 0.37 drums). Same rule as the file row and the caption.
        {
            auto segFullMix = database.jsonObjectEntries(machine, "$.instrument_normalized", 6);
            if (segFullMix.empty()) segFullMix = database.jsonObjectEntries(machine, "$.instrument", 6);
            std::vector<std::pair<std::string, double>> segStem;
            if (isStem)
            {
                segStem = database.jsonObjectEntries(machine, "$.stem_instrument_normalized", 6);
                if (segStem.empty()) segStem = database.jsonObjectEntries(machine, "$.stem_instrument.scores", 6);
            }
            bool segFullMixSaysPercussion =
                !segFullMix.empty()
                && (segFullMix.front().first == "drums" || segFullMix.front().first == "percussion")
                && segFullMix.front().second >= 0.30;
            const auto& segPrimary = (isStem && !segStem.empty() && !segFullMixSaysPercussion) ? segStem : segFullMix;

            // Stems stay essentially one instrument; mixes/tracks list everything.
            double segFloor = isStem ? 0.30 : 0.10;
            juce::StringArray labels;
            for (size_t i = 0; i < segPrimary.size(); ++i)
                if (i == 0 || segPrimary[i].second >= segFloor)
                    labels.addIfNotAlreadyThere(juce::String(segPrimary[i].first), true);
            auto humanSegmentInstruments = database.jsonStringArray(segment->human, "$.instruments");
            setRowValue(instrumentRow,
                        humanSegmentInstruments.empty() ? labels.joinIntoString(", ")
                                                        : joinCommaSeparated(humanSegmentInstruments),
                        formatRanked(segPrimary));
        }
        for (const auto& field : fields)
        {
            std::vector<std::pair<std::string, double>> entries;
            for (const auto* path : field.machinePaths)
            {
                entries = database.jsonObjectEntries(machine, path, 6);
                if (!entries.empty()) break;
            }
            auto human = database.jsonStringArray(segment->human, field.humanPath);
            juce::StringArray above;
            for (const auto& [label, score] : entries)
                if (score >= 0.10 || above.isEmpty()) above.add(label);
            setRowValue(*field.row, human.empty() ? above.joinIntoString(", ") : joinCommaSeparated(human),
                        formatRanked(entries));
        }
    }
    else if (!isBatch())
    {
        auto file = juce::File(record.path);
        titleLabel.setText(file.getFileName(), juce::dontSendNotification);
        pathLabel.setText(file.getFullPathName(), juce::dontSendNotification);

        // "why are u [omitting] the details" -- Loudness and Active % were in the table's
        // own columns but never made it into this summary line; added here so the two
        // views (list row vs details window) show the same set of fields, not
        // overlapping subsets.
        auto durationVal = database.jsonExtractDouble(record.machine, "$.duration_seconds");
        auto sampleRateVal = database.jsonExtractDouble(record.machine, "$.sample_rate");
        auto loudnessVal = database.jsonExtractDouble(record.machine, "$.dsp.integrated_loudness_lufs");
        juce::StringArray metaParts;
        metaParts.add(file.getFileExtension().trimCharactersAtStart(".").toUpperCase());
        metaParts.add(durationVal ? juce::String(juce::roundToInt(*durationVal)) + "s" : dash());
        metaParts.add(sampleRateVal ? juce::String(static_cast<int>(*sampleRateVal / 1000.0)) + "kHz" : dash());
        metaParts.add(loudnessVal ? juce::String(*loudnessVal, 1) + " LUFS" : dash());
        metaParts.add(record.activeRatio ? juce::String(juce::roundToInt(*record.activeRatio * 100)) + "% active"
                                          : dash());
        metaLabel.setText(metaParts.joinIntoString(juce::String(juce::CharPointer_UTF8(" \xc2\xb7 "))),
                           juce::dontSendNotification);

        juce::String statusText = record.analyzedAt
                                       ? "analyzed " + juce::Time(*record.analyzedAt * 1000).toString(true, true)
                                       : juce::String("not analyzed yet");
        statusLabel.setText("Status: " + statusText + "  \xc2\xb7  Content type: " + juce::String(record.contentType),
                             juce::dontSendNotification);

        auto essentiaVer = database.jsonExtractString(record.provenance, "$.essentia_version");
        auto gitHash = database.jsonExtractString(record.provenance, "$.mira_git_hash");
        auto beatThisModel = database.jsonExtractString(record.provenance, "$.beat_this_model");
        provenanceLabel.setText(
            record.analyzedAt
                ? "mira " + juce::String(gitHash ? *gitHash : "?") + "  \xc2\xb7  essentia "
                      + juce::String(essentiaVer ? *essentiaVer : "?") + "  \xc2\xb7  beat_this "
                      + juce::String(beatThisModel ? *beatThisModel : "?")
                : juce::String(juce::CharPointer_UTF8("no analysis run yet \xe2\x80\x94 nothing to show provenance for")),
            juce::dontSendNotification);

        auto fields = computeFields(record);
        setRowValue(bpmRow, fields.bpm, fields.bpmRef);
        setRowValue(keyRow, fields.key, fields.keyRef);
        setRowValue(genreRow, fields.genre, fields.genreRef);
        setRowValue(instrumentRow, fields.instrument, fields.instrumentRef);
        setRowValue(moodRow, fields.mood, fields.moodRef);
    }
    else
    {
        auto count = static_cast<int>(records.size());
        titleLabel.setText(juce::String(count) + " files selected", juce::dontSendNotification);

        // The deepest folder every selected file sits under -- more useful than listing
        // N paths, and it's how a multi-selection is usually described anyway ("the kicks
        // in that pack").
        juce::String commonParent = juce::File(records.front().path).getParentDirectory().getFullPathName();
        for (const auto& r : records)
        {
            auto parent = juce::File(r.path).getParentDirectory().getFullPathName();
            while (commonParent.isNotEmpty() && !parent.startsWith(commonParent))
                commonParent = juce::File(commonParent).getParentDirectory().getFullPathName();
        }
        pathLabel.setText(commonParent.isEmpty() ? juce::String("(files span multiple volumes)") : commonParent,
                           juce::dontSendNotification);

        int analyzed = 0;
        double totalSeconds = 0.0;
        std::map<std::string, int> contentTypes;
        for (const auto& r : records)
        {
            if (r.analyzedAt) ++analyzed;
            if (auto d = database.jsonExtractDouble(r.machine, "$.duration_seconds")) totalSeconds += *d;
            ++contentTypes[r.contentType];
        }
        juce::StringArray typeParts;
        for (const auto& [type, n] : contentTypes) typeParts.add(juce::String(n) + " " + juce::String(type));
        metaLabel.setText(juce::String(count) + " files  \xc2\xb7  "
                               + juce::String(juce::roundToInt(totalSeconds)) + "s total  \xc2\xb7  "
                               + typeParts.joinIntoString(", "),
                           juce::dontSendNotification);
        statusLabel.setText("Status: " + juce::String(analyzed) + " of " + juce::String(count) + " analyzed",
                             juce::dontSendNotification);
        provenanceLabel.setText(
            juce::String(juce::CharPointer_UTF8(
                "editing all " )) + juce::String(count)
                + juce::String(juce::CharPointer_UTF8(" at once \xe2\x80\x94 only fields you actually change get written")),
            juce::dontSendNotification);

        // Collapse each field across the selection: unanimous -> show and edit that
        // value; mixed -> a placeholder plus a count of how many distinct values there
        // are, so the mixedness is legible rather than just "blank".
        struct Collapsed { juce::String value, reference; };
        auto collapse = [count](const std::vector<juce::String>& values) {
            std::set<juce::String> distinct(values.begin(), values.end());
            if (distinct.size() == 1)
                return Collapsed { *distinct.begin(), "same across all " + juce::String(count) + " files" };
            juce::StringArray sample;
            for (const auto& v : distinct)
            {
                if (sample.size() >= 4) { sample.add(juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"))); break; }
                sample.add(v.isEmpty() ? juce::String("(empty)") : v);
            }
            return Collapsed { juce::String(),
                                juce::String(static_cast<int>(distinct.size())) + " distinct values: "
                                    + sample.joinIntoString(juce::String(juce::CharPointer_UTF8(" \xc2\xb7 "))) };
        };

        std::vector<juce::String> bpms, keys, genres, instruments, moods;
        for (const auto& r : records)
        {
            auto f = computeFields(r);
            bpms.push_back(f.bpm);
            keys.push_back(f.key);
            genres.push_back(f.genre);
            instruments.push_back(f.instrument);
            moods.push_back(f.mood);
        }

        const std::pair<FieldRow*, const std::vector<juce::String>*> rows[] = {
            { &bpmRow, &bpms }, { &keyRow, &keys }, { &genreRow, &genres },
            { &instrumentRow, &instruments }, { &moodRow, &moods },
        };
        for (const auto& [row, values] : rows)
        {
            auto collapsed = collapse(*values);
            setRowValue(*row, collapsed.value, collapsed.reference);
            if (collapsed.value.isEmpty())
                row->editor.setTextToShowWhenEmpty(multipleValuesPlaceholder(), MiraLookAndFeel::textFaint);
        }
    }

    titleLabel.setFont(laf.sansSemiBold(16.0f));
    pathLabel.setFont(laf.monoRegular(11.0f));
    metaLabel.setFont(laf.monoRegular(12.0f));
    statusLabel.setFont(laf.sansRegular(12.0f));
    provenanceLabel.setFont(laf.monoRegular(10.5f));

    reloadCaption();

    resized();
    repaint();
}

void FileDetailsContent::reloadCaption()
{
    // A caption is a per-file object -- there is no meaningful "caption of 40 files", and
    // showing the first one's would be actively misleading about what Save does. The
    // whole block just isn't there in batch mode.
    bool showCaption = !isBatch(); // segment mode is single-file, so it keeps the caption
    const std::initializer_list<juce::Component*> captionComponents {
        &captionSectionLabel, &captionHintLabel, &triggerLabel, &tagsLabel,
        &triggerEditor,       &proseEditor,      &tagsEditor,   &copyProseButton,
        &copyJsonButton,
    };
    for (auto* c : captionComponents) c->setVisible(showCaption);
    if (!showCaption) return;

    if (!record.analyzedAt)
    {
        proseEditor.setText(juce::String(juce::CharPointer_UTF8(
                                 "not analyzed yet \xe2\x80\x94 nothing to caption")),
                             juce::dontSendNotification);
        tagsEditor.setText({}, juce::dontSendNotification);
        copyProseButton.setEnabled(false);
        copyJsonButton.setEnabled(false);
        return;
    }

    copyProseButton.setEnabled(true);
    copyJsonButton.setEnabled(true);

    // Exactly the CLI's `mira caption` path (main.cpp's caption command), not a
    // reimplementation: extractCaptionFields applies the confidence gating and the human
    // overrides, renderSa3Prose/renderSa3Tags shape the result. The UI deliberately owns
    // none of that logic, so what shows here is byte-identical to what gets written into
    // a training set.
    auto trigger = triggerEditor.getText().trim().toStdString();
    // In segment mode this is the caption for *this* segment -- the whole point of
    // declaring one (SA3 has no per-clip timeline conditioning, so a long file can only
    // be captioned by cutting it into per-segment clips).
    auto fields = isSegmentMode() ? mira::extractCaptionFieldsForSegment(database, record, *segment)
                                  : mira::extractCaptionFields(database, record);

    proseEditor.setText(juce::String(mira::renderSa3Prose(fields, trigger)), juce::dontSendNotification);

    juce::StringArray tagLines;
    for (const auto& [key, value] : mira::renderSa3Tags(fields, trigger))
    {
        // "prompt" is renderSa3Tags' verbatim copy of the prose already shown above it —
        // repeating a whole sentence in the tag list just pushes the real tags out of view.
        if (key == "prompt") continue;
        tagLines.add(juce::String(key) + ": " + juce::String(value));
    }
    tagsEditor.setText(tagLines.isEmpty()
                            ? juce::String(juce::CharPointer_UTF8(
                                  "every field gated out \xe2\x80\x94 nothing confident enough to assert"))
                            : tagLines.joinIntoString("\n"),
                        juce::dontSendNotification);
}

void FileDetailsContent::save()
{
    // Each field is only written if its text actually changed since reload() filled it
    // in -- setHumanField merges one key at a time (json_set), so an untouched field
    // simply isn't included in this pass; PRD §11 "human field always wins on conflict"
    // still applies, just scoped to the keys someone actually edited here.
    //
    // The `initialText` comparison is load-bearing, not a micro-optimisation. Without it
    // every field the editor is *showing* gets written, which for a machine-derived value
    // means silently promoting analysis output into a human override that then outranks
    // every future re-analysis of that file -- and in a batch, stamping one file's
    // analyzed values onto the whole selection.
    auto changed = [](const FieldRow& row) { return row.editor.getText().trim() != row.initialText.trim(); };

    auto bpmText = bpmRow.editor.getText().trim();
    bool writeBpm = changed(bpmRow) && bpmText.isNotEmpty() && bpmText.containsOnly("0123456789.");
    auto keyText = keyRow.editor.getText().trim();
    auto genreText = genreRow.editor.getText().trim();
    auto instrumentText = instrumentRow.editor.getText().trim();
    auto moodText = moodRow.editor.getText().trim();

    // Segment mode writes to the segment's own `human`, never the file's.
    if (isSegmentMode())
    {
        if (writeBpm) database.setSegmentHumanField(segmentId, "$.bpm", bpmText.toStdString());
        if (changed(keyRow) && keyText.isNotEmpty())
            database.setSegmentHumanField(segmentId, "$.key",
                                           juce::JSON::toString(juce::var(keyText), true).toStdString());
        if (changed(genreRow) && genreText.isNotEmpty())
            database.setSegmentHumanField(segmentId, "$.genre", jsonStringArrayLiteral(genreText).toStdString());
        if (changed(instrumentRow) && instrumentText.isNotEmpty())
            database.setSegmentHumanField(segmentId, "$.instruments",
                                           jsonStringArrayLiteral(instrumentText).toStdString());
        if (changed(moodRow) && moodText.isNotEmpty())
            database.setSegmentHumanField(segmentId, "$.moods", jsonStringArrayLiteral(moodText).toStdString());
        reload();
        return;
    }

    for (const auto& r : records)
    {
        if (writeBpm) database.setHumanField(r.id, "$.bpm", bpmText.toStdString());
        if (changed(keyRow) && keyText.isNotEmpty())
            database.setHumanField(r.id, "$.key", juce::JSON::toString(juce::var(keyText), true).toStdString());
        if (changed(genreRow) && genreText.isNotEmpty())
            database.setHumanField(r.id, "$.genre", jsonStringArrayLiteral(genreText).toStdString());
        if (changed(instrumentRow) && instrumentText.isNotEmpty())
            database.setHumanField(r.id, "$.instruments", jsonStringArrayLiteral(instrumentText).toStdString());
        if (changed(moodRow) && moodText.isNotEmpty())
            database.setHumanField(r.id, "$.moods", jsonStringArrayLiteral(moodText).toStdString());
    }

    reload();
}

void FileDetailsContent::revert()
{
    if (isSegmentMode())
    {
        database.clearSegmentHumanFields(segmentId);
        reload();
        return;
    }
    for (const auto& r : records)
        database.clearHumanFields(r.id);
    reload();
}

void FileDetailsContent::paint(juce::Graphics& g)
{
    g.fillAll(MiraLookAndFeel::surface);

    // "make it interesting" -- a distinct tinted header block (surface2, like the rest
    // of mira's chrome-vs-content convention) instead of one flat undifferentiated
    // panel, plus a small accent bar per field row so the eye has something to track
    // scanning down the page rather than five identical-looking stacked blocks.
    g.setColour(MiraLookAndFeel::surface2);
    g.fillRect(0, 0, getWidth(), headerHeight);
    g.setColour(MiraLookAndFeel::borderSoft);
    g.drawLine(0.0f, static_cast<float>(headerHeight), static_cast<float>(getWidth()),
               static_cast<float>(headerHeight), 1.0f);

    for (auto* row : { &bpmRow, &keyRow, &genreRow, &instrumentRow, &moodRow })
    {
        auto barY = row->sectionLabel.getY();
        auto barBottom = row->editor.getBottom();
        g.setColour(MiraLookAndFeel::accent.withAlpha(0.6f));
        g.fillRoundedRectangle(8.0f, static_cast<float>(barY), 3.0f, static_cast<float>(barBottom - barY), 1.5f);
    }

    if (isBatch()) return; // no caption block in batch mode -- nothing below to mark

    // The caption block gets the same tracking bar, but in `good` rather than `accent`:
    // the field rows above are editable inputs, this one is derived output — a different
    // kind of thing, worth reading as one at a glance.
    {
        auto barY = captionSectionLabel.getY();
        auto barBottom = tagsEditor.getBottom();
        g.setColour(MiraLookAndFeel::good.withAlpha(0.6f));
        g.fillRoundedRectangle(8.0f, static_cast<float>(barY), 3.0f, static_cast<float>(barBottom - barY), 1.5f);
    }
}

void FileDetailsContent::resized()
{
    constexpr int margin = 24;
    constexpr int rowH = 22;
    // 320, not the pop-out window's old 400: the sidebar can be as narrow as 360 minus
    // its scrollbar, and the viewport has no horizontal scrollbar to reach an overflow.
    auto width = juce::jmax(320, getWidth());
    int y = 14;

    titleLabel.setBounds(margin, y, width - margin * 2, 24);
    y += 26;
    pathLabel.setBounds(margin, y, width - margin * 2, 16);
    y += 18;
    metaLabel.setBounds(margin, y, width - margin * 2, rowH);
    y += rowH + 10;
    headerHeight = y;
    y += 14;

    statusLabel.setBounds(margin, y, width - margin * 2, rowH);
    y += rowH;
    provenanceLabel.setBounds(margin, y, width - margin * 2, 16);
    y += 28;

    for (auto* row : { &bpmRow, &keyRow, &genreRow, &instrumentRow, &moodRow })
    {
        row->sectionLabel.setBounds(margin, y, width - margin * 2, 19);
        y += 19;
        row->referenceLabel.setBounds(margin, y, width - margin * 2, 16);
        y += 18;
        row->editor.setBounds(margin, y, width - margin * 2, 28);
        y += 28 + 16;
    }

    // Caption block -- single-file only (see reloadCaption). The two read-outs get fixed
    // heights with their own internal scrollbars rather than growing to fit: TextEditor
    // only knows its wrapped text height *after* it has been given bounds, so sizing the
    // block to the text would mean laying out twice on every keystroke while the trigger
    // is being typed.
    if (!isBatch())
    {
        constexpr int proseH = 88;
        constexpr int tagsH = 132;
        auto innerWidth = width - margin * 2;

        y += 8;
        captionSectionLabel.setBounds(margin, y, innerWidth, 19);
        y += 19;
        captionHintLabel.setBounds(margin, y, innerWidth, 16);
        y += 22;

        triggerLabel.setBounds(margin, y + 5, 56, 18);
        triggerEditor.setBounds(margin + 62, y, juce::jmin(200, innerWidth - 62), 28);
        y += 36;

        proseEditor.setBounds(margin, y, innerWidth, proseH);
        y += proseH + 8;

        copyProseButton.setBounds(margin, y, 100, 24);
        copyJsonButton.setBounds(margin + 108, y, 100, 24);
        y += 32;

        tagsLabel.setBounds(margin, y, innerWidth, 16);
        y += 18;
        tagsEditor.setBounds(margin, y, innerWidth, tagsH);
        y += tagsH + 16;
    }

    contentHeight = y + 8;
    setSize(width, contentHeight);
}

FileDetailsRoot::FileDetailsRoot(mira::Database& databaseIn, const MiraLookAndFeel& lafIn,
                                  std::vector<int64_t> fileIdsIn, std::function<void()> onSavedIn,
                                  std::function<void()> onCloseIn, int64_t segmentIdIn)
{
    content = std::make_unique<FileDetailsContent>(databaseIn, lafIn, std::move(fileIdsIn), segmentIdIn);
    viewport.setViewedComponent(content.get(), false);
    viewport.setScrollBarsShown(true, false);
    addAndMakeVisible(viewport);

    saveButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::accent);
    saveButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff1a1204));
    saveButton.onClick = [this, onSavedIn] {
        content->save();
        if (onSavedIn) onSavedIn();
    };
    addAndMakeVisible(saveButton);

    revertButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
    revertButton.setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
    revertButton.onClick = [this, onSavedIn] {
        content->revert();
        if (onSavedIn) onSavedIn();
    };
    addAndMakeVisible(revertButton);

    closeButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
    closeButton.setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
    closeButton.onClick = onCloseIn;
    addAndMakeVisible(closeButton);
}

void FileDetailsRoot::paint(juce::Graphics& g)
{
    g.fillAll(MiraLookAndFeel::surface2);
}

void FileDetailsRoot::resized()
{
    auto bounds = getLocalBounds();
    auto buttonRow = bounds.removeFromBottom(kButtonBarHeight).reduced(12, 8);
    closeButton.setBounds(buttonRow.removeFromRight(80));
    buttonRow.removeFromRight(8);
    revertButton.setBounds(buttonRow.removeFromRight(140));
    buttonRow.removeFromRight(8);
    saveButton.setBounds(buttonRow.removeFromRight(80));

    viewport.setBounds(bounds);
    content->setSize(bounds.getWidth(), content->getPreferredHeight());
}
