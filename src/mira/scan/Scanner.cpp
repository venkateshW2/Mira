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

    for (const auto& root : options.roots) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
            std::cerr << "mira scan: skipping non-directory root: " << root << std::endl;
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

            const auto& entry = *it;
            if (!entry.is_regular_file(ec)) continue;

            const std::string path = entry.path().string();
            if (!hasSupportedAudioExtension(path)) {
                stats.filesSkippedUnsupported++;
                continue;
            }

            stats.filesSeen++;

            auto mtime = fs::last_write_time(entry.path(), ec).time_since_epoch().count();
            auto sizeBytes = static_cast<int64_t>(entry.file_size(ec));

            // sha256 is the expensive step; skip it when mtime alone shows nothing
            // changed, so a re-scan of an untouched drive is cheap.
            if (auto existing = db.findByPath(path);
                existing && existing->mtime == static_cast<int64_t>(mtime)) {
                stats.filesUnchanged++;
                continue;
            }

            std::string hash = sha256File(path);
            if (hash.empty()) {
                std::cerr << "mira scan: could not read " << path << std::endl;
                continue;
            }

            bool isNew = db.upsertScannedFile(path, hash, static_cast<int64_t>(mtime),
                                               sizeBytes, nowUnix());
            if (isNew) stats.filesNew++;
            else stats.filesUpdated++;
        }
    }

    return stats;
}

} // namespace mira
