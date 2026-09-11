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

// A time-ranged caption boundary, layered on top of `human` (PRD §15's "no per-clip
// timeline conditioning" limitation -- see Database.cpp's schema comment on the
// `segments` table for the full rationale). Exactly one of groupId/fileId is set.
struct SegmentRecord {
    int64_t id = 0;
    std::optional<std::string> groupId;
    std::optional<int64_t> fileId;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    std::string human = "{}";
    int64_t createdAt = 0;
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

    // PRD §8 `mira stats` support. `countByContentType` groups the existing content_type
    // column; `countAnalyzed` is files with analyzed_at set; `countEmbeddings` reads
    // vec_embeddings directly (not machine JSON — same reasoning as getEmbeddingById);
    // `countWhereMachineHas` is a generic "how many rows have this JSON path set"
    // (coverage per classification head — e.g. "$.genre", "$.stem_instrument").
    std::vector<std::pair<std::string, int64_t>> countByContentType();
    int64_t countAnalyzed();
    int64_t countEmbeddings();
    int64_t countDclapEmbeddings();
    int64_t countWhereMachineHas(const std::string& jsonPath);

    // PRD §8 `mira search`. `whereClauseSql` is a caller-built SQL boolean expression
    // (mira search's own small filter-expression translator builds this from a fixed
    // grammar over a known field table — not raw passthrough of arbitrary user SQL),
    // evaluated as `SELECT ... FROM files WHERE <whereClauseSql>`.
    std::vector<FileRecord> queryFiles(const std::string& whereClauseSql);

    // Sets content_type='stem', content_type_source='declared' for a path — PRD §12.3
    // route 3, always overrides router-based detection and is never re-routed.
    void declareStem(const std::string& path);

    // PRD §6/§11: `human` overrides -- the one column mira's own analysis never writes,
    // reserved for a person's edits (CaptionFields.cpp documents the specific top-level
    // keys it reads back out of this: genre/instruments/moods/keywords/bpm/key/
    // is_instrumental). `setHumanField` merges one key into the existing `human` object
    // via SQLite's own json_set/json() (never touches any other key already set there);
    // `jsonValueJson` must already be valid JSON text (a quoted string, array, number, or
    // bool) -- the caller's responsibility, same discipline as queryFiles's
    // whereClauseSql. `clearHumanFields` resets the whole object back to '{}'.
    void setHumanField(int64_t fileId, const std::string& jsonPath, const std::string& jsonValueJson);
    void clearHumanFields(int64_t fileId);

    // Segment tagging (TASKS.md Phase 3 addition; see Database.cpp's `segments` schema
    // comment). `createSegment` takes exactly one of groupId/fileId (caller's
    // responsibility -- the CLI resolves which one applies before calling this) and an
    // already-built `human` JSON object (CaptionFields.cpp's convention, same as files.
    // human). `findFilesByGroupId` is how a caller turns a group_id back into the actual
    // set of sibling stem files to cut.
    int64_t createSegment(std::optional<std::string> groupId, std::optional<int64_t> fileId,
                           double startSeconds, double endSeconds, const std::string& humanJson);
    std::vector<SegmentRecord> findSegmentsForGroup(const std::string& groupId);
    std::vector<SegmentRecord> findSegmentsForFile(int64_t fileId);
    std::vector<FileRecord> findFilesByGroupId(const std::string& groupId);

    // Folder-level `human` defaults (TASKS.md Phase 3 addition; see Database.cpp's
    // `folder_defaults` schema comment). `setFolderDefaultField` merges one key into the
    // folder's `human` object (creating the row if it doesn't exist yet), same
    // one-key-at-a-time semantics as `setHumanField`. `findFolderDefaultsForPath` returns
    // every stored folder default whose path is a prefix of `filePath`, ordered
    // shortest-to-longest (root-most ancestor first) -- callers apply them in that order
    // so a more specific folder's default overrides a shallower ancestor's for the same
    // field, exactly like `human` already overrides machine-derived values.
    void setFolderDefaultField(const std::string& folderPath, const std::string& jsonPath,
                                const std::string& jsonValueJson);
    void clearFolderDefault(const std::string& folderPath);
    std::vector<std::string> findFolderDefaultsForPath(const std::string& filePath);

    // Reads back the `[[start,end],...]` active-span array Router/ActiveRegions.cpp
    // writes into files.active_spans -- same lean-on-SQLite's-own-json approach as
    // jsonExtractDouble/jsonArrayLength above, not a C++ JSON parser.
    std::vector<std::pair<double, double>> parseActiveSpans(const std::string& activeSpansJson);

