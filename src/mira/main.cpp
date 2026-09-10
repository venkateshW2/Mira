#include "analyze/EssentiaEngine.h"
#include "analyze/Router.h"
#include "db/Database.h"
#include "scan/Scanner.h"

#include <chrono>
#include <cmath>
#include <filesystem>
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
        "\n"
        "Not yet implemented: similar, search, inspect, models, stats (see TASKS.md)\n";
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
// files routed in *this* analyze run only — a folder analyzed across multiple runs won't
// be grouped correctly yet (see TASKS.md).
struct RoutedFile {
    mira::FileRecord record;
    mira::RoutingResult routing;
    std::string parentDir;
};

std::string siblingKey(const std::string& parentDir, double durationSeconds) {
    // Round to the nearest 50ms — "identical length" allowing for header/encoder jitter.
    double rounded = std::round(durationSeconds * 20.0) / 20.0;
    std::ostringstream oss;
    oss << parentDir << "|" << rounded;
    return oss.str();
}

std::string durationOnsetJson(const mira::RoutingResult& r) {
    std::ostringstream oss;
    oss << "{\"duration_seconds\":" << r.durationSeconds
        << ",\"onset_rate\":" << r.onsetRate
        << ",\"onset_count\":" << r.onsetCount << "}";
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

    auto candidates = db.findFilesForRouting(force);
    if (candidates.empty()) {
        std::cout << "nothing to route (use --force to re-route already-routed files)"
                   << std::endl;
        return 0;
    }

    mira::EssentiaEngine engine; // essentia::init() for the lifetime of this command

    std::vector<RoutedFile> routed;
    int failed = 0;
    for (auto& record : candidates) {
        auto result = mira::routeContentType(record.path);
        if (!result.ok) {
            std::cerr << "mira analyze: could not decode " << record.path << std::endl;
            failed++;
            continue;
        }
        routed.push_back({record, result, std::filesystem::path(record.path).parent_path().string()});
    }

    // Sibling-set stem detection over the batch just routed.
    std::map<std::string, std::vector<size_t>> siblingGroups;
    for (size_t i = 0; i < routed.size(); ++i) {
        siblingGroups[siblingKey(routed[i].parentDir, routed[i].routing.durationSeconds)]
            .push_back(i);
    }

    int64_t analyzedAt = nowUnix();
    std::map<std::string, int> counts;

    for (auto& [key, indices] : siblingGroups) {
        bool isSiblingStemSet = indices.size() >= 2;
        std::optional<std::string> groupId;
        if (isSiblingStemSet) groupId = key;

        for (size_t idx : indices) {
            auto& rf = routed[idx];
            std::string contentType = isSiblingStemSet ? "stem" : rf.routing.contentType;
            counts[contentType]++;

            mira::Database::RoutingUpdate update;
            update.id = rf.record.id;
            update.contentType = contentType;
            update.groupId = groupId;
            update.machineJson = durationOnsetJson(rf.routing);
            update.analyzedAt = analyzedAt;
            db.applyRouting(update);
        }
    }

    std::cout << "routed " << routed.size() << " files: ";
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

    std::cerr << "mira: unknown command '" << command << "'\n\n";
    printUsage();
    return 1;
}
