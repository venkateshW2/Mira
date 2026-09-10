// Uses libyaml's document API (already linked into mira transitively via Essentia's own
// dependency, PkgConfig::YAML in CMakeLists — not a new dependency, just newly used
// directly by mira's own code). The taxonomy/*.yaml files are deliberately simple (a
// flat two-level mapping of model-key -> {raw-label: canonical-term}, no lists, no
// anchors) so a direct document-tree walk is enough; no need for a general-purpose YAML
// C++ wrapper for this.

#include "Taxonomy.h"

#include <yaml.h>

namespace mira {

namespace {
std::string scalarValue(yaml_node_t* node) {
    if (!node || node->type != YAML_SCALAR_NODE) return "";
    return std::string(reinterpret_cast<const char*>(node->data.scalar.value),
                        node->data.scalar.length);
}
} // namespace

Taxonomy::Taxonomy(const std::string& yamlPath, const std::string& modelKey) {
    FILE* file = fopen(yamlPath.c_str(), "rb");
    if (!file) return;

    yaml_parser_t parser;
    yaml_document_t document;
    bool parserInitialized = false;
    bool documentLoaded = false;

    do {
        if (!yaml_parser_initialize(&parser)) break;
        parserInitialized = true;
        yaml_parser_set_input_file(&parser, file);

        if (!yaml_parser_load(&parser, &document)) break;
        documentLoaded = true;

        yaml_node_t* root = yaml_document_get_root_node(&document);
        if (!root || root->type != YAML_MAPPING_NODE) break;

        // Find the top-level entry matching modelKey.
        yaml_node_t* modelNode = nullptr;
        for (yaml_node_pair_t* pair = root->data.mapping.pairs.start;
             pair < root->data.mapping.pairs.top; ++pair) {
            yaml_node_t* keyNode = yaml_document_get_node(&document, pair->key);
            if (scalarValue(keyNode) == modelKey) {
                modelNode = yaml_document_get_node(&document, pair->value);
                break;
            }
        }
        if (!modelNode || modelNode->type != YAML_MAPPING_NODE) break;

        // Walk modelNode's own {raw: canonical} pairs.
        for (yaml_node_pair_t* pair = modelNode->data.mapping.pairs.start;
             pair < modelNode->data.mapping.pairs.top; ++pair) {
            yaml_node_t* keyNode = yaml_document_get_node(&document, pair->key);
            yaml_node_t* valueNode = yaml_document_get_node(&document, pair->value);
            std::string raw = scalarValue(keyNode);
            std::string canonical = scalarValue(valueNode);
            if (!raw.empty() && !canonical.empty()) mapping_[raw] = canonical;
        }

        ok_ = !mapping_.empty();
    } while (false);

    if (documentLoaded) yaml_document_delete(&document);
    if (parserInitialized) yaml_parser_delete(&parser);
    fclose(file);
}

std::optional<std::string> Taxonomy::normalize(const std::string& rawLabel) const {
    auto it = mapping_.find(rawLabel);
    if (it == mapping_.end()) return std::nullopt;
    return it->second;
}

} // namespace mira
