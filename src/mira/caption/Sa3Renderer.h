#pragma once

#include <string>
#include <utility>
#include <vector>

#include "CaptionFields.h"

namespace mira {

// Renders a CaptionFields document into Stable Audio 3's own trained-in prompt shape.
//
// Grounded directly in SA3's own source (sa3-studio/stable-audio-3), not just its public
// docs: interface/reprompt.py is Stability's own LLM-assisted "write me a good SA3
// prompt" tool, and every one of its four templates ends in the same pattern -- a short
// descriptive clause, then trailing `BPM: X. Length: Y seconds` (period-joined, not
// comma-joined AudioSparx-style `Genre: x, Instruments: y` tags). `TRACK_TYPE_PREFIXES`
// there gives the exact prefix strings: "TrackType: Music, VocalType: Instrumental, " /
// "TrackType: Instrument, " / "TrackType: SFX, " (one-shots get no prefix at all --
// "one_shot" isn't one of those three keys). This renderer reproduces that shape.
//
// mira has no text-generation step (deliberately -- PRD §12.6 forbids asserting anything
// unmeasured, and mira's descriptors don't cover production-texture language like
// "cavernous reverb" at all yet), so the descriptive clause here is a grammatical join of
// CaptionFields' own gated labels, not free prose.
//
// 45-word soft ceiling matches reprompt.py's own `_has_artifacts` check
// (`len(text.split()) > 45`); 256-token hard ceiling is the T5Gemma conditioner's own
// tokenizer truncation (sa3-medium / sa3-sm-music training_template.json) -- enforced
// here by word-budget trimming since mira has no T5Gemma tokenizer at render time.
std::string renderSa3Prose(const CaptionFields& fields, const std::string& trigger = "");

// Flat key -> string-value pairs for a JSON sidecar underfit's dataset loader will read
// (dataset_processing/pre_encode.py's JSON-sidecar path keeps every string/int/float
// key it finds, and the dashboard's tag-pill UI dynamically discovers whatever keys are
// present). Every value here is a plain string, never a JSON array: underfit's
// prompt_templates.py `_get()` silently keeps only the first element of a list-valued
// tag (`val[0] if isinstance(val, (list, tuple))`) -- a real bug found by reading that
// code, not a style choice -- so multi-value fields are pre-joined with ", " instead.
// Includes a "prompt" key holding renderSa3Prose()'s output verbatim: underfit treats
// "prompt" as a passthrough tag key, so a caller can point a dataset at just that one key
// to use mira's assembled sentence directly instead of per-field tags.
std::vector<std::pair<std::string, std::string>> renderSa3Tags(const CaptionFields& fields,
                                                                 const std::string& trigger = "");

// Serializes renderSa3Tags()'s pairs as one flat JSON object -- PRD §6 / TASKS.md Phase
// 3's `--emit-sidecars`.
std::string renderSa3SidecarJson(const CaptionFields& fields, const std::string& trigger = "");

} // namespace mira