    // Rows `mira analyze` should (re-)process: declared stems ARE included (they still
    // need active-region detection and everything after it — declaration only skips the
    // *routing* decision, PRD §12.3) — and unless `force`, only rows never analyzed.
    // `contentTypeFilter` (only meaningful with `force`, since otherwise unrouted rows
    // are all still 'unknown') restricts to a single existing content_type; `limit` caps
    // how many rows come back, both per PRD §8's `analyze` flags.
    std::vector<FileRecord> findFilesForAnalysis(
        bool force, std::optional<std::string> contentTypeFilter = std::nullopt,
        std::optional<int> limit = std::nullopt);

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
        std::string provenanceJson = "{}"; // replaces `provenance` wholesale (PRD §6)
        int64_t analyzedAt = 0;
    };
    void applyAnalysis(const AnalysisUpdate& update);

    // Similarity (PRD §2c, §6, §3 "no ANN index — brute-force KNN, exact, not
    // approximate", spike/04_sqlite_vec proved the mechanics at 10k x 1280-dim). A
    // separate vec0 virtual table keyed by files.id as rowid, not a `files` column —
    // sqlite-vec's vec0 tables have their own storage format. `embedding` must be
    // exactly 1280 floats (discogs-effnet's dimension, Embedding.h); mismatched sizes
    // are the caller's bug, not silently handled here.
    void upsertEmbedding(int64_t fileId, const std::vector<float>& embedding);

    // Reads a stored embedding back out of vec_embeddings directly (the raw blob, not
    // parsed out of `machine`'s JSON — that would mean 1280 individual json_extract
    // calls per lookup). nullopt if this file has no stored embedding (e.g. analyzed
    // before this feature existed, or the file was too short for even one mel patch).
    std::optional<std::vector<float>> getEmbeddingById(int64_t fileId);

    struct SimilarMatch {
        int64_t id = 0;
        double distance = 0.0; // sqlite-vec's default: squared L2 (smaller = more similar)
    };
    // Exact brute-force KNN against every stored embedding. `excludeId`, when set, drops
    // that row from the results — the common case is "files similar to file X" where X
    // itself would otherwise always be the (distance 0) top result.
    std::vector<SimilarMatch> findSimilar(const std::vector<float>& embedding, int topK,
                                           std::optional<int64_t> excludeId = std::nullopt);

    // DCLAP's separate 512-dim embedding space (TASKS.md Phase 4 "Embedding A/B",
    // DclapEmbedding.h) — its own vec0 table (vec_embeddings_dclap), mirroring the
    // discogs-effnet methods above exactly rather than parameterizing over table/dim,
    // since the two spaces are never queried against each other.
    void upsertDclapEmbedding(int64_t fileId, const std::vector<float>& embedding);
    std::optional<std::vector<float>> getDclapEmbeddingById(int64_t fileId);
    std::vector<SimilarMatch> findSimilarDclap(const std::vector<float>& embedding, int topK,
                                                std::optional<int64_t> excludeId = std::nullopt);

    // TASKS.md Phase 4 "per-dimension similarity" (PRD §12 item 5, and `mira similar
    // --by overall|timbre|rhythm|spectrum` in PRD §8's CLI spec) — dimensions derived
    // from what mira already measures per file (Descriptors.h), not Sononym's fixed
    // five. `timbre` is the 13-coefficient MFCC vector (its own vec0 table, same
    // brute-force-KNN pattern as the embeddings above — MFCC coefficients are already
    // comparable units to each other, no extra normalization needed for a defensible
    // first cut). `spectrum` is a small 2-dim [centroid, flatness] vector — see
    // DspDescriptors.h and main.cpp's construction of it for why harmonicity and chroma
    // are deliberately left out (harmonicity's "0.0 means unmeasured" convention can't
    // be folded into a metric distance without corrupting it; chroma has no named PRD
    // dimension to attach to yet). `rhythm` has no per-file vector at all today (only a
    // scalar BPM) — findSimilarByBpm is a plain SQL brute-force scan, not a vec0 table,
    // since there's nothing to vectorize.
    void upsertTimbre(int64_t fileId, const std::vector<float>& mfcc);
    std::optional<std::vector<float>> getTimbreById(int64_t fileId);
    std::vector<SimilarMatch> findSimilarTimbre(const std::vector<float>& mfcc, int topK,
                                                 std::optional<int64_t> excludeId = std::nullopt);

    void upsertSpectrum(int64_t fileId, const std::vector<float>& centroidFlatness);
    std::optional<std::vector<float>> getSpectrumById(int64_t fileId);
    std::vector<SimilarMatch> findSimilarSpectrum(const std::vector<float>& centroidFlatness, int topK,
                                                   std::optional<int64_t> excludeId = std::nullopt);

    // No stored vector — reads files.machine directly for every candidate. `bpm` is the
    // query file's own beat_this_bpm (caller looks it up); rows with no BPM, a BPM of
    // exactly 0 (RhythmResult's "unmeasured" value), or content_type='one_shot' (tempo on
    // a one-shot is meaningless, PRD §5) are excluded, not treated as distance-0 matches.
    std::vector<SimilarMatch> findSimilarByBpm(double bpm, int topK,
                                                std::optional<int64_t> excludeId = std::nullopt);

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
