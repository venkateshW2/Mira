#pragma once

#include <string>
#include <vector>

namespace mira {

// mira's first audio-*writing* path -- everything before this only ever decoded (PRD §7,
// AudioLoader.h). Needed for TASKS.md Phase 3's segment export: cutting a long file or a
// synced stem group into per-segment training clips (see Database.cpp's `segments` table
// comment for why cutting is unavoidable -- SA3 has a hard per-clip duration ceiling
// regardless of captioning).
//
// Writes 32-bit float PCM WAV: lossless, matches the precision AudioLoader already
// decoded into (`LoadedAudio::left`/`right`), and needs no encoder/codec at all -- a WAV
// header is ~44 bytes of arithmetic, not a library. `left`/`right` must be the same
// length; pass `right` equal to `left` for a mono source, matching AudioLoader's own
// mono-duplication convention. Returns false if the file couldn't be opened for writing.
bool writeWavFile(const std::string& path, const std::vector<float>& left,
                   const std::vector<float>& right, int sampleRate, bool stereo);

} // namespace mira
