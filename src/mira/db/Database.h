#pragma once

#include <SQLiteCpp/SQLiteCpp.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

    std::optional<FileRecord> findByPath(const std::string& path);
    std::optional<FileRecord> findById(int64_t id);

    int64_t countFiles();

    // Sets content_type='stem', content_type_source='declared' for a path — PRD §12.3
    // route 3, always overrides router-based detection and is never re-routed.
    void declareStem(const std::string& path);

    // Rows `mira analyze` should (re-)process: declared stems ARE included (they still
    // need active-region detection and everything after it — declaration only skips the
    // *routing* decision, PRD §12.3) — and unless `force`, only rows never analyzed.
    std::vector<FileRecord> findFilesForAnalysis(bool force);

    struct AnalysisUpdate {
        int64_t id = 0;
        // nullopt = leave content_type/content_type_source untouched (a declared stem —
        // its content type was never in question). Set = a router decision; always
        // written with content_type_source='router'.
        std::optional<std::string> contentType;
        std::optional<std::string> groupId;
        std::optional<double> activeRatio;
        std::optional<std::string> activeSpansJson;
        std::string machineJson = "{}";   // merged into `machine`, not replacing other fields
        int64_t analyzedAt = 0;
    };
    void applyAnalysis(const AnalysisUpdate& update);

    // Small JSON helpers for reading `machine` (mira inspect's job) — mira has no C++
    // JSON parser vendored (kept off the dependency list deliberately), so these lean on
    // SQLite's own json_extract/json_array_length instead of parsing in C++.
    std::optional<std::string> jsonExtractString(const std::string& json, const std::string& path);
    std::optional<double> jsonExtractDouble(const std::string& json, const std::string& path);
    std::optional<int64_t> jsonArrayLength(const std::string& json, const std::string& path);

private:
    void migrate();

    SQLite::Database db;
};

} // namespace mira
