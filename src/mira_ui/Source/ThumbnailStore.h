#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_core/juce_core.h>

// TASKS.md Phase 5 leftovers: "pre-generated waveform previews during Scan --
// AudioThumbnail generation is lazy (on first click), so a 40-minute file takes a few
// seconds to show its waveform the first time. Persisting thumbnail peaks during Scan
// would make this instant, but needs a real on-disk thumbnail cache design (today's
// AudioThumbnailCache is in-memory only)."
//
// This is that design, and it is deliberately small: JUCE's AudioThumbnail already
// serialises its own peak data (saveTo/loadFrom), so nothing here re-implements peak
// storage — it only answers "where does this file's peaks live, and is that copy still
// valid". One file per source file under ~/.mira/thumbnails/, no index, no database
// table: the cache is pure derived data, and a cache entry that can be regenerated from
// the audio in a couple of seconds is not worth a schema migration or a row that has to
// stay consistent with `files`.
//
// Validity is (path, modification time, size) hashed into the filename rather than
// checked from inside the file. A source file edited in place gets a different hash and
// therefore simply misses the cache — there is no stale-entry case to detect, and no
// reader has to trust a cached header that claims to describe audio it can't see.
// Entries for deleted or re-edited files are never cleaned up: the hash is one-way, so
// an entry can't be traced back to the file it came from, and they're small enough that
// a real eviction policy (size cap, LRU) is a later problem, not this pass's.
namespace mira_ui::thumbnails
{

// ~/.mira/thumbnails — a sibling of library.db, not inside the app bundle or a system
// cache directory, so a library and its derived previews travel together.
juce::File cacheDirectory();

juce::File cacheFileFor(const juce::File& audioFile);

// Fills `thumbnail` from a valid on-disk entry. False means "no usable cache" (missing,
// unreadable, or written by a different JUCE version) — the caller falls back to normal
// lazy generation, which is exactly today's behaviour, so a cache miss is never an error.
bool loadInto(juce::AudioThumbnail& thumbnail, const juce::File& audioFile);

// Generates peaks for `audioFile` and writes them to its cache entry, blocking until
// they're complete. Meant for a background thread (ThumbnailPrecacheJob) — this is the
// several-seconds-per-long-file work the whole cache exists to move off the click path.
// False means the file couldn't be read or peaks never completed; already-cached files
// return true immediately without re-reading the audio.
bool generateAndSave(juce::AudioFormatManager& formatManager, const juce::File& audioFile,
                      const std::function<bool()>& shouldAbort);

} // namespace mira_ui::thumbnails
