#include "analyze/ActiveRegions.h"
#include "analyze/AudioLoader.h"
#include "analyze/Descriptors.h"
#include "analyze/Chords.h"
#include "analyze/ContentGate.h"
#include "analyze/Embedding.h"
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
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
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
        "               [--chords] [--transcribe] [--recheck-tempo] [--verbose]\n"
        "        content-type router (one_shot/loop/track/stem) + DSP + embedding +\n"
        "        content gate + moodtheme/instrument/danceability/genre/voice-instrumental\n"
        "        heads (Phase 2, PRD §2c) + rhythm + key by default; classification heads\n"
        "        gated on the content gate's is_music signal; stems additionally get a\n"
        "        second, isolated-audio-tuned instrument opinion (stem_instrument —\n"
        "        mtg_jamendo_instrument's embedding is full-mix-trained and unreliable on\n"
        "        isolated stems, TASKS.md); rhythm defaults to beat_this_cpp only (the\n"
        "        more accurate of the two tempo estimators); --recheck-tempo also runs\n"
        "        Essentia's RhythmExtractor2013 for comparison (bpm_ratio); --chords and\n"
        "        --transcribe are opt-in (15.0s/3.7s on a 5:08 song, vs 0.4s for key alone\n"
        "        — see TASKS.md); --content-type requires --force; --verbose prints\n"
        "        per-stage timing to stderr, per file\n"
        "  mira inspect <file|id> [--db <path>]\n"
        "        human-readable report; flags low-confidence tempo/key, active_ratio\n"
        "  mira similar <file|id> [--db <path>] [--n N]\n"
        "        exact brute-force KNN over the discogs-effnet embedding (sqlite-vec, no\n"
        "        ANN index — PRD §3); only files already in the library are supported for\n"
        "        now, and only the overall embedding (--by/--filter not yet implemented)\n"
        "\n"
        "Not yet implemented: search, models, stats (see TASKS.md)\n";
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
class StageTimer {
public:
    explicit StageTimer(bool enabled) : enabled_(enabled) {}
    void mark(const std::string& stageName) {
        if (!enabled_) return;
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - last_).count();
        std::cerr << "  " << stageName << ": " << static_cast<int>(ms) << "ms" << std::endl;
        last_ = now;
    }

private:
    bool enabled_;
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
};

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
    bool runChords = false;
    bool runTranscription = false;
    bool runRecheckTempo = false;
    std::optional<std::string> contentTypeFilter;
    std::optional<int> limit;

    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& arg = args[i];
        if (arg == "--db" && i + 1 < args.size()) {
            dbPath = args[++i];
        } else if (arg == "--force") {
            force = true;
        } else if (arg == "--verbose") {
            verbose = true;
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

    auto candidateRecords = db.findFilesForAnalysis(force, contentTypeFilter, limit);
    if (candidateRecords.empty()) {
        std::cout << "nothing to analyze (use --force to re-analyze)" << std::endl;
        return 0;
    }

    mira::EssentiaEngine engine; // essentia::init() for the lifetime of this command

    std::vector<Candidate> candidates;
    int failed = 0;
    for (auto& record : candidateRecords) {
        StageTimer decodeTimer(verbose);
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
            c.contentType = "stem";
        } else {
            c.routing = mira::routeContentType(audio->mono, audio->sampleRate);
            // Route 1 (filename/folder pattern) overrides the duration-based classification
            // but stays content_type_source='router', not 'declared' — only an explicit
            // `--as stem` is a real declaration (PRD §12.3).
            c.contentType = looksLikeStemPath(record.path) ? "stem" : c.routing.contentType;
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
        bool isSiblingSet = indices.size() >= 2;
        std::optional<std::string> groupId;
        if (isSiblingSet) groupId = key;

        for (size_t idx : indices) {
            auto& c = candidates[idx];
            std::string finalContentType = (isSiblingSet || c.isDeclared) ? "stem" : c.contentType;
            counts[finalContentType]++;

            if (verbose) std::cerr << c.record.path << ":" << std::endl;
            StageTimer timer(verbose);

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

            if (mira::shouldRunActiveRegionDetection(finalContentType, duration)) {
                auto activeRegions = mira::detectActiveRegions(c.audio.mono, c.audio.sampleRate);
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
                    if (transcription.ok)
                        machine << ",\"notes\":" << mira::toJson(transcription);
                    timer.mark("note transcription (Basic Pitch)");
                }
            }

            machine << "}";
            update.machineJson = machine.str();
            update.analyzedAt = analyzedAt;
            db.applyAnalysis(update);
            if (embedding.ok) {
                std::vector<float> embeddingF(embedding.vector.begin(), embedding.vector.end());
                db.upsertEmbedding(c.record.id, embeddingF);
            }
            timer.mark("database write");
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

// PRD §8: `mira similar <file|id>`. Deliberately scoped narrow for this first cut: only
// the overall discogs-effnet embedding (no --by timbre|rhythm|spectrum subsets yet, no
// --filter, no external not-yet-scanned files) — those are real, separately-tracked
// TASKS.md items, not silently dropped. Exact brute-force KNN (PRD §3: "no ANN index").
int runSimilar(const std::vector<std::string>& args) {
    std::string dbPath = defaultDbPath();
    int topN = 10;
    std::vector<std::string> positional;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--db" && i + 1 < args.size()) dbPath = args[++i];
        else if (args[i] == "--n" && i + 1 < args.size()) topN = std::stoi(args[++i]);
        else positional.push_back(args[i]);
    }

    if (positional.empty()) {
        std::cerr << "mira similar: a file path or numeric id is required" << std::endl;
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

    auto embedding = db.getEmbeddingById(record->id);
    if (!embedding) {
        std::cerr << "mira similar: '" << record->path
                  << "' has no stored embedding — run `mira analyze` on it first "
                     "(or it was too short for even one mel patch, PRD §16.3)"
                  << std::endl;
        return 1;
    }

    auto matches = db.findSimilar(*embedding, topN, record->id);
    if (matches.empty()) {
        std::cout << "no similar files found (library may only contain this one embedding)\n";
        return 0;
    }

    std::cout << "similar to " << record->path << ":\n";
    for (const auto& m : matches) {
        auto matchRecord = db.findById(m.id);
        std::cout << "  " << std::fixed << std::setprecision(4) << m.distance << "  "
                  << (matchRecord ? matchRecord->path : "(id " + std::to_string(m.id) + ", not found)")
                  << "\n";
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

    std::cerr << "mira: unknown command '" << command << "'\n\n";
    printUsage();
    return 1;
}
