#include "RobertaTokenizer.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>

namespace mira {
namespace {

// One codepoint as UTF-8. The byte-level table only ever needs the BMP, but this is the
// general form so there is nothing to get wrong later.
std::string utf8FromCodepoint(unsigned cp) {
    std::string s;
    if (cp < 0x80) {
        s += static_cast<char>(cp);
    } else if (cp < 0x800) {
        s += static_cast<char>(0xC0 | (cp >> 6));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (cp >> 12));
        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (cp & 0x3F));
    }
    return s;
}

// GPT-2's pre-tokenizer regex, which RoBERTa inherits:
//   's|'t|'re|'ve|'m|'ll|'d| ?\p{L}+| ?\p{N}+| ?[^\s\p{L}\p{N}]+|\s+(?!\S)|\s+
// Hand-written rather than std::regex: std::regex has no Unicode property support at all,
// so \p{L} cannot be expressed, and mira's queries are plain text where "is this byte part
// of a letter" can be decided from UTF-8 structure alone -- any byte >= 0x80 is part of a
// multi-byte sequence, and those are letters for every language a caption is written in.
// The pieces this splits into are then byte-encoded, so a wrong guess about *which* kind
// of letter it is cannot change the output; only the split points matter.
bool isAsciiLetter(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isAsciiDigit(unsigned char c) { return c >= '0' && c <= '9'; }
bool isSpace(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
// Non-ASCII bytes are treated as letters (see above).
bool isLetterByte(unsigned char c) { return isAsciiLetter(c) || c >= 0x80; }

std::vector<std::string> preTokenize(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0, n = text.size();
    auto at = [&](size_t k) -> unsigned char {
        return k < n ? static_cast<unsigned char>(text[k]) : 0;
    };
    while (i < n) {
        // Contractions: 's 't 're 've 'm 'll 'd -- matched before anything else, and only
        // in lowercase, exactly as the original pattern has them.
        if (at(i) == '\'') {
            static const char* kContractions[] = { "'s", "'t", "'re", "'ve", "'m", "'ll", "'d" };
            bool matched = false;
            for (const char* c : kContractions) {
                size_t len = std::char_traits<char>::length(c);
                if (text.compare(i, len, c) == 0) {
                    out.emplace_back(c);
                    i += len;
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
        }

        size_t start = i;
        // An optional single leading space belongs to the *following* piece -- this is the
        // whole reason BPE vocabularies are full of tokens beginning with the marker for
        // a space, and getting it wrong shifts every id in the sentence.
        bool leadingSpace = (at(i) == ' ');
        size_t j = leadingSpace ? i + 1 : i;

        if (isLetterByte(at(j))) {
            while (j < n && isLetterByte(at(j))) ++j;
            out.push_back(text.substr(start, j - start));
            i = j;
            continue;
        }
        if (isAsciiDigit(at(j))) {
            while (j < n && isAsciiDigit(at(j))) ++j;
            out.push_back(text.substr(start, j - start));
            i = j;
            continue;
        }
        if (j < n && !isSpace(at(j))) {
            while (j < n && !isSpace(at(j)) && !isLetterByte(at(j)) && !isAsciiDigit(at(j))) ++j;
            out.push_back(text.substr(start, j - start));
            i = j;
            continue;
        }

        // Whitespace runs. `\s+(?!\S)` keeps all but the last space of a run that is
        // followed by a non-space; that last space then starts the next piece.
        size_t runEnd = i;
        while (runEnd < n && isSpace(at(runEnd))) ++runEnd;
        if (runEnd < n && runEnd - i > 1) {
            out.push_back(text.substr(i, runEnd - i - 1));
            i = runEnd - 1;
        } else {
            out.push_back(text.substr(i, runEnd - i));
            i = runEnd;
        }
    }
    return out;
}

} // namespace

RobertaTokenizer::RobertaTokenizer(const std::string& vocabTsvPath, const std::string& mergesPath) {
    // The byte-to-unicode table, built exactly as GPT-2's bytes_to_unicode() does: the
    // printable ASCII/Latin-1 ranges map to themselves, and every remaining byte is given
    // a codepoint from 0x100 upwards in byte order.
    bool used[256] = { false };
    auto mark = [&](int lo, int hi) {
        for (int b = lo; b <= hi; ++b) {
            byteToUnicode_[b] = utf8FromCodepoint(static_cast<unsigned>(b));
            used[b] = true;
        }
    };
    mark('!', '~');
    mark(0xA1, 0xAC);
    mark(0xAE, 0xFF);
    unsigned next = 0;
    for (int b = 0; b < 256; ++b)
        if (!used[b]) byteToUnicode_[b] = utf8FromCodepoint(256 + next++);

    std::ifstream vocabFile(vocabTsvPath);
    if (!vocabFile) {
        error_ = "can't open " + vocabTsvPath;
        return;
    }
    std::string line;
    while (std::getline(vocabFile, line)) {
        if (line.empty()) continue;
        auto tab = line.rfind('\t');
        if (tab == std::string::npos) continue;
        vocab_.emplace(line.substr(0, tab), std::stoll(line.substr(tab + 1)));
    }

    std::ifstream mergesFile(mergesPath);
    if (!mergesFile) {
        error_ = "can't open " + mergesPath;
        return;
    }
    int rank = 0;
    while (std::getline(mergesFile, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        mergeRank_.emplace(line, rank++);
    }

    auto id = [&](const char* tok, int64_t fallback) {
        auto it = vocab_.find(tok);
        return it != vocab_.end() ? it->second : fallback;
    };
    bosId_ = id("<s>", 0);
    eosId_ = id("</s>", 2);
    unkId_ = id("<unk>", 3);

    ok_ = !vocab_.empty() && !mergeRank_.empty();
    if (!ok_ && error_.empty()) error_ = "vocab or merges file was empty";
}

// Standard BPE: repeatedly merge the adjacent pair with the lowest merge rank until none
// of the remaining pairs is in the table.
std::vector<std::string> RobertaTokenizer::bpe(const std::string& encodedWord) const {
    // Split into UTF-8 characters -- the byte-encoded form is all printable codepoints,
    // so a "symbol" here is one codepoint, not one byte.
    std::vector<std::string> symbols;
    for (size_t i = 0; i < encodedWord.size();) {
        unsigned char c = static_cast<unsigned char>(encodedWord[i]);
        size_t len = c < 0x80 ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        len = std::min(len, encodedWord.size() - i);
        symbols.push_back(encodedWord.substr(i, len));
        i += len;
    }
    if (symbols.size() < 2) return symbols;

    while (true) {
        int bestRank = std::numeric_limits<int>::max();
        size_t bestIndex = 0;
        for (size_t i = 0; i + 1 < symbols.size(); ++i) {
            auto it = mergeRank_.find(symbols[i] + " " + symbols[i + 1]);
            if (it != mergeRank_.end() && it->second < bestRank) {
                bestRank = it->second;
                bestIndex = i;
            }
        }
        if (bestRank == std::numeric_limits<int>::max()) break;
        symbols[bestIndex] += symbols[bestIndex + 1];
        symbols.erase(symbols.begin() + static_cast<long>(bestIndex) + 1);
        if (symbols.size() == 1) break;
    }
    return symbols;
}

std::vector<int64_t> RobertaTokenizer::encode(const std::string& text) const {
    std::vector<int64_t> ids;
    ids.push_back(bosId_);
    if (ok_) {
        for (const auto& piece : preTokenize(text)) {
            std::string encoded;
            for (char ch : piece) encoded += byteToUnicode_[static_cast<unsigned char>(ch)];
            for (const auto& token : bpe(encoded)) {
                auto it = vocab_.find(token);
                ids.push_back(it != vocab_.end() ? it->second : unkId_);
            }
        }
    }
    ids.push_back(eosId_);
    return ids;
}

} // namespace mira
