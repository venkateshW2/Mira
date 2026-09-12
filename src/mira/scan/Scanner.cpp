#include "Scanner.h"

#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace mira {

namespace {
const std::unordered_set<std::string> kSupportedExtensions = {
    ".wav", ".aiff", ".aif", ".flac", ".ogg", ".mp3", ".m4a", ".caf"
};

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace

bool hasSupportedAudioExtension(const std::string& path) {
    return kSupportedExtensions.count(toLower(fs::path(path).extension().string())) > 0;
}

bool isAppleDoubleSidecar(const std::string& path) {
    std::string filename = fs::path(path).filename().string();
    return filename.size() >= 2 && filename[0] == '.' && filename[1] == '_';
}

std::string sha256File(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";

    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);

    std::array<char, 1 << 16> buffer;
    while (file.read(buffer.data(), buffer.size()) || file.gcount() > 0) {
        CC_SHA256_Update(&ctx, buffer.data(), static_cast<CC_LONG>(file.gcount()));
    }

    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(digest, &ctx);

    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (unsigned char b : digest) hex << std::setw(2) << static_cast<int>(b);
    return hex.str();
}

ScanStats scan(Database& db, const ScanOptions& options) {
    ScanStats stats;

    // One file's indexing, factored out of the directory walk so a root can be a single
    // FILE as well as a directory ("i just want to add the three files and not the
    // folders"). mira_ui's Add Files... passes the chosen files straight through here
    // rather than adding their parent folder as a root and pulling in everything beside
    // them. Identical work either way -- there is no second code path that could drift.
    auto indexEntry = [&](const fs::directory_entry& entry) {
        std::error_code ec;
        if (!entry.is_regular_file(ec)) return;

        const std::string path = entry.path().string();
        if (isAppleDoubleSidecar(path)) {
            stats.filesSkippedAppleDouble++;
            return;
        }
        if (!hasSupportedAudioExtension(path)) {
            stats.filesSkippedUnsupported++;
            return;
        }

        stats.filesSeen++;
        // Every file, not throttled by count here — a folder of a few dozen huge
        // stem files (each taking real time to SHA-256) could otherwise sit at
        // "0 files" for ages between updates, reading as hung rather than working.
        // A caller that needs to bound UI update frequency for a very large library
        // (mira_ui's ScanJob) throttles by wall-clock time on its own end instead.
        if (options.onProgress) options.onProgress(stats, path);

        auto mtime = fs::last_write_time(entry.path(), ec).time_since_epoch().count();
        auto sizeBytes = static_cast<int64_t>(entry.file_size(ec));

        // sha256 is the expensive step; skip it when mtime alone shows nothing
        // changed, so a re-scan of an untouched drive is cheap.
        if (auto existing = db.findByPath(path);
            existing && existing->mtime == static_cast<int64_t>(mtime)) {
            stats.filesUnchanged++;
            if (options.declareAsStem && existing->contentTypeSource != "declared"
                && !filenameSuggestsFullMix(path))
                db.declareStem(path);
            return;
        }

        std::string hash = sha256File(path);
        if (hash.empty()) {
            std::cerr << "mira scan: could not read " << path << std::endl;
            return;
        }

        bool isNew = db.upsertScannedFile(path, hash, static_cast<int64_t>(mtime),
                                           sizeBytes, nowUnix());
        if (isNew) stats.filesNew++;
        else stats.filesUpdated++;

        // A mix sitting in a stem folder is not a stem (review round 4). Declaring it
        // one would hand it the isolated-audio instrument model, which is exactly
        // wrong for a full mix -- measured on Bhabi-BGM-StemMix.wav, where the
        // stem-tuned model answered "voice 54%" for a whole arrangement.
        if (options.declareAsStem && !filenameSuggestsFullMix(path)) db.declareStem(path);
    };

    for (const auto& root : options.roots) {
        std::error_code ec;

        if (fs::is_regular_file(root, ec)) {
            indexEntry(fs::directory_entry(root));
            continue;
        }
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
            std::cerr << "mira scan: skipping root that is neither a file nor a directory: "
                      << root << std::endl;
            continue;
        }

        auto iterOptions = options.followSymlinks
                                ? fs::directory_options::follow_directory_symlink
                                : fs::directory_options::none;

        for (auto it = fs::recursive_directory_iterator(root, iterOptions, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) {
                std::cerr << "mira scan: error walking " << root << ": " << ec.message() << std::endl;
                ec.clear();
                continue;
            }
            indexEntry(*it);
        }
    }

    if (options.onProgress) options.onProgress(stats, "");

    return stats;
}

std::optional<std::string> instrumentFromFilename(const std::string& path) {
    auto slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));

    // Ordered: the first match wins, so more specific spellings come before the looser
    // ones they contain. Labels are taxonomy vocabulary, so they merge with model output
    // rather than sitting beside a differently-spelled duplicate.
    static const std::pair<const char*, const char*> kHints[] = {
        {"vox", "voice"},          {"vocal", "voice"},    {"voice", "voice"},
        {"choir", "voice"},        {"kick", "drums"},     {"snare", "drums"},
        {"hihat", "drums"},        {"hi-hat", "drums"},   {"drum", "drums"},
        {"perc", "percussion"},    {"shaker", "percussion"}, {"tabla", "percussion"},
        {"bass", "bass"},          {"gtr", "electric guitar"}, {"guitar", "electric guitar"},
        // After the guitar entries on purpose: "RHYTHM GTR"/"RHYTHM GUITAR" is a common
        // delivery name and is a guitar, not a drum kit, so the named instrument has to
        // win. "rhtm" is the spelling this library's score deliveries actually use
        // (RHTM 1_1.wav, RHTM-2_1.wav); both were reading as "organ" before this, because
        // IRMAS has no drums class and can only answer with a wrong melodic instrument.
        {"rhtm", "drums"},         {"rhythm", "drums"},
        {"string", "strings"},     {"brass", "brass"},    {"horn", "brass"},
        {"trumpet", "trumpet"},    {"trombone", "brass"}, {"sax", "saxophone"},
        {"flute", "flute"},        {"cello", "cello"},    {"violin", "violin"},
        {"harp", "harp"},          {"organ", "organ"},    {"piano", "piano"},
        {"rhodes", "electric piano"}, {"keys", "keyboard"}, {"synth", "synthesizer"},
        {"pad", "synthesizer"},
    };
    for (const auto& [word, label] : kHints)
        if (name.find(word) != std::string::npos) return std::string(label);
    return std::nullopt;
}

bool filenameSuggestsFullMix(const std::string& path) {
    auto slash = path.find_last_of('/');
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    for (auto& c : name)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));

    // "mix" also catches "stemmix"/"mixdown"; "master"/"bounce" are the other two names a
    // delivered full mix usually carries.
    static const char* const kMixWords[] = {"mix", "master", "bounce"};
    bool hasMixWord = false;
    for (const auto* word : kMixWords)
        if (name.find(word) != std::string::npos) { hasMixWord = true; break; }
    if (!hasMixWord) return false;

    // ...but a mix word with a SECTION in front of it is a bus, not the full mix:
    // "FX MASTER.wav" is a real file in this library (EP9), and it is an effects stem with
    // an active ratio of 0.004 -- calling it the mix made cue detection take a
    // near-silent file as its map of where the music is and return nothing at all.
    // Same reasoning as the path/filename split above, one level finer: "MASTER" on its
    // own is the mix, "<something> MASTER" is that something's bus.
    if (name.find("fx") != std::string::npos) return false;
    if (instrumentFromFilename(name)) return false;
    return true;
}

} // namespace mira
