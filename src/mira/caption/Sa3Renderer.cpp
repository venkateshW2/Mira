#include "Sa3Renderer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace mira {

namespace {

constexpr int kSa3MaxWords = 45; // reprompt.py's own `_has_artifacts` ceiling

std::string joinLabels(const std::vector<ScoredLabel>& labels, size_t maxCount) {
    std::string out;
    size_t n = std::min(maxCount, labels.size());
    for (size_t i = 0; i < n; ++i) {
        if (i > 0) out += ", ";
        out += labels[i].label;
    }
    return out;
}

std::string joinStrings(const std::vector<std::string>& values) {
    std::string out;
    for (size_t i = 0; i < values.size(); ++i) {
        if (i > 0) out += ", ";
        out += values[i];
    }
    return out;
}

// One descriptive-adjective clause out of both moodtheme's gated labels and a person's
// own free-form keywords ("funny", "quirky", "tense drama scene") -- the two sources
// read the same way to a reader (and to the frozen text encoder), so they're joined into
// one list rather than two separate clauses. Keywords are never confidence-gated (there
// is no score to gate on -- a human wrote them), and always included regardless of the
// mood truncation lever below: dropping a person's own hand-picked label to save two
// words is exactly the kind of decision that should stay with the person, not the
// renderer.
std::string moodClause(const CaptionFields& f, bool includeMood) {
    std::vector<std::string> words = f.keywords;
    if (includeMood) {
        for (auto& m : f.moods) words.push_back(m.label);
    }
    if (words.empty()) return "";
    return " with a " + joinStrings(words) + " mood";
}

int wordCount(const std::string& s) {
    int n = 0;
    bool inWord = false;
    for (unsigned char c : s) {
        bool isSpace = std::isspace(c) != 0;
        if (!isSpace && !inWord) {
            ++n;
            inWord = true;
        } else if (isSpace) {
            inWord = false;
        }
    }
    return n;
}

std::string formatRounded(double value) { return std::to_string(std::lround(value)); }

// Exact prefix strings from SA3's own interface/reprompt.py TRACK_TYPE_PREFIXES. Stems
// map onto reprompt.py's "instrument" category (its own classifier defines "instrument"
// as "a musical instrument... or with words solo or stem" -- literally names stems).
// One-shots get no prefix at all: "one_shot" isn't one of reprompt.py's three
// TRACK_TYPE_PREFIXES keys, so `TRACK_TYPE_PREFIXES.get(category, "")` returns "".
std::string trackTypePrefix(const CaptionFields& f) {
    if (f.contentType == "one_shot") return "";
    if (f.contentType == "stem") return "TrackType: Instrument, ";
    std::string prefix = "TrackType: Music, ";
    if (f.isInstrumental.has_value()) {
        prefix += "VocalType: ";
        prefix += (*f.isInstrumental ? "Instrumental" : "Vocal");
        prefix += ", ";
    }
    return prefix;
}

// The descriptive clause. Not free prose -- a grammatical join of CaptionFields' own
// gated labels (see Sa3Renderer.h's header comment on why mira has no text-generation
// step). `instrumentBudget`/`includeMood` are the two truncation levers used by
// renderSa3Prose()'s word-budget trim, applied in that order (mood first, since it's the
// least load-bearing field for a training caption; then extra instruments beyond the
// single top one) -- BPM/Length are never dropped, they're the cheapest, most
// information-dense tokens in the whole string.
std::string proseBody(const CaptionFields& f, size_t instrumentBudget, bool includeMood) {
    std::ostringstream out;
    bool wroteGenre = !f.genre.empty();
    if (wroteGenre) out << joinLabels(f.genre, f.genre.size());

    if (!f.instruments.empty() && instrumentBudget > 0) {
        std::string instr = joinLabels(f.instruments, instrumentBudget);
        if (wroteGenre) out << " ";
        if (f.contentType == "one_shot") {
            out << instr << " one-shot";
        } else if (f.contentType == "stem") {
            out << instr << " stem";
        } else {
            out << "featuring " << instr;
        }
    } else if (f.contentType == "one_shot") {
        out << (wroteGenre ? " " : "") << "one-shot";
    } else if (f.contentType == "stem") {
        out << (wroteGenre ? " " : "") << "stem";
    } else if (!wroteGenre) {
        out << "instrumental track";
    }

    out << moodClause(f, includeMood);

    return out.str();
}

} // namespace

