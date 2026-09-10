#include "Database.h"

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

bool Database::isUnchanged(const std::string& path, const std::string& sha256, int64_t mtime) {
    SQLite::Statement q(db, "SELECT 1 FROM files WHERE path = ? AND sha256 = ? AND mtime = ?");
    q.bind(1, path);
    q.bind(2, sha256);
    q.bind(3, mtime);
    return q.executeStep();
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

} // namespace mira
