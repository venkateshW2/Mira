#pragma once

#include "../db/Database.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mira {

struct ScanStats;

struct ScanOptions {
    std::vector<std::string> roots;
    bool followSymlinks = false;
    // PRD §12.3 route 3: declare every file under `roots` as a stem, overriding whatever
    // the router (mira analyze) would otherwise decide. Always correct, per the PRD.
    bool declareAsStem = false;

    // Optional, throttled progress callback (every ~25 files, plus once at the end) —
    // called on whatever thread scan() itself runs on, same as scan() overall; mira_core
    // deliberately has no threading/UI dependency of its own (that's the whole reason
    // it's a separate lightweight target from the CLI's heavy ML deps), so a caller that
    // needs UI-thread marshaling (mira_ui's ScanJob) does that itself in its own lambda.
    // No total-file count is available ahead of a filesystem walk, so this is a running
    // count, not a percentage — a real number instead of a fabricated one.
    std::function<void(const ScanStats& statsSoFar, const std::string& currentPath)> onProgress;
};

struct ScanStats {
    int64_t filesSeen = 0;
    int64_t filesNew = 0;
    int64_t filesUpdated = 0;
    int64_t filesUnchanged = 0;
    int64_t filesSkippedUnsupported = 0;
    int64_t filesSkippedAppleDouble = 0;
};

// Indexes audio files under `options.roots` into `db` — path, sha256, mtime, size.
// No decoding, no analysis (PRD §8: "mira scan ... # index files, no analysis"). Content
// type, descriptors, and everything else is the analyzer's job (Phase 1, still to come).
ScanStats scan(Database& db, const ScanOptions& options);

// True if the extension (checked case-insensitively) is one of the formats mira's
// eventual JUCE-based decoder will read (PRD §7): wav, aiff/aif, flac, ogg, mp3, m4a, caf.
bool hasSupportedAudioExtension(const std::string& path);

// True if the filename is a macOS AppleDouble sidecar ("._Foo.wav") — metadata resource
// forks macOS writes onto filesystems that can't store them natively (exFAT, network
// shares, many external drives). They carry the real file's extension, so they'd
// otherwise pass hasSupportedAudioExtension and get indexed as if they were audio; found
// by testing against a real exFAT stem-delivery drive, where they outnumbered the real
// files 1:1 in every folder.
bool isAppleDoubleSidecar(const std::string& path);

// True if the *filename* says this is a full mix rather than one stem ("...StemMix.wav",
// "mixdown", "master"). Review round 4: a Score/Music Stems folder declares every file
// under it a stem (ScanOptions::declareAsStem), which hands a mix the isolated-audio
// instrument model -- measured answering "voice 54%" for a whole arrangement. In a stem
// delivery the filename is strong, cheap evidence, the same reasoning FileTable.cpp's
// percussion fallback already relies on.
//
// Deliberately the filename only, never the path: "WORKING MASTER" is a real folder in
// this library, and matching on it would turn every stem underneath into a "mix".
bool filenameSuggestsFullMix(const std::string& path);

// The instrument a stem's *filename* names, normalized to mira's taxonomy vocabulary
// ("BRASS_1.wav" -> "brass", "VOX-SOLO_1.wav" -> "voice", "GTR_1.wav" -> "electric
// guitar"), or nullopt when the name says nothing. Review round 5: "in case of stems the
// name of the file defines a lot of facts that can be used to check".
//
// Both instrument models are genuinely unreliable on isolated stems -- the stem-tuned one
// (IRMAS) has no drums, bass, synth or brass-section class at all, and the full-mix one
// scored "synthesizer 0.44" on an isolated vocal -- while a delivery stem's filename is
// written by whoever bounced it. Filename only, never the path, so a folder called
// "STRINGS" can't label every stem inside it.
std::optional<std::string> instrumentFromFilename(const std::string& path);

// Streams the file and returns its SHA-256 as a lowercase hex string.
std::string sha256File(const std::string& path);

} // namespace mira
