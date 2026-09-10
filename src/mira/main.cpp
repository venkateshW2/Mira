#include "analyze/ActiveRegions.h"
#include "analyze/AudioLoader.h"
#include "analyze/Descriptors.h"
#include "analyze/Chords.h"
#include "analyze/EssentiaEngine.h"
#include "analyze/Key.h"
#include "analyze/Mir.h"
#include "analyze/Router.h"
#include "analyze/Transcription.h"
#include "db/Database.h"
#include "scan/Scanner.h"

#include <version.h> // essentia's, not libc++'s — ESSENTIA_VERSION/ESSENTIA_GIT_SHA

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

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
        "  mira analyze [--db <path>] [--force]\n"
        "        content-type router (one_shot/loop/track/stem) over scanned files\n"
        "  mira inspect <file|id> [--db <path>]\n"
        "        human-readable report; flags low-confidence tempo/key, active_ratio\n"
        "\n"
        "Not yet implemented: similar, search, models, stats (see TASKS.md)\n";
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
               << stats.filesSkippedUnsupported << " non-audio files skipped" << std::endl;
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

int runAnalyze(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    bool force = false;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--db" && i + 1 < args.size()) {
            dbPath = args[++i];
        } else if (arg == "--force") {
            force = true;
        } else {
            std::cerr << "mira analyze: unknown argument " << arg << std::endl;
            return 1;
        }
    }

    std::cout << "database: " << dbPath << std::endl;
    mira::Database db(dbPath);

    auto candidateRecords = db.findFilesForAnalysis(force);
    if (candidateRecords.empty()) {
        std::cout << "nothing to analyze (use --force to re-analyze)" << std::endl;
        return 0;
    }

    mira::EssentiaEngine engine; // essentia::init() for the lifetime of this command

    std::vector<Candidate> candidates;
    int failed = 0;
    for (auto& record : candidateRecords) {
        auto audio = mira::loadAudio(record.path);
        if (!audio) {
            std::cerr << "mira analyze: could not decode " << record.path << std::endl;
            failed++;
            continue;
        }

        Candidate c;
        c.record = record;
        c.isDeclared = (record.contentTypeSource == "declared");
        c.parentDir = std::filesystem::path(record.path).parent_path().string();

        if (c.isDeclared) {
            c.contentType = "stem";
        } else {
            c.routing = mira::routeContentType(audio->mono, audio->sampleRate);
            c.contentType = c.routing.contentType;
        }
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

    int64_t analyzedAt = nowUnix();
    std::map<std::string, int> counts;

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

    for (auto& [key, indices] : siblingGroups) {
        bool isSiblingSet = indices.size() >= 2;
        std::optional<std::string> groupId;
        if (isSiblingSet) groupId = key;

        for (size_t idx : indices) {
            auto& c = candidates[idx];
            std::string finalContentType = (isSiblingSet || c.isDeclared) ? "stem" : c.contentType;
            counts[finalContentType]++;

            double duration = c.audio.durationSeconds;

            std::ostringstream machine;
            machine << "{\"duration_seconds\":" << duration
                    << ",\"sample_rate\":" << c.audio.sampleRate
                    << ",\"num_channels\":" << c.audio.numChannels;
            if (!c.isDeclared) {
                machine << ",\"onset_rate\":" << c.routing.onsetRate
                        << ",\"onset_count\":" << c.routing.onsetCount;
            }

            mira::Database::AnalysisUpdate update;
            update.id = c.record.id;
            update.provenanceJson = provenanceJson;
            if (!c.isDeclared) update.contentType = finalContentType; // never touch a declared row
            if (groupId) update.groupId = groupId;

            // PRD §5: always for stems, regardless of duration; otherwise only past 5 min.
            if (mira::shouldRunActiveRegionDetection(finalContentType, duration)) {
                auto activeRegions = mira::detectActiveRegions(c.audio.mono, c.audio.sampleRate);
                update.activeRatio = activeRegions.activeRatio;
                update.activeSpansJson = spansToJson(activeRegions.spans);
                machine << ",\"active_ratio\":" << activeRegions.activeRatio;
            }

            // DSP descriptors (PRD §5A) — all content types. Not yet restricted to
            // active spans on stems/long tracks (TASKS.md notes this as still open).
            auto dsp = mira::computeDspDescriptors(c.audio.left, c.audio.right, c.audio.sampleRate);
            machine << ",\"dsp\":" << mira::toJson(dsp);

            // MIR (PRD §5B) — loops/tracks/stems only. Tempo on a 300ms one-shot is
            // "wasted work [producing] confident nonsense" (PRD §5).
            if (finalContentType != "one_shot") {
                auto rhythm = mira::analyzeRhythm(c.audio.mono, c.audio.sampleRate,
                                                   MIRA_BEAT_THIS_MODEL);
                if (rhythm.ok) machine << ",\"rhythm\":" << mira::toJson(rhythm);

                // Key and chords: both gated on harmonic content (PRD §12b) — never run
                // blindly on a rhythm stem or noise. See Key.h for the spectral-flatness
                // proxy caveat (applies to chords too, same gate reused).
                if (mira::shouldRunKeyDetection(dsp.spectralFlatness)) {
                    auto key = mira::detectKey(c.audio.mono, c.audio.sampleRate);
                    machine << ",\"key\":" << mira::toJson(key);

                    auto chords = mira::detectChords(c.audio.mono, c.audio.sampleRate);
                    if (chords.ok) machine << ",\"chords\":" << mira::toJson(chords);
                }

                // Note transcription (PRD §5, §12b) — "a first-class feature, not a MIR
                // afterthought." Shares the rhythm/key/chords one_shot gate above (a
                // 300ms clip has nothing to transcribe) but, unlike key/chords, is NOT
                // additionally gated on harmonic content — it's useful on percussive
                // material too, and a transcription that finds few or no notes there is
                // itself informative, not "confident nonsense".
                auto transcription = mira::transcribe(c.audio.mono, c.audio.sampleRate,
                                                        MIRA_BASIC_PITCH_MODEL);
                if (transcription.ok)
                    machine << ",\"notes\":" << mira::toJson(transcription);
            }

            machine << "}";
            update.machineJson = machine.str();
            update.analyzedAt = analyzedAt;
            db.applyAnalysis(update);
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

    auto essentiaBpm = db.jsonExtractDouble(r.machine, "$.rhythm.essentia_bpm");
    if (essentiaBpm) {
        std::cout << "\nRhythm:\n  essentia:      " << *essentiaBpm << " BPM";
        if (auto conf = db.jsonExtractDouble(r.machine, "$.rhythm.essentia_confidence"))
            std::cout << "  (confidence " << *conf << ")";
        std::cout << "\n";
        if (auto beatThisBpm = db.jsonExtractDouble(r.machine, "$.rhythm.beat_this_bpm"))
            std::cout << "  beat_this:     " << *beatThisBpm << " BPM\n";
        if (auto ratio = db.jsonExtractDouble(r.machine, "$.rhythm.bpm_ratio")) {
            if (std::abs(*ratio - 1.0) > 0.05) {
                std::cout << std::setprecision(3)
                           << "  ⚠ estimators disagree (ratio " << *ratio
                           << ") — treat both with caution (PRD §14.1)\n"
                           << std::setprecision(2);
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
    } else if (essentiaBpm) {
        std::cout << "\nKey:    not analyzed (gated on harmonic content — likely noisy/non-tonal)\n";
    }

    if (auto chordCount = db.jsonArrayLength(r.machine, "$.chords"))
        std::cout << "Chords: " << *chordCount << " segments\n";
    if (auto noteCount = db.jsonArrayLength(r.machine, "$.notes"))
        std::cout << "Notes:  " << *noteCount << " transcribed\n";

    if (r.human != "{}") std::cout << "\nhuman overrides: " << r.human << "\n";

    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
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

    std::cerr << "mira: unknown command '" << command << "'\n\n";
    printUsage();
    return 1;
}
