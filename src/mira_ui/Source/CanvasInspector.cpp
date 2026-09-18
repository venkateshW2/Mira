#include "CanvasInspector.h"

namespace mira::canvas {

void Inspector::refreshLoras()
{
    loraFiles.clear();
    // The SAME directory the generate window reads -- sa3_gradio.py's loras/<model>/
    // convention, which sa3-studio/generate.sh keeps populating. Pointing this anywhere
    // else would give the canvas a different list of LoRAs from the rest of mira, which
    // is worse than having no list at all.
    const auto dir = studioRoot.getChildFile("stable-audio-3/optimized/mlx/loras/sa3-medium");
    if (dir.isDirectory())
        for (const auto& f : dir.findChildFiles(juce::File::findFiles, false, "*.safetensors"))
            loraFiles.add(f);
    loraFiles.sort();

    for (int i = 0; i < kSlots; ++i)
    {
        auto& box = slots[(size_t) i].box;
        const int previous = box.getSelectedId();
        box.clear(juce::dontSendNotification);
        box.addItem("(empty)", 1);
        // Ids are k + 2 over this sorted array, the same array currentSettings() indexes.
        for (int k = 0; k < loraFiles.size(); ++k)
            box.addItem(loraFiles[k].getFileNameWithoutExtension(), k + 2);
        box.setSelectedId(box.indexOfItemId(previous) >= 0 ? previous : 1, juce::dontSendNotification);
    }
}

juce::var Inspector::currentSettings() const
{
    auto* o = new juce::DynamicObject();
    o->setProperty("prompt", prompt.getText());
    o->setProperty("seconds", seconds.getValue());
    o->setProperty("steps", (int) steps.getValue());
    o->setProperty("cfg", cfg.getValue());
    o->setProperty("seed", (int) seed.getValue());

    juce::Array<juce::var> used;
    for (int i = 0; i < kSlots; ++i)
    {
        const auto& s = slots[(size_t) i];
        const int sel = s.box.getSelectedId();
        if (sel <= 1 || sel - 1 > loraFiles.size()) continue;
        auto* e = new juce::DynamicObject();
        // By NAME as well as path: a path goes stale the moment the checkpoint moves, and
        // the name is what the dropdown showed when the choice was made.
        e->setProperty("name", loraFiles[sel - 2].getFileNameWithoutExtension());
        e->setProperty("path", loraFiles[sel - 2].getFullPathName());
        e->setProperty("strength", s.blend.getValue());
        used.add(juce::var(e));
    }
    if (!used.isEmpty()) o->setProperty("loras", juce::var(used));
    return juce::var(o);
}

void Inspector::startGenerate()
{
    if (busy || !currentFolder.getFullPathName().isNotEmpty()) return;
    if (!currentFolder.isDirectory() && !currentFolder.createDirectory().wasOk())
    {
        status.setText("could not create " + currentFolder.getFullPathName(), juce::dontSendNotification);
        return;
    }

    const auto settings = currentSettings();
    auto* so = settings.getDynamicObject();

    // Named for the block and the seed, so a folder of takes reads as a history of what
    // was tried rather than a pile of timestamps.
    const auto stem = juce::File::createLegalFileName(blockName.getText())
                    + "-s" + juce::String((int) seed.getValue())
                    + "-" + juce::Time::getCurrentTime().formatted("%H%M%S");
    auto wav = currentFolder.getChildFile(stem + ".wav");
    for (int n = 2; wav.existsAsFile(); ++n) wav = currentFolder.getChildFile(stem + "-" + juce::String(n) + ".wav");

    auto* req = new juce::DynamicObject();
    req->setProperty("cmd", "generate");
    req->setProperty("prompt", so->getProperty("prompt"));
    req->setProperty("seconds", so->getProperty("seconds"));
    req->setProperty("steps", so->getProperty("steps"));
    req->setProperty("cfg", so->getProperty("cfg"));
    req->setProperty("seed", so->getProperty("seed"));
    req->setProperty("out", wav.getFullPathName());

    // The worker wants {path, strength}; the block stores {name, path, strength} so it can
    // still show what it used when the file has moved.
    if (auto* arr = so->getProperty("loras").getArray())
    {
        juce::Array<juce::var> specs;
        for (const auto& e : *arr)
            if (auto* src = e.getDynamicObject())
            {
                auto* spec = new juce::DynamicObject();
                spec->setProperty("path", src->getProperty("path"));
                spec->setProperty("strength", src->getProperty("strength"));
                specs.add(juce::var(spec));
            }
        if (!specs.isEmpty()) req->setProperty("loras", juce::var(specs));
    }

    juce::String error;
    auto* worker = hub.get(error);
    if (worker == nullptr)
    {
        // Never a silent no-op: a Generate button that does nothing and says nothing is
        // indistinguishable from one that is working (convention 6).
        status.setText(error.isNotEmpty() ? error : "worker is not running", juce::dontSendNotification);
        return;
    }

    busy = true;
    setEnabledForBlock(true);
    status.setText("generating... (first run loads the model, about 44 s)", juce::dontSendNotification);
    if (onBusyChanged) onBusyChanged();

    juce::Component::SafePointer<Inspector> safe (this);
    worker->send(juce::DynamicObject::Ptr (req), [safe, wav](bool ok, juce::var payload) {
        if (safe.getComponent() == nullptr) return;
        auto& self = *safe.getComponent();
        self.busy = false;
        self.setEnabledForBlock(true);
        if (self.onBusyChanged) self.onBusyChanged();

        if (!ok || !wav.existsAsFile())
        {
            self.status.setText("generate failed: " + payload.getProperty("error", "unknown").toString(),
                                 juce::dontSendNotification);
            return;
        }
        // The recipe beside the audio, same as the generate window writes -- a take you
        // cannot reproduce is a take you cannot learn from.
        wav.withFileExtension("json").replaceWithText(juce::JSON::toString(self.currentSettings(), false));

        const auto ms = (int) payload.getProperty("wall_ms", 0);
        self.status.setText("done in " + juce::String(ms / 1000.0, 1) + "s", juce::dontSendNotification);

        self.takeModel.files = self.currentFolder.findChildFiles(juce::File::findFiles, false, "*.wav");
        self.takeModel.files.sort();
        self.takeModel.chosen = wav;
        self.takeList.updateContent();
        self.takeList.repaint();
        // A fresh take becomes the block's audio. It is the one you just asked for; making
        // you click it again to hear it would be a step with no decision in it.
        if (self.onTakeChosen) self.onTakeChosen(wav);
    });
}

void Inspector::resized()
{
    auto r = getLocalBounds().reduced(10, 8);
    auto row = [&r](int h, int gap = 6) { auto x = r.removeFromTop(h); r.removeFromTop(gap); return x; };

    heading.setBounds(row(16, 2));
    blockName.setBounds(row(20));
    prompt.setBounds(row(86));

    for (int i = 0; i < kSlots; ++i)
    {
        auto& s = slots[(size_t) i];
        s.box.setBounds(row(24, 3));
        s.blend.setBounds(row(20, 6));
    }

    auto number = [&](juce::Label& l, juce::Slider& sl) {
        auto line = row(22, 4);
        l.setBounds(line.removeFromLeft(58));
        sl.setBounds(line);
    };
    number(secondsLabel, seconds);
    number(stepsLabel, steps);
    number(cfgLabel, cfg);
    number(seedLabel, seed);

    {
        auto line = row(28);
        reseed.setBounds(line.removeFromRight(76));
        line.removeFromRight(6);
        generate.setBounds(line);
    }
    status.setBounds(row(30));

    takesHeading.setBounds(row(16, 2));
    takeList.setBounds(r);
    takeModel.laf = &laf;
    takeModel.onPick = [this](juce::File f) {
        takeModel.chosen = f;
        takeList.repaint();
        if (onTakeChosen) onTakeChosen(f);
    };
}

} // namespace mira::canvas
