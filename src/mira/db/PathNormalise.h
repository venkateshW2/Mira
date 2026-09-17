#pragma once

#include <string>

namespace mira {

// macOS hands out the SAME filename as two different byte sequences depending on which
// API asked for it, and mira had both in play at once:
//
//   scanner (std::filesystem / readdir)   G o U+0308  ->  47 6f cc 88   NFD
//   JUCE (RangedDirectoryIterator)        G U+00F6    ->  47 c3 b6      NFC
//
// `files.path` is a TEXT primary lookup key compared with `=`, so those two never match.
// Every file with a decomposable character in its name -- "Göransson", "Jóhann", any
// accent at all -- was invisible to the file table's database pairing: 46 of 82 files in
// one folder showed as unanalysed while holding 50-80 KB of analysis each. The same
// mismatch made `mira analyze` say "nothing to analyze" when the UI handed it those
// paths, which is why analysing from the right-click menu appeared to do nothing at all.
//
// This is not a display bug with a display fix. Any two components that discover a path
// by different routes will disagree, so the comparison itself has to stop being naive.
//
// Deliberately NOT a migration of the stored paths. Rewriting 2,500 rows to one form
// fixes today and breaks again the moment anything writes the other form -- and both
// forms are legitimate; the filesystem accepts either and returns whichever the calling
// API prefers. Matching on a canonical key is the invariant; normalising the data is a
// snapshot of one.
std::string toNfd(const std::string& utf8);
std::string toNfc(const std::string& utf8);

// True when two paths name the same file, whatever normalisation each arrived in. Cheap
// in the overwhelmingly common case: a plain byte comparison first, and the CoreFoundation
// round-trip only when that fails.
bool pathsEquivalent(const std::string& a, const std::string& b);

} // namespace mira
