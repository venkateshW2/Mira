#pragma once

#include "../db/Database.h"
#include <cstdint>
#include <string>
#include <vector>

namespace mira {

struct ScanOptions {
    std::vector<std::string> roots;
    bool followSymlinks = false;
    // PRD §12.3 route 3: declare every file under `roots` as a stem, overriding whatever
    // the router (mira analyze) would otherwise decide. Always correct, per the PRD.
    bool declareAsStem = false;
};

struct ScanStats {
    int64_t filesSeen = 0;
    int64_t filesNew = 0;
    int64_t filesUpdated = 0;
    int64_t filesUnchanged = 0;
    int64_t filesSkippedUnsupported = 0;
};

// Indexes audio files under `options.roots` into `db` — path, sha256, mtime, size.
// No decoding, no analysis (PRD §8: "mira scan ... # index files, no analysis"). Content
// type, descriptors, and everything else is the analyzer's job (Phase 1, still to come).
ScanStats scan(Database& db, const ScanOptions& options);

// True if the extension (checked case-insensitively) is one of the formats mira's
// eventual JUCE-based decoder will read (PRD §7): wav, aiff/aif, flac, ogg, mp3, m4a, caf.
bool hasSupportedAudioExtension(const std::string& path);

// Streams the file and returns its SHA-256 as a lowercase hex string.
std::string sha256File(const std::string& path);

} // namespace mira
