#include "analyze/ActiveRegions.h"
#include "analyze/ActiveSpanMap.h"
#include "analyze/AudioLoader.h"
#include "analyze/Descriptors.h"
#include "analyze/Chords.h"
#include "analyze/ContentGate.h"
#include "analyze/Embedding.h"
#include "analyze/ClapText.h"
#include "analyze/DclapEmbedding.h"
#include "analyze/EssentiaEngine.h"
#include "analyze/Key.h"
#include "analyze/Mir.h"
#include "analyze/MoodTheme.h"
#include "analyze/MoodThemeLabels.h"
#include "analyze/Instrument.h"
#include "analyze/InstrumentLabels.h"
#include "analyze/Danceability.h"
#include "analyze/StemInstrument.h"
#include "analyze/Genre.h"
#include "analyze/GenreLabels.h"
#include "analyze/VoiceInstrumental.h"
#include "taxonomy/Taxonomy.h"
#include "caption/CaptionFields.h"
#include "caption/Sa3Renderer.h"
#include "export/AudioWriter.h"
#include "analyze/Router.h"
#include "analyze/Transcription.h"
#include "db/Database.h"
#include "scan/Scanner.h"

#include <version.h> // essentia's, not libc++'s — ESSENTIA_VERSION/ESSENTIA_GIT_SHA
#include <sqlite3.h>
#include <sqlite-vec.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {

// PRD §5 label normalization: builds a {canonical_label: score} JSON object alongside a
// head's raw output (never replacing it — see Taxonomy.h). `names`/`scores` must be the
// same length and in the same order (a head's fixed label-array order). A raw label with
// no taxonomy entry is simply omitted here, not dropped from the analysis — it's still
// present in the raw object next to this one; PRD §8's mira stats is where "how many
// labels are still unmapped" gets surfaced, not silently here.
std::string normalizedLabelsJson(const std::vector<std::string>& names,
                                  const std::vector<double>& scores, const mira::Taxonomy& taxonomy) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (size_t i = 0; i < names.size() && i < scores.size(); ++i) {
        auto canonical = taxonomy.normalize(names[i]);
        if (!canonical) continue;
        if (!first) oss << ",";
        oss << "\"" << *canonical << "\":" << scores[i];
        first = false;
    }
    oss << "}";
    return oss.str();
}

std::string defaultDbPath() {
    const char* home = std::getenv("HOME");
    std::filesystem::path dir = home ? std::filesystem::path(home) / ".mira"
                                      : std::filesystem::path(".mira");
    std::filesystem::create_directories(dir);
    return (dir / "library.db").string();
}

int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void printUsage() {
    std::cout <<
        "mira — local audio understanding and similarity search\n"
        "\n"
        "Usage:\n"
        "  mira scan <dir>... [--db <path>] [--follow-symlinks] [--as stem]\n"
        "        index files, no analysis; --as stem declares them delivery stems (§12.3)\n"
        "  mira analyze [--db <path>] [--force] [--limit N] [--content-type <type>]\n"
        "               [--paths-from <file>] [--dclap] [--chords] [--transcribe]\n"
        "               [--recheck-tempo] [--verbose] [--progress-stages]\n"
        "        --paths-from <file>: analyze exactly the files listed (one path per\n"
        "        line), always re-analyzing regardless of analyzed_at, ignoring --force/\n"
        "        --content-type/--limit; a path not already scanned is skipped\n"
        "        content-type router (one_shot/loop/track/stem) + DSP + embedding +\n"
        "        content gate + moodtheme/instrument/danceability/genre/voice-instrumental\n"
        "        heads (Phase 2, PRD §2c) + rhythm + key by default; classification heads\n"
        "        gated on the content gate's is_music signal; stems additionally get a\n"
        "        second, isolated-audio-tuned instrument opinion (stem_instrument —\n"
        "        mtg_jamendo_instrument's embedding is full-mix-trained and unreliable on\n"
        "        isolated stems, TASKS.md); rhythm defaults to beat_this_cpp only (the\n"
        "        more accurate of the two tempo estimators); --recheck-tempo also runs\n"
        "        Essentia's RhythmExtractor2013 for comparison (bpm_ratio); --dclap adds\n"
        "        the second (DCLAP) embedding space, off by default because it is 27% of a\n"
        "        file's analysis and feeds only `mira similar --embedding dclap`/--text,\n"
        "        never a caption or a label; --chords and\n"
        "        --transcribe are opt-in (15.0s/3.7s on a 5:08 song, vs 0.4s for key alone\n"
        "        — see TASKS.md); --content-type requires --force; --progress-stages\n"
        "        prints machine-readable `starting:`/`stage:` lines to stdout (what\n"
        "        mira_ui's progress readout reads, so a multi-minute file isn't silent\n"
        "        between its start and its completion); --verbose prints\n"
        "        per-stage timing to stderr, per file\n"
        "  mira inspect <file|id> [--db <path>]\n"
        "        human-readable report; flags low-confidence tempo/key, active_ratio\n"
        "  mira similar <file|id> | --text \"<description>\" [--db <path>] [--n N]\n"
        "               [--embedding effnet|dclap] [--by overall|timbre|rhythm|spectrum]\n"
        "        exact brute-force KNN (sqlite-vec, no ANN index — PRD §3). By example:\n"
        "        pass a file already in the library. By description: --text \"brass swell\"\n"
        "        searches the DCLAP space directly with the CLAP text tower — open\n"
        "        vocabulary, so it is not limited to the taxonomy's genres/instruments.\n"
        "        --text only finds files analyzed with `analyze --dclap`.\n"
        "  mira search --filter \"...\" [--db <path>]\n"
        "        comma-separated conditions, ANDed: field OP value (bpm>120, duration<180)\n"
        "        or tag:value (genre:Flamenco, instrument:guitar); fields: content_type,\n"
        "        key, bpm/tempo, duration, loudness, danceable, harmonicity, centroid,\n"
        "        crest, music_score, voice_probability; operators >, >=, <, <=, =, !=\n"
        "  mira models --list | --download [--db <path>]\n"
        "        --list: which of the 10 models this build expects are present on disk;\n"
        "        --download: mira doesn't fetch at runtime (PRD §2d) — points at\n"
        "        scripts/fetch-vendor.sh or lab/'s conversion scripts for what's missing\n"
        "  mira stats [--db <path>]\n"
        "        library composition, per-head classification coverage, embeddings\n"
        "        stored, and taxonomy completeness (which raw labels have no normalized\n"
        "        form yet, taxonomy/*.yaml)\n"
        "  mira caption <file|id> [--trigger <token>] [--emit-sidecar] [--db <path>]\n"
        "        renders CaptionFields (confidence-gated genre/instrument/mood/bpm/key)\n"
        "        into Stable Audio 3's own trained prompt shape (PRD §11/§15); prints the\n"
        "        prose form plus the flat tag set, and with --emit-sidecar also writes\n"
        "        <file>.json next to the source for underfit's dataset loader to pick up\n"
        "  mira tag <file|id> [--genre \"a, b\"] [--instruments \"a, b\"] [--moods \"a, b\"]\n"
        "            [--keywords \"funny, quirky\"] [--bpm N] [--key \"F minor\"]\n"
        "            [--is-instrumental true|false] [--clear] [--db <path>]\n"
        "        sets `human` overrides mira's own analysis never touches and `mira\n"
        "        caption` always prefers over the machine-derived value; --keywords is\n"
        "        the one field with no machine equivalent at all (scene/vibe words like\n"
        "        \"action\" or \"drama\" mira has no analyzer for). Each flag merges just\n"
        "        that field; --clear resets all human overrides for the file\n"
        "  mira tag-folder <folder> [tag flags as above] [--clear] [--db <path>]\n"
        "        sets a `human` default for every scanned file under <folder> (plain\n"
        "        path-prefix match against files.path -- write it the same way, relative\n"
        "        or absolute, you scanned with); a file's own `mira tag`/`mira\n"
        "        tag-segment` values always win over a folder default for the same field\n"
        "  mira tag-segment <group_id|file|id> --start <s> --end <s> [tag flags as above]\n"
        "        declares a time-ranged caption boundary -- a group_id (from `mira\n"
        "        stats`/DB, format \"<dir>|<duration>\") applies to every file in that\n"
        "        synced stem set at the same timestamps; a file/id applies to one file.\n"
        "        Only records the boundary; run `mira export-segments` to actually cut\n"
        "  mira export-segments <group_id|file|id> --out-dir <dir> [--trigger <token>]\n"
        "        cuts every declared segment out of every covered file into its own WAV\n"
        "        + SA3 sidecar JSON under <dir>/seg<N>_<start>-<end>s/ -- a group_id\n"
        "        target cuts every sibling stem at identical boundaries, so the set\n"
        "        stays synced across the cut\n";
}

int runScan(const std::vector<std::string>& args) {
    mira::ScanOptions options;
    std::string dbPath = defaultDbPath();

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--db" && i + 1 < args.size()) {
            dbPath = args[++i];
        } else if (arg == "--follow-symlinks") {
            options.followSymlinks = true;
        } else if (arg == "--as" && i + 1 < args.size() && args[i + 1] == "stem") {
            options.declareAsStem = true;
            ++i;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "mira scan: unknown flag " << arg << std::endl;
            return 1;
        } else {
            options.roots.push_back(arg);
        }
    }

    if (options.roots.empty()) {
        std::cerr << "mira scan: at least one directory is required" << std::endl;
        return 1;
    }

    std::cout << "database: " << dbPath << std::endl;

    mira::Database db(dbPath);
    auto stats = mira::scan(db, options);

    std::cout << "scanned " << stats.filesSeen << " audio files "
               << "(" << stats.filesNew << " new, "
               << stats.filesUpdated << " updated, "
               << stats.filesUnchanged << " unchanged), "
               << stats.filesSkippedUnsupported << " non-audio files skipped";
    if (stats.filesSkippedAppleDouble > 0)
        std::cout << ", " << stats.filesSkippedAppleDouble << " macOS AppleDouble sidecars skipped";
    std::cout << std::endl;
    if (options.declareAsStem)
        std::cout << "all scanned files declared as stems (content_type_source=declared)"
                   << std::endl;
    std::cout << "library now has " << db.countFiles() << " files total" << std::endl;

    return 0;
}

// PRD §12.3 route 2, the general case: N files of (near-)identical duration in the same
// folder are treated as a sibling set of delivery stems. Grouping is over the batch of
// files analyzed in *this* run only — a folder analyzed across multiple runs won't be
// grouped correctly yet (see TASKS.md). Declared stems (content_type_source='declared')
// participate too, so a folder mixing a declared stem with router-detected siblings of
// the same duration still ends up in one group.
struct Candidate {
    mira::FileRecord record;
    mira::LoadedAudio audio;    // kept for the whole run — see TASKS.md's scaling note
    bool isDeclared = false;
    std::string contentType;    // declared: "stem"; else the router's classification
    mira::RoutingResult routing; // only meaningful when !isDeclared
    std::string parentDir;
};

// PRD §12.3 route 1: "filename or folder pattern, where one happens to exist... cheapest
// and exact, but the user's naming differs every time, so this is opportunistic only."
// Deliberately narrow — just the word "stem"/"stems" as a path component or substring,
// case-insensitively, matching the PRD's own examples (CUE_03_STRINGS.wav, a STEMS/
// folder). Anything fancier (instrument-role keyword guessing) risks false positives the
// PRD doesn't ask for; sibling-set detection (route 2) is "the general case" for a reason.
bool looksLikeStemPath(const std::string& path) {
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return lower.find("stem") != std::string::npos;
}

std::string siblingKey(const std::string& parentDir, double durationSeconds) {
    // Round to the nearest 50ms — "identical length" allowing for header/encoder jitter.
    double rounded = std::round(durationSeconds * 20.0) / 20.0;
    std::ostringstream oss;
    oss << parentDir << "|" << rounded;
    return oss.str();
}

std::string spansToJson(const std::vector<mira::ActiveSpan>& spans) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < spans.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "[" << spans[i].startSeconds << "," << spans[i].endSeconds << "]";
    }
    oss << "]";
    return oss.str();
}

// Stage timer for --verbose: prints wall-clock time for each analysis stage to stderr,
// per file, so a slow run can be attributed to a specific stage instead of guessed at.
//
// It also drives --progress-stages, which prints the same stage boundaries to *stdout* in
// a fixed `stage: <name>` form for mira_ui to read. The two are deliberately separate:
// --verbose is a human-readable diagnostic with timings on stderr, this is a machine
// readout with no numbers in it. It exists because a 41-minute score stem spends many
// minutes inside one `mira analyze` invocation that otherwise says nothing at all until
// the file is finished -- which is indistinguishable from being hung.
class StageTimer {
public:
    explicit StageTimer(bool enabled, bool emitProgress = false)
        : enabled_(enabled), emitProgress_(emitProgress) {}
    void mark(const std::string& stageName) {
        if (emitProgress_) std::cout << "stage: " << stageName << std::endl;
        if (!enabled_) return;
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - last_).count();
        std::cerr << "  " << stageName << ": " << static_cast<int>(ms) << "ms" << std::endl;
        last_ = now;
    }

private:
    bool enabled_;
    bool emitProgress_;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

// Defined further down, next to tag-segment, which it was written for.
std::string buildSegmentMachineJson(const std::vector<float>& mono, const std::vector<float>& left,
                                     const std::vector<float>& right, int sampleRate,
                                     mira::Taxonomy& instrumentTaxonomy, mira::Taxonomy& genreTaxonomy,
                                     mira::Taxonomy& stemInstrumentTaxonomy, bool stemParent);

// Auto-segments (review round 2, "option A"): a stem's active spans become real segment
// rows, each with its own segment_analysis, the moment the stem is analyzed -- so a
// 37-minute score stem arrives as N tagged, searchable pieces instead of one averaged
// label (the brass stem that averaged out to "acoustic guitar"). Agreed framing: these
// are *samples of the stem*, not cues -- cue detection is its own later project.
//
// Raw spans aren't used as-is. Detection bridges only 300ms gaps (ActiveRegions.h), so a
// phrase with a breath in it arrives as several spans, and some spans are slivers (the
// EP6 brass stem's first is 85ms) far too short for effnet or the heads to say anything.
// Spans closer than kAutoSegmentMergeGapSeconds merge; anything still shorter than
// kAutoSegmentMinSeconds is dropped. Both are first-pass numbers picked from that one
// stem, not measured across a library -- same caveat ActiveRegions.h gives its own.
constexpr double kAutoSegmentMergeGapSeconds = 1.5;
constexpr double kAutoSegmentMinSeconds = 2.0;

