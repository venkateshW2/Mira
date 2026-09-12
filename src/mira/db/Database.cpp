#include "Database.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <sstream>

namespace mira {

namespace {
// PRD §6: path, sha256, mtime, content type, descriptors/tags (JSON columns), provenance.
// content_type/machine/human/provenance stay at their defaults until the analyzer runs —
// scan only ever indexes (PRD §8: "mira scan ... # index files, no analysis").
constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS files (
    id                   INTEGER PRIMARY KEY,
    path                 TEXT UNIQUE NOT NULL,
    sha256               TEXT NOT NULL,
    mtime                INTEGER NOT NULL,
    size_bytes           INTEGER NOT NULL,
    content_type         TEXT NOT NULL DEFAULT 'unknown',
    content_type_source  TEXT NOT NULL DEFAULT 'router',
    group_id             TEXT,
    active_ratio         REAL,
    active_spans         TEXT NOT NULL DEFAULT '[]',
    machine              TEXT NOT NULL DEFAULT '{}',
    human                TEXT NOT NULL DEFAULT '{}',
    provenance           TEXT NOT NULL DEFAULT '{}',
    scanned_at           INTEGER NOT NULL,
    analyzed_at          INTEGER
);
CREATE INDEX IF NOT EXISTS idx_files_content_type ON files(content_type);
CREATE INDEX IF NOT EXISTS idx_files_group_id ON files(group_id);
CREATE VIRTUAL TABLE IF NOT EXISTS vec_embeddings USING vec0(embedding float[1280]);

-- TASKS.md Phase 4 "Embedding A/B" (PRD §4, §14.5): a second, separate embedding space
-- (DCLAP, 512-dim) alongside discogs-effnet's vec_embeddings above -- a distinct vec0
-- table, not a wider column on the same one, since vec0's dimension is fixed per table
-- and the two spaces are never compared to each other, only independently against a
-- query of their own kind.
CREATE VIRTUAL TABLE IF NOT EXISTS vec_embeddings_dclap USING vec0(embedding float[512]);

-- TASKS.md Phase 4 "per-dimension similarity" — timbre (13-dim MFCC) and spectrum
-- (2-dim [centroid, flatness]) each get their own small vec0 table, same pattern as the
-- embeddings above. Rhythm has no table (Database.h's findSimilarByBpm comment).
CREATE VIRTUAL TABLE IF NOT EXISTS vec_timbre USING vec0(embedding float[13]);
CREATE VIRTUAL TABLE IF NOT EXISTS vec_spectrum USING vec0(embedding float[2]);

-- TASKS.md Phase 3 addition: a time-ranged counterpart to files.human, for the case a
-- whole-file caption can't be honest about -- a single long, through-composed file (or a
-- synced set of delivery stems sharing one files.group_id) whose character changes at a
-- particular timestamp (PRD §15: SA3 has no per-clip timeline conditioning at all, so
-- there is no way to caption this except by cutting it into per-segment training clips,
-- one caption each). Exactly one of group_id/file_id is set per row: group_id means "this
-- boundary and its tags apply to every file in that stem group, at the same timestamps,
-- so a synced stem set stays synced"; file_id means a single non-stem file being
-- segmented directly. `human` reuses files.human's own JSON convention (CaptionFields.cpp)
-- verbatim, just scoped to this time range instead of the whole file.
CREATE TABLE IF NOT EXISTS segments (
    id             INTEGER PRIMARY KEY,
    group_id       TEXT,
    file_id        INTEGER REFERENCES files(id),
    start_seconds  REAL NOT NULL,
    end_seconds    REAL NOT NULL,
    human          TEXT NOT NULL DEFAULT '{}',
    created_at     INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_segments_group_id ON segments(group_id);
CREATE INDEX IF NOT EXISTS idx_segments_file_id ON segments(file_id);

-- TASKS.md Phase 3 addition: folder-level `human` defaults, for tagging a whole library
-- at once instead of one `mira tag` call per file (e.g. every file under
-- "SCORE_2026/COMEDY_CUES/" defaulting to keywords "funny, quirky"). `folder_path` is a
-- plain string prefix, matched at read time against files.path -- not resolved/
-- canonicalized against the filesystem, so it must be written the same way (relative vs
-- absolute) as the paths mira actually scanned (CLI documents this). `human` reuses the
-- same JSON convention as files.human and segments.human.
CREATE TABLE IF NOT EXISTS folder_defaults (
    id            INTEGER PRIMARY KEY,
    folder_path   TEXT UNIQUE NOT NULL,
    human         TEXT NOT NULL DEFAULT '{}',
    created_at    INTEGER NOT NULL
);

-- TASKS.md Phase 4 "segment-level analysis replacing whole-track averaging". Keyed by
-- (segment_id, file_id), NOT just segment_id -- a group_id-scoped segment (the common
-- case: a synced stem set) is *one* row in `segments` but covers *multiple* files, and
-- each stem's own audio in that time range is different, so each needs its own machine
-- JSON. Same shape as files.machine (a subset of it -- see main.cpp's
-- buildSegmentMachineJson for exactly which analyzers rerun per segment and which don't).
CREATE TABLE IF NOT EXISTS segment_analysis (
    segment_id   INTEGER NOT NULL REFERENCES segments(id),
    file_id      INTEGER NOT NULL REFERENCES files(id),
    machine      TEXT NOT NULL DEFAULT '{}',
    analyzed_at  INTEGER NOT NULL,
    PRIMARY KEY (segment_id, file_id)
);

-- TASKS.md Phase 5 (mira_ui folder tree): library roots the user has explicitly added
-- via "Add Folder" -- NOT a live browse-anywhere filesystem tree (tried, rejected in
-- the planning discussion: unrestricted system browsing is "a bit pointless", Soundly's
-- own model is a curated set of added roots, each independently browsable). The
-- sidebar's tree is built from this table, one FileTreeComponent per row, not from any
-- single fixed starting directory.
CREATE TABLE IF NOT EXISTS ui_folder_roots (
    id         INTEGER PRIMARY KEY,
    path       TEXT UNIQUE NOT NULL,
    added_at   INTEGER NOT NULL
);

-- "can we group folders just inside mira and mira's database not the real hard disk" --
-- purely organizational containers a ui_folder_roots row can be filed under
-- (ui_folder_roots.group_id, added via migration below since this table postdates it).
-- Deleting a group here never touches the filesystem or the roots inside it; it only
-- ungroups them (see Database::deleteFolderGroup).
CREATE TABLE IF NOT EXISTS ui_folder_groups (
    id         INTEGER PRIMARY KEY,
    name       TEXT NOT NULL,
    added_at   INTEGER NOT NULL
);
)SQL";

FileRecord fromRow(SQLite::Statement& q) {
    FileRecord r;
    r.id = q.getColumn("id").getInt64();
    r.path = q.getColumn("path").getString();
    r.sha256 = q.getColumn("sha256").getString();
    r.mtime = q.getColumn("mtime").getInt64();
    r.sizeBytes = q.getColumn("size_bytes").getInt64();
    r.contentType = q.getColumn("content_type").getString();
    r.contentTypeSource = q.getColumn("content_type_source").getString();
    if (!q.getColumn("group_id").isNull())
        r.groupId = q.getColumn("group_id").getString();
    if (!q.getColumn("active_ratio").isNull())
        r.activeRatio = q.getColumn("active_ratio").getDouble();
    r.activeSpans = q.getColumn("active_spans").getString();
    r.machine = q.getColumn("machine").getString();
    r.human = q.getColumn("human").getString();
    r.provenance = q.getColumn("provenance").getString();
    r.scannedAt = q.getColumn("scanned_at").getInt64();
    if (!q.getColumn("analyzed_at").isNull())
        r.analyzedAt = q.getColumn("analyzed_at").getInt64();
    return r;
}
} // namespace

Database::Database(const std::string& path)
    : db(path, SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE) {
    db.exec("PRAGMA journal_mode=WAL");
    db.exec("PRAGMA foreign_keys=ON");
    migrate();
}

void Database::migrate() {
    db.exec(kSchema);

    // Added after ui_folder_roots already shipped, so CREATE TABLE IF NOT EXISTS above
    // won't retrofit it onto an existing library.db -- "scanning is miras job not the
    // user job": tracks whether a root's most recent scan ran to completion, so mira can
    // tell a fully-indexed root apart from one interrupted mid-scan (app quit, error) and
    // knows to resume it automatically next launch rather than silently leaving it half
    // done. try/catch is the migration guard itself: SQLite has no "ADD COLUMN IF NOT
    // EXISTS", and re-running this on an already-migrated database throws "duplicate
    // column name", which is exactly the signal that this migration already happened.
    try {
        db.exec("ALTER TABLE ui_folder_roots ADD COLUMN scan_complete INTEGER NOT NULL DEFAULT 0");
    } catch (const SQLite::Exception&) {
        // already migrated
    }

    // display_name/group_id: mira-side-only sidebar organization (see ui_folder_groups'
    // schema comment above) -- "so can we rename the folder or can we group folders...
    // not the real hard disk". Both nullable: no override / no group is the default,
    // same "unmeasured, not zero" discipline the rest of Database.cpp already follows.
    try {
        db.exec("ALTER TABLE ui_folder_roots ADD COLUMN display_name TEXT");
    } catch (const SQLite::Exception&) {
        // already migrated
    }
    try {
        db.exec("ALTER TABLE ui_folder_roots ADD COLUMN group_id INTEGER REFERENCES ui_folder_groups(id)");
    } catch (const SQLite::Exception&) {
        // already migrated
    }

    // "when we import we know a dialog to choose from -- stems or sample or music"
    // (TASKS.md Phase 5) -- built-in group categories, so Add Folder can find-or-create
    // the one "Stems"/"Samples"/"Music" group instead of the user having to file each
    // new root into a group by hand every time.
    try {
        db.exec("ALTER TABLE ui_folder_groups ADD COLUMN category TEXT");
    } catch (const SQLite::Exception&) {
        // already migrated
    }

    // segments.source (review round 2, auto-segments): which rows `mira analyze` made
    // from active-region detection versus ones a person declared. Existing rows all came
    // from tag-segment, so 'manual' is the correct backfill, not just a convenient one.
    try {
        db.exec("ALTER TABLE segments ADD COLUMN source TEXT NOT NULL DEFAULT 'manual'");
    } catch (const SQLite::Exception&) {
        // already migrated
    }
}

bool Database::upsertScannedFile(const std::string& path, const std::string& sha256,
                                  int64_t mtime, int64_t sizeBytes, int64_t scannedAt) {
    SQLite::Statement existing(db, "SELECT id FROM files WHERE path = ?");
    existing.bind(1, path);
    bool isNew = !existing.executeStep();

    if (isNew) {
        SQLite::Statement insert(db,
            "INSERT INTO files (path, sha256, mtime, size_bytes, scanned_at) "
            "VALUES (?, ?, ?, ?, ?)");
        insert.bind(1, path);
        insert.bind(2, sha256);
        insert.bind(3, mtime);
        insert.bind(4, sizeBytes);
        insert.bind(5, scannedAt);
        insert.exec();
    } else {
        SQLite::Statement update(db,
            "UPDATE files SET sha256 = ?, mtime = ?, size_bytes = ?, scanned_at = ? "
            "WHERE path = ?");
        update.bind(1, sha256);
        update.bind(2, mtime);
        update.bind(3, sizeBytes);
        update.bind(4, scannedAt);
        update.bind(5, path);
        update.exec();
    }
    return isNew;
}

std::optional<FileRecord> Database::findByPath(const std::string& path) {
    SQLite::Statement q(db, "SELECT * FROM files WHERE path = ?");
    q.bind(1, path);
    if (!q.executeStep()) return std::nullopt;
    return fromRow(q);
}

std::optional<FileRecord> Database::findById(int64_t id) {
    SQLite::Statement q(db, "SELECT * FROM files WHERE id = ?");
    q.bind(1, id);
    if (!q.executeStep()) return std::nullopt;
    return fromRow(q);
}

int64_t Database::countFiles() {
    SQLite::Statement q(db, "SELECT COUNT(*) FROM files");
    q.executeStep();
    return q.getColumn(0).getInt64();
}

std::vector<std::pair<std::string, int64_t>> Database::countByContentType() {
    std::vector<std::pair<std::string, int64_t>> result;
    SQLite::Statement q(db, "SELECT content_type, COUNT(*) FROM files GROUP BY content_type ORDER BY content_type");
    while (q.executeStep()) {
        result.emplace_back(q.getColumn(0).getString(), q.getColumn(1).getInt64());
    }
    return result;
}

int64_t Database::countAnalyzed() {
    SQLite::Statement q(db, "SELECT COUNT(*) FROM files WHERE analyzed_at IS NOT NULL");
    q.executeStep();
    return q.getColumn(0).getInt64();
}

int64_t Database::countEmbeddings() {
    SQLite::Statement q(db, "SELECT COUNT(*) FROM vec_embeddings");
    q.executeStep();
    return q.getColumn(0).getInt64();
}

int64_t Database::countDclapEmbeddings() {
    SQLite::Statement q(db, "SELECT COUNT(*) FROM vec_embeddings_dclap");
    q.executeStep();
    return q.getColumn(0).getInt64();
}

int64_t Database::countWhereMachineHas(const std::string& jsonPath) {
    SQLite::Statement q(db, "SELECT COUNT(*) FROM files WHERE json_extract(machine, ?) IS NOT NULL");
    q.bind(1, jsonPath);
    q.executeStep();
    return q.getColumn(0).getInt64();
}

std::vector<FileRecord> Database::queryFiles(const std::string& whereClauseSql) {
    std::vector<FileRecord> result;
    SQLite::Statement q(db, "SELECT * FROM files WHERE " + whereClauseSql);
    while (q.executeStep()) {
        result.push_back(fromRow(q));
    }
    return result;
}

void Database::declareStem(const std::string& path) {
    SQLite::Statement update(db,
        "UPDATE files SET content_type = 'stem', content_type_source = 'declared' "
        "WHERE path = ?");
    update.bind(1, path);
    update.exec();
}

void Database::setHumanField(int64_t fileId, const std::string& jsonPath, const std::string& jsonValueJson) {
    SQLite::Statement update(db, "UPDATE files SET human = json_set(human, ?, json(?)) WHERE id = ?");
    update.bind(1, jsonPath);
    update.bind(2, jsonValueJson);
    update.bind(3, fileId);
    update.exec();
}

void Database::clearHumanFields(int64_t fileId) {
    SQLite::Statement update(db, "UPDATE files SET human = '{}' WHERE id = ?");
    update.bind(1, fileId);
    update.exec();
}

void Database::setHuman(int64_t fileId, const std::string& humanJson) {
    SQLite::Statement stmt(db, "UPDATE files SET human = json(?) WHERE id = ?");
    stmt.bind(1, humanJson);
    stmt.bind(2, fileId);
    stmt.exec();
}

void Database::setSegmentHuman(int64_t segmentId, const std::string& humanJson) {
    SQLite::Statement stmt(db, "UPDATE segments SET human = json(?) WHERE id = ?");
    stmt.bind(1, humanJson);
    stmt.bind(2, segmentId);
    stmt.exec();
}

int64_t Database::createSegmentWithId(int64_t id, std::optional<std::string> groupId,
                                       std::optional<int64_t> fileId, double startSeconds,
                                       double endSeconds, const std::string& humanJson,
                                       const std::string& source) {
    SQLite::Statement insert(db,
        "INSERT INTO segments (id, group_id, file_id, start_seconds, end_seconds, human, created_at, source) "
        "VALUES (?, ?, ?, ?, ?, json(?), ?, ?)");
    insert.bind(1, id);
    if (groupId) insert.bind(2, *groupId); else insert.bind(2);
    if (fileId) insert.bind(3, *fileId); else insert.bind(3);
    insert.bind(4, startSeconds);
    insert.bind(5, endSeconds);
    insert.bind(6, humanJson);
    insert.bind(7, static_cast<int64_t>(std::time(nullptr)));
    insert.bind(8, source);
    insert.exec();
    return id;
}

std::vector<Database::SegmentAnalysisRow> Database::segmentAnalysisRows(int64_t segmentId) {
    std::vector<SegmentAnalysisRow> out;
    SQLite::Statement q(db,
        "SELECT file_id, machine, analyzed_at FROM segment_analysis WHERE segment_id = ?");
    q.bind(1, segmentId);
    while (q.executeStep())
        out.push_back({ q.getColumn(0).getInt64(), q.getColumn(1).getString(),
                        q.getColumn(2).getInt64() });
    return out;
}

int64_t Database::createSegment(std::optional<std::string> groupId, std::optional<int64_t> fileId,
                                 double startSeconds, double endSeconds, const std::string& humanJson,
                                 const std::string& source) {
    SQLite::Statement insert(db,
        "INSERT INTO segments (group_id, file_id, start_seconds, end_seconds, human, created_at, source) "
        "VALUES (?, ?, ?, ?, json(?), ?, ?)");
    insert.bind(7, source);
    if (groupId) insert.bind(1, *groupId); else insert.bind(1);
    if (fileId) insert.bind(2, *fileId); else insert.bind(2);
    insert.bind(3, startSeconds);
    insert.bind(4, endSeconds);
    insert.bind(5, humanJson);
    insert.bind(6, static_cast<int64_t>(std::time(nullptr)));
    insert.exec();
    return db.getLastInsertRowid();
}

namespace {
SegmentRecord segmentFromRow(SQLite::Statement& q) {
    SegmentRecord s;
    s.id = q.getColumn("id").getInt64();
    if (!q.getColumn("group_id").isNull()) s.groupId = q.getColumn("group_id").getString();
    if (!q.getColumn("file_id").isNull()) s.fileId = q.getColumn("file_id").getInt64();
    s.startSeconds = q.getColumn("start_seconds").getDouble();
    s.endSeconds = q.getColumn("end_seconds").getDouble();
    s.human = q.getColumn("human").getString();
    s.createdAt = q.getColumn("created_at").getInt64();
    s.source = q.getColumn("source").getString();
    return s;
}
} // namespace

void Database::deleteUntouchedAutoSegmentsForGroup(const std::string& groupId) {
    // Mirrors deleteUntouchedAutoSegmentsForFile; see its comment for why `human = '{}'`
    // is a reliable "nobody has edited this" test.
    SQLite::Statement dropAnalysis(db,
        "DELETE FROM segment_analysis WHERE segment_id IN "
        "(SELECT id FROM segments WHERE group_id = ? AND source = 'auto' AND human = '{}')");
    dropAnalysis.bind(1, groupId);
    dropAnalysis.exec();
    SQLite::Statement drop(db, "DELETE FROM segments WHERE group_id = ? AND source = 'auto' AND human = '{}'");
    drop.bind(1, groupId);
    drop.exec();
}

std::vector<SegmentRecord> Database::findSegmentsForGroup(const std::string& groupId) {
    std::vector<SegmentRecord> result;
    SQLite::Statement q(db, "SELECT * FROM segments WHERE group_id = ? ORDER BY start_seconds");
    q.bind(1, groupId);
    while (q.executeStep()) result.push_back(segmentFromRow(q));
    return result;
}

std::vector<SegmentRecord> Database::findSegmentsForFile(int64_t fileId) {
    std::vector<SegmentRecord> result;
    SQLite::Statement q(db, "SELECT * FROM segments WHERE file_id = ? ORDER BY start_seconds");
    q.bind(1, fileId);
    while (q.executeStep()) result.push_back(segmentFromRow(q));
    return result;
}

void Database::setSegmentHumanField(int64_t segmentId, const std::string& jsonPath,
                                     const std::string& jsonValueJson) {
    SQLite::Statement update(db, "UPDATE segments SET human = json_set(human, ?, json(?)) WHERE id = ?");
    update.bind(1, jsonPath);
    update.bind(2, jsonValueJson);
    update.bind(3, segmentId);
    update.exec();
}

void Database::setSegmentBounds(int64_t segmentId, double startSeconds, double endSeconds) {
    SQLite::Statement stmt(db, "UPDATE segments SET start_seconds = ?, end_seconds = ? WHERE id = ?");
    stmt.bind(1, startSeconds);
    stmt.bind(2, endSeconds);
    stmt.bind(3, segmentId);
    stmt.exec();
}

void Database::markSegmentEdited(int64_t segmentId) {
    SQLite::Statement stmt(db,
        "UPDATE segments SET human = json_set(json(human), '$.edited', json('true')) WHERE id = ?");
    stmt.bind(1, segmentId);
    stmt.exec();
}

void Database::deleteSegment(int64_t segmentId) {
    // segment_analysis rows are keyed on (segment_id, file_id) with no ON DELETE CASCADE
    // declared, so they'd outlive the segment and be unreachable -- dropped explicitly
    // here rather than left as orphans.
    SQLite::Statement dropAnalysis(db, "DELETE FROM segment_analysis WHERE segment_id = ?");
    dropAnalysis.bind(1, segmentId);
    dropAnalysis.exec();
    SQLite::Statement drop(db, "DELETE FROM segments WHERE id = ?");
    drop.bind(1, segmentId);
    drop.exec();
}

void Database::deleteUntouchedAutoSegmentsForFile(int64_t fileId) {
    // `human = '{}'` is the "untouched" test: createSegment stores human through json(),
    // which normalises an empty object to exactly '{}', and any Edit Tags save (even one
    // that clears every field) writes keys into it -- so an edited auto segment reliably
    // stops matching and survives re-analysis.
    SQLite::Statement dropAnalysis(db,
        "DELETE FROM segment_analysis WHERE segment_id IN "
        "(SELECT id FROM segments WHERE file_id = ? AND source = 'auto' AND human = '{}')");
    dropAnalysis.bind(1, fileId);
    dropAnalysis.exec();
    SQLite::Statement drop(db, "DELETE FROM segments WHERE file_id = ? AND source = 'auto' AND human = '{}'");
    drop.bind(1, fileId);
    drop.exec();
}

std::optional<SegmentRecord> Database::findSegmentById(int64_t segmentId) {
    SQLite::Statement q(db, "SELECT * FROM segments WHERE id = ?");
    q.bind(1, segmentId);
    if (!q.executeStep()) return std::nullopt;
    return segmentFromRow(q);
}

void Database::clearSegmentHumanFields(int64_t segmentId) {
    SQLite::Statement update(db, "UPDATE segments SET human = '{}' WHERE id = ?");
    update.bind(1, segmentId);
    update.exec();
}

std::vector<FileRecord> Database::findFilesByGroupId(const std::string& groupId) {
    std::vector<FileRecord> result;
    SQLite::Statement q(db, "SELECT * FROM files WHERE group_id = ? ORDER BY path");
    q.bind(1, groupId);
    while (q.executeStep()) result.push_back(fromRow(q));
    return result;
}

void Database::upsertSegmentAnalysis(int64_t segmentId, int64_t fileId, const std::string& machineJson,
                                      int64_t analyzedAt) {
    SQLite::Statement upsert(db,
        "INSERT INTO segment_analysis (segment_id, file_id, machine, analyzed_at) VALUES (?, ?, json(?), ?) "
        "ON CONFLICT(segment_id, file_id) DO UPDATE SET machine = excluded.machine, "
        "analyzed_at = excluded.analyzed_at");
    upsert.bind(1, segmentId);
    upsert.bind(2, fileId);
    upsert.bind(3, machineJson);
    upsert.bind(4, analyzedAt);
    upsert.exec();
}

std::optional<std::string> Database::getSegmentMachine(int64_t segmentId, int64_t fileId) {
    SQLite::Statement q(db, "SELECT machine FROM segment_analysis WHERE segment_id = ? AND file_id = ?");
    q.bind(1, segmentId);
    q.bind(2, fileId);
    if (!q.executeStep()) return std::nullopt;
    return q.getColumn(0).getString();
}

void Database::setFolderDefaultField(const std::string& folderPath, const std::string& jsonPath,
                                      const std::string& jsonValueJson) {
    SQLite::Statement upsert(db,
        "INSERT INTO folder_defaults (folder_path, human, created_at) "
        "VALUES (?, json_set('{}', ?, json(?)), ?) "
        "ON CONFLICT(folder_path) DO UPDATE SET human = json_set(human, ?, json(?))");
    upsert.bind(1, folderPath);
    upsert.bind(2, jsonPath);
    upsert.bind(3, jsonValueJson);
    upsert.bind(4, static_cast<int64_t>(std::time(nullptr)));
    upsert.bind(5, jsonPath);
    upsert.bind(6, jsonValueJson);
    upsert.exec();
}

void Database::clearFolderDefault(const std::string& folderPath) {
    SQLite::Statement del(db, "DELETE FROM folder_defaults WHERE folder_path = ?");
    del.bind(1, folderPath);
    del.exec();
}

std::vector<std::string> Database::findFolderDefaultsForPath(const std::string& filePath) {
    std::vector<std::pair<std::string, std::string>> matches; // (folderPath, human)
    SQLite::Statement q(db, "SELECT folder_path, human FROM folder_defaults");
    while (q.executeStep()) {
        std::string folderPath = q.getColumn(0).getString();
        std::string human = q.getColumn(1).getString();
        // Plain string-prefix match with a directory-boundary check, so "SCORE_2" can't
        // false-match "SCORE_20/..." -- not filesystem-canonicalized (see the schema
        // comment: folderPath must already be written the same way, relative vs
        // absolute, as the scanned file paths).
        if (filePath.size() >= folderPath.size() &&
            filePath.compare(0, folderPath.size(), folderPath) == 0 &&
            (filePath.size() == folderPath.size() || filePath[folderPath.size()] == '/')) {
            matches.emplace_back(std::move(folderPath), std::move(human));
        }
    }
    std::sort(matches.begin(), matches.end(),
              [](const auto& a, const auto& b) { return a.first.size() < b.first.size(); });
    std::vector<std::string> result;
    result.reserve(matches.size());
    for (auto& m : matches) result.push_back(std::move(m.second));
    return result;
}

std::vector<std::pair<double, double>> Database::parseActiveSpans(const std::string& activeSpansJson) {
    std::vector<std::pair<double, double>> spans;
    auto len = jsonArrayLength(activeSpansJson, "$");
    if (!len) return spans;
    for (int64_t i = 0; i < *len; ++i) {
        std::ostringstream startPath, endPath;
        startPath << "$[" << i << "][0]";
        endPath << "$[" << i << "][1]";
        auto start = jsonExtractDouble(activeSpansJson, startPath.str());
        auto end = jsonExtractDouble(activeSpansJson, endPath.str());
        if (start && end) spans.emplace_back(*start, *end);
    }
    return spans;
}

std::vector<Database::ChordChange> Database::parseChords(const std::string& machineJson) {
    std::vector<ChordChange> chords;
    SQLite::Statement q(db,
        "SELECT json_extract(je.value, '$.t'), json_extract(je.value, '$.chord') "
        "FROM json_each(?, '$.chords') je");
    q.bind(1, machineJson);
    while (q.executeStep()) {
        if (q.getColumn(0).isNull() || q.getColumn(1).isNull()) continue;
        chords.push_back({ q.getColumn(0).getDouble(), q.getColumn(1).getString() });
    }
    return chords;
}

std::vector<Database::NoteEvent> Database::parseNotes(const std::string& machineJson) {
    std::vector<NoteEvent> notes;
    SQLite::Statement q(db,
        "SELECT json_extract(je.value, '$.start'), json_extract(je.value, '$.end'), "
        "       json_extract(je.value, '$.pitch'), json_extract(je.value, '$.amplitude') "
        "FROM json_each(?, '$.notes') je");
    q.bind(1, machineJson);
    while (q.executeStep()) {
        if (q.getColumn(0).isNull() || q.getColumn(1).isNull() || q.getColumn(2).isNull()) continue;
        notes.push_back({ q.getColumn(0).getDouble(), q.getColumn(1).getDouble(),
                           q.getColumn(2).getInt(),
                           q.getColumn(3).isNull() ? 1.0 : q.getColumn(3).getDouble() });
    }
    return notes;
}

std::vector<Database::AnalyzedSibling> Database::findAnalyzedSiblingsInDir(const std::string& parentDir) {
    // substr(path, 1, n) = dir || '/' is an exact prefix test that survives folder names
    // containing LIKE metacharacters, and deliberately excludes deeper subdirectories:
    // siblingKey is parent-directory-scoped, so a nested folder is a different set.
    std::vector<AnalyzedSibling> siblings;
    SQLite::Statement q(db,
        "SELECT id, path, group_id, json_extract(machine, '$.duration_seconds') "
        "FROM files "
        "WHERE analyzed_at IS NOT NULL "
        "  AND substr(path, 1, ?) = ? "
        "  AND instr(substr(path, ? + 1), '/') = 0");
    auto prefix = parentDir + "/";
    q.bind(1, static_cast<int64_t>(prefix.size()));
    q.bind(2, prefix);
    q.bind(3, static_cast<int64_t>(prefix.size()));
    while (q.executeStep()) {
        if (q.getColumn(3).isNull()) continue; // analyzed before duration was stored
        AnalyzedSibling s;
        s.id = q.getColumn(0).getInt64();
        s.path = q.getColumn(1).getString();
        if (!q.getColumn(2).isNull()) s.groupId = q.getColumn(2).getString();
        s.durationSeconds = q.getColumn(3).getDouble();
        siblings.push_back(std::move(s));
    }
    return siblings;
}

void Database::setGroupId(int64_t fileId, const std::string& groupId) {
    SQLite::Statement stmt(db, "UPDATE files SET group_id = ? WHERE id = ?");
    stmt.bind(1, groupId);
    stmt.bind(2, fileId);
    stmt.exec();
}

std::vector<FileRecord> Database::findFilesForAnalysis(bool force,
                                                         std::optional<std::string> contentTypeFilter,
                                                         std::optional<int> limit) {
    std::vector<std::string> clauses;
    if (!force) clauses.push_back("analyzed_at IS NULL");
    if (contentTypeFilter) clauses.push_back("content_type = ?");

    std::string sql = "SELECT * FROM files";
    if (!clauses.empty()) {
        sql += " WHERE " + clauses[0];
        for (size_t i = 1; i < clauses.size(); ++i) sql += " AND " + clauses[i];
    }
    if (limit) sql += " LIMIT ?";

    SQLite::Statement q(db, sql);
    int bindIndex = 1;
    if (contentTypeFilter) q.bind(bindIndex++, *contentTypeFilter);
    if (limit) q.bind(bindIndex++, *limit);

    std::vector<FileRecord> results;
    while (q.executeStep()) results.push_back(fromRow(q));
    return results;
}

std::optional<std::string> Database::jsonExtractString(const std::string& json,
                                                         const std::string& path) {
    SQLite::Statement q(db, "SELECT json_extract(?, ?)");
    q.bind(1, json);
    q.bind(2, path);
    if (!q.executeStep() || q.getColumn(0).isNull()) return std::nullopt;
    return q.getColumn(0).getString();
}

std::optional<double> Database::jsonExtractDouble(const std::string& json,
                                                    const std::string& path) {
    SQLite::Statement q(db, "SELECT json_extract(?, ?)");
    q.bind(1, json);
    q.bind(2, path);
    if (!q.executeStep() || q.getColumn(0).isNull()) return std::nullopt;
    return q.getColumn(0).getDouble();
}

std::optional<int64_t> Database::jsonArrayLength(const std::string& json,
                                                   const std::string& path) {
    SQLite::Statement q(db, "SELECT json_array_length(?, ?)");
    q.bind(1, json);
    q.bind(2, path);
    if (!q.executeStep() || q.getColumn(0).isNull()) return std::nullopt;
    return q.getColumn(0).getInt64();
}

void Database::addFolderRoot(const std::string& path) {
    SQLite::Statement ins(db, "INSERT OR IGNORE INTO ui_folder_roots (path, added_at) VALUES (?, ?)");
    ins.bind(1, path);
    ins.bind(2, static_cast<int64_t>(std::time(nullptr)));
    ins.exec();
}

void Database::removeFolderRoot(const std::string& path) {
    SQLite::Statement del(db, "DELETE FROM ui_folder_roots WHERE path = ?");
    del.bind(1, path);
    del.exec();
}

std::vector<std::string> Database::listFolderRoots() {
    std::vector<std::string> result;
    SQLite::Statement q(db, "SELECT path FROM ui_folder_roots ORDER BY added_at");
    while (q.executeStep()) result.push_back(q.getColumn(0).getString());
    return result;
}

void Database::setFolderRootScanComplete(const std::string& path, bool complete) {
    SQLite::Statement upd(db, "UPDATE ui_folder_roots SET scan_complete = ? WHERE path = ?");
    upd.bind(1, complete ? 1 : 0);
    upd.bind(2, path);
    upd.exec();
}

// Roots whose most recent scan never finished (app quit mid-scan, an error, or a root
// that's simply never been scanned at all since being added) — mira_ui resumes these
// automatically at startup, since "scanning is mira's job not the user's job".
std::vector<std::string> Database::listIncompleteFolderRoots() {
    std::vector<std::string> result;
    SQLite::Statement q(db, "SELECT path FROM ui_folder_roots WHERE scan_complete = 0 ORDER BY added_at");
    while (q.executeStep()) result.push_back(q.getColumn(0).getString());
    return result;
}

std::vector<Database::FolderRootInfo> Database::listFolderRootInfos() {
    std::vector<FolderRootInfo> result;
    SQLite::Statement q(db, "SELECT path, display_name, group_id FROM ui_folder_roots ORDER BY added_at");
    while (q.executeStep()) {
        FolderRootInfo info;
        info.path = q.getColumn(0).getString();
        if (!q.getColumn(1).isNull()) info.displayName = q.getColumn(1).getString();
        if (!q.getColumn(2).isNull()) info.groupId = q.getColumn(2).getInt64();
        result.push_back(std::move(info));
    }
    return result;
}

void Database::setFolderRootDisplayName(const std::string& path, const std::optional<std::string>& displayName) {
    SQLite::Statement upd(db, "UPDATE ui_folder_roots SET display_name = ? WHERE path = ?");
    if (displayName) upd.bind(1, *displayName); else upd.bind(1);
    upd.bind(2, path);
    upd.exec();
}

void Database::setFolderRootGroup(const std::string& path, const std::optional<int64_t>& groupId) {
    SQLite::Statement upd(db, "UPDATE ui_folder_roots SET group_id = ? WHERE path = ?");
    if (groupId) upd.bind(1, *groupId); else upd.bind(1);
    upd.bind(2, path);
    upd.exec();
}

int64_t Database::createFolderGroup(const std::string& name, const std::optional<std::string>& category) {
    SQLite::Statement ins(db, "INSERT INTO ui_folder_groups (name, added_at, category) VALUES (?, ?, ?)");
    ins.bind(1, name);
    ins.bind(2, static_cast<int64_t>(std::time(nullptr)));
    if (category) ins.bind(3, *category); else ins.bind(3);
    ins.exec();
    return db.getLastInsertRowid();
}

void Database::renameFolderGroup(int64_t groupId, const std::string& name) {
    SQLite::Statement upd(db, "UPDATE ui_folder_groups SET name = ? WHERE id = ?");
    upd.bind(1, name);
    upd.bind(2, groupId);
    upd.exec();
}

void Database::deleteFolderGroup(int64_t groupId) {
    SQLite::Statement ungroup(db, "UPDATE ui_folder_roots SET group_id = NULL WHERE group_id = ?");
    ungroup.bind(1, groupId);
    ungroup.exec();
    SQLite::Statement del(db, "DELETE FROM ui_folder_groups WHERE id = ?");
    del.bind(1, groupId);
    del.exec();
}

std::vector<Database::FolderGroup> Database::listFolderGroups() {
    std::vector<FolderGroup> result;
    SQLite::Statement q(db, "SELECT id, name, category FROM ui_folder_groups ORDER BY added_at");
    while (q.executeStep()) {
        FolderGroup g;
        g.id = q.getColumn(0).getInt64();
        g.name = q.getColumn(1).getString();
        if (!q.getColumn(2).isNull()) g.category = q.getColumn(2).getString();
        result.push_back(std::move(g));
    }
    return result;
}

std::optional<Database::FolderGroup> Database::findFolderGroupByCategory(const std::string& category) {
    SQLite::Statement q(db, "SELECT id, name, category FROM ui_folder_groups WHERE category = ? ORDER BY added_at LIMIT 1");
    q.bind(1, category);
    if (!q.executeStep()) return std::nullopt;
    FolderGroup g;
    g.id = q.getColumn(0).getInt64();
    g.name = q.getColumn(1).getString();
    g.category = q.getColumn(2).getString();
    return g;
}

std::optional<Database::FolderGroup> Database::findFolderGroupByName(const std::string& name) {
    SQLite::Statement q(db, "SELECT id, name, category FROM ui_folder_groups WHERE name = ? COLLATE NOCASE "
                            "ORDER BY added_at LIMIT 1");
    q.bind(1, name);
    if (!q.executeStep()) return std::nullopt;
    FolderGroup g;
    g.id = q.getColumn(0).getInt64();
    g.name = q.getColumn(1).getString();
    if (!q.getColumn(2).isNull()) g.category = q.getColumn(2).getString();
    return g;
}

void Database::setFolderGroupCategory(int64_t groupId, const std::string& category) {
    SQLite::Statement upd(db, "UPDATE ui_folder_groups SET category = ? WHERE id = ?");
    upd.bind(1, category);
    upd.bind(2, groupId);
    upd.exec();
}

std::optional<std::string> Database::jsonObjectTopKey(const std::string& json, const std::string& path) {
    SQLite::Statement q(db,
        "SELECT je.key FROM json_each(?, ?) je ORDER BY je.value DESC LIMIT 1");
    q.bind(1, json);
    q.bind(2, path);
    if (!q.executeStep() || q.getColumn(0).isNull()) return std::nullopt;
    return q.getColumn(0).getString();
}

std::vector<std::pair<std::string, double>> Database::jsonObjectEntries(const std::string& json,
                                                                          const std::string& path, int limit) {
    SQLite::Statement q(db, "SELECT je.key, je.value FROM json_each(?, ?) je ORDER BY je.value DESC LIMIT ?");
    q.bind(1, json);
    q.bind(2, path);
    q.bind(3, limit);
    std::vector<std::pair<std::string, double>> result;
    while (q.executeStep()) result.emplace_back(q.getColumn(0).getString(), q.getColumn(1).getDouble());
    return result;
}

std::vector<std::string> Database::jsonStringArray(const std::string& json, const std::string& path) {
    SQLite::Statement q(db, "SELECT je.value FROM json_each(?, ?) je");
    q.bind(1, json);
    q.bind(2, path);
    std::vector<std::string> result;
    while (q.executeStep()) result.push_back(q.getColumn(0).getString());
    return result;
}

std::vector<double> Database::jsonDoubleArray(const std::string& json, const std::string& path) {
    SQLite::Statement q(db, "SELECT je.value FROM json_each(?, ?) je");
    q.bind(1, json);
    q.bind(2, path);
    std::vector<double> result;
    while (q.executeStep()) result.push_back(q.getColumn(0).getDouble());
    return result;
}

void Database::applyAnalysis(const AnalysisUpdate& update) {
    SQLite::Statement stmt(db,
        "UPDATE files SET "
        "content_type = COALESCE(?, content_type), "
        "content_type_source = CASE WHEN ? IS NOT NULL THEN 'router' ELSE content_type_source END, "
        "group_id = COALESCE(?, group_id), "
        "active_ratio = COALESCE(?, active_ratio), "
        "active_spans = COALESCE(?, active_spans), "
        "machine = json_patch(machine, ?), "
        "provenance = ?, "
        "analyzed_at = ? "
        "WHERE id = ?");

    if (update.contentType) { stmt.bind(1, *update.contentType); stmt.bind(2, *update.contentType); }
    else { stmt.bind(1); stmt.bind(2); }

    if (update.groupId) stmt.bind(3, *update.groupId);
    else stmt.bind(3);

    if (update.activeRatio) stmt.bind(4, *update.activeRatio);
    else stmt.bind(4);

    if (update.activeSpansJson) stmt.bind(5, *update.activeSpansJson);
    else stmt.bind(5);

    stmt.bind(6, update.machineJson);
    stmt.bind(7, update.provenanceJson);
    stmt.bind(8, update.analyzedAt);
    stmt.bind(9, update.id);
    stmt.exec();
}

void Database::upsertEmbedding(int64_t fileId, const std::vector<float>& embedding) {
    if (embedding.size() != 1280) return; // caller's bug — Embedding.h's contract, not silently coerced
    // vec0 tables don't support UPDATE/INSERT OR REPLACE on the same rowid directly in
    // every sqlite-vec version — delete-then-insert is the documented-safe pattern.
    SQLite::Statement del(db, "DELETE FROM vec_embeddings WHERE rowid = ?");
    del.bind(1, fileId);
    del.exec();

    SQLite::Statement ins(db, "INSERT INTO vec_embeddings(rowid, embedding) VALUES (?, ?)");
    ins.bind(1, fileId);
    ins.bind(2, embedding.data(), static_cast<int>(embedding.size() * sizeof(float)));
    ins.exec();
}

std::optional<std::vector<float>> Database::getEmbeddingById(int64_t fileId) {
    SQLite::Statement q(db, "SELECT embedding FROM vec_embeddings WHERE rowid = ?");
    q.bind(1, fileId);
    if (!q.executeStep()) return std::nullopt;

    const void* blob = q.getColumn(0).getBlob();
    int bytes = q.getColumn(0).getBytes();
    if (bytes != 1280 * static_cast<int>(sizeof(float))) return std::nullopt;

    std::vector<float> embedding(1280);
    std::memcpy(embedding.data(), blob, bytes);
    return embedding;
}

std::vector<Database::SimilarMatch> Database::findSimilar(const std::vector<float>& embedding, int topK,
                                                            std::optional<int64_t> excludeId) {
    std::vector<SimilarMatch> results;
    if (embedding.size() != 1280) return results;

    // Over-fetch by one when excluding a row, since that row (typically the query file
    // itself, at distance 0) would otherwise consume one of the topK slots.
    int fetchK = excludeId ? topK + 1 : topK;
    SQLite::Statement q(db,
        "SELECT rowid, distance FROM vec_embeddings WHERE embedding MATCH ? AND k = ? ORDER BY distance");
    q.bind(1, embedding.data(), static_cast<int>(embedding.size() * sizeof(float)));
    q.bind(2, fetchK);

    while (q.executeStep()) {
        int64_t rowId = q.getColumn(0).getInt64();
        if (excludeId && rowId == *excludeId) continue;
        results.push_back({rowId, q.getColumn(1).getDouble()});
        if (static_cast<int>(results.size()) >= topK) break;
    }
    return results;
}

void Database::upsertDclapEmbedding(int64_t fileId, const std::vector<float>& embedding) {
    if (embedding.size() != 512) return; // caller's bug — DclapEmbedding.h's contract
    SQLite::Statement del(db, "DELETE FROM vec_embeddings_dclap WHERE rowid = ?");
    del.bind(1, fileId);
    del.exec();

    SQLite::Statement ins(db, "INSERT INTO vec_embeddings_dclap(rowid, embedding) VALUES (?, ?)");
    ins.bind(1, fileId);
    ins.bind(2, embedding.data(), static_cast<int>(embedding.size() * sizeof(float)));
    ins.exec();
}

std::optional<std::vector<float>> Database::getDclapEmbeddingById(int64_t fileId) {
    SQLite::Statement q(db, "SELECT embedding FROM vec_embeddings_dclap WHERE rowid = ?");
    q.bind(1, fileId);
    if (!q.executeStep()) return std::nullopt;

    const void* blob = q.getColumn(0).getBlob();
    int bytes = q.getColumn(0).getBytes();
    if (bytes != 512 * static_cast<int>(sizeof(float))) return std::nullopt;

    std::vector<float> embedding(512);
    std::memcpy(embedding.data(), blob, bytes);
    return embedding;
}

std::vector<Database::SimilarMatch> Database::findSimilarDclap(const std::vector<float>& embedding, int topK,
                                                                 std::optional<int64_t> excludeId) {
    std::vector<SimilarMatch> results;
    if (embedding.size() != 512) return results;

    int fetchK = excludeId ? topK + 1 : topK;
    SQLite::Statement q(db,
        "SELECT rowid, distance FROM vec_embeddings_dclap WHERE embedding MATCH ? AND k = ? ORDER BY distance");
    q.bind(1, embedding.data(), static_cast<int>(embedding.size() * sizeof(float)));
    q.bind(2, fetchK);

    while (q.executeStep()) {
        int64_t rowId = q.getColumn(0).getInt64();
        if (excludeId && rowId == *excludeId) continue;
        results.push_back({rowId, q.getColumn(1).getDouble()});
        if (static_cast<int>(results.size()) >= topK) break;
    }
    return results;
}

void Database::upsertTimbre(int64_t fileId, const std::vector<float>& mfcc) {
    if (mfcc.size() != 13) return; // caller's bug — Descriptors.h's kNumMfccCoefficients
    SQLite::Statement del(db, "DELETE FROM vec_timbre WHERE rowid = ?");
    del.bind(1, fileId);
    del.exec();
    SQLite::Statement ins(db, "INSERT INTO vec_timbre(rowid, embedding) VALUES (?, ?)");
    ins.bind(1, fileId);
    ins.bind(2, mfcc.data(), static_cast<int>(mfcc.size() * sizeof(float)));
    ins.exec();
}

std::optional<std::vector<float>> Database::getTimbreById(int64_t fileId) {
    SQLite::Statement q(db, "SELECT embedding FROM vec_timbre WHERE rowid = ?");
    q.bind(1, fileId);
    if (!q.executeStep()) return std::nullopt;
    const void* blob = q.getColumn(0).getBlob();
    int bytes = q.getColumn(0).getBytes();
    if (bytes != 13 * static_cast<int>(sizeof(float))) return std::nullopt;
    std::vector<float> v(13);
    std::memcpy(v.data(), blob, bytes);
    return v;
}

std::vector<Database::SimilarMatch> Database::findSimilarTimbre(const std::vector<float>& mfcc, int topK,
                                                                   std::optional<int64_t> excludeId) {
    std::vector<SimilarMatch> results;
    if (mfcc.size() != 13) return results;
    int fetchK = excludeId ? topK + 1 : topK;
    SQLite::Statement q(db,
        "SELECT rowid, distance FROM vec_timbre WHERE embedding MATCH ? AND k = ? ORDER BY distance");
    q.bind(1, mfcc.data(), static_cast<int>(mfcc.size() * sizeof(float)));
    q.bind(2, fetchK);
    while (q.executeStep()) {
        int64_t rowId = q.getColumn(0).getInt64();
        if (excludeId && rowId == *excludeId) continue;
        results.push_back({rowId, q.getColumn(1).getDouble()});
        if (static_cast<int>(results.size()) >= topK) break;
    }
    return results;
}

void Database::upsertSpectrum(int64_t fileId, const std::vector<float>& centroidFlatness) {
    if (centroidFlatness.size() != 2) return; // caller's bug — [centroid, flatness]
    SQLite::Statement del(db, "DELETE FROM vec_spectrum WHERE rowid = ?");
    del.bind(1, fileId);
    del.exec();
    SQLite::Statement ins(db, "INSERT INTO vec_spectrum(rowid, embedding) VALUES (?, ?)");
    ins.bind(1, fileId);
    ins.bind(2, centroidFlatness.data(), static_cast<int>(centroidFlatness.size() * sizeof(float)));
    ins.exec();
}

std::optional<std::vector<float>> Database::getSpectrumById(int64_t fileId) {
    SQLite::Statement q(db, "SELECT embedding FROM vec_spectrum WHERE rowid = ?");
    q.bind(1, fileId);
    if (!q.executeStep()) return std::nullopt;
    const void* blob = q.getColumn(0).getBlob();
    int bytes = q.getColumn(0).getBytes();
    if (bytes != 2 * static_cast<int>(sizeof(float))) return std::nullopt;
    std::vector<float> v(2);
    std::memcpy(v.data(), blob, bytes);
    return v;
}

std::vector<Database::SimilarMatch> Database::findSimilarSpectrum(const std::vector<float>& centroidFlatness,
                                                                     int topK, std::optional<int64_t> excludeId) {
    std::vector<SimilarMatch> results;
    if (centroidFlatness.size() != 2) return results;
    int fetchK = excludeId ? topK + 1 : topK;
    SQLite::Statement q(db,
        "SELECT rowid, distance FROM vec_spectrum WHERE embedding MATCH ? AND k = ? ORDER BY distance");
    q.bind(1, centroidFlatness.data(), static_cast<int>(centroidFlatness.size() * sizeof(float)));
    q.bind(2, fetchK);
    while (q.executeStep()) {
        int64_t rowId = q.getColumn(0).getInt64();
        if (excludeId && rowId == *excludeId) continue;
        results.push_back({rowId, q.getColumn(1).getDouble()});
        if (static_cast<int>(results.size()) >= topK) break;
    }
    return results;
}

std::vector<Database::SimilarMatch> Database::findSimilarByBpm(double bpm, int topK,
                                                                  std::optional<int64_t> excludeId) {
    std::vector<SimilarMatch> results;
    if (bpm <= 0.0) return results;
    SQLite::Statement q(db,
        "SELECT id, ABS(json_extract(machine,'$.rhythm.beat_this_bpm') - ?) AS d "
        "FROM files "
        "WHERE content_type != 'one_shot' "
        "AND json_extract(machine,'$.rhythm.beat_this_bpm') IS NOT NULL "
        "AND json_extract(machine,'$.rhythm.beat_this_bpm') > 0 "
        "ORDER BY d");
    q.bind(1, bpm);
    while (q.executeStep()) {
        int64_t rowId = q.getColumn(0).getInt64();
        if (excludeId && rowId == *excludeId) continue;
        results.push_back({rowId, q.getColumn(1).getDouble()});
        if (static_cast<int>(results.size()) >= topK) break;
    }
    return results;
}

} // namespace mira
