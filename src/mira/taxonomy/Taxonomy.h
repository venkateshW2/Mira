#pragma once

#include <optional>
#include <string>
#include <unordered_map>

namespace mira {

// Label normalization (PRD §5). Loads a model's raw-label -> canonical-term mapping from
// one of the versioned YAML files in taxonomy/ (not code) — see those files' own header
// comments for the design rule: only merge labels that are the exact same concept across
// models, never collapse genuinely distinct things for tidiness (e.g. electric guitar and
// acoustic guitar stay separate canonical terms).
class Taxonomy {
public:
    // `yamlPath` is one of taxonomy/*.yaml. `modelKey` is that file's top-level key for
    // the specific model being normalized — a single file can hold mappings for more
    // than one model (e.g. instrument-labels.yaml has both mtg_jamendo_instrument and
    // irmas_predominant_instrument, since they're the same taxonomy).
    Taxonomy(const std::string& yamlPath, const std::string& modelKey);

    // Returns the canonical term for a raw label, or nullopt if this label has no entry
    // yet — an "unmapped" label (PRD §8: mira stats surfaces these). Callers should keep
    // the raw label available regardless of whether normalization succeeds; this never
    // silently drops a signal, only offers an additional normalized view of it.
    std::optional<std::string> normalize(const std::string& rawLabel) const;

    // False if the YAML file or the requested modelKey couldn't be loaded/found —
    // callers should degrade to raw-labels-only, not treat this as a hard failure.
    bool ok() const { return ok_; }

private:
    std::unordered_map<std::string, std::string> mapping_;
    bool ok_ = false;
};

} // namespace mira