int createAutoSegments(mira::Database& db, int64_t fileId, const std::vector<mira::ActiveSpan>& spans,
                       const std::vector<float>& mono, const std::vector<float>& left,
                       const std::vector<float>& right, int sampleRate,
                       mira::Taxonomy& instrumentTaxonomy, mira::Taxonomy& genreTaxonomy,
                       mira::Taxonomy& stemInstrumentTaxonomy, bool stemParent) {
    std::vector<mira::ActiveSpan> merged;
    for (const auto& span : spans) {
        if (!merged.empty() && span.startSeconds - merged.back().endSeconds < kAutoSegmentMergeGapSeconds)
            merged.back().endSeconds = std::max(merged.back().endSeconds, span.endSeconds);
        else
            merged.push_back(span);
    }

    // Regenerate only what nobody has touched; anything kept (hand-made, or an auto one
    // someone tagged) wins over a new span that overlaps it, so re-analysis never
    // stacks a fresh duplicate on top of a segment a person has already worked on.
    // Drop the too-short ones up front: how many segments a file really has decides
    // whether any of them needs its own analysis (below).
    std::vector<mira::ActiveSpan> usable;
    for (const auto& span : merged)
        if (span.endSeconds - span.startSeconds >= kAutoSegmentMinSeconds) usable.push_back(span);

    const auto totalSamples = static_cast<int64_t>(std::min({mono.size(), left.size(), right.size()}));
    const double durationSeconds = sampleRate > 0 ? static_cast<double>(totalSamples) / sampleRate : 0.0;

    // One segment means the file doesn't actually change character partway through, and a
    // segment is only worth having when it says something the file doesn't (review round
    // 4: "why does the track and segment get analysed?"). Whole-file analysis already runs
    // on active audio only -- silence is spliced out before any model sees it -- so a
    // lone segment covers the same audio the file pass already measured, and re-running
    // the models on it costs ~1.4s per file to produce the same labels. Measured: 26 of
    // this library's 34 segmented files have exactly one segment.
    //
    //   - covers essentially the whole file: no segment at all, it would add a row that
    //     says nothing (the mix's own "0:00 - 1:04" row in the screenshots);
    //   - covers less: keep the row, since `mira export-segments` uses it to trim the
    //     silence away, but skip its analysis -- CaptionFields already falls back to the
    //     file's own document for a segment that has none.
    constexpr double kWholeFileCoverage = 0.95;
    if (usable.size() == 1 && durationSeconds > 0.0
        && (usable.front().endSeconds - usable.front().startSeconds) >= kWholeFileCoverage * durationSeconds) {
        db.deleteUntouchedAutoSegmentsForFile(fileId);
        return 0;
    }
    const bool analyzeSegments = usable.size() >= 2;

    db.deleteUntouchedAutoSegmentsForFile(fileId);
    auto kept = db.findSegmentsForFile(fileId);

    int created = 0;
    for (const auto& span : usable) {
        bool overlapsKept = std::any_of(kept.begin(), kept.end(), [&](const mira::SegmentRecord& k) {
            return span.startSeconds < k.endSeconds && span.endSeconds > k.startSeconds;
        });
        if (overlapsKept) continue;

        auto startSample = std::clamp<int64_t>(static_cast<int64_t>(span.startSeconds * sampleRate), 0, totalSamples);
        auto endSample = std::clamp<int64_t>(static_cast<int64_t>(span.endSeconds * sampleRate), 0, totalSamples);
        if (endSample <= startSample) continue;

        int64_t segId = db.createSegment(std::nullopt, fileId, span.startSeconds, span.endSeconds, "{}", "auto");
        if (analyzeSegments) {
            std::vector<float> segMono(mono.begin() + startSample, mono.begin() + endSample);
            std::vector<float> segLeft(left.begin() + startSample, left.begin() + endSample);
            std::vector<float> segRight(right.begin() + startSample, right.begin() + endSample);
            db.upsertSegmentAnalysis(segId, fileId,
                                      buildSegmentMachineJson(segMono, segLeft, segRight, sampleRate,
                                                              instrumentTaxonomy, genreTaxonomy,
                                                              stemInstrumentTaxonomy, stemParent),
                                      static_cast<int64_t>(std::time(nullptr)));
        }
        ++created;
    }
    return created;
}

