#include "CaptionFields.h"

#include <algorithm>
#include <sstream>

#include "../analyze/GenreLabels.h"
#include "../analyze/InstrumentLabels.h"
#include "../analyze/MoodThemeLabels.h"

namespace mira {

namespace {

std::vector<ScoredLabel> topScored(Database& db, const std::string& machine,
                                    const char* const* names, int count,
                                    const char* objectPath, double threshold, int maxCount) {
    std::vector<ScoredLabel> scored;
    for (int i = 0; i < count; ++i) {
        std::string path = std::string(objectPath) + "." + names[i];
        if (auto score = db.jsonExtractDouble(machine, path)) {
            if (*score >= threshold) scored.push_back({names[i], *score});
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (static_cast<int>(scored.size()) > maxCount) scored.resize(maxCount);
    return scored;
}

// Genre needs the normalized-then-raw fallback and double-quoted JSON path segment, same
// as `mira inspect` (main.cpp's runInspect) -- duplicated rather than shared, since
// inspect's version is a display-only top-5 (no threshold, shows whatever's there) and
// this one is confidence-gated for captioning.
std::vector<ScoredLabel> topGenre(Database& db, const std::string& machine) {
    std::vector<ScoredLabel> scored;
    for (int i = 0; i < kGenreClassCount; ++i) {
        std::string rawName = kGenreClassNames[i];
        std::string normalizedName = rawName;
        size_t sep = normalizedName.find("---");
        if (sep != std::string::npos) normalizedName.replace(sep, 3, ": ");

        std::string normalizedPath = std::string("$.genre_normalized.\"") + normalizedName + "\"";
        std::string rawPath = std::string("$.genre.\"") + rawName + "\"";

        double score;
        std::string label;
        if (auto s = db.jsonExtractDouble(machine, normalizedPath)) {
            score = *s;
            label = normalizedName;
        } else if (auto s = db.jsonExtractDouble(machine, rawPath)) {
            score = *s;
            label = rawName;
        } else {
            continue;
        }
        if (score >= kCaptionGenreThreshold) scored.push_back({label, score});
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (static_cast<int>(scored.size()) > kCaptionMaxGenre) scored.resize(kCaptionMaxGenre);
    return scored;
}

// IRMAS's own label order (write_metadata_irmas.py's label_dict), display names --
// same table `mira inspect` uses.
std::vector<ScoredLabel> topStemInstrument(Database& db, const std::string& machine) {
    static const std::pair<const char*, const char*> kCodes[] = {
        {"cel", "cello"}, {"cla", "clarinet"}, {"flu", "flute"}, {"gac", "acoustic guitar"},
        {"gel", "electric guitar"}, {"org", "organ"}, {"pia", "piano"}, {"sax", "saxophone"},
        {"tru", "trumpet"}, {"vio", "violin"}, {"voi", "voice"},
    };
    std::vector<ScoredLabel> scored;
    for (const auto& [code, name] : kCodes) {
        std::string path = std::string("$.stem_instrument.scores.") + code;
        if (auto score = db.jsonExtractDouble(machine, path)) {
            if (*score >= kCaptionInstrumentThreshold) scored.push_back({name, *score});
        }
    }
    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) { return a.score > b.score; });
    if (static_cast<int>(scored.size()) > kCaptionMaxInstruments) scored.resize(kCaptionMaxInstruments);
    return scored;
}

std::optional<std::vector<std::string>> readHumanStringArray(Database& db, const std::string& human,
                                                               const std::string& path) {
    auto len = db.jsonArrayLength(human, path);
    if (!len) return std::nullopt;
    std::vector<std::string> out;
    for (int64_t i = 0; i < *len; ++i) {
        std::ostringstream p;
        p << path << "[" << i << "]";
        if (auto v = db.jsonExtractString(human, p.str())) out.push_back(*v);
    }
    return out;
}

// Applies one `human`-shaped JSON object's overrides onto `f` in place (PRD §11: "human
// field always wins on conflict"). Shared between file-level (`extractCaptionFields`) and
// segment-level (`extractCaptionFieldsForSegment`) extraction, since both read the exact
// same convention -- a segment's `human` is scoped to a time range instead of a whole
// file, but the keys inside it mean the same thing. genre/instruments/moods/bpm/key/
// is_instrumental each *replace* the current value wholesale when present; keywords is
// additive (see its own comment below) and capped, not score-gated.
void applyHumanOverrides(Database& db, const std::string& human, CaptionFields& f) {
    if (human == "{}") return;

    if (auto genreArr = readHumanStringArray(db, human, "$.genre")) {
        f.genre.clear();
        for (auto& g : *genreArr) f.genre.push_back({g, 1.0});
    }
    if (auto instArr = readHumanStringArray(db, human, "$.instruments")) {
        f.instruments.clear();
        for (auto& i : *instArr) f.instruments.push_back({i, 1.0});
    }
    if (auto moodArr = readHumanStringArray(db, human, "$.moods")) {
        f.moods.clear();
        for (auto& m : *moodArr) f.moods.push_back({m, 1.0});
    }
    if (auto bpm = db.jsonExtractDouble(human, "$.bpm")) f.bpm = *bpm;
    if (auto key = db.jsonExtractString(human, "$.key")) f.keyScale = *key;
    if (auto isInstrumentalVal = db.jsonExtractDouble(human, "$.is_instrumental"))
        f.isInstrumental = (*isInstrumentalVal != 0.0);
    // Additive, not a replacement -- unlike genre/instruments/moods above, keywords has
    // no machine-derived value to override in the first place. Capped (not score-gated --
    // there's no score for a human-written label) purely so the renderer's word-budget
    // trim, which never drops keywords ahead of a person's choice (Sa3Renderer.cpp),
    // can't be defeated by an unbounded list.
    if (auto kw = readHumanStringArray(db, human, "$.keywords")) {
        for (auto& k : *kw) f.keywords.push_back(k);
        if (static_cast<int>(f.keywords.size()) > kCaptionMaxKeywords) f.keywords.resize(kCaptionMaxKeywords);
    }
}

} // namespace

CaptionFields extractCaptionFields(Database& db, const FileRecord& record) {
    CaptionFields f;
    f.contentType = record.contentType;
    f.durationSeconds = db.jsonExtractDouble(record.machine, "$.duration_seconds").value_or(0.0);

    // Instrument source depends on content type: StemInstrument (the IRMAS-trained
    // predominant-instrument model) is the one TASKS.md's Phase 2 found actually
    // reliable on isolated stems; mtg_jamendo_instrument was validated on full mixes and
    // is unreliable on stems specifically (that finding is why stem_instrument exists as
    // a separate head at all).
    if (record.contentType == "stem") {
        f.instruments = topStemInstrument(db, record.machine);
    } else {
        f.instruments = topScored(db, record.machine, kInstrumentClassNames, kInstrumentClassCount,
                                   "$.instrument", kCaptionInstrumentThreshold, kCaptionMaxInstruments);
    }
    f.genre = topGenre(db, record.machine);
    f.moods = topScored(db, record.machine, kMoodThemeClassNames, kMoodThemeClassCount,
                         "$.moodtheme", kCaptionMoodThreshold, kCaptionMaxMood);

    // BPM: never for one-shots -- tempo on a sub-second clip is meaningless (PRD §5,
    // confirmed independently by SA3's own reprompt.py, whose One-shot template never
    // includes a BPM field either). Also omitted when Mir.cpp's tempo-stability check
    // flagged the file as not having one representative tempo (tempo_unstable, needs
    // >=3 windows to judge) -- asserting "128 BPM" over a piece that actually runs
    // 60-175 BPM across sections is exactly the hallucinated-field case PRD §12.6 warns
    // against; the render-time gate exists for cases like this, not just low raw scores.
    if (record.contentType != "one_shot") {
        auto bpmRaw = db.jsonExtractDouble(record.machine, "$.rhythm.beat_this_bpm");
        bool stable = true;
        auto windowCount = db.jsonExtractDouble(record.machine, "$.rhythm.tempo_window_count");
        if (windowCount && *windowCount >= 3) {
            auto unstable = db.jsonExtractDouble(record.machine, "$.rhythm.tempo_unstable");
            stable = !(unstable && *unstable != 0.0);
        }
        if (bpmRaw && *bpmRaw > 0.0 && stable) f.bpm = *bpmRaw;
    }

    if (auto key = db.jsonExtractString(record.machine, "$.key.key")) f.keyScale = *key;

    if (auto voiceProb = db.jsonExtractDouble(record.machine, "$.voice_instrumental.voice_probability")) {
        f.isInstrumental = (*voiceProb < kCaptionVoiceThreshold);
    }

    // Folder-level defaults (TASKS.md Phase 3 addition), applied before the file's own
    // `human` so a per-file tag always wins over a folder default -- same "more specific
    // wins" rule as segment-over-file. Ordered shortest-to-longest matching folder path
    // (Database::findFolderDefaultsForPath), so a deeper folder's default overrides a
    // shallower ancestor's for the same field.
    for (const auto& folderHuman : db.findFolderDefaultsForPath(record.path))
        applyHumanOverrides(db, folderHuman, f);

    // Human overrides (PRD §11: "human field always wins on conflict"). Minimal
    // convention, CaptionFields' own -- not a project-wide human schema (none exists
    // yet): see applyHumanOverrides() above for exactly which top-level `human` JSON
    // keys are read and how each one is applied.
    applyHumanOverrides(db, record.human, f);

    return f;
}

CaptionFields extractCaptionFieldsForSegment(Database& db, const FileRecord& record,
                                              const SegmentRecord& segment) {
    // Starts from the whole-file document -- mira has no per-segment classification
    // (Phase 4's "segment-level analysis replacing whole-track averaging" is still
    // unbuilt; TASKS.md), so genre/instruments/moods/bpm/key here are the file's own
    // whole-file measurements, not re-measured for this specific time range. Documented
    // simplification, not hidden: a segment's BPM in particular inherits the file's
    // single average tempo even though the whole reason this function exists is that a
    // long file's *character* varies by section -- tempo can vary the same way (Mir.cpp's
    // own tempo-stability finding). Revisit once Phase 4 lands.
    CaptionFields f = extractCaptionFields(db, record);

    // The one thing that must change regardless: duration is the segment's own length,
    // not the whole file's -- this is what makes the exported clip fit SA3's per-clip
    // duration ceiling at all.
    f.durationSeconds = segment.endSeconds - segment.startSeconds;

    // A segment's own `human` (the time-ranged tags -- "funny" from 2:00-3:30) is layered
    // on top of the file-level document *last*, so a segment-specific tag always wins
    // over both the machine-derived value and the file's own (untimed) human override --
    // the more specific scope takes precedence, same rule PRD §11 already establishes for
    // human-over-machine, just applied one level deeper.
    applyHumanOverrides(db, segment.human, f);

    return f;
}

} // namespace mira
