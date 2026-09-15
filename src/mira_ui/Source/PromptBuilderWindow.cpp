#include "PromptBuilderWindow.h"

namespace {

// The sidecars beside the latents ARE the training captions -- reading them is how this
// panel knows what words exist, with no second list to drift out of sync. Fields that
// hold a comma-separated list are split; the rest are taken whole.
const char* kListFields[] = { "genre", "instruments", "moods", "keywords" };

bool isListField(const juce::String& f) {
    for (auto* k : kListFields) if (f == k) return true;
    return false;
}

} // namespace

PromptBuilderContent::PromptBuilderContent(const MiraLookAndFeel& l, juce::File studioRoot)
    : laf(l)
{
    scanVocabulary(studioRoot);

    headerLabel.setText("Pick from what your corpora actually contain, or type your own. "
                        "Blank rows are left out.", juce::dontSendNotification);
    headerLabel.setFont(juce::Font(12.0f));
    headerLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    addAndMakeVisible(headerLabel);

    // Order here is the order they are emitted in. underfit shuffles tag order during
    // training, so this is a readability choice, not a correctness one.
    // multi: blending two LoRAs means both their triggers in one prompt, and picking
    // them from what was actually encoded is how the trigger stops being a thing you
    // have to remember correctly.
    addField("",            "trigger",     "trigger",     true);
    addField("TrackType",   "TrackType",   "TrackType",   false);
    addField("VocalType",   "VocalType",   "VocalType",   false);
    addField("Genre",       "Genre",       "genre",       true);
    addField("Instruments", "Instruments", "instruments", true);
    addField("Moods",       "Moods",       "moods",       true);
    addField("Keywords",    "Keywords",    "keywords",    true);
    addField("Texture",     "Texture",     "texture",     false);
    addField("Rhythm",      "Rhythm",      "rhythm",      false);
    addField("Dynamics",    "Dynamics",    "dynamics",    false);
    addField("Timing",      "Timing",      "timing",      false);
    addField("Palette",     "Palette",     "palette",     false);
    addField("Keyscale",    "Keyscale",    "keyscale",    false);
    addField("BPM",         "BPM",         "",            false);
    addField("",            "also add",    "",            false);

    previewLabel.setFont(juce::Font(11.0f));
    previewLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    previewLabel.setJustificationType(juce::Justification::topLeft);
    addAndMakeVisible(previewLabel);

    constructButton.onClick = [this] { if (onConstruct) onConstruct(build()); };
    addAndMakeVisible(constructButton);

    clearButton.onClick = [this] {
        for (auto& f : fields) f->value.clear();
        previewLabel.setText({}, juce::dontSendNotification);
    };
    addAndMakeVisible(clearButton);
}

void PromptBuilderContent::scanVocabulary(const juce::File& studioRoot) {
    const auto latents = studioRoot.getChildFile("latents");
    if (!latents.isDirectory()) return;
    for (const auto& dir : latents.findChildFiles(juce::File::findDirectories, false)) {
        for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.json")) {
            if (f.getFileName() == "details.json") continue;
            const auto parsed = juce::JSON::parse(f.loadFileAsString());
            auto* obj = parsed.getDynamicObject();
            if (obj == nullptr) continue;
            for (const auto& prop : obj->getProperties()) {
                const auto key = prop.name.toString();
                const auto text = prop.value.toString();
                if (text.isEmpty()) continue;
                if (isListField(key)) {
                    for (const auto& piece : juce::StringArray::fromTokens(text, ",", ""))
                        if (piece.trim().isNotEmpty()) vocab[key][piece.trim()]++;
                } else {
                    vocab[key][text.trim()]++;
                }
            }
        }
    }
}

std::vector<juce::String> PromptBuilderContent::byFrequency(const juce::String& field) const {
    std::vector<juce::String> out;
    const auto it = vocab.find(field);
    if (it == vocab.end()) return out;
    std::vector<std::pair<juce::String, int>> pairs(it->second.begin(), it->second.end());
    std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    for (const auto& p : pairs) out.push_back(p.first);
    return out;
}

PromptBuilderContent::Field& PromptBuilderContent::addField(const juce::String& key,
                                                            const juce::String& shown,
                                                            const juce::String& vocabKey,
                                                            bool multi) {
    fields.push_back(std::make_unique<Field>());
    auto& f = *fields.back();
    f.key = key;
    f.multi = multi;

    f.label.setText(shown, juce::dontSendNotification);
    f.label.setFont(juce::Font(12.0f));
    addAndMakeVisible(f.label);

    f.value.setMultiLine(false);
    addAndMakeVisible(f.value);

    if (vocabKey.isNotEmpty()) {
        const auto values = byFrequency(vocabKey);
        if (!values.empty()) {
            // Multi fields are an ADD action, so the picker returns to its prompt after
            // each pick; single fields are a CHOICE, so the picker stays on what was
            // chosen. Showing "--" after a successful pick made every row look unset.
            f.picker.addItem(multi ? "+ add..." : "-- none --", 1);
            for (int i = 0; i < static_cast<int>(values.size()); ++i)
                f.picker.addItem(values[static_cast<size_t>(i)], i + 2);
            f.picker.setSelectedId(1, juce::dontSendNotification);
            auto* fp = &f;
            f.picker.onChange = [this, fp] {
                const int sel = fp->picker.getSelectedId();
                if (sel <= 1) return;
                const auto picked = fp->picker.getItemText(fp->picker.getSelectedItemIndex());
                // Multi fields APPEND (that is what makes an instrument list possible
                // without a multi-select widget); single fields replace.
                if (fp->multi) {
                    const auto existing = fp->value.getText().trim();
                    if (existing.isEmpty()) {
                        fp->value.setText(picked);
                    } else {
                        const auto parts = juce::StringArray::fromTokens(existing, ",", "");
                        bool already = false;
                        for (const auto& p : parts) if (p.trim() == picked) already = true;
                        if (!already) fp->value.setText(existing + ", " + picked);
                    }
                    fp->picker.setSelectedId(1, juce::dontSendNotification);
                } else {
                    fp->value.setText(picked);   // picker stays on the chosen item
                }
            };
            addAndMakeVisible(f.picker);
        }
    }
    return f;
}

juce::String PromptBuilderContent::build() const {
    juce::StringArray parts;
    for (const auto& f : fields) {
        const auto v = f->value.getText().trim();
        if (v.isEmpty()) continue;                 // blank rows are simply omitted
        parts.add(f->key.isEmpty() ? v : f->key + ": " + v);
    }
    return parts.joinIntoString(", ");
}

void PromptBuilderContent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff1a1a1a));
}

void PromptBuilderContent::resized() {
    auto r = getLocalBounds().reduced(12);
    headerLabel.setBounds(r.removeFromTop(30));
    r.removeFromTop(4);

    auto foot = r.removeFromBottom(30);
    constructButton.setBounds(foot.removeFromLeft(140));
    foot.removeFromLeft(6);
    clearButton.setBounds(foot.removeFromLeft(70));
    r.removeFromBottom(6);
    previewLabel.setBounds(r.removeFromBottom(46));
    r.removeFromBottom(6);

    for (auto& f : fields) {
        auto line = r.removeFromTop(24);
        r.removeFromTop(3);
        f->label.setBounds(line.removeFromLeft(86));
        if (f->picker.getNumItems() > 0) {
            f->picker.setBounds(line.removeFromLeft(190).reduced(0, 1));
            line.removeFromLeft(6);
        }
        f->value.setBounds(line);
    }
}