int runAnalyze(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    bool force = false;
    bool verbose = false;
    // Chords (Chordino) and transcription (Basic Pitch) are opt-in, not run by default:
    // measured at 15.0s and 3.7s respectively on a 5:08 real song (39% and 10% of a
    // 38.4s total analyze time), well past what "BPM/key/loudness across a drive" (the
    // Phase 1 headline goal, PRD §9) needs. Rhythm and key stay on by default — they're
    // core to that goal and comparatively cheap (9.3s and 0.4s on the same file).
    // Rhythm's default estimator is beat_this_cpp only — measured more accurate than
    // Essentia's RhythmExtractor2013 (only one that gives downbeats; better on syncopated
    // material) and, on the same song, running it costs 9.2s vs Essentia's 2.7s, so
    // Essentia is an opt-in recheck (--recheck-tempo) for comparing the two, not a
    // default-on second opinion.
    bool runDclap = false;   // --dclap, see the call site for why this is opt-in
    bool runChords = false;
    bool runTranscription = false;
    bool progressStages = false;
    bool runRecheckTempo = false;
    std::optional<std::string> contentTypeFilter;
    std::optional<int> limit;
    // mira_ui's Analyze button (TASKS.md Phase 5): "for selected files or the folder" —
    // a newline-delimited file of exact paths, written by the UI to a temp file rather
    // than passed as N argv entries (an arbitrarily large selection could exceed a
    // sensible command-line length). A small, surgical addition (not a refactor of
    // runAnalyze's own file-list-driven pipeline below) -- it just replaces how
    // candidateRecords gets populated a few lines down.
    std::optional<std::string> pathsFromFile;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--db" && i + 1 < args.size()) {
            dbPath = args[++i];
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--verbose") {
            verbose = true;
        } else if (arg == "--progress-stages") {
            progressStages = true;
        } else if (arg == "--dclap") {
            runDclap = true;
        } else if (arg == "--chords") {
            runChords = true;
        } else if (arg == "--transcribe") {
            runTranscription = true;
        } else if (arg == "--recheck-tempo") {
            runRecheckTempo = true;
        } else if (arg == "--content-type" && i + 1 < args.size()) {
            contentTypeFilter = args[++i];
        } else if (arg == "--limit" && i + 1 < args.size()) {
            limit = std::stoi(args[++i]);
        } else if (arg == "--paths-from" && i + 1 < args.size()) {
            pathsFromFile = args[++i];
        } else {
            std::cerr << "mira analyze: unknown argument " << arg << std::endl;
            return 1;
        }
    }

    if (contentTypeFilter && !force) {
        std::cerr << "mira analyze: --content-type only has an effect with --force "
                      "(unrouted files are all content_type='unknown')"
                   << std::endl;
        return 1;
    }

    std::cout << "database: " << dbPath << std::endl;
    mira::Database db(dbPath);

    std::vector<mira::FileRecord> candidateRecords;
    if (pathsFromFile) {
        // Explicit selection -- always (re-)analyzed regardless of analyzed_at, the same
        // way clicking "Analyze" on an already-analyzed file in the UI means "do it
        // again", not "skip it silently". A path not yet in the files table (never
        // scanned) is just skipped -- Scan always runs before Analyze in mira_ui's own
        // flow, so this is a defensive skip, not the expected case.
        std::ifstream pathsFile(*pathsFromFile);
        if (!pathsFile) {
            std::cerr << "mira analyze: could not read --paths-from file " << *pathsFromFile << std::endl;
            return 1;
        }
        std::string line;
        while (std::getline(pathsFile, line)) {
            if (line.empty()) continue;
            if (auto record = db.findByPath(line)) candidateRecords.push_back(*record);
        }
    } else {
        candidateRecords = db.findFilesForAnalysis(force, contentTypeFilter, limit);
    }
    if (candidateRecords.empty()) {
        std::cout << "nothing to analyze (use --force to re-analyze)" << std::endl;
        return 0;
    }

    mira::EssentiaEngine engine; // essentia::init() for the lifetime of this command

    std::vector<Candidate> candidates;
    int failed = 0;
    for (auto& record : candidateRecords) {
        StageTimer decodeTimer(verbose, progressStages);
        if (progressStages) std::cout << "decoding: " << record.path << std::endl;
        if (verbose) std::cerr << record.path << " (decode+route):" << std::endl;

        auto audio = mira::loadAudio(record.path);
        if (!audio) {
            std::cerr << "mira analyze: could not decode " << record.path << std::endl;
            failed++;
            continue;
        }
        decodeTimer.mark("decode");

        Candidate c;
        c.record = record;
        c.isDeclared = (record.contentTypeSource == "declared");
        c.parentDir = std::filesystem::path(record.path).parent_path().string();

        if (c.isDeclared) {
            // A declaration comes from the folder's category ("everything here is a
            // stem"). The filename is more specific evidence, and a mix delivered
            // alongside its stems is not a stem (review round 4) -- routing it as one
            // hands it the isolated-audio instrument model, measured answering
            // "voice 54%" for a whole arrangement.
            c.contentType = mira::filenameSuggestsFullMix(record.path) ? "track" : "stem";
        } else {
            c.routing = mira::routeContentType(audio->mono, audio->sampleRate);
            // Route 1 (filename/folder pattern) overrides the duration-based classification
            // but stays content_type_source='router', not 'declared' — only an explicit
            // `--as stem` is a real declaration (PRD §12.3).
            // ...and a mix inside a stems folder is still not a stem (review round 4).
            // looksLikeStemPath matches "stem" anywhere in the *path*, so every file in a
            // "..._Stems/" folder hits it, mix included; the filename describes this file
            // rather than its folder, so it wins.
            c.contentType = (looksLikeStemPath(record.path) && !mira::filenameSuggestsFullMix(record.path))
                                ? "stem"
                                : c.routing.contentType;
        }
        decodeTimer.mark("router (duration/onset)");
        c.audio = std::move(*audio);
        candidates.push_back(std::move(c));
    }

    // Sibling-set stem detection + group_id assignment, over every candidate whose
    // content type is already 'stem' (declared) or could become one (router-classified).
    std::map<std::string, std::vector<size_t>> siblingGroups;
    for (size_t i = 0; i < candidates.size(); ++i) {
        siblingGroups[siblingKey(candidates[i].parentDir, candidates[i].audio.durationSeconds)]
            .push_back(i);
    }

    // ...and the other half of that detection: siblings already analyzed by an EARLIER
    // run (TASKS.md Phase 5 timeline-lanes review -- EP6's 13 identical-length stems all
    // had an empty group_id because only one of them had ever been analyzed). Grouping
    // used to see only the current run's candidates, so "analyze this one file" could
    // never find a set, and a set established by a full-folder run was invisible to
    // every later single-file re-analysis.
    //
    // Two things come out of this map, keyed the same way siblingGroups is:
    //   - extraMembers: analyzed rows that make a lone candidate part of a real set.
    //   - existingGroupId: the group this set already goes by, so a later run JOINS it
    //     rather than minting a second id for the same folder+duration.
    // Rows whose path matches a candidate are skipped -- a --force re-analysis has its
    // own row in the DB and must not count as its own sibling.
    std::map<std::string, std::vector<mira::Database::AnalyzedSibling>> priorSiblings;
    {
        std::set<std::string> candidatePaths, dirsSeen;
        for (const auto& c : candidates) candidatePaths.insert(c.record.path);
        for (const auto& c : candidates) {
            if (!dirsSeen.insert(c.parentDir).second) continue;
            for (auto& sib : db.findAnalyzedSiblingsInDir(c.parentDir)) {
                if (candidatePaths.count(sib.path)) continue;
                priorSiblings[siblingKey(c.parentDir, sib.durationSeconds)].push_back(std::move(sib));
            }
        }
    }

    // The *run's* timestamp. It identifies this invocation -- which model versions and
    // which mira build produced these rows -- so it belongs in `provenance` below, where
    // it is shared by every file in the run on purpose.
    //
    // It is deliberately NOT what goes into `files.analyzed_at` any more. That column used
    // to be stamped with this same run-start value for every file, which meant the database
    // could not answer "how long did this file take" or "how far into this run are we" at
    // all -- a 38-file album analyzed over 31 minutes landed as 38 rows sharing one second.
    // `applyAnalysis` already writes one row at a time, so a real per-file completion time
    // costs nothing beyond calling the clock again at the point the file is actually done.
    int64_t analyzedAt = nowUnix();
    std::map<std::string, int> counts;
    int completedCount = 0; // mira_ui's AnalyzeJob parses the "progress: " line below for its own progress readout

    // PRD §6: "provenance — mira version, Essentia version, model names/versions,
    // analysis timestamp. Lets a later model upgrade identify exactly which files need
    // re-analysis." Same for every file in this run — built once outside the loop.
    std::ostringstream provenance;
    provenance << "{\"mira_git_hash\":\"" << MIRA_GIT_HASH << "\""
               << ",\"essentia_version\":\"" << ESSENTIA_VERSION << "\""
               << ",\"essentia_git_sha\":\"" << ESSENTIA_GIT_SHA << "\""
               << ",\"beat_this_model\":\"" << MIRA_BEAT_THIS_MODEL << "\""
               << ",\"basic_pitch_model\":\"" << MIRA_BASIC_PITCH_MODEL << "\""
               << ",\"analyzed_at\":" << analyzedAt
               << "}";
    std::string provenanceJson = provenance.str();

    // Label normalization (PRD §5, taxonomy/*.yaml) — loaded once, not per file. Loading
    // failure degrades to raw-labels-only (Taxonomy::ok()), not a hard error: a missing
    // or malformed taxonomy file shouldn't stop the whole analyze run, since the raw
    // model output is still fully usable without it.
    mira::Taxonomy instrumentTaxonomy(MIRA_INSTRUMENT_TAXONOMY, "mtg_jamendo_instrument");
    mira::Taxonomy stemInstrumentTaxonomy(MIRA_INSTRUMENT_TAXONOMY, "irmas_predominant_instrument");
    // Purely mechanical (Genre---Style -> "Genre: Style"), unlike the instrument taxonomy
    // above which encodes real judgment calls — see taxonomy/genre-labels.yaml. moodtheme
    // and the content gate's AudioSet labels have no taxonomy file: both are already
    // clean, human-readable words/phrases as-is (checked directly against their full
    // label lists), so a passthrough mapping would only double JSON size for zero change.
    mira::Taxonomy genreTaxonomy(MIRA_GENRE_TAXONOMY, "genre_discogs400");

    for (auto& [key, indices] : siblingGroups) {
        auto priorIt = priorSiblings.find(key);
        const std::vector<mira::Database::AnalyzedSibling> noPriors;
        const auto& priors = priorIt != priorSiblings.end() ? priorIt->second : noPriors;

        // A set of two is still a set whether both halves are in this run or one of them
        // was analyzed last week.
        bool isSiblingSet = indices.size() + priors.size() >= 2;
        std::optional<std::string> groupId;
        if (isSiblingSet) {
            groupId = key;
            for (const auto& sib : priors)
                if (sib.groupId) { groupId = *sib.groupId; break; } // adopt, don't mint a second id

            // Backfill the ones that predate the set being recognised. Without this, the
            // file that was analyzed alone first would keep its empty group_id forever
            // while everything analyzed after it got the group -- exactly the half-grouped
            // state the EP6 folder was found in.
            for (const auto& sib : priors)
                if (sib.groupId != groupId) db.setGroupId(sib.id, *groupId);
        }

        for (size_t idx : indices) {
            auto& c = candidates[idx];
            // A mix delivered alongside its stems is not a stem, and that has to win over
            // the folder's blanket declaration -- otherwise the guard below writes "stem"
            // back over the very row it says it is correcting, which is what happened to
            // Paintball-BGM-StemMix.wav (stored content_type='stem', indistinguishable
            // from its 9 siblings). Identifying the mix inside a synced set is also what
            // cue detection needs: the mix is the one file that plays wherever any cue
            // plays (measured active_ratio 0.902 against 0.129-0.782 for its stems).
            std::string finalContentType = mira::filenameSuggestsFullMix(c.record.path)
                                                ? "track"
                                                : ((isSiblingSet || c.isDeclared) ? "stem" : c.contentType);
            counts[finalContentType]++;

            // Announced BEFORE the work, unlike `progress:` below which is printed
            // after the database write. On a 41-minute stem the gap between the two is
            // minutes long, and without this the UI has nothing to show for it -- which
            // is exactly the "is it hung?" that made this necessary.
            std::cout << "starting: " << (completedCount + 1) << "/" << candidates.size() << " "
                      << c.record.path << std::endl;
            if (verbose) std::cerr << c.record.path << ":" << std::endl;
            StageTimer timer(verbose, progressStages);

            double duration = c.audio.durationSeconds;

            std::ostringstream machine;
            // Everything time-stamped in `machine` is a position in the ORIGINAL file,
            // not in the spliced active audio the analyzers actually ran on. That was not
            // true before 2026-09-12 (see the remapping below and ActiveSpanMap.h), so the
            // marker is what lets a reader tell a converted row from a legacy one instead
            // of guessing from provenance.mira_git_hash.
            machine << "{\"timebase\":\"file\""
                    << ",\"duration_seconds\":" << duration
                    << ",\"sample_rate\":" << c.audio.sampleRate
                    << ",\"num_channels\":" << c.audio.numChannels;
            if (!c.isDeclared) {
                machine << ",\"onset_rate\":" << c.routing.onsetRate
                        << ",\"onset_count\":" << c.routing.onsetCount;
            }

            mira::Database::AnalysisUpdate update;
            update.id = c.record.id;
            update.provenanceJson = provenanceJson;
            // A declared row keeps its declaration, with one exception: a mix was
            // declared a stem by its folder's category, and the row itself has to be
            // corrected or every later read still calls it a stem.
            if (!c.isDeclared || mira::filenameSuggestsFullMix(c.record.path))
                update.contentType = finalContentType;
            if (groupId) update.groupId = groupId;

            // PRD §5: "descriptors, MIR and the embedding then see only those spans" —
            // active-region detection runs first (always for stems, else only past 5
            // min), and when it does, everything downstream operates on the active audio
            // only, not the whole file. A stem's 140 silent seconds never reach a
            // descriptor. Caveat worth naming: splicing spans together introduces an
            // artificial discontinuity at each boundary, which tempo/beat tracking could
            // in principle misread as a transient — the PRD asks for this restriction
            // regardless, and it's a real problem only on multi-span, rhythmically-dense
            // material, which is rare for the mostly-silent-stem case this exists for.
            const auto* mono = &c.audio.mono;
            const auto* left = &c.audio.left;
            const auto* right = &c.audio.right;
            std::vector<float> activeMono, activeLeft, activeRight;
            std::vector<mira::ActiveSpan> activeSpans; // kept past this block for createAutoSegments

            if (mira::shouldRunActiveRegionDetection(finalContentType, duration)) {
                auto activeRegions = mira::detectActiveRegions(c.audio.mono, c.audio.sampleRate);
                activeSpans = activeRegions.spans;
                update.activeRatio = activeRegions.activeRatio;
                update.activeSpansJson = spansToJson(activeRegions.spans);
                machine << ",\"active_ratio\":" << activeRegions.activeRatio;

                activeMono = mira::extractActiveAudio(c.audio.mono, c.audio.sampleRate, activeRegions.spans);
                activeLeft = mira::extractActiveAudio(c.audio.left, c.audio.sampleRate, activeRegions.spans);
                activeRight = mira::extractActiveAudio(c.audio.right, c.audio.sampleRate, activeRegions.spans);
                mono = &activeMono;
                left = &activeLeft;
                right = &activeRight;
            }
            timer.mark("active-region detection");

            // Everything from here down runs on `*mono`, which is the SPLICED active
            // audio whenever detection ran above -- so every timestamp those analyzers
            // produce needs mapping back through these spans before it is stored
            // (ActiveSpanMap.h). Empty when detection didn't run, which makes every
            // mapping below an identity rather than a special case to remember.
            std::vector<std::pair<double, double>> fileTimeSpans;
            fileTimeSpans.reserve(activeSpans.size());
            for (const auto& span : activeSpans)
                fileTimeSpans.emplace_back(span.startSeconds, span.endSeconds);

            // DSP descriptors (PRD §5A) — all content types.
            auto dsp = mira::computeDspDescriptors(*left, *right, c.audio.sampleRate);
            machine << ",\"dsp\":" << mira::toJson(dsp);
            timer.mark("DSP descriptors (incl. harmonicity)");

            // Classification (Phase 2, PRD §2c vertical slice). Embedding + content gate
            // run on *every* content type, unlike rhythm/key below — embedding-based
            // similarity is exactly the point of comparing one-shots (Sononym-style "find
            // similar samples"), so it isn't content-type-gated. moodtheme is gated only
            // on the content gate's is_music signal, not on content_type at all — "Music
            // heads must only run on music" (PRD §2c), the same honesty principle already
            // applied to key/chords via the harmonicity gate below, just using a real
            // content classifier instead of a DSP proxy.
            auto embedding = mira::computeEmbedding(*mono, c.audio.sampleRate, MIRA_EFFNET_MODEL);
            if (embedding.ok) machine << ",\"embedding\":" << mira::toJson(embedding);
            timer.mark("embedding (discogs-effnet)");

            // Second embedding space (TASKS.md Phase 4 "Embedding A/B", PRD §4, §14.5).
            //
            // Opt-in (--dclap), and the only stage here that is opt-in for its *cost*
            // rather than its runtime: measured at 8.1s on a 4:05 track, 27% of that
            // file's entire analysis -- the most expensive single stage, tied with
            // beat_this. It used to run on every file unconditionally, on the reasoning
            // that comparing across content types needs every file in both spaces.
            //
            // What changed is what it buys. This vector feeds nothing mira shows: no
            // caption field, no label, nothing in the UI. Its only readers are
            // `mira similar --embedding dclap` and `--text`, neither of them the default.
            // Paying 27% of every analysis for a search space most libraries will never
            // query is the wrong default; paying it deliberately, when text search is
            // wanted, is not. Turn it on for the folders that should be text-searchable.
            //
            // The cost of leaving it off is honest and recoverable: those files are absent
            // from the DCLAP index, so --text and --embedding dclap won't find them until
            // they are re-analyzed with the flag. Nothing else degrades.
            std::optional<mira::DclapEmbeddingResult> dclapEmbedding;
            if (runDclap) {
                dclapEmbedding = mira::computeDclapEmbedding(*mono, c.audio.sampleRate,
                                                              MIRA_DCLAP_AUDIO_MODEL);
                if (dclapEmbedding->ok) machine << ",\"dclap_embedding\":" << mira::toJson(*dclapEmbedding);
                timer.mark("embedding (dclap)");
            }

            auto contentGate = mira::runContentGate(*mono, c.audio.sampleRate, MIRA_CED_MODEL);
            if (contentGate.ok) machine << ",\"content_gate\":" << mira::toJson(contentGate);
            timer.mark("content gate (CED-small)");

            if (embedding.ok && contentGate.ok && contentGate.isMusic) {
                auto moodTheme = mira::classifyMoodTheme(embedding.vector, MIRA_MOODTHEME_MODEL);
                if (moodTheme.ok) machine << ",\"moodtheme\":" << mira::toJson(moodTheme);
                timer.mark("moodtheme (mtg_jamendo_moodtheme)");

                auto instrument = mira::classifyInstrument(embedding.vector, MIRA_INSTRUMENT_MODEL);
                if (instrument.ok) {
                    machine << ",\"instrument\":" << mira::toJson(instrument);
                    if (instrumentTaxonomy.ok()) {
                        std::vector<std::string> names(mira::kInstrumentClassNames,
                                                        mira::kInstrumentClassNames + mira::kInstrumentClassCount);
                        machine << ",\"instrument_normalized\":"
                                << normalizedLabelsJson(names, instrument.scores, instrumentTaxonomy);
                    }
                }
                timer.mark("instrument (mtg_jamendo_instrument)");

                auto danceabilityHead = mira::classifyDanceability(embedding.vector, MIRA_DANCEABILITY_MODEL);
                if (danceabilityHead.ok) machine << ",\"danceability_head\":" << mira::toJson(danceabilityHead);
                timer.mark("danceability head (model-based)");

                auto genre = mira::classifyGenre(embedding.vector, MIRA_GENRE_MODEL);
                if (genre.ok) {
                    machine << ",\"genre\":" << mira::toJson(genre);
                    if (genreTaxonomy.ok()) {
                        std::vector<std::string> names(mira::kGenreClassNames,
                                                        mira::kGenreClassNames + mira::kGenreClassCount);
                        machine << ",\"genre_normalized\":"
                                << normalizedLabelsJson(names, genre.scores, genreTaxonomy);
                    }
                }
                timer.mark("genre (genre_discogs400)");

                auto voiceInstrumental =
                    mira::classifyVoiceInstrumental(embedding.vector, MIRA_VOICE_INSTRUMENTAL_MODEL);
                if (voiceInstrumental.ok)
                    machine << ",\"voice_instrumental\":" << mira::toJson(voiceInstrumental);
                timer.mark("voice/instrumental");

                // Stem-specific: mtg_jamendo_instrument's embedding is full-mix-trained
                // and unreliable on isolated stems (real finding, TASKS.md) — this is a
                // complementary signal for stems only, not a replacement, since the two
                // can and do disagree. Operates on *mono directly, not the embedding
                // (different model family, its own 16kHz/1s-window frontend).
                if (finalContentType == "stem") {
                    auto stemInstrument = mira::classifyStemInstrument(*mono, c.audio.sampleRate,
                                                                        MIRA_IRMAS_INSTRUMENT_MODEL);
                    if (stemInstrument.ok) {
                        machine << ",\"stem_instrument\":" << mira::toJson(stemInstrument);
                        if (stemInstrumentTaxonomy.ok()) {
                            // IRMAS's own raw code order (write_metadata_irmas.py's
                            // label_dict) — matches StemInstrumentResult::scores' order.
                            static const std::vector<std::string> kIrmasCodes = {
                                "cel", "cla", "flu", "gac", "gel", "org", "pia", "sax", "tru", "vio", "voi"};
                            machine << ",\"stem_instrument_normalized\":"
                                    << normalizedLabelsJson(kIrmasCodes, stemInstrument.scores,
                                                             stemInstrumentTaxonomy);
                        }
                    }
                    timer.mark("stem instrument (IRMAS/nii-yamagishilab)");
                }
            }

            // MIR (PRD §5B) — loops/tracks/stems only. Tempo on a 300ms one-shot is
            // "wasted work [producing] confident nonsense" (PRD §5).
            if (finalContentType != "one_shot") {
                auto rhythm = mira::analyzeRhythm(*mono, c.audio.sampleRate, MIRA_BEAT_THIS_MODEL,
                                                   runRecheckTempo);
                // Beats and downbeats are positions too, and mira_ui draws a bar ruler
                // from them -- a downbeat left in spliced time would put the bar lines
                // somewhere the audio isn't. BPM itself needs no conversion: it comes
                // from the mean interval between beats within the spliced audio, and
                // splicing out silence doesn't change the tempo of what remains.
                for (auto& beat : rhythm.beatThisBeats)
                    beat = mira::activeTimeToFileTime(fileTimeSpans, beat);
                for (auto& downbeat : rhythm.beatThisDownbeats)
                    downbeat = mira::activeTimeToFileTime(fileTimeSpans, downbeat);
                for (auto& tick : rhythm.essentiaBeatTicks)
                    tick = mira::activeTimeToFileTime(fileTimeSpans, tick);
                if (rhythm.ok) machine << ",\"rhythm\":" << mira::toJson(rhythm);
                timer.mark("rhythm (essentia + beat_this_cpp)");

                // Key and chords: both gated on harmonic content (PRD §12b) — never run
                // blindly on a rhythm stem or noise. Uses the real harmonicity descriptor
                // (Descriptors.h) now, not a proxy; same gate reused for both.
                if (mira::shouldRunKeyDetection(dsp.harmonicity, dsp.harmonicityFrameCount)) {
                    auto key = mira::detectKey(*mono, c.audio.sampleRate);
                    machine << ",\"key\":" << mira::toJson(key);
                    timer.mark("key (libKeyFinder)");

                    // Opt-in (--chords): measured at 15.0s on a 5:08 song, 39% of the
                    // total analyze time — well past what the Phase 1 headline goal needs.
                    if (runChords) {
                        auto chords = mira::detectChords(*mono, c.audio.sampleRate);
                        // Back into file time before serialising: Chordino saw the
                        // spliced buffer, so every timeSeconds it returned is an offset
                        // into that, not a position in the file (ActiveSpanMap.h).
                        for (auto& chord : chords.chords)
                            chord.timeSeconds = mira::activeTimeToFileTime(fileTimeSpans, chord.timeSeconds);
                        if (chords.ok) machine << ",\"chords\":" << mira::toJson(chords);
                        timer.mark("chords (Chordino)");
                    }
                }

                // Note transcription (PRD §5, §12b) — "a first-class feature, not a MIR
                // afterthought," but opt-in via --transcribe: measured at 3.7s on a
                // 5:08 song (10% of total analyze time). Shares the one_shot gate above
                // (a 300ms clip has nothing to transcribe) but, unlike key/chords, would
                // NOT be additionally gated on harmonic content if enabled — it's useful
                // on percussive material too, and a transcription that finds few or no
                // notes there is itself informative, not "confident nonsense".
                if (runTranscription) {
                    auto transcription = mira::transcribe(*mono, c.audio.sampleRate,
                                                            MIRA_BASIC_PITCH_MODEL);
                    // Same conversion as chords above. Both endpoints are mapped
                    // independently here rather than split at splice points the way the
                    // UI splits a drawn block: `notes` is a record of note events, and
                    // turning one straddling note into two would misstate how many notes
                    // were transcribed. A consumer that draws them clips to active_spans
                    // instead (Main.cpp's reloadTimelineLanes).
                    for (auto& note : transcription.notes) {
                        note.startSeconds = mira::activeTimeToFileTime(fileTimeSpans, note.startSeconds);
                        note.endSeconds = mira::activeTimeToFileTime(fileTimeSpans, note.endSeconds);
                    }
                    if (transcription.ok)
                        machine << ",\"notes\":" << mira::toJson(transcription);
                    timer.mark("note transcription (Basic Pitch)");
                }
            }

            machine << "}";
            update.machineJson = machine.str();
            // Per file, not the run's start time -- see the `analyzedAt` comment above.
            update.analyzedAt = nowUnix();
            db.applyAnalysis(update);
            if (embedding.ok) {
                std::vector<float> embeddingF(embedding.vector.begin(), embedding.vector.end());
                db.upsertEmbedding(c.record.id, embeddingF);
            }
            if (dclapEmbedding && dclapEmbedding->ok) {
                std::vector<float> dclapEmbeddingF(dclapEmbedding->vector.begin(),
                                                    dclapEmbedding->vector.end());
                db.upsertDclapEmbedding(c.record.id, dclapEmbeddingF);
            }
            // Per-dimension similarity (TASKS.md Phase 4, PRD §12 item 5, `mira similar
            // --by timbre|spectrum`). dsp.mfcc/spectralCentroidHz/spectralFlatness come
            // from the same shared frame loop (Descriptors.cpp) -- mfcc.empty() is the
            // one guard needed, since an empty mfcc means no frame was measurable at all
            // (the file was too short), and centroid/flatness are computed in that same
            // loop. 11000.0 here is Descriptors.cpp's own MFCC highFrequencyBound, reused
            // as the normalization ceiling rather than inventing a new constant.
            if (!dsp.mfcc.empty()) {
                std::vector<float> mfccF(dsp.mfcc.begin(), dsp.mfcc.end());
                db.upsertTimbre(c.record.id, mfccF);

                double centroidNorm = std::log1p(dsp.spectralCentroidHz) / std::log1p(11000.0);
                std::vector<float> spectrumF = {static_cast<float>(centroidNorm),
                                                 static_cast<float>(dsp.spectralFlatness)};
                db.upsertSpectrum(c.record.id, spectrumF);
            }
            timer.mark("database write");

            // Stems only, per review ("samples of the stem"). Long non-stem files also get
            // active-region detection past 5 minutes, but a track's active region is
            // nearly the whole track -- one giant segment adds nothing but a row.
            if (finalContentType == "stem" && !activeSpans.empty()) {
                createAutoSegments(db, c.record.id, activeSpans, c.audio.mono, c.audio.left, c.audio.right,
                                   c.audio.sampleRate, instrumentTaxonomy, genreTaxonomy,
                                   stemInstrumentTaxonomy, /*stemParent=*/true);
                timer.mark("auto-segments");
            } else {
                // No longer a stem (a mix that used to be routed as one, review round 4):
                // drop the auto-segments that routing gave it, keeping anything a person
                // made or tagged, exactly as a re-analysis would.
                db.deleteUntouchedAutoSegmentsForFile(c.record.id);
            }

            // A stable, plain-stdout progress marker (unlike the per-stage timings
            // above, which only print under --verbose and go to stderr) -- mira_ui's
            // AnalyzeJob reads this line-by-line off the subprocess's stdout the same
            // way ScanJob already reads mira::scan()'s own in-process progress callback.
            std::cout << "progress: " << ++completedCount << "/" << candidates.size() << " " << c.record.path
                       << std::endl;
        }
    }

    std::cout << "analyzed " << candidates.size() << " files: ";
    bool first = true;
    for (auto& [type, n] : counts) {
        if (!first) std::cout << ", ";
        std::cout << n << " " << type;
        first = false;
    }
    std::cout << std::endl;
    if (failed > 0) std::cout << failed << " files could not be decoded" << std::endl;

    return 0;
}

