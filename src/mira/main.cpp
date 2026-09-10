#include "db/Database.h"
#include "scan/Scanner.h"

#include <filesystem>
#include <iostream>
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

void printUsage() {
    std::cout <<
        "mira — local audio understanding and similarity search\n"
        "\n"
        "Usage:\n"
        "  mira scan <dir>... [--db <path>] [--follow-symlinks]   index files, no analysis\n"
        "\n"
        "Not yet implemented: analyze, similar, search, inspect, models, stats (see TASKS.md)\n";
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
    std::cout << "library now has " << db.countFiles() << " files total" << std::endl;

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

    std::cerr << "mira: unknown command '" << command << "'\n\n";
    printUsage();
    return 1;
}
