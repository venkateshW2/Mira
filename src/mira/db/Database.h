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
    // "manual" (tag-segment, mira_ui's + Segment) or "auto" (created by `mira analyze`
    // from active-region detection, review round 2). Re-analysis only ever replaces auto
    // rows nobody has edited -- see deleteUntouchedAutoSegmentsForFile.
    std::string source = "manual";
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
    // Replaces `human` wholesale. Exists for undo (mira_ui's UndoManager), which restores a
    // captured object rather than replaying the individual field writes that produced it --
    // replaying is how an undo ends up subtly different from the state it claimed to
    // restore. Not for ordinary editing: `setHumanField` is still the one-key path.
    void setHuman(int64_t fileId, const std::string& humanJson);

    // Segment tagging (TASKS.md Phase 3 addition; see Database.cpp's `segments` schema
    // comment). `createSegment` takes exactly one of groupId/fileId (caller's
    // responsibility -- the CLI resolves which one applies before calling this) and an
    // already-built `human` JSON object (CaptionFields.cpp's convention, same as files.
    // human). `findFilesByGroupId` is how a caller turns a group_id back into the actual
    // set of sibling stem files to cut.
    int64_t createSegment(std::optional<std::string> groupId, std::optional<int64_t> fileId,
                           double startSeconds, double endSeconds, const std::string& humanJson,
                           const std::string& source = "manual");
    // Removes this file's auto-created segments that are still exactly as analysis left
    // them (`human` still '{}'), plus their segment_analysis rows, so a re-analysis can
    // regenerate them. A segment someone tagged, or created by hand, is never touched.
    void deleteUntouchedAutoSegmentsForFile(int64_t fileId);
    // The group-scoped counterpart, for cue detection (review round 6). Same "untouched"
    // test and the same guarantee: re-detecting cues replaces only the proposals nobody
    // has tagged, and a cue someone named ("action", "funny") is never overwritten. That
    // matters more here than for file segments, because cue boundaries are expected to be
    // corrected by hand -- a detector that discarded those corrections on its next run
    // would make the editing pointless.
    void deleteUntouchedAutoSegmentsForGroup(const std::string& groupId);
    std::vector<SegmentRecord> findSegmentsForGroup(const std::string& groupId);
    std::vector<SegmentRecord> findSegmentsForFile(int64_t fileId);
    std::vector<FileRecord> findFilesByGroupId(const std::string& groupId);

    // Synced-stem-set grouping across analyze runs (TASKS.md Phase 5 timeline-lanes
    // review: "EP6's 13 stems all run 36:56 but have an empty group_id ... check whether
    // sibling grouping only runs across one analyze batch" -- it did). main.cpp's
    // grouping only ever saw the files in its own run, so analyzing 1 of 13 stems found
    // no sibling and assigned no group. This is the missing other half: already-analyzed
    // rows in the same parent directory, with their stored duration, so a later run can
    // join the set an earlier run established.
    //
    // `parentDir` is matched as a literal path prefix plus a separator, not LIKE (a
    // folder name containing '%' or '_' would otherwise match far too much); duration
    // comes from machine.$.duration_seconds, the only place it is stored, which is why
    // rows that were never analyzed can't be considered here at all.
    struct AnalyzedSibling {
        int64_t id = 0;
        std::string path;
        double durationSeconds = 0.0;
        std::optional<std::string> groupId;
    };
    std::vector<AnalyzedSibling> findAnalyzedSiblingsInDir(const std::string& parentDir);

    // Backfill for the same fix: an existing analyzed row that predates its set being
    // recognised gets the group_id the new run just established. Only ever called for a
    // row whose duration already matched, never as a general-purpose setter.
    void setGroupId(int64_t fileId, const std::string& groupId);

    // Editing and removing an already-declared boundary. The CLI never needed these (a
    // `tag-segment` run only ever declares one), but mira_ui's marker workflow is
    // interactive: a boundary dragged in the wrong place has to be removable, and its
    // tags editable, without dropping to SQL. `setSegmentHumanField` merges one key the
    // same way `setHumanField` does for a file; `deleteSegment` also drops that
    // segment's `segment_analysis` rows, since they are keyed on a segment that no
    // longer exists.
    // The segment counterparts of setHuman above, and of createSegment with the id chosen
    // by the caller -- both for undo. Restoring a deleted segment under a NEW id would
    // orphan its `segment_analysis` rows (keyed on segment_id) and leave every id captured
    // by a later undo step pointing at nothing, so the id has to come back with it.
    // `segments.id` is `INTEGER PRIMARY KEY`, so an explicit id is a plain insert.
    void setSegmentHuman(int64_t segmentId, const std::string& humanJson);
    int64_t createSegmentWithId(int64_t id, std::optional<std::string> groupId,
                                 std::optional<int64_t> fileId, double startSeconds, double endSeconds,
                                 const std::string& humanJson, const std::string& source);
    // Every (file_id, machine, analyzed_at) row for one segment, so a delete can be undone
    // with its analysis intact rather than silently losing it.
    struct SegmentAnalysisRow { int64_t fileId; std::string machine; int64_t analyzedAt; };
    std::vector<SegmentAnalysisRow> segmentAnalysisRows(int64_t segmentId);

    void setSegmentHumanField(int64_t segmentId, const std::string& jsonPath,
                               const std::string& jsonValueJson);
    void deleteSegment(int64_t segmentId);
    // Moving a declared boundary (review round 6's cue editing). Dragging one cue boundary
    // writes twice -- the cue that starts there and the one that ends there -- because cues
    // in a reel are a partition, not islands, and a boundary belongs to both neighbours.
    // Editing a cue's bounds also makes it "touched", so it stops being a regeneration
    // candidate; the caller is responsible for that (see markSegmentEdited).
    void setSegmentBounds(int64_t segmentId, double startSeconds, double endSeconds);
    // Flags a segment as human-touched without putting a tag on it, so a boundary someone
    // dragged survives the next detect run. `human = '{}'` is the untouched test everywhere
    // else, so this writes a marker key into it rather than inventing a second column.
    void markSegmentEdited(int64_t segmentId);
    // One segment by id, and the segment counterpart of clearHumanFields -- both for
    // mira_ui's details panel, which edits the segment a child row selected (review
    // round 4: "the details should change according to the segment").
    std::optional<SegmentRecord> findSegmentById(int64_t segmentId);
    void clearSegmentHumanFields(int64_t segmentId);

    // TASKS.md Phase 4 "segment-level analysis replacing whole-track averaging" —
    // per-(segment, file) machine JSON (Database.cpp's `segment_analysis` schema comment
    // explains the composite key). Upsert replaces the row wholesale, same "machine is
    // rewritten wholesale on re-analysis" convention as files.machine. nullopt from the
    // getter means this (segment, file) pair was never analyzed — CaptionFields.cpp
    // falls back to the file's own whole-file machine data in that case, not an error.
    void upsertSegmentAnalysis(int64_t segmentId, int64_t fileId, const std::string& machineJson,
                                int64_t analyzedAt);
    std::optional<std::string> getSegmentMachine(int64_t segmentId, int64_t fileId);

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
    // One folder's own stored `human`, exact path match, for editing it. The finder above
    // answers "what applies to this FILE" (every matching ancestor, merged in order);
    // this answers "what did someone set on THIS folder", which is what a tag dialog has
    // to show before it can offer to change it -- without it a re-tag silently replaces
    // whatever was there instead of editing it.
    std::optional<std::string> getFolderDefault(const std::string& folderPath);

    // Reads back the `[[start,end],...]` active-span array Router/ActiveRegions.cpp
    // writes into files.active_spans -- same lean-on-SQLite's-own-json approach as
    // jsonExtractDouble/jsonArrayLength above, not a C++ JSON parser.
    std::vector<std::pair<double, double>> parseActiveSpans(const std::string& activeSpansJson);

    // The other two time-stamped structures `machine` already carries, read back for
    // mira_ui's timeline lanes (TASKS.md Phase 5, "data already exists, nothing draws
    // it"): Chords.cpp's `$.chords` and BasicPitchNotes.cpp's `$.notes`. Both are one
    // json_each statement over the whole array, not parseActiveSpans' two json_extract
    // calls per element -- a 211-chord file would otherwise cost 422 statements to draw
    // one lane. An absent or malformed array is an empty vector, never an error: most
    // files have neither (32 of 108 analyzed rows have chords, 29 have notes), and a
    // lane with no data simply doesn't render.
    struct ChordChange {
        double t = 0.0;        // seconds; the chord holds until the next change
        std::string chord;     // "Dm", "F#:maj7", or "N" for no-chord
    };
    std::vector<ChordChange> parseChords(const std::string& machineJson);

    struct NoteEvent {
        double startSeconds = 0.0;
        double endSeconds = 0.0;
        int pitch = 0;          // MIDI note number
        double amplitude = 0.0; // 0..1, drives the overlay's alpha
    };
    std::vector<NoteEvent> parseNotes(const std::string& machineJson);

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

    // mira_ui's sidebar (TASKS.md Phase 5): library roots the user has explicitly added
    // via "Add Folder" — see Database.cpp's `ui_folder_roots` schema comment for why
    // this isn't a live browse-anywhere filesystem tree. addFolderRoot is idempotent
    // (INSERT OR IGNORE — adding the same path twice is a no-op, not an error).
    void addFolderRoot(const std::string& path);
    void removeFolderRoot(const std::string& path);
    std::vector<std::string> listFolderRoots();

    // Scan-completion tracking per root ("scanning is mira's job not the user's job" —
    // TASKS.md Phase 5 discussion): set false the moment a scan starts, true only once it
    // runs to completion, so an interrupted scan (app quit, error) is distinguishable
    // from a finished one and mira_ui can resume it automatically at the next launch
    // instead of leaving a silently half-indexed folder.
    void setFolderRootScanComplete(const std::string& path, bool complete);
    std::vector<std::string> listIncompleteFolderRoots();

    // "can we group folders just inside mira and mira's database not the real hard
    // disk" — a purely organizational layer over ui_folder_roots: mira-side virtual
    // folders (ui_folder_groups) a real added root can be filed under, and a mira-side
    // display name overriding how its own row shows in the sidebar. Neither touches the
    // filesystem at all — a root's `path` (what Scan/the file list actually walk) never
    // changes; groupId/displayName are purely how FolderTreeView renders that row.
    struct FolderRootInfo {
        std::string path;
        std::optional<std::string> displayName;
        std::optional<int64_t> groupId;
    };
    std::vector<FolderRootInfo> listFolderRootInfos();
    void setFolderRootDisplayName(const std::string& path, const std::optional<std::string>& displayName);
    void setFolderRootGroup(const std::string& path, const std::optional<int64_t>& groupId);

    // category is one of the three built-in kinds mira_ui offers on Add Folder ("stems"
    // | "samples" | "music") or nullopt for a user-created "New Group..." with no
    // built-in meaning -- FolderTreeView picks a distinct icon per category
    // (FolderGroupTreeItem::paintItem), unlike a plain custom group's generic one.
    struct FolderGroup {
        int64_t id = 0;
        std::string name;
        std::optional<std::string> category;
    };
    int64_t createFolderGroup(const std::string& name, const std::optional<std::string>& category = std::nullopt);
    void renameFolderGroup(int64_t groupId, const std::string& name);
    // Ungroups every member root (their real folders/files are untouched) before
    // removing the group row itself -- a group is purely mira's own container, deleting
    // it is not a "delete the folders in it" operation.
    void deleteFolderGroup(int64_t groupId);
    std::vector<FolderGroup> listFolderGroups();
    // First existing group with this category, so "Add Folder -> Stems" reuses the one
    // "Stems" group across every add rather than creating a new one each time.
    std::optional<FolderGroup> findFolderGroupByCategory(const std::string& category);
    // "i had already made groups so now this is doubling" -- before creating a new
    // category group, Add Folder also checks for a pre-existing user-made group with a
    // matching name (case-insensitive) and adopts it via setFolderGroupCategory instead
    // of creating a redundant second one.
    std::optional<FolderGroup> findFolderGroupByName(const std::string& name);
    void setFolderGroupCategory(int64_t groupId, const std::string& category);

    // Small key/value store for UI preferences that must survive a relaunch (see the
    // ui_settings schema comment). `setSetting(key, nullopt)` erases the key rather than
    // storing an empty string, so "never set" and "set to nothing" stay distinguishable
    // -- the same omit-rather-than-guess discipline the caption fields follow.
    std::optional<std::string> getSetting(const std::string& key);
    void setSetting(const std::string& key, const std::optional<std::string>& value);

    // Collections -- mira-side folders whose members are individual files rather than
    // folder roots ("allow me to add files and then i can make a folder inside mira and
    // organise it"). See the ui_collections comment in Database.cpp for why this is its
    // own concept and not another kind of FolderGroup.
    //
    // Membership is by reference throughout: adding a file to a collection copies
    // nothing, moves nothing, and leaves it exactly where it already lives. One file can
    // belong to any number of collections.
    struct Collection {
        int64_t id = 0;
        std::string name;
        int fileCount = 0; // members, for the sidebar's count
    };
    int64_t createCollection(const std::string& name);
    void renameCollection(int64_t collectionId, const std::string& name);
    // Drops the collection and its membership rows. Never touches the files themselves,
    // their analysis, or the folders they live in.
    void deleteCollection(int64_t collectionId);
    std::vector<Collection> listCollections();
    std::optional<Collection> findCollectionByName(const std::string& name);
    // Idempotent: adding a file already in the collection is a no-op rather than an
    // error, so "add these 12, 3 of which are already here" does the obvious thing.
    void addFilesToCollection(int64_t collectionId, const std::vector<int64_t>& fileIds);
    void removeFilesFromCollection(int64_t collectionId, const std::vector<int64_t>& fileIds);
    // In insertion order -- a collection is something the user assembled, so the order
    // they put it together in is meaningful in a way alphabetical isn't.
    std::vector<FileRecord> filesInCollection(int64_t collectionId);

    // mira_ui's file table (TASKS.md Phase 5): the highest-scoring key of a JSON object
    // at `path` (e.g. "$.genre_normalized" -> the top genre label), regardless of
    // threshold — a compact single-label column has no room for CaptionFields.cpp's
    // multi-label, confidence-gated extraction, this is a display convenience only.
    // nullopt when the object is empty/missing, not when a key exists with a low score.
    std::optional<std::string> jsonObjectTopKey(const std::string& json, const std::string& path);

    // "we need details of the analysis... instruments are not right" -- the file
    // details dialog (mira_ui) needs the FULL ranked distribution, not just the single
    // top label jsonObjectTopKey above collapses everything to. Sorted descending by
    // score, capped at `limit`.
    std::vector<std::pair<std::string, double>> jsonObjectEntries(const std::string& json, const std::string& path,
                                                                    int limit);

    // Every string element of a JSON array at `path` -- CaptionFields.cpp has its own
    // private equivalent (readHumanStringArray) for the caption pipeline; this is the
    // same read exposed publicly for mira_ui's file details dialog (reading `human`'s
    // genre/instruments/moods arrays to prefill its editable fields).
    std::vector<std::string> jsonStringArray(const std::string& json, const std::string& path);

    // The numeric counterpart, for `$.rhythm.beat_this_beats` / `beat_this_downbeats` --
    // mira_ui's bar ruler draws real detected downbeats rather than a synthetic grid laid
    // out from the BPM scalar, which would drift away from the audio on anything that
    // isn't metronomic.
    std::vector<double> jsonDoubleArray(const std::string& json, const std::string& path);

private:
    void migrate();

    SQLite::Database db;
};

} // namespace mira