// PRD §8: "human-readable report, flags low-confidence fields... surfaces low-confidence
// tempo/key rather than hiding it, and reports active_ratio so a mostly-silent stem is
// visibly mostly silent." Reads `machine` via SQLite's json_extract (Database's small
// JSON helpers) rather than a C++ JSON parser — mira doesn't vendor one.
int runInspect(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
        else positional.push_back(args[i]);
    }

    if (positional.empty()) {
        std::cerr << "mira inspect: a file path or numeric id is required" << std::endl;
        return 1;
    }
    const std::string& target = positional[0];

    mira::Database db(dbPath);

    std::optional<mira::FileRecord> record;
    bool isNumeric = !target.empty() &&
                      std::all_of(target.begin(), target.end(), [](unsigned char c) { return std::isdigit(c); });
    if (isNumeric) record = db.findById(std::stoll(target));
    if (!record) record = db.findByPath(target);

    if (!record) {
        std::cerr << "mira inspect: no file found for '" << target << "'" << std::endl;
        return 1;
    }

    const auto& r = *record;
    std::cout << std::fixed << std::setprecision(2);

    std::cout << "path:          " << r.path << "\n";
    std::cout << "content_type:  " << r.contentType << " (" << r.contentTypeSource << ")\n";
    std::cout << "sha256:        " << r.sha256 << "\n";

    auto duration = db.jsonExtractDouble(r.machine, "$.duration_seconds");
    if (duration) {
        std::cout << "duration:      " << *duration << "s";
        if (auto sr = db.jsonExtractDouble(r.machine, "$.sample_rate"))
            std::cout << "   sample_rate: " << static_cast<int>(*sr) << " Hz";
        if (auto ch = db.jsonExtractDouble(r.machine, "$.num_channels"))
            std::cout << "   channels: " << static_cast<int>(*ch);
        std::cout << "\n";
    } else {
        std::cout << "duration:      not yet analyzed (run `mira analyze`)\n";
        return 0;
    }

    if (r.activeRatio) {
        std::cout << "active_ratio:  " << *r.activeRatio;
        if (*r.activeRatio < 0.5) std::cout << "  ⚠ mostly silent";
        std::cout << "\n";
    } else {
        std::cout << "active_ratio:  not computed (only tracked for stems, or files over 5 min — PRD §5)\n";
    }

    if (auto lufs = db.jsonExtractDouble(r.machine, "$.dsp.integrated_loudness_lufs")) {
        std::cout << "\nDSP:\n  loudness:      " << *lufs << " LUFS";
        if (auto lra = db.jsonExtractDouble(r.machine, "$.dsp.loudness_range_lu"))
            std::cout << "   range " << *lra << " LU";
        if (auto peak = db.jsonExtractDouble(r.machine, "$.dsp.true_peak_db"))
            std::cout << "   true peak " << *peak << " dB";
        std::cout << "\n";
        if (auto crest = db.jsonExtractDouble(r.machine, "$.dsp.crest_factor"))
            std::cout << "  crest factor:  " << *crest << "\n";
        auto centroid = db.jsonExtractDouble(r.machine, "$.dsp.spectral_centroid_hz");
        auto flatness = db.jsonExtractDouble(r.machine, "$.dsp.spectral_flatness");
        if (centroid || flatness) {
            std::cout << "  spectral:      ";
            if (centroid) std::cout << "centroid " << *centroid << " Hz (brightness)";
            if (flatness)
                std::cout << "   flatness " << *flatness << (*flatness > 0.3 ? " (noisy)" : " (tonal)");
            std::cout << "\n";
        }
        if (auto attack = db.jsonExtractDouble(r.machine, "$.dsp.attack_time_seconds"))
            std::cout << "  attack time:   " << *attack << "s\n";
    }

    if (auto patchCount = db.jsonExtractDouble(r.machine, "$.embedding.patch_count")) {
        std::cout << "\nClassification:\n  embedding:     discogs-effnet, " << *patchCount
                   << " mel patch(es) pooled\n";
    }
    if (auto musicScore = db.jsonExtractDouble(r.machine, "$.content_gate.music_score")) {
        // is_music is a JSON boolean; SQLite's json_extract returns it as integer 0/1,
        // not text, so read it as a double like every other boolean-ish field here.
        auto isMusicVal = db.jsonExtractDouble(r.machine, "$.content_gate.is_music");
        std::cout << "  content gate:  music score " << *musicScore;
        if (isMusicVal) std::cout << " (" << (*isMusicVal != 0.0 ? "music" : "not music") << ")";
        std::cout << "\n";
        if (auto labelCount = db.jsonArrayLength(r.machine, "$.content_gate.top_labels")) {
            std::cout << "  top labels:    ";
            for (int64_t i = 0; i < *labelCount && i < 5; ++i) {
                std::ostringstream namePath, scorePath;
                namePath << "$.content_gate.top_labels[" << i << "].name";
                scorePath << "$.content_gate.top_labels[" << i << "].score";
                auto name = db.jsonExtractString(r.machine, namePath.str());
                auto score = db.jsonExtractDouble(r.machine, scorePath.str());
                if (!name || !score) continue;
                if (i > 0) std::cout << ", ";
                std::cout << *name << " (" << *score << ")";
            }
            std::cout << "\n";
        }
    }
    if (auto sample = db.jsonExtractDouble(r.machine, "$.moodtheme.action")) {
        (void)sample; // presence check only — moodtheme is an object keyed by label name,
                       // not an array, so there's no single "is it there" field to probe
        std::vector<std::pair<std::string, double>> scored;
        for (int i = 0; i < mira::kMoodThemeClassCount; ++i) {
            std::string path = std::string("$.moodtheme.") + mira::kMoodThemeClassNames[i];
            if (auto score = db.jsonExtractDouble(r.machine, path))
                scored.emplace_back(mira::kMoodThemeClassNames[i], *score);
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::cout << "  moodtheme:     ";
        for (size_t i = 0; i < scored.size() && i < 5; ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << scored[i].first << " (" << scored[i].second << ")";
        }
        std::cout << "\n";
    }
    if (auto sample = db.jsonExtractDouble(r.machine, "$.instrument.piano")) {
        (void)sample; // presence check only — same object-keyed-by-label-name shape as moodtheme
        std::vector<std::pair<std::string, double>> scored;
        for (int i = 0; i < mira::kInstrumentClassCount; ++i) {
            std::string path = std::string("$.instrument.") + mira::kInstrumentClassNames[i];
            if (auto score = db.jsonExtractDouble(r.machine, path))
                scored.emplace_back(mira::kInstrumentClassNames[i], *score);
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::cout << "  instruments:   ";
        for (size_t i = 0; i < scored.size() && i < 5; ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << scored[i].first << " (" << scored[i].second << ")";
        }
        std::cout << "\n";
    }
    if (auto danceableProb = db.jsonExtractDouble(r.machine, "$.danceability_head.danceable_probability")) {
        std::cout << "  danceable:     " << *danceableProb << " (model-based; see also DSP danceability below)\n";
    }
    if (auto voiceProb = db.jsonExtractDouble(r.machine, "$.voice_instrumental.voice_probability")) {
        std::cout << "  voice/instr:   " << *voiceProb << " probability of voice (vs. instrumental)\n";
    }
    // Genre labels (unlike moodtheme/instrument) contain spaces and punctuation
    // ("Blues---Boogie Woogie", "Rock---Yé-Yé") — SQLite's json_extract path syntax
    // needs the key segment double-quoted whenever it's not a bare identifier. Prefers
    // genre_normalized (taxonomy/genre-labels.yaml's "Genre: Style" form) over the raw
    // "Genre---Style" for display, falling back to raw if normalization wasn't available
    // for this row (e.g. analyzed before this feature existed, or the taxonomy file
    // failed to load) — never silently showing nothing just because the nicer field is
    // missing. The "---" -> ": " transform below duplicates genre-labels.yaml's own
    // mechanical rule (documented there) rather than reading it back out of the taxonomy
    // file a second time — it's a one-line formatting rule, not a judgment call.
    if (auto sample = db.jsonExtractDouble(
            r.machine, std::string("$.genre.\"") + mira::kGenreClassNames[0] + "\"")) {
        (void)sample; // presence check only — genre is an object keyed by label name,
                       // not an array, so there's no single fixed field to probe; use
                       // whatever the first label actually is rather than guessing one
        std::vector<std::pair<std::string, double>> scored;
        for (int i = 0; i < mira::kGenreClassCount; ++i) {
            std::string rawName = mira::kGenreClassNames[i];
            std::string normalizedName = rawName;
            size_t sep = normalizedName.find("---");
            if (sep != std::string::npos) normalizedName.replace(sep, 3, ": ");

            std::string normalizedPath = std::string("$.genre_normalized.\"") + normalizedName + "\"";
            std::string rawPath = std::string("$.genre.\"") + rawName + "\"";
            if (auto score = db.jsonExtractDouble(r.machine, normalizedPath)) {
                scored.emplace_back(normalizedName, *score);
            } else if (auto rawScore = db.jsonExtractDouble(r.machine, rawPath)) {
                scored.emplace_back(rawName, *rawScore);
            }
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::cout << "  genre:         ";
        for (size_t i = 0; i < scored.size() && i < 5; ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << scored[i].first << " (" << scored[i].second << ")";
        }
        std::cout << "\n";
    }
    if (auto windowCount = db.jsonExtractDouble(r.machine, "$.stem_instrument.window_count")) {
        // IRMAS's own label order (write_metadata_irmas.py's label_dict) with display names.
        static const std::pair<const char*, const char*> kStemInstrumentLabels[] = {
            {"cel", "cello"}, {"cla", "clarinet"}, {"flu", "flute"}, {"gac", "acoustic guitar"},
            {"gel", "electric guitar"}, {"org", "organ"}, {"pia", "piano"}, {"sax", "saxophone"},
            {"tru", "trumpet"}, {"vio", "violin"}, {"voi", "voice"},
        };
        std::vector<std::pair<std::string, double>> scored;
        for (const auto& [code, name] : kStemInstrumentLabels) {
            std::string path = std::string("$.stem_instrument.scores.") + code;
            if (auto score = db.jsonExtractDouble(r.machine, path)) scored.emplace_back(name, *score);
        }
        std::sort(scored.begin(), scored.end(),
                  [](const auto& a, const auto& b) { return a.second > b.second; });
        std::cout << "  stem instrument (predominant-instrument model, " << *windowCount << " windows): ";
        for (size_t i = 0; i < scored.size() && i < 3; ++i) {
            if (i > 0) std::cout << ", ";
            std::cout << scored[i].first << " (" << scored[i].second << ")";
        }
        std::cout << "\n";
    }

    // These numeric rhythm fields are always serialized (default 0.0 when unmeasured,
    // e.g. essentia_bpm/bpm_ratio when --recheck-tempo wasn't used), so json_extract
    // always returns a present-but-possibly-0 value — an optional-has-value check alone
    // would treat "unmeasured" as "measured as zero." Gate on a real BPM (>0) instead.
    auto beatThisBpmRaw = db.jsonExtractDouble(r.machine, "$.rhythm.beat_this_bpm");
    auto essentiaBpmRaw = db.jsonExtractDouble(r.machine, "$.rhythm.essentia_bpm");
    bool haveBeatThis = beatThisBpmRaw && *beatThisBpmRaw > 0.0;
    bool haveEssentia = essentiaBpmRaw && *essentiaBpmRaw > 0.0;
    if (haveBeatThis || haveEssentia) {
        std::cout << "\nRhythm:\n";
        if (haveBeatThis) std::cout << "  beat_this:     " << *beatThisBpmRaw << " BPM (default estimator)\n";
        if (haveEssentia) {
            std::cout << "  essentia:      " << *essentiaBpmRaw << " BPM (--recheck-tempo)";
            if (auto conf = db.jsonExtractDouble(r.machine, "$.rhythm.essentia_confidence"))
                std::cout << "  (confidence " << *conf << ")";
            std::cout << "\n";
        }
        if (haveBeatThis && haveEssentia) {
            if (auto ratio = db.jsonExtractDouble(r.machine, "$.rhythm.bpm_ratio")) {
                if (std::abs(*ratio - 1.0) > 0.05) {
                    std::cout << std::setprecision(3)
                               << "  ⚠ estimators disagree (ratio " << *ratio
                               << ") — treat both with caution (PRD §14.1)\n"
                               << std::setprecision(2);
                }
            }
        } else if (haveBeatThis) {
            std::cout << "  (single estimator — pass --recheck-tempo to compare against Essentia)\n";
        }
        auto tempoWindows = db.jsonExtractDouble(r.machine, "$.rhythm.tempo_window_count");
        if (tempoWindows && *tempoWindows >= 3) {
            auto unstable = db.jsonExtractDouble(r.machine, "$.rhythm.tempo_unstable");
            if (unstable && *unstable != 0.0) {
                auto stddev = db.jsonExtractDouble(r.machine, "$.rhythm.tempo_stability_bpm_stddev");
                auto range = db.jsonExtractDouble(r.machine, "$.rhythm.tempo_range_bpm");
                std::cout << "  ⚠ tempo is not stable across the file (stddev "
                           << (stddev ? *stddev : 0.0) << " BPM, range " << (range ? *range : 0.0)
                           << " BPM over " << *tempoWindows
                           << " windows) — the single BPM above is an average, not a summary\n";
            }
        }
        if (auto dance = db.jsonExtractDouble(r.machine, "$.rhythm.danceability"))
            std::cout << "  danceability:  " << *dance << "\n";
    } else if (r.contentType == "one_shot") {
        std::cout << "\nRhythm: not analyzed (one-shots are skipped — tempo on a short clip is meaningless)\n";
    }

    if (auto keyName = db.jsonExtractString(r.machine, "$.key.key")) {
        std::cout << "\nKey:    " << *keyName;
        auto camelot = db.jsonExtractString(r.machine, "$.key.camelot");
        auto openKey = db.jsonExtractString(r.machine, "$.key.open_key");
        if (camelot || openKey) {
            std::cout << "   (";
            if (camelot) std::cout << "Camelot " << *camelot;
            if (camelot && openKey) std::cout << ", ";
            if (openKey) std::cout << "Open Key " << *openKey;
            std::cout << ")";
        }
        std::cout << "\n";
    } else if (haveBeatThis || haveEssentia) {
        std::cout << "\nKey:    not analyzed (gated on harmonic content — likely noisy/non-tonal)\n";
    }

    if (auto chordCount = db.jsonArrayLength(r.machine, "$.chords"))
        std::cout << "Chords: " << *chordCount << " segments\n";
    if (auto noteCount = db.jsonArrayLength(r.machine, "$.notes"))
        std::cout << "Notes:  " << *noteCount << " transcribed\n";

    if (r.human != "{}") std::cout << "\nhuman overrides: " << r.human << "\n";

    return 0;
}

namespace {

std::string jsonEscapeForTag(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        out += static_cast<char>(c);
    }
    return out;
}

// Builds a JSON string-array literal from a CLI value like "funny, quirky" -- whitespace
// around each comma-separated item is trimmed, empty items are dropped.
std::string jsonStringArrayLiteral(const std::string& commaSeparated) {
    std::ostringstream out;
    out << "[";
    std::istringstream iss(commaSeparated);
    std::string item;
    bool first = true;
    while (std::getline(iss, item, ',')) {
        size_t start = item.find_first_not_of(" \t");
        size_t end = item.find_last_not_of(" \t");
        if (start == std::string::npos) continue;
        std::string trimmed = item.substr(start, end - start + 1);
        if (!first) out << ",";
        out << "\"" << jsonEscapeForTag(trimmed) << "\"";
        first = false;
    }
    out << "]";
    return out.str();
}

} // namespace

// PRD §11/§15, TASKS.md Phase 3: renders CaptionFields (the one analysis document) into
// SA3's own trained prompt shape via Sa3Renderer -- not a generic caption. PRD's "N
// renderers" stays N=1 until a second trainer target is actually being built; adding one
// later means a second Renderer.h, not a change to CaptionFields.
int runCaption(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    std::string trigger;
    bool emitSidecar = false;
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
        else if (args[i] == "--trigger" && i + 1 < args.size()) trigger = args[++i];
        else if (args[i] == "--emit-sidecar") emitSidecar = true;
        else positional.push_back(args[i]);
    }

    if (positional.empty()) {
        std::cerr << "mira caption: a file path or numeric id is required" << std::endl;
        return 1;
    }
    const std::string& target = positional[0];

    mira::Database db(dbPath);

    std::optional<mira::FileRecord> record;
    bool isNumeric = !target.empty() &&
                      std::all_of(target.begin(), target.end(), [](unsigned char c) { return std::isdigit(c); });
    if (isNumeric) record = db.findById(std::stoll(target));
    if (!record) record = db.findByPath(target);

    if (!record) {
        std::cerr << "mira caption: no file found for '" << target << "'" << std::endl;
        return 1;
    }
    if (!record->analyzedAt) {
        std::cerr << "mira caption: '" << target << "' has not been analyzed yet (run `mira analyze`)"
                   << std::endl;
        return 1;
    }

    mira::CaptionFields fields = mira::extractCaptionFields(db, *record);

    std::cout << "prose: " << mira::renderSa3Prose(fields, trigger) << "\n";
    std::cout << "tags:\n";
    for (auto& [key, value] : mira::renderSa3Tags(fields, trigger)) {
        if (key == "prompt") continue; // identical to the `prose:` line above, skip the duplicate
        std::cout << "  " << key << ": " << value << "\n";
    }

    if (emitSidecar) {
        std::string sidecarPath = record->path;
        size_t dot = sidecarPath.find_last_of('.');
        sidecarPath = (dot == std::string::npos ? sidecarPath : sidecarPath.substr(0, dot)) + ".json";
        std::ofstream out(sidecarPath);
        if (!out) {
            std::cerr << "mira caption: could not write sidecar to " << sidecarPath << std::endl;
            return 1;
        }
        out << mira::renderSa3SidecarJson(fields, trigger);
        std::cout << "\nsidecar written: " << sidecarPath << "\n";
    }

    return 0;
}

