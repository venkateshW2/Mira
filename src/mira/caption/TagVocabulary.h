#pragma once

#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

// The folder-tag vocabularies (CAPTION-TAGGING.md, 2026-09-14).
//
// Fixed lists, never free text: a LoRA learns whatever mapping it is fed, so a vocabulary
// that drifts between folders teaches the model nothing. These four name the things no
// analyzer can measure -- mira has no head that knows a score is "fantasy" rather than
// "sci-fi", and none can be trained to, because the answer is about the film and not the
// audio. Everything that CAN be measured (palette, timing, rhythm, dynamics, texture,
// bpm, key, instruments, genre, moods) is derived in CaptionFields.cpp and is deliberately
// absent here: never ask a person for something the machine already knows.
//
// Lives in its own header because BOTH the CLI (`mira tag-folder`) and mira_ui's folder
// context menu offer these choices. Two copies would drift, and a vocabulary that drifts
// is the one failure this whole design exists to prevent.
//
// All four land in CaptionFields::keywords, which CaptionFields.h already documents as
// existing "purely so a person can hand-label what genre/instrument/mood classifiers
// can't" and which "only ever comes from `human`". No new field, no renderer change.

namespace mira {

// What KIND of thing this is. Sets how everything else should be read.
inline constexpr const char* kMaterialVocab[] = {
    "score", "song", "beat", "sound-design", "live-set",
};

// Where it lives -- the narrative or scene world. One or two per folder; a folder needing
// three is not one style, and splitting it into two folders will caption better than
// blurring it into one.
inline constexpr const char* kWorldVocab[] = {
    // film
    "fantasy", "sci-fi", "noir", "heist", "chase", "horror", "western", "war",
    "post-apocalyptic", "superhero", "survival",
    // music
    "club", "basement", "arena", "lo-fi", "industrial", "psychedelic", "spiritual",
};

// Each value names the chord shape it stands for, so the word is a musical instruction
// rather than a vibe: heroic = major/Mixolydian/Lydian with open fifths; lament = minor
// with a descending bass; menace = Phrygian, tritones, semitone clusters; alien =
// whole-tone/octatonic drones with no clear tonic.
inline constexpr const char* kHarmonicVocab[] = {
    "heroic", "lament", "menace", "alien", "static-drone", "modal-folk",
    "blues-pentatonic", "jazz-extended", "atonal",
};

// Names one person's habit rather than a genre -- the slot that makes an individual
// producer's style trainable next to an orchestral score. Expected to grow as artists are
// added; still a list, so the same habit is always spelled the same way.
inline constexpr const char* kSignatureVocab[] = {
    "glitch-swing", "boom-bap", "broken-beat", "wall-of-noise", "wide-rubato",
};

inline constexpr size_t kMaterialVocabCount = std::size(kMaterialVocab);
inline constexpr size_t kWorldVocabCount = std::size(kWorldVocab);
inline constexpr size_t kHarmonicVocabCount = std::size(kHarmonicVocab);
inline constexpr size_t kSignatureVocabCount = std::size(kSignatureVocab);

// A folder may carry at most this many `world` values -- see kWorldVocab's comment.
inline constexpr size_t kMaxWorldsPerFolder = 2;

inline bool inVocab(const std::string& value, const char* const* vocab, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (value == vocab[i]) return true;
    return false;
}

inline std::string vocabList(const char* const* vocab, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        if (i) out += ", ";
        out += vocab[i];
    }
    return out;
}

} // namespace mira
