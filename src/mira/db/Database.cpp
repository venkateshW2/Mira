#include "Database.h"

#include <cstring>

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

} // namespace mira