// PRD §6/§11: the write side of `human` overrides -- CaptionFields.cpp documents exactly
// which keys it reads back out. Each flag here merge-updates the one field it names via
// Database::setHumanField, leaving every other `human` key (set by an earlier `mira tag`
// call, or not) untouched. This is deliberately the *only* way to put a scene/vibe word
// like "funny" or "action" into a caption -- mira has no analyzer for that kind of
// judgment and isn't meant to grow one; --keywords exists specifically for it.
int runTag(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    std::vector<std::string> positional;
    std::optional<std::string> genre, instruments, moods, keywords, key;
    std::optional<double> bpm;
    std::optional<bool> isInstrumental;
    bool clear = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
        else if (args[i] == "--genre" && i + 1 < args.size()) genre = args[++i];
        else if (args[i] == "--instruments" && i + 1 < args.size()) instruments = args[++i];
        else if (args[i] == "--moods" && i + 1 < args.size()) moods = args[++i];
        else if (args[i] == "--keywords" && i + 1 < args.size()) keywords = args[++i];
        else if (args[i] == "--bpm" && i + 1 < args.size()) bpm = std::stod(args[++i]);
        else if (args[i] == "--key" && i + 1 < args.size()) key = args[++i];
        else if (args[i] == "--is-instrumental" && i + 1 < args.size()) {
            std::string v = args[++i];
            isInstrumental = (v == "true" || v == "1" || v == "yes");
        } else if (args[i] == "--clear") {
            clear = true;
        } else {
            positional.push_back(args[i]);
        }
    }

    if (positional.empty()) {
        std::cerr << "mira tag: a file path or numeric id is required" << std::endl;
        return 1;
    }
    const std::string& target = positional[0];

    mira::Database db(dbPath);

    std::optional<mira::FileRecord> record;
    bool isNumeric = !target.empty() &&
                      std::all_of(target.begin(), target.end(), [](unsigned char c) { return std::isdigit(c); });
    if (isNumeric) record = db.findById(std::stoll(target));
    if (!record) record = db.findByPath(target);

    if (!record) {
        std::cerr << "mira tag: no file found for '" << target << "'" << std::endl;
        return 1;
    }

    if (clear) {
        db.clearHumanFields(record->id);
        std::cout << "cleared human overrides for " << record->path << std::endl;
        return 0;
    }

    if (!genre && !instruments && !moods && !keywords && !bpm && !key && !isInstrumental) {
        std::cerr << "mira tag: nothing to set -- pass at least one of --genre / --instruments / "
                     "--moods / --keywords / --bpm / --key / --is-instrumental, or --clear"
                  << std::endl;
        return 1;
    }

    if (genre) db.setHumanField(record->id, "$.genre", jsonStringArrayLiteral(*genre));
    if (instruments) db.setHumanField(record->id, "$.instruments", jsonStringArrayLiteral(*instruments));
    if (moods) db.setHumanField(record->id, "$.moods", jsonStringArrayLiteral(*moods));
    if (keywords) db.setHumanField(record->id, "$.keywords", jsonStringArrayLiteral(*keywords));
    if (bpm) db.setHumanField(record->id, "$.bpm", std::to_string(*bpm));
    if (key) db.setHumanField(record->id, "$.key", "\"" + jsonEscapeForTag(*key) + "\"");
    if (isInstrumental) db.setHumanField(record->id, "$.is_instrumental", *isInstrumental ? "true" : "false");

    auto updated = db.findById(record->id);
    std::cout << "human overrides for " << updated->path << ": " << updated->human << std::endl;
    return 0;
}

// TASKS.md Phase 3 addition: folder-level `human` defaults -- `mira tag` alone means
// tagging a real library is one call per file, which doesn't scale. `folder` is matched
// as a plain string prefix against files.path at caption-render time
// (Database::findFolderDefaultsForPath), not resolved against the filesystem, so it must
// be written the same way (relative vs absolute) the target files were actually scanned
// with -- this command only checks the folder exists on disk as a sanity check, it
// doesn't normalize the string used for matching.
int runTagFolder(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    std::vector<std::string> positional;
    std::optional<std::string> genre, instruments, moods, keywords, key;
    std::optional<double> bpm;
    std::optional<bool> isInstrumental;
    bool clear = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
        else if (args[i] == "--genre" && i + 1 < args.size()) genre = args[++i];
        else if (args[i] == "--instruments" && i + 1 < args.size()) instruments = args[++i];
        else if (args[i] == "--moods" && i + 1 < args.size()) moods = args[++i];
        else if (args[i] == "--keywords" && i + 1 < args.size()) keywords = args[++i];
        else if (args[i] == "--bpm" && i + 1 < args.size()) bpm = std::stod(args[++i]);
        else if (args[i] == "--key" && i + 1 < args.size()) key = args[++i];
        else if (args[i] == "--is-instrumental" && i + 1 < args.size()) {
            std::string v = args[++i];
            isInstrumental = (v == "true" || v == "1" || v == "yes");
        } else if (args[i] == "--clear") {
            clear = true;
        } else {
            positional.push_back(args[i]);
        }
    }

    if (positional.empty()) {
        std::cerr << "mira tag-folder: a folder path is required" << std::endl;
        return 1;
    }
    std::string folderPath = positional[0];
    while (folderPath.size() > 1 && folderPath.back() == '/') folderPath.pop_back();

    if (!std::filesystem::is_directory(folderPath)) {
        std::cerr << "mira tag-folder: '" << folderPath << "' is not a directory" << std::endl;
        return 1;
    }

    mira::Database db(dbPath);

    if (clear) {
        db.clearFolderDefault(folderPath);
        std::cout << "cleared folder default for " << folderPath << std::endl;
        return 0;
    }

    if (!genre && !instruments && !moods && !keywords && !bpm && !key && !isInstrumental) {
        std::cerr << "mira tag-folder: nothing to set -- pass at least one of --genre / --instruments / "
                     "--moods / --keywords / --bpm / --key / --is-instrumental, or --clear"
                  << std::endl;
        return 1;
    }

    if (genre) db.setFolderDefaultField(folderPath, "$.genre", jsonStringArrayLiteral(*genre));
    if (instruments) db.setFolderDefaultField(folderPath, "$.instruments", jsonStringArrayLiteral(*instruments));
    if (moods) db.setFolderDefaultField(folderPath, "$.moods", jsonStringArrayLiteral(*moods));
    if (keywords) db.setFolderDefaultField(folderPath, "$.keywords", jsonStringArrayLiteral(*keywords));
    if (bpm) db.setFolderDefaultField(folderPath, "$.bpm", std::to_string(*bpm));
    if (key) db.setFolderDefaultField(folderPath, "$.key", "\"" + jsonEscapeForTag(*key) + "\"");
    if (isInstrumental)
        db.setFolderDefaultField(folderPath, "$.is_instrumental", *isInstrumental ? "true" : "false");

    std::cout << "folder default set for " << folderPath
               << " -- applies to every scanned file under it whose own `human`/segment tags"
                  " don't already override the same field"
               << std::endl;
    return 0;
}

