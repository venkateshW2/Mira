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

int64_t Database::createSegment(std::optional<std::string> groupId, std::optional<int64_t> fileId,
                                 double startSeconds, double endSeconds, const std::string& humanJson) {
    SQLite::Statement insert(db,
        "INSERT INTO segments (group_id, file_id, start_seconds, end_seconds, human, created_at) "
        "VALUES (?, ?, ?, ?, json(?), ?)");
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
    return s;
}
} // namespace

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

std::vector<FileRecord> Database::findFilesByGroupId(const std::string& groupId) {
    std::vector<FileRecord> result;
    SQLite::Statement q(db, "SELECT * FROM files WHERE group_id = ? ORDER BY path");
    q.bind(1, groupId);
    while (q.executeStep()) result.push_back(fromRow(q));
    return result;
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
