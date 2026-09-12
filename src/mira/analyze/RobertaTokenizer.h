#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace mira {

// Byte-level BPE tokenizer for roberta-base, which is what the CLAP text tower
// (ClapText.h) expects -- it takes input_ids/attention_mask, not text, so mira has to
// tokenize identically to HuggingFace or the resulting vector is silently wrong rather
// than obviously broken. Same failure mode spike/05_dclap_parity existed to rule out on
// the audio side, and checked the same way: lab/export_roberta_tokenizer.py writes a
// parity fixture of real queries plus the edge cases a hand-rolled BPE most often gets
// wrong (leading space, casing, punctuation, non-ASCII, emoji, empty string), and the
// smoke test asserts every one matches HuggingFace's own ids exactly.
//
// Hand-rolled rather than vendoring tokenizers-cpp: that would pull a Rust toolchain into
// a build that has none, for one model's text input. The tables are loaded from plain
// text (lab/export_roberta_tokenizer.py flattens HuggingFace's tokenizer.json), so this
// needs no JSON parser either.
class RobertaTokenizer {
public:
    // `vocabTsvPath` is <token>\t<id> per line; `mergesPath` is "<a> <b>" per line in rank
    // order. ok() is false if either file is missing or malformed -- callers degrade to
    // "text search unavailable" rather than tokenizing wrongly.
    RobertaTokenizer(const std::string& vocabTsvPath, const std::string& mergesPath);

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    // Token ids with <s>/</s> added, exactly as HuggingFace's encode() returns them.
    std::vector<int64_t> encode(const std::string& text) const;

private:
    // GPT-2/RoBERTa's byte-to-unicode table: every one of the 256 byte values maps to a
    // printable codepoint so the BPE vocabulary can be plain text with no control
    // characters in it. Stored as the UTF-8 spelling of that codepoint, since that is
    // what the vocabulary file actually contains.
    std::string byteToUnicode_[256];

    std::vector<std::string> bpe(const std::string& encodedWord) const;

    std::unordered_map<std::string, int64_t> vocab_;
    // Merge rank by "<a> <b>" -- lower rank wins, which is the whole of BPE's ordering.
    std::unordered_map<std::string, int> mergeRank_;
    int64_t bosId_ = 0, eosId_ = 2, unkId_ = 3;
    bool ok_ = false;
    std::string error_;
};

} // namespace mira