// TASKS.md Phase 4 "segment-level analysis replacing whole-track averaging". Runs a
// *subset* of the whole-file analyze loop's pipeline (above) against one already-sliced
// segment buffer: DSP descriptors, both embeddings, and -- gated on the content gate's
// is_music exactly like the whole-file loop -- moodtheme/instrument/danceability/genre/
// voice_instrumental. Deliberately excludes: active-region extraction (the slice was
// already hand-picked by whoever called `mira tag-segment`; re-restricting it to "active"
// sub-spans would fight that choice, not honor it), rhythm/key/chords/transcription (a
// scoped-out Phase 4 decision -- beat_this in particular needs a few seconds of audio to
// be reliable, and per-segment reruns of the full MIR stack were judged not worth the
// added cost for v1 -- CaptionFields.cpp's extractCaptionFieldsForSegment keeps bpm/key
// at the file's whole-file values for exactly this reason), and stem_instrument (not
// re-run per segment; CaptionFields.cpp's applySegmentMachine only ever reads the plain
// "$.instrument" path back out, so this never writes "$.stem_instrument" either).
std::string buildSegmentMachineJson(const std::vector<float>& mono, const std::vector<float>& left,
                                     const std::vector<float>& right, int sampleRate,
                                     mira::Taxonomy& instrumentTaxonomy, mira::Taxonomy& genreTaxonomy,
                                     mira::Taxonomy& stemInstrumentTaxonomy, bool stemParent) {
    std::ostringstream machine;
    machine << "{\"duration_seconds\":" << (static_cast<double>(mono.size()) / sampleRate)
            << ",\"sample_rate\":" << sampleRate;

    auto dsp = mira::computeDspDescriptors(left, right, sampleRate);
    machine << ",\"dsp\":" << mira::toJson(dsp);

    auto embedding = mira::computeEmbedding(mono, sampleRate, MIRA_EFFNET_MODEL);
    if (embedding.ok) machine << ",\"embedding\":" << mira::toJson(embedding);

    auto dclapEmbedding = mira::computeDclapEmbedding(mono, sampleRate, MIRA_DCLAP_AUDIO_MODEL);
    if (dclapEmbedding.ok) machine << ",\"dclap_embedding\":" << mira::toJson(dclapEmbedding);

    auto contentGate = mira::runContentGate(mono, sampleRate, MIRA_CED_MODEL);
    if (contentGate.ok) machine << ",\"content_gate\":" << mira::toJson(contentGate);

    if (embedding.ok && contentGate.ok && contentGate.isMusic) {
        auto moodTheme = mira::classifyMoodTheme(embedding.vector, MIRA_MOODTHEME_MODEL);
        if (moodTheme.ok) machine << ",\"moodtheme\":" << mira::toJson(moodTheme);

        auto instrument = mira::classifyInstrument(embedding.vector, MIRA_INSTRUMENT_MODEL);
        if (instrument.ok) {
            machine << ",\"instrument\":" << mira::toJson(instrument);
            if (instrumentTaxonomy.ok()) {
                std::vector<std::string> names(mira::kInstrumentClassNames,
                                                mira::kInstrumentClassNames + mira::kInstrumentClassCount);
                machine << ",\"instrument_normalized\":"
                        << normalizedLabelsJson(names, instrument.scores, instrumentTaxonomy);
            }
        }

        // The stem-tuned opinion, for a segment of a stem (review round 4). Without it a
        // segment could only ever report the full-mix model, so a segment of a vocal
        // stem read "synthesizer" under a file that read "voice" -- the two rows
        // disagreeing by construction. Same model, same taxonomy, same raw-code order as
        // the whole-file pass above.
        if (stemParent) {
            auto stemInstrument = mira::classifyStemInstrument(mono, sampleRate, MIRA_IRMAS_INSTRUMENT_MODEL);
            if (stemInstrument.ok) {
                machine << ",\"stem_instrument\":" << mira::toJson(stemInstrument);
                if (stemInstrumentTaxonomy.ok()) {
                    static const std::vector<std::string> kIrmasCodes = {
                        "cel", "cla", "flu", "gac", "gel", "org", "pia", "sax", "tru", "vio", "voi"};
                    machine << ",\"stem_instrument_normalized\":"
                            << normalizedLabelsJson(kIrmasCodes, stemInstrument.scores, stemInstrumentTaxonomy);
                }
            }
        }

        auto danceabilityHead = mira::classifyDanceability(embedding.vector, MIRA_DANCEABILITY_MODEL);
        if (danceabilityHead.ok) machine << ",\"danceability_head\":" << mira::toJson(danceabilityHead);

        auto genre = mira::classifyGenre(embedding.vector, MIRA_GENRE_MODEL);
        if (genre.ok) {
            machine << ",\"genre\":" << mira::toJson(genre);
            if (genreTaxonomy.ok()) {
                std::vector<std::string> names(mira::kGenreClassNames,
                                                mira::kGenreClassNames + mira::kGenreClassCount);
                machine << ",\"genre_normalized\":" << normalizedLabelsJson(names, genre.scores, genreTaxonomy);
            }
        }

        auto voiceInstrumental = mira::classifyVoiceInstrumental(embedding.vector, MIRA_VOICE_INSTRUMENTAL_MODEL);
        if (voiceInstrumental.ok) machine << ",\"voice_instrumental\":" << mira::toJson(voiceInstrumental);
    }

    machine << "}";
    return machine.str();
}

// TASKS.md Phase 3/4: the write side of a time-ranged caption boundary (see
// Database.cpp's `segments` table comment for the full rationale -- a long through-
// composed file, or a synced set of delivery stems sharing one files.group_id, whose
// character changes partway through can't get one honest whole-file caption). Target
// resolution mirrors `mira tag`/`mira caption` exactly, plus one more case: a target
// containing '|' is a group_id (main.cpp's own siblingKey format, `<parentDir>|
// <durationSeconds>` -- never valid in a plain path or numeric id, so unambiguous),
// meaning this boundary and its tags apply to every file in that stem group at the same
// timestamps, keeping the set in sync. Since TASKS.md Phase 4, this also immediately
// slices and analyzes the declared range for every affected file (buildSegmentMachineJson
// above) -- the segment is fully described the moment it's created, not just bounded.
// `mira export-segments` is still what actually cuts and writes the audio clips.
int runTagSegment(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    std::vector<std::string> positional;
    std::optional<double> startSeconds, endSeconds, bpm;
    std::optional<std::string> genre, instruments, moods, keywords, key;
    std::optional<bool> isInstrumental;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
        else if (args[i] == "--start" && i + 1 < args.size()) startSeconds = std::stod(args[++i]);
        else if (args[i] == "--end" && i + 1 < args.size()) endSeconds = std::stod(args[++i]);
        else if (args[i] == "--genre" && i + 1 < args.size()) genre = args[++i];
        else if (args[i] == "--instruments" && i + 1 < args.size()) instruments = args[++i];
        else if (args[i] == "--moods" && i + 1 < args.size()) moods = args[++i];
        else if (args[i] == "--keywords" && i + 1 < args.size()) keywords = args[++i];
        else if (args[i] == "--bpm" && i + 1 < args.size()) bpm = std::stod(args[++i]);
        else if (args[i] == "--key" && i + 1 < args.size()) key = args[++i];
        else if (args[i] == "--is-instrumental" && i + 1 < args.size()) {
            std::string v = args[++i];
            isInstrumental = (v == "true" || v == "1" || v == "yes");
        } else {
            positional.push_back(args[i]);
        }
    }

    if (positional.empty()) {
        std::cerr << "mira tag-segment: a group_id, file path, or numeric id is required" << std::endl;
        return 1;
    }
    if (!startSeconds || !endSeconds) {
        std::cerr << "mira tag-segment: --start and --end are required" << std::endl;
        return 1;
    }
    if (*endSeconds <= *startSeconds) {
        std::cerr << "mira tag-segment: --end must be greater than --start" << std::endl;
        return 1;
    }

    mira::Database db(dbPath);
    const std::string& target = positional[0];

    std::optional<std::string> groupId;
    std::optional<int64_t> fileId;
    std::vector<mira::FileRecord> targetFiles; // every file this segment's analysis runs against

    if (target.find('|') != std::string::npos) {
        groupId = target;
        targetFiles = db.findFilesByGroupId(target);
        if (targetFiles.empty()) {
            std::cerr << "mira tag-segment: no files found for group_id '" << target << "'" << std::endl;
            return 1;
        }
    } else {
        std::optional<mira::FileRecord> record;
        bool isNumeric = !target.empty() &&
                          std::all_of(target.begin(), target.end(), [](unsigned char c) { return std::isdigit(c); });
        if (isNumeric) record = db.findById(std::stoll(target));
        if (!record) record = db.findByPath(target);
        if (!record) {
            std::cerr << "mira tag-segment: no file found for '" << target << "'" << std::endl;
            return 1;
        }
        fileId = record->id;
        targetFiles.push_back(*record);
    }

    std::ostringstream human;
    human << "{";
    bool first = true;
    auto addField = [&](const char* fieldKey, const std::string& valueJson) {
        if (!first) human << ",";
        human << "\"" << fieldKey << "\":" << valueJson;
        first = false;
    };
    if (genre) addField("genre", jsonStringArrayLiteral(*genre));
    if (instruments) addField("instruments", jsonStringArrayLiteral(*instruments));
    if (moods) addField("moods", jsonStringArrayLiteral(*moods));
    if (keywords) addField("keywords", jsonStringArrayLiteral(*keywords));
    if (bpm) addField("bpm", std::to_string(*bpm));
    if (key) addField("key", "\"" + jsonEscapeForTag(*key) + "\"");
    if (isInstrumental) addField("is_instrumental", *isInstrumental ? "true" : "false");
    human << "}";

    int64_t segId = db.createSegment(groupId, fileId, *startSeconds, *endSeconds, human.str());
    std::cout << "segment " << segId << " created: " << *startSeconds << "s-" << *endSeconds << "s";
    if (groupId) std::cout << " for group " << *groupId;
    else std::cout << " for file id " << *fileId;
    std::cout << "\nhuman: " << human.str() << std::endl;

    // TASKS.md Phase 4 "segment-level analysis" — analyze immediately, one file at a
    // time (a group_id segment covers multiple sibling stems, each with its own audio in
    // this time range — Database.cpp's `segment_analysis` schema comment). Loaded once
    // here (not deferred to `mira export-segments`) because the whole point is that the
    // segment is fully described the moment it's declared.
    mira::EssentiaEngine engine; // essentia::init() for loadAudio + the analyzers below
    mira::Taxonomy instrumentTaxonomy(MIRA_INSTRUMENT_TAXONOMY, "mtg_jamendo_instrument");
    mira::Taxonomy genreTaxonomy(MIRA_GENRE_TAXONOMY, "genre_discogs400");
    mira::Taxonomy stemInstrumentTaxonomy(MIRA_INSTRUMENT_TAXONOMY, "irmas_predominant_instrument");

    int analyzedCount = 0, skipCount = 0;
    for (const auto& file : targetFiles) {
        auto audio = mira::loadAudio(file.path);
        if (!audio) {
            std::cerr << "  skip (could not decode): " << file.path << std::endl;
            ++skipCount;
            continue;
        }
        if (*startSeconds >= audio->durationSeconds) {
            std::cerr << "  skip (segment starts past end of file, " << audio->durationSeconds
                       << "s): " << file.path << std::endl;
            ++skipCount;
            continue;
        }

        int64_t startSample = static_cast<int64_t>(*startSeconds * audio->sampleRate);
        int64_t endSample = std::min<int64_t>(static_cast<int64_t>(*endSeconds * audio->sampleRate),
                                               static_cast<int64_t>(audio->left.size()));
        startSample = std::max<int64_t>(0, startSample);
        if (endSample <= startSample) {
            std::cerr << "  skip (empty slice after clamping to file length): " << file.path << std::endl;
            ++skipCount;
            continue;
        }

        std::vector<float> mono(audio->mono.begin() + startSample, audio->mono.begin() + endSample);
        std::vector<float> left(audio->left.begin() + startSample, audio->left.begin() + endSample);
        std::vector<float> right(audio->right.begin() + startSample, audio->right.begin() + endSample);

        std::string machineJson = buildSegmentMachineJson(mono, left, right, audio->sampleRate,
                                                            instrumentTaxonomy, genreTaxonomy,
                                                            stemInstrumentTaxonomy, file.contentType == "stem");
        db.upsertSegmentAnalysis(segId, file.id, machineJson, static_cast<int64_t>(std::time(nullptr)));
        ++analyzedCount;
    }
    std::cout << "analyzed segment for " << analyzedCount << " file(s)";
    if (skipCount > 0) std::cout << ", " << skipCount << " skipped";
    std::cout << std::endl;

    return 0;
}

// TASKS.md Phase 3 addition: cuts every file covered by the target's declared segments
// (mira tag-segment) into per-segment WAV files (AudioWriter.h -- mira's first audio-
// writing path), each with its own SA3 sidecar caption (CaptionFields::
// extractCaptionFieldsForSegment + Sa3Renderer, exactly the machinery `mira caption`
// already uses for whole files). A group_id target cuts every sibling stem at the *same*
// declared boundaries, so a synced stem set stays synced across the cut -- that sync is
// the entire reason group_id-scoped segments exist rather than per-file ones.
// TASKS.md Phase 3 addition: how much of a declared [start,end) segment actually
// overlaps this particular file's active spans (Phase 1's silence-gate, ActiveRegions.cpp
// -- already computed per file, never previously cross-checked against a segment
// boundary). Returns nullopt when the file has no active-span data at all (active-region
// detection only runs for stems/declared stems/files over 5 minutes, PRD §5 -- nothing to
// validate against otherwise, and that's not an error). A low fraction here is expected
// and legitimate for one stem in a synced set (e.g. brass simply not playing during a
// "funny" phrase) -- this is surfaced as information, never used to skip the export.
std::optional<double> activeFractionInRange(mira::Database& db, const mira::FileRecord& file,
                                             double rangeStart, double rangeEnd) {
    if (file.activeSpans == "[]" && !file.activeRatio) return std::nullopt;
    auto spans = db.parseActiveSpans(file.activeSpans);
    if (spans.empty()) return std::nullopt;
    double overlap = 0.0;
    for (const auto& [spanStart, spanEnd] : spans) {
        double lo = std::max(spanStart, rangeStart);
        double hi = std::min(spanEnd, rangeEnd);
        if (hi > lo) overlap += (hi - lo);
    }
    double rangeLength = rangeEnd - rangeStart;
    return rangeLength > 0.0 ? (overlap / rangeLength) : 0.0;
}

