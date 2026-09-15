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

    randomiseButton.onClick = [this] { randomise(); };
    addAndMakeVisible(randomiseButton);

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
            const auto trig = obj->getProperty("trigger").toString().trim();
            for (const auto& prop : obj->getProperties()) {
                const auto key = prop.name.toString();
                const auto text = prop.value.toString();
                if (text.isEmpty()) continue;
                if (isListField(key)) {
                    for (const auto& piece : juce::StringArray::fromTokens(text, ",", ""))
                        if (piece.trim().isNotEmpty()) {
                            vocab[key][piece.trim()]++;
                            if (trig.isNotEmpty()) vocabByTrigger[trig][key][piece.trim()]++;
                        }
                } else {
                    vocab[key][text.trim()]++;
                    if (trig.isNotEmpty()) vocabByTrigger[trig][key][text.trim()]++;
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

    if (key.isNotEmpty() && vocabKey.isNotEmpty()) vocabKeyForField[key] = vocabKey;

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

PromptBuilderContent::Vocab PromptBuilderContent::pooled(const juce::String& field) const {
    juce::StringArray chosen;
    for (const auto& f : fields)
        if (f->key.isEmpty() && f->label.getText() == "trigger")
            for (const auto& p : juce::StringArray::fromTokens(f->value.getText(), ",", ""))
                if (p.trim().isNotEmpty()) chosen.add(p.trim());

    if (chosen.isEmpty()) {                      // no trigger picked: draw from everything
        const auto it = vocab.find(field);
        return it == vocab.end() ? Vocab{} : it->second;
    }
    Vocab out;
    for (const auto& trig : chosen) {
        const auto t = vocabByTrigger.find(trig);
        if (t == vocabByTrigger.end()) continue;
        const auto f = t->second.find(field);
        if (f == t->second.end()) continue;
        for (const auto& [value, count] : f->second) out[value] += count;
    }
    return out;
}

juce::String PromptBuilderContent::weightedPick(const Vocab& from) const {
    int total = 0;
    for (const auto& [value, count] : from) total += count;
    if (total <= 0) return {};
    int r = const_cast<juce::Random&>(rng).nextInt(total);
    for (const auto& [value, count] : from) {
        r -= count;
        if (r < 0) return value;
    }
    return from.begin()->first;
}

void PromptBuilderContent::randomise() {
    for (auto& f : fields) {
        // The trigger row is the QUESTION, not part of the answer -- rolling it would
        // change which corpus every other field is drawn from, so a re-roll would never
        // settle. The free-text tail is the user's, and is left alone too.
        if (f->key.isEmpty()) continue;
        const auto it = vocabKeyForField.find(f->key);
        if (it == vocabKeyForField.end()) {
            if (f->key == "BPM") {               // BPM has no picker but is still a real value
                const auto pick = weightedPick(pooled("bpm"));
                f->value.setText(pick, juce::dontSendNotification);
            }
            continue;
        }
        const auto candidates = pooled(it->second);
        if (candidates.empty()) { f->value.clear(); continue; }

        if (!f->multi) {
            f->value.setText(weightedPick(candidates), juce::dontSendNotification);
            continue;
        }
        // Multi fields get a handful. Three to five is what a real caption carries; more
        // reads as a shopping list and dilutes every word in it.
        const int want = 3 + rng.nextInt(3);
        juce::StringArray picked;
        for (int tries = 0; tries < want * 6 && picked.size() < want; ++tries) {
            const auto v = weightedPick(candidates);
            if (v.isNotEmpty() && !picked.contains(v)) picked.add(v);
        }
        f->value.setText(picked.joinIntoString(", "), juce::dontSendNotification);
    }
    previewLabel.setText("rolled - edit anything, then Construct", juce::dontSendNotification);
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
    foot.removeFromLeft(6);
    randomiseButton.setBounds(foot.removeFromLeft(100));
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