std::string renderSa3Prose(const CaptionFields& fields, const std::string& trigger) {
    std::string prefix = trackTypePrefix(fields);

    std::ostringstream suffixStream;
    if (fields.bpm) suffixStream << "BPM: " << formatRounded(*fields.bpm) << ". ";
    suffixStream << "Length: " << formatRounded(fields.durationSeconds) << " seconds";
    std::string suffix = suffixStream.str();

    auto assemble = [&](size_t instrumentBudget, bool includeMood) {
        std::string body = proseBody(fields, instrumentBudget, includeMood);
        std::string result;
        if (!trigger.empty()) result += trigger + ", ";
        result += prefix;
        result += body;
        if (!result.empty()) result += ". ";
        result += suffix;
        return result;
    };

    size_t instrumentBudget = fields.instruments.size();
    bool includeMood = true;

    std::string out = assemble(instrumentBudget, includeMood);
    if (wordCount(out) > kSa3MaxWords && includeMood) {
        includeMood = false;
        out = assemble(instrumentBudget, includeMood);
    }
    if (wordCount(out) > kSa3MaxWords && instrumentBudget > 1) {
        instrumentBudget = 1;
        out = assemble(instrumentBudget, includeMood);
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> renderSa3Tags(const CaptionFields& fields,
                                                                 const std::string& trigger) {
    std::vector<std::pair<std::string, std::string>> tags;
    if (!trigger.empty()) tags.emplace_back("trigger", trigger);

    if (fields.contentType == "stem") {
        tags.emplace_back("TrackType", "Instrument");
    } else if (fields.contentType != "one_shot") {
        tags.emplace_back("TrackType", "Music");
        if (fields.isInstrumental.has_value())
            tags.emplace_back("VocalType", *fields.isInstrumental ? "Instrumental" : "Vocal");
    }
    // one_shot: no TrackType tag -- reprompt.py's own classifier keeps "one_shot" as a
    // category distinct from "instrument"/"music"/"sfx", with no TrackType prefix at all.

    if (!fields.genre.empty()) tags.emplace_back("genre", joinLabels(fields.genre, fields.genre.size()));
    if (!fields.instruments.empty())
        tags.emplace_back("instruments", joinLabels(fields.instruments, fields.instruments.size()));
    if (!fields.moods.empty()) tags.emplace_back("moods", joinLabels(fields.moods, fields.moods.size()));
    if (!fields.keywords.empty()) tags.emplace_back("keywords", joinStrings(fields.keywords));
    // Shape fields (CaptionFields.h). Emitted as their own tag keys rather than folded
    // into `moods` so underfit's tag-pill UI shows them as independent dials the training
    // prompts can drop or keep per sample -- the whole point is that they vary
    // independently of mood, which is what makes them learnable.
    if (fields.rhythm) tags.emplace_back("rhythm", *fields.rhythm);
    if (fields.dynamics) tags.emplace_back("dynamics", *fields.dynamics);
    if (fields.texture) tags.emplace_back("texture", *fields.texture);
    if (fields.bpm) tags.emplace_back("bpm", formatRounded(*fields.bpm));
    if (fields.keyScale) tags.emplace_back("keyscale", *fields.keyScale);
    // Not underfit's `seconds_total` (that's computed independently, straight off the
    // audio file, by pre_encode.py -- a separate numeric conditioning channel, PRD §15).
    // This is a text tag so a "prompt"-only dataset config still has a duration mention.
    tags.emplace_back("length_seconds", formatRounded(fields.durationSeconds));

    tags.emplace_back("prompt", renderSa3Prose(fields, trigger));
    return tags;
}

namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

} // namespace

std::string renderSa3SidecarJson(const CaptionFields& fields, const std::string& trigger) {
    auto tags = renderSa3Tags(fields, trigger);
    std::ostringstream out;
    out << "{";
    for (size_t i = 0; i < tags.size(); ++i) {
        if (i > 0) out << ",";
        out << "\"" << jsonEscape(tags[i].first) << "\":\"" << jsonEscape(tags[i].second) << "\"";
    }
    out << "}";
    return out.str();
}

} // namespace mira