int runExportSegments(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    std::string outDir;
    std::string trigger;
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
        else if (args[i] == "--out-dir" && i + 1 < args.size()) outDir = args[++i];
        else if (args[i] == "--trigger" && i + 1 < args.size()) trigger = args[++i];
        else positional.push_back(args[i]);
    }

    if (positional.empty()) {
        std::cerr << "mira export-segments: a group_id, file path, or numeric id is required" << std::endl;
        return 1;
    }
    if (outDir.empty()) {
        std::cerr << "mira export-segments: --out-dir is required" << std::endl;
        return 1;
    }

    mira::Database db(dbPath);
    const std::string& target = positional[0];

    std::vector<mira::FileRecord> files;
    std::vector<mira::SegmentRecord> segments;

    if (target.find('|') != std::string::npos) {
        files = db.findFilesByGroupId(target);
        segments = db.findSegmentsForGroup(target);
        if (files.empty()) {
            std::cerr << "mira export-segments: no files found for group_id '" << target << "'" << std::endl;
            return 1;
        }
    } else {
        std::optional<mira::FileRecord> record;
        bool isNumeric = !target.empty() &&
                          std::all_of(target.begin(), target.end(), [](unsigned char c) { return std::isdigit(c); });
        if (isNumeric) record = db.findById(std::stoll(target));
        if (!record) record = db.findByPath(target);
        if (!record) {
            std::cerr << "mira export-segments: no file found for '" << target << "'" << std::endl;
            return 1;
        }
        files.push_back(*record);
        segments = db.findSegmentsForFile(record->id);
    }

    if (segments.empty()) {
        std::cerr << "mira export-segments: no segments declared for '" << target
                   << "' -- run `mira tag-segment` first" << std::endl;
        return 1;
    }

    mira::EssentiaEngine engine; // essentia::init() for the lifetime of this command -- loadAudio needs it

    std::filesystem::create_directories(outDir);

    int cutCount = 0, skipCount = 0;
    for (size_t s = 0; s < segments.size(); ++s) {
        const auto& seg = segments[s];
        std::ostringstream segDirName;
        segDirName << "seg" << (s + 1) << "_" << static_cast<int>(seg.startSeconds) << "-"
                   << static_cast<int>(seg.endSeconds) << "s";
        std::string segDir = outDir + "/" + segDirName.str();
        std::filesystem::create_directories(segDir);

        for (const auto& file : files) {
            if (!file.analyzedAt) {
                std::cerr << "  skip (not analyzed): " << file.path << std::endl;
                ++skipCount;
                continue;
            }
            auto audio = mira::loadAudio(file.path);
            if (!audio) {
                std::cerr << "  skip (could not decode): " << file.path << std::endl;
                ++skipCount;
                continue;
            }
            if (seg.startSeconds >= audio->durationSeconds) {
                std::cerr << "  skip (segment starts past end of file, " << audio->durationSeconds
                           << "s): " << file.path << std::endl;
                ++skipCount;
                continue;
            }

            int64_t startSample = static_cast<int64_t>(seg.startSeconds * audio->sampleRate);
            int64_t endSample = std::min<int64_t>(
                static_cast<int64_t>(seg.endSeconds * audio->sampleRate),
                static_cast<int64_t>(audio->left.size()));
            startSample = std::max<int64_t>(0, startSample);
            if (endSample <= startSample) {
                std::cerr << "  skip (empty slice after clamping to file length): " << file.path
                           << std::endl;
                ++skipCount;
                continue;
            }

            std::vector<float> left(audio->left.begin() + startSample, audio->left.begin() + endSample);
            std::vector<float> right(audio->right.begin() + startSample, audio->right.begin() + endSample);

            std::string baseName = std::filesystem::path(file.path).stem().string();
            std::string wavPath = segDir + "/" + baseName + ".wav";
            if (!mira::writeWavFile(wavPath, left, right, audio->sampleRate, audio->numChannels > 1)) {
                std::cerr << "  skip (could not write " << wavPath << ")" << std::endl;
                ++skipCount;
                continue;
            }

            mira::CaptionFields fields = mira::extractCaptionFieldsForSegment(db, file, seg);
            std::ofstream sidecar(segDir + "/" + baseName + ".json");
            sidecar << mira::renderSa3SidecarJson(fields, trigger);

            std::cout << "  cut: " << wavPath;
            constexpr double kLowActiveFractionWarning = 0.1; // first-pass threshold, undocumented elsewhere
            if (auto activeFraction = activeFractionInRange(db, file, seg.startSeconds, seg.endSeconds)) {
                if (*activeFraction < kLowActiveFractionWarning) {
                    std::cout << "  (note: only " << std::fixed << std::setprecision(0)
                               << (*activeFraction * 100.0)
                               << "% of this range is active for this file -- mostly silent here,"
                                  " which may be expected for one stem in a set)"
                               << std::defaultfloat;
                }
            }
            std::cout << std::endl;
            ++cutCount;
        }
    }

    std::cout << cutCount << " clips written, " << skipCount << " skipped, to " << outDir << std::endl;
    return cutCount > 0 ? 0 : 1;
}

// PRD §8: `mira similar <file|id> [--by overall|timbre|rhythm|spectrum]`. `--filter` and
// external not-yet-scanned files are still separately-tracked TASKS.md items, not
// silently dropped — everything else in the PRD's spec'd CLI surface is now wired.
// Exact brute-force KNN throughout (PRD §3: "no ANN index").
//
// --embedding=effnet|dclap (default effnet), only meaningful with --by overall (the
// default), selects which of the two stored embedding spaces to query (TASKS.md Phase 4
// "Embedding A/B") — this is the actual A/B mechanism: run the same query file against
// both and compare which neighbors come back more sensible for a given content type.
//
// --by timbre|spectrum query the small per-dimension vec0 tables built in main.cpp's
// analyze loop (TASKS.md Phase 4 "per-dimension similarity", PRD §12 item 5 — dimensions
// derived from what mira already measures, not Sononym's fixed five). --by rhythm has no
// stored vector (Database.h's findSimilarByBpm comment) — it reads the query file's own
// BPM and brute-force-scans `files` directly.
int runSimilar(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    int topN = 10;
    std::string embeddingSpace = "effnet";
    std::string by = "overall";
    std::string textQuery; // --text "<words>" -- a description instead of a file
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
        else if (args[i] == "--n" && i + 1 < args.size()) topN = std::stoi(args[++i]);
        else if (args[i] == "--embedding" && i + 1 < args.size()) embeddingSpace = args[++i];
        else if (args[i] == "--by" && i + 1 < args.size()) by = args[++i];
        else if (args[i] == "--text" && i + 1 < args.size()) textQuery = args[++i];
        else positional.push_back(args[i]);
    }

    if (embeddingSpace != "effnet" && embeddingSpace != "dclap") {
        std::cerr << "mira similar: --embedding must be 'effnet' or 'dclap', got '"
                  << embeddingSpace << "'" << std::endl;
        return 1;
    }
    if (by != "overall" && by != "timbre" && by != "rhythm" && by != "spectrum") {
        std::cerr << "mira similar: --by must be 'overall', 'timbre', 'rhythm', or 'spectrum', got '"
                  << by << "'" << std::endl;
        return 1;
    }

    // --text searches by description instead of by example. It only means anything in the
    // DCLAP space: the text tower was trained jointly with the DCLAP audio tower and lands
    // in that same 512-dim space, while discogs-effnet's space has no text side at all.
    // So --text implies --embedding dclap rather than silently comparing across two
    // unrelated spaces, which would return confident nonsense.
    if (!textQuery.empty()) {
        if (by != "overall") {
            std::cerr << "mira similar: --text only works with --by overall (the default); "
                         "timbre/rhythm/spectrum are measured from audio and have no text side"
                      << std::endl;
            return 1;
        }
        mira::Database textDb(dbPath);
        auto text = mira::computeTextEmbedding(textQuery, MIRA_DCLAP_TEXT_MODEL,
                                                MIRA_DCLAP_TEXT_VOCAB, MIRA_DCLAP_TEXT_MERGES);
        if (!text.ok) {
            std::cerr << "mira similar: " << text.error << std::endl;
            return 1;
        }
        auto textMatches = textDb.findSimilarDclap(text.vector, topN, /*excludeId=*/-1);
        if (textMatches.empty()) {
            std::cerr << "mira similar: nothing in the DCLAP index yet — text search only "
                         "finds files analyzed with --dclap (it is off by default; see "
                         "`mira analyze --help`)"
                      << std::endl;
            return 1;
        }
        std::cout << "similar to \"" << textQuery << "\" (--text, dclap):" << std::endl;
        for (const auto& m : textMatches) {
            auto matchRecord = textDb.findById(m.id);
            std::cout << "  " << std::fixed << std::setprecision(4) << m.distance << "  "
                      << (matchRecord ? matchRecord->path : "(id " + std::to_string(m.id) + ", not found)")
                      << std::endl;
        }
        return 0;
    }

    if (positional.empty()) {
        std::cerr << "mira similar: a file path or numeric id is required "
                     "(or --text \"<description>\")" << std::endl;
        return 1;
    }
    const std::string& target = positional[0];

    mira::Database db(dbPath);

    std::optional<mira::FileRecord> record;
    bool isNumeric = !target.empty() &&
                      std::all_of(target.begin(), target.end(), [](unsigned char c) { return std::isdigit(c); });
    if (isNumeric) record = db.findById(std::stoll(target));
    if (!record) record = db.findByPath(target);

    if (!record) {
        std::cerr << "mira similar: no file found for '" << target << "' (only files already in "
                     "the library are supported for now — an external-file mode is a separate "
                     "TASKS.md item)"
                  << std::endl;
        return 1;
    }

    std::vector<mira::Database::SimilarMatch> matches;

    if (by == "timbre") {
        auto vec = db.getTimbreById(record->id);
        if (!vec) {
            std::cerr << "mira similar: '" << record->path
                      << "' has no stored timbre vector — run `mira analyze` on it first "
                         "(or it was too short for even one DSP frame)"
                      << std::endl;
            return 1;
        }
        matches = db.findSimilarTimbre(*vec, topN, record->id);
    } else if (by == "spectrum") {
        auto vec = db.getSpectrumById(record->id);
        if (!vec) {
            std::cerr << "mira similar: '" << record->path
                      << "' has no stored spectrum vector — run `mira analyze` on it first "
                         "(or it was too short for even one DSP frame)"
                      << std::endl;
            return 1;
        }
        matches = db.findSimilarSpectrum(*vec, topN, record->id);
    } else if (by == "rhythm") {
        auto bpm = db.jsonExtractDouble(record->machine, "$.rhythm.beat_this_bpm");
        if (!bpm || *bpm <= 0.0) {
            std::cerr << "mira similar: '" << record->path
                      << "' has no measured BPM — either a one-shot (tempo is never "
                         "measured, PRD §5) or `mira analyze` hasn't run on it yet"
                      << std::endl;
            return 1;
        }
        matches = db.findSimilarByBpm(*bpm, topN, record->id);
    } else {
        bool useDclap = embeddingSpace == "dclap";
        auto embedding = useDclap ? db.getDclapEmbeddingById(record->id) : db.getEmbeddingById(record->id);
        if (!embedding) {
            std::cerr << "mira similar: '" << record->path << "' has no stored " << embeddingSpace
                      << " embedding — run `mira analyze` on it first "
                         "(or it was too short to embed, PRD §16.3)"
                      << std::endl;
            return 1;
        }
        matches = useDclap ? db.findSimilarDclap(*embedding, topN, record->id)
                            : db.findSimilar(*embedding, topN, record->id);
    }

    if (matches.empty()) {
        std::cout << "no similar files found (library may only contain this one measurement)\n";
        return 0;
    }

    std::cout << "similar to " << record->path << " (--by " << by << "):\n";
    for (const auto& m : matches) {
        auto matchRecord = db.findById(m.id);
        std::cout << "  " << std::fixed << std::setprecision(4) << m.distance << "  "
                  << (matchRecord ? matchRecord->path : "(id " + std::to_string(m.id) + ", not found)")
                  << "\n";
    }

    return 0;
}

