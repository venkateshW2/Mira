#pragma once

#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdint>
#include <optional>
#include <string>

namespace mira {

// One row per scanned file (PRD §6). `machine` is rewritten wholesale on every
// re-analysis; `human` is never touched by mira itself (PRD §6's machine/human split).
struct FileRecord {
    int64_t id = 0;
    std::string path;
    std::string sha256;
    int64_t mtime = 0;
    int64_t sizeBytes = 0;
    std::string contentType = "unknown";       // one_shot | loop | track | stem | unknown
    std::string contentTypeSource = "router";   // router | declared | human
    std::optional<std::string> groupId;
    std::optional<double> activeRatio;
    std::string activeSpans = "[]";             // JSON array of [start, end] seconds
    std::string machine = "{}";                 // JSON, machine-derived, rewritten wholesale
    std::string human = "{}";                   // JSON, human overrides, never auto-written
    std::string provenance = "{}";              // JSON: mira/essentia version, model versions, ts
    int64_t scannedAt = 0;
    std::optional<int64_t> analyzedAt;
};

// Wraps the mira SQLite database: schema creation and the file-table operations the
// scanner and (later) the analyzer need. Deliberately thin — SQLiteCpp already keeps
// sqlite3* out of view (PRD §7); this doesn't hide it further than that.
class Database {
public:
    explicit Database(const std::string& path);

    // Insert a new file row, or update path/mtime/sizeBytes/sha256/scannedAt for an
    // existing one (matched by path). Never touches content_type, machine, human, or
    // provenance — those belong to the analyzer, not the scanner. Returns true if a new
    // row was inserted, false if an existing row was updated.
    bool upsertScannedFile(const std::string& path, const std::string& sha256,
                            int64_t mtime, int64_t sizeBytes, int64_t scannedAt);

    // True if a row for this path already has this exact sha256 + mtime — i.e. nothing
    // about the file has changed since it was last scanned.
    bool isUnchanged(const std::string& path, const std::string& sha256, int64_t mtime);

    std::optional<FileRecord> findByPath(const std::string& path);
    std::optional<FileRecord> findById(int64_t id);

    int64_t countFiles();

private:
    void migrate();

    SQLite::Database db;
};

} // namespace mira