// PRD §8: `mira stats` — library composition, coverage, unmapped labels. Coverage is
// read directly from what's actually stored (json_extract presence checks), not
// estimated — same discipline as everything else here. "Unmapped labels" is a taxonomy
// completeness check (Taxonomy.h), not a per-file DB scan: for each label a model can
// actually produce, does the taxonomy have an entry for it? A label with no entry still
// gets analyzed and stored (raw), it just doesn't get a normalized form — this surfaces
// that gap directly rather than requiring someone to notice it by accident.
int runStats(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
    }

    mira::Database db(dbPath);

    int64_t total = db.countFiles();
    int64_t analyzed = db.countAnalyzed();
    std::cout << "library: " << total << " files (" << analyzed << " analyzed, "
              << (total - analyzed) << " scanned but not yet analyzed)\n";

    std::cout << "\ncontent type breakdown:\n";
    for (const auto& [type, count] : db.countByContentType()) {
        std::cout << "  " << type << ": " << count << "\n";
    }

    std::cout << "\nclassification coverage (of " << analyzed << " analyzed files):\n";
    struct CoverageField {
        const char* label;
        const char* jsonPath;
    };
    static const CoverageField kCoverageFields[] = {
        {"embedding", "$.embedding"},         {"content gate", "$.content_gate"},
        {"moodtheme", "$.moodtheme"},         {"instrument", "$.instrument"},
        {"genre", "$.genre"},                 {"voice/instrumental", "$.voice_instrumental"},
        {"danceability (model)", "$.danceability_head"}, {"stem instrument", "$.stem_instrument"},
    };
    for (const auto& field : kCoverageFields) {
        int64_t count = db.countWhereMachineHas(field.jsonPath);
        std::cout << "  " << field.label << ": " << count;
        if (analyzed > 0) {
            std::cout << " (" << std::fixed << std::setprecision(0)
                       << (100.0 * count / analyzed) << "%)";
        }
        std::cout << "\n";
    }
    int64_t embeddingCount = db.countEmbeddings();
    std::cout << "  embeddings stored (sqlite-vec, discogs-effnet): " << embeddingCount << "\n";
    int64_t dclapEmbeddingCount = db.countDclapEmbeddings();
    std::cout << "  embeddings stored (sqlite-vec, dclap): " << dclapEmbeddingCount << "\n";

    std::cout << "\nunmapped labels (models can produce these, taxonomy/*.yaml has no entry yet):\n";
    mira::Taxonomy instrumentTax(MIRA_INSTRUMENT_TAXONOMY, "mtg_jamendo_instrument");
    mira::Taxonomy stemInstrumentTax(MIRA_INSTRUMENT_TAXONOMY, "irmas_predominant_instrument");
    mira::Taxonomy genreTax(MIRA_GENRE_TAXONOMY, "genre_discogs400");

    auto reportUnmapped = [](const char* headName, const mira::Taxonomy& tax, bool taxOk,
                              const std::vector<std::string>& rawLabels) {
        if (!taxOk) {
            std::cout << "  " << headName << ": taxonomy file failed to load, skipped\n";
            return;
        }
        std::vector<std::string> unmapped;
        for (const auto& raw : rawLabels) {
            if (!tax.normalize(raw)) unmapped.push_back(raw);
        }
        if (unmapped.empty()) {
            std::cout << "  " << headName << ": none (" << rawLabels.size() << "/"
                       << rawLabels.size() << " labels mapped)\n";
        } else {
            std::cout << "  " << headName << ": " << unmapped.size() << " of " << rawLabels.size()
                       << " unmapped —";
            for (size_t i = 0; i < unmapped.size() && i < 10; ++i) std::cout << " " << unmapped[i];
            if (unmapped.size() > 10) std::cout << " ...";
            std::cout << "\n";
        }
    };
    reportUnmapped("instrument (mtg_jamendo_instrument)", instrumentTax, instrumentTax.ok(),
                    std::vector<std::string>(mira::kInstrumentClassNames,
                                              mira::kInstrumentClassNames + mira::kInstrumentClassCount));
    reportUnmapped("stem instrument (IRMAS codes)", stemInstrumentTax, stemInstrumentTax.ok(),
                    {"cel", "cla", "flu", "gac", "gel", "org", "pia", "sax", "tru", "vio", "voi"});
    reportUnmapped("genre (genre_discogs400)", genreTax, genreTax.ok(),
                    std::vector<std::string>(mira::kGenreClassNames,
                                              mira::kGenreClassNames + mira::kGenreClassCount));
    std::cout << "  moodtheme, content gate (AudioSet): no taxonomy file by design — raw labels\n"
                 "    are already clean human-readable words/phrases (TASKS.md Phase 2)\n";

    return 0;
}

// PRD §8: `mira models --download | --list`. `--list` reports whether each model this
// build was compiled to look for is actually present on disk at its configured path —
// straight filesystem checks against the same compile-time paths every analyzer uses
// (MIRA_*_MODEL), so this can never drift from what analyze actually loads. `--download`
// deliberately doesn't fetch anything itself — mira has no Python/network dependency at
// runtime by design (PRD §2d); it points at the actual fetch mechanism (scripts/
// fetch-vendor.sh for 7 of the 9, lab/'s tf2onnx conversion and
// export_irmas_instrument_onnx.py for the other 2) for whatever's actually missing.
int runModels(const std::vector<std::string>& args) {
    bool doList = false, doDownload = false;
    for (const auto& arg : args) {
        if (arg == "--list") doList = true;
        else if (arg == "--download") doDownload = true;
    }
    if (!doList && !doDownload) {
        std::cerr << "mira models: --list or --download is required" << std::endl;
        return 1;
    }

    struct ModelEntry {
        const char* name;
        const char* path;
        const char* howToFetch;
    };
    const ModelEntry models[] = {
        {"discogs-effnet (embedding)", MIRA_EFFNET_MODEL, "scripts/fetch-vendor.sh"},
        {"DCLAP audio encoder (embedding, TASKS.md Phase 4)", MIRA_DCLAP_AUDIO_MODEL,
         "gh release download v1 --repo NeptuneHub/AudioMuse-AI-DCLAP -D models/similarity-embeddings/dclap"},
        {"DCLAP text tower (`similar --text`, TASKS.md Phase 6)", MIRA_DCLAP_TEXT_MODEL,
         "gh release download v1 --repo NeptuneHub/AudioMuse-AI-DCLAP -D models/similarity-embeddings/dclap"},
        {"RoBERTa vocab for the text tower", MIRA_DCLAP_TEXT_VOCAB,
         "lab/export_roberta_tokenizer.py"},
        {"RoBERTa merges for the text tower", MIRA_DCLAP_TEXT_MERGES,
         "lab/export_roberta_tokenizer.py"},
        {"CED-small (content gate)", MIRA_CED_MODEL, "scripts/fetch-vendor.sh"},
        {"mtg_jamendo_moodtheme", MIRA_MOODTHEME_MODEL, "scripts/fetch-vendor.sh"},
        {"mtg_jamendo_instrument", MIRA_INSTRUMENT_MODEL, "scripts/fetch-vendor.sh"},
        {"danceability", MIRA_DANCEABILITY_MODEL, "scripts/fetch-vendor.sh"},
        {"genre_discogs400", MIRA_GENRE_MODEL, "lab/'s tf2onnx conversion (see TASKS.md Phase 2)"},
        {"voice_instrumental", MIRA_VOICE_INSTRUMENTAL_MODEL, "lab/'s tf2onnx conversion (see TASKS.md Phase 2)"},
        {"IRMAS predominant-instrument", MIRA_IRMAS_INSTRUMENT_MODEL, "lab/export_irmas_instrument_onnx.py"},
        {"beat_this_cpp (rhythm)", MIRA_BEAT_THIS_MODEL, "scripts/fetch-vendor.sh"},
        {"Basic Pitch (transcription)", MIRA_BASIC_PITCH_MODEL, "scripts/fetch-vendor.sh"},
    };

    std::vector<const ModelEntry*> missing;
    if (doList) std::cout << "models (compiled into this build at these paths):\n";
    for (const auto& m : models) {
        bool exists = std::filesystem::exists(m.path);
        if (!exists) missing.push_back(&m);
        if (doList) {
            std::cout << "  [" << (exists ? "x" : " ") << "] " << m.name;
            if (exists) {
                auto bytes = std::filesystem::file_size(m.path);
                std::cout << " (" << (bytes / 1024 / 1024) << " MB)";
            } else {
                std::cout << " — MISSING: " << m.path;
            }
            std::cout << "\n";
        }
    }

    if (doDownload) {
        if (missing.empty()) {
            std::cout << "all models present — nothing to fetch\n";
        } else {
            std::cout << "missing models and how to get them (mira doesn't fetch at runtime, PRD §2d):\n";
            for (const auto* m : missing) {
                std::cout << "  " << m->name << ": run " << m->howToFetch << "\n";
            }
        }
    }

    return missing.empty() ? 0 : 1;
}

// PRD §8: `mira search --filter "..."`. A small, fixed grammar, not a general query
// language: comma-separated conditions (ANDed), each either `field OP value` (a known
// numeric/string field, see kSearchFields below) or `tag:value` (a substring match
// against whichever head's *_normalized or raw object actually has a matching key —
// checked across all of them since the caller shouldn't need to know which model
// happened to produce a given tag; substring, not exact match, since genre's canonical
// keys are "Genre: Style" and a search for the style alone should still hit).
namespace {
struct SearchField {
    const char* name;
    const char* jsonPath; // empty = the `content_type` column itself, not JSON
    bool isNumeric;
};
constexpr SearchField kSearchFields[] = {
    {"content_type", "", false},
    {"key", "$.key.key", false},
    {"bpm", "$.rhythm.beat_this_bpm", true},
    {"tempo", "$.rhythm.beat_this_bpm", true},
    {"duration", "$.duration_seconds", true},
    {"loudness", "$.dsp.integrated_loudness_lufs", true},
    {"danceable", "$.danceability_head.danceable_probability", true},
    {"harmonicity", "$.dsp.harmonicity", true},
    {"centroid", "$.dsp.spectral_centroid_hz", true},
    {"crest", "$.dsp.crest_factor", true},
    {"music_score", "$.content_gate.music_score", true},
    {"voice_probability", "$.voice_instrumental.voice_probability", true},
};
constexpr double kSearchTagThreshold = 0.2;
// Every place a tag (genre/instrument/mood name) could show up — normalized where one
// exists, raw where it doesn't (moodtheme has no taxonomy, TASKS.md Phase 2).
constexpr const char* kSearchTagPaths[] = {
    "$.genre_normalized", "$.instrument_normalized", "$.stem_instrument_normalized", "$.moodtheme",
};

std::string sqlQuoteString(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "''";
        else out += c;
    }
    out += "'";
    return out;
}

// Returns the WHERE-clause fragment for one condition, or nullopt if it doesn't parse —
// callers report that as a user error (an unknown field or bad syntax), not silently
// dropped.
std::optional<std::string> translateSearchCondition(const std::string& condition) {
    // tag:value — no recognized comparison operator, and a literal colon present.
    size_t colon = condition.find(':');
    bool hasComparisonOp = condition.find_first_of("<>=!") != std::string::npos;
    if (colon != std::string::npos && !hasComparisonOp) {
        std::string tag = condition.substr(colon + 1);
        std::ostringstream oss;
        oss << "(";
        constexpr size_t kNumTagPaths = sizeof(kSearchTagPaths) / sizeof(kSearchTagPaths[0]);
        for (size_t i = 0; i < kNumTagPaths; ++i) {
            if (i > 0) oss << " OR ";
            // json_each, not a LIKE against the whole object's raw JSON text: a naive
            // "does this substring appear anywhere in the object" check is nearly always
            // true for a 400-entry genre object where every label is stored regardless of
            // score (most near zero) — real bug, found by testing "genre:Techno" against
            // a flamenco file and getting a match, because *some* genre label somewhere
            // in the 400 contained "Techno" at a near-zero score. json_each lets the key
            // (substring match — genre's canonical keys are "Genre: Style", so a search
            // for the style alone should still hit) and the score (real threshold) be
            // checked together, per entry, properly.
            oss << "EXISTS (SELECT 1 FROM json_each(machine, '" << kSearchTagPaths[i]
                << "') WHERE key LIKE '%" << tag << "%' AND value > " << kSearchTagThreshold << ")";
        }
        oss << ")";
        return oss.str();
    }

    // field OP value — check two-character operators before their one-character prefixes.
    static const std::pair<const char*, const char*> kOps[] = {
        {">=", ">="}, {"<=", "<="}, {"!=", "!="}, {">", ">"}, {"<", "<"}, {"=", "="},
    };
    for (const auto& [opText, opSql] : kOps) {
        size_t pos = condition.find(opText);
        if (pos == std::string::npos) continue;
        std::string fieldName = condition.substr(0, pos);
        std::string value = condition.substr(pos + std::string(opText).length());
        for (const auto& f : kSearchFields) {
            if (fieldName != f.name) continue;
            if (std::string(f.jsonPath).empty()) { // content_type column
                return std::string("content_type ") + opSql + " " + sqlQuoteString(value);
            }
            if (f.isNumeric) {
                return std::string("json_extract(machine, '") + f.jsonPath + "') " + opSql + " " + value;
            }
            return std::string("json_extract(machine, '") + f.jsonPath + "') " + opSql + " " +
                   sqlQuoteString(value);
        }
        return std::nullopt; // matched an operator but not a known field name
    }
    return std::nullopt;
}
} // namespace

int runSearch(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    std::optional<std::string> filter;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
        else if (args[i] == "--filter" && i + 1 < args.size()) filter = args[++i];
    }
    if (!filter) {
        std::cerr << "mira search: --filter \"...\" is required, e.g. --filter \"bpm>120,genre:Flamenco\"\n"
                     "  known fields: content_type, key, bpm/tempo, duration, loudness, danceable,\n"
                     "  harmonicity, centroid, crest, music_score, voice_probability; operators\n"
                     "  >, >=, <, <=, =, !=; tag filters like instrument:guitar or genre:Flamenco;\n"
                     "  comma-separate multiple conditions to AND them"
                  << std::endl;
        return 1;
    }

    std::vector<std::string> conditions;
    std::string current;
    for (char c : *filter) {
        if (c == ',') { conditions.push_back(current); current.clear(); }
        else current += c;
    }
    if (!current.empty()) conditions.push_back(current);

    std::vector<std::string> sqlConditions;
    for (const auto& cond : conditions) {
        auto translated = translateSearchCondition(cond);
        if (!translated) {
            std::cerr << "mira search: could not parse condition '" << cond << "'" << std::endl;
            return 1;
        }
        sqlConditions.push_back(*translated);
    }

    std::string whereClause;
    for (size_t i = 0; i < sqlConditions.size(); ++i) {
        if (i > 0) whereClause += " AND ";
        whereClause += sqlConditions[i];
    }

    mira::Database db(dbPath);
    auto matches = db.queryFiles(whereClause);

    std::cout << matches.size() << " match" << (matches.size() == 1 ? "" : "es") << ":\n";
    for (const auto& m : matches) {
        std::cout << "  " << m.path << " (" << m.contentType << ")\n";
    }

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
    // sqlite-vec (PRD §6, §7) — statically linked, so per its own README it must be
    // registered as an auto-extension before any SQLite connection opens (spike/04_sqlite_vec
    // proved this ordering matters). Every command below constructs a Database, so this
    // has to happen once, here, before any of them run.
    sqlite3_auto_extension(reinterpret_cast<void (*)()>(sqlite3_vec_init));

    std::vector<std::string> args(argv + 1, argv + argc);

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        printUsage();
        return args.empty() ? 1 : 0;
    }

    std::string command = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (command == "scan") {
        return runScan(rest);
    }
    if (command == "analyze") {
        return runAnalyze(rest);
    }
    if (command == "inspect") {
        return runInspect(rest);
    }
    if (command == "similar") {
        return runSimilar(rest);
    }
    if (command == "stats") {
        return runStats(rest);
    }
    if (command == "models") {
        return runModels(rest);
    }
    if (command == "search") {
        return runSearch(rest);
    }
    if (command == "caption") {
        return runCaption(rest);
    }
    if (command == "tag") {
        return runTag(rest);
    }
    if (command == "tag-folder") {
        return runTagFolder(rest);
    }
    if (command == "tag-segment") {
        return runTagSegment(rest);
    }
    if (command == "export-segments") {
        return runExportSegments(rest);
    }

    std::cerr << "mira: unknown command '" << command << "'\n\n";
    printUsage();
    return 1;
}
