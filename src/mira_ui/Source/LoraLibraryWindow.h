#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "mira/db/Database.h"
#include "MiraLookAndFeel.h"

// "let have it in the osx toolbar -- we make a default folder for loras, pre load the
// lora in the folder with proper names, so the dropdown just gets that."
//
// The default folder already existed and already worked; what was missing was a NAME.
// `tar-step20000-epoch833.safetensors` is a filename -- it says which run and which
// checkpoint, and nothing about what the thing sounds like. This window is where a
// checkpoint gets called "Tron — bright" and the generate window's dropdown shows that.
//
// Names live in ui_settings under one JSON object keyed by FILENAME, not by path: the
// same checkpoint copied to another machine keeps its name, and a name whose file has
// been deleted costs one stale key rather than a broken row. Nothing here renames a file
// on disk -- PRD §1, and a renamed checkpoint would break every recipe that recorded it.
class LoraLibraryContent : public juce::Component,
                            public juce::TableListBoxModel
{
public:
    static constexpr const char* kNamesKey = "lora_names";

    LoraLibraryContent(const MiraLookAndFeel& lafIn, juce::File loraDirIn, mira::Database& db)
        : laf(lafIn), loraDir(std::move(loraDirIn)), database(db)
    {
        table.setModel(this);
        table.setColour(juce::ListBox::backgroundColourId, MiraLookAndFeel::surface);
        table.setRowHeight(26);
        table.getHeader().addColumn("Name", 1, 260, 120, -1, juce::TableHeaderComponent::notSortable);
        table.getHeader().addColumn("Checkpoint", 2, 300, 140, -1, juce::TableHeaderComponent::notSortable);
        table.getHeader().setStretchToFitActive(true);
        addAndMakeVisible(table);

        addButton.onClick = [this] { chooseFiles(); };
        addAndMakeVisible(addButton);

        revealButton.onClick = [this] { loraDir.revealToUser(); };
        addAndMakeVisible(revealButton);

        removeButton.onClick = [this] { removeSelected(); };
        removeButton.setEnabled(false);
        addAndMakeVisible(removeButton);

        table.setMultipleSelectionEnabled(true);

        hint.setText("Double-click a name to rename it. Select rows and Remove to take them out of "
                      "the library. Names show in the generate window's LoRA menus; files on "
                      "disk are never renamed.",
                      juce::dontSendNotification);
        hint.setFont(juce::Font(juce::FontOptions(11.0f)));
        hint.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
        addAndMakeVisible(hint);

        refresh();
    }

    ~LoraLibraryContent() override { table.setModel(nullptr); }

    void refresh()
    {
        files.clear();
        if (loraDir.isDirectory())
            for (const auto& f : loraDir.findChildFiles(juce::File::findFiles, false, "*.safetensors"))
                files.add(f);
        files.sort();
        table.updateContent();
        table.repaint();
    }

    // The one place that answers "what should this checkpoint be called?", so the table
    // here and the dropdown there can never disagree.
    static juce::String displayNameFor(mira::Database& db, const juce::File& file)
    {
        auto names = storedNames(db);
        if (auto* obj = names.getDynamicObject())
            if (obj->hasProperty(file.getFileName()))
            {
                auto name = obj->getProperty(file.getFileName()).toString().trim();
                if (name.isNotEmpty()) return name;
            }
        return {};
    }

    static juce::var storedNames(mira::Database& db)
    {
        auto stored = db.getSetting(kNamesKey);
        if (!stored) return juce::var(new juce::DynamicObject());
        auto parsed = juce::JSON::parse(juce::String(*stored));
        if (parsed.getDynamicObject() == nullptr) return juce::var(new juce::DynamicObject());
        return parsed;
    }

    std::function<void()> onChanged;

    void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface); }

    void resized() override
    {
        auto r = getLocalBounds().reduced(10);
        auto top = r.removeFromTop(26);
        addButton.setBounds(top.removeFromLeft(130));
        top.removeFromLeft(6);
        revealButton.setBounds(top.removeFromLeft(130));
        // Removal sits apart from the two additive buttons, on the other side of the row.
        removeButton.setBounds(top.removeFromRight(130));
        r.removeFromTop(6);
        hint.setBounds(r.removeFromBottom(30));
        table.setBounds(r);
    }

    int getNumRows() override { return files.size(); }

    void paintRowBackground(juce::Graphics& g, int row, int, int, bool selected) override
    {
        g.fillAll(selected ? MiraLookAndFeel::surface2
                           : (row % 2 ? MiraLookAndFeel::surface : MiraLookAndFeel::surface.brighter(0.02f)));
    }

    void paintCell(juce::Graphics& g, int row, int columnId, int width, int height, bool) override
    {
        if (row < 0 || row >= files.size()) return;
        const auto& file = files[row];
        g.setFont(juce::Font(juce::FontOptions(12.0f)));

        if (columnId == 1)
        {
            auto name = displayNameFor(database, file);
            // An unnamed checkpoint says so, rather than quietly repeating its filename
            // in both columns and looking like it has been named when it has not.
            g.setColour(name.isEmpty() ? MiraLookAndFeel::textDim : MiraLookAndFeel::text);
            g.drawText(name.isEmpty() ? "(unnamed)" : name,
                        juce::Rectangle<int>(6, 0, width - 8, height),
                        juce::Justification::centredLeft, true);
        }
        else
        {
            g.setColour(MiraLookAndFeel::textDim);
            g.drawText(file.getFileNameWithoutExtension(),
                        juce::Rectangle<int>(6, 0, width - 8, height),
                        juce::Justification::centredLeft, true);
        }
    }

    void cellDoubleClicked(int row, int, const juce::MouseEvent&) override { promptRename(row); }

    void selectedRowsChanged(int) override { removeButton.setEnabled(table.getNumSelectedRows() > 0); }

private:
    // "also allow to remove the loras". A symlink is UNLINKED -- deleting it must never
    // reach through to the 38 MB checkpoint on the external drive it points at. A real
    // file in the folder goes to the Trash, never a hard delete: a checkpoint is hours of
    // GPU time and the same "recoverable" rule Discard and Clean up already follow.
    void removeSelected()
    {
        juce::Array<juce::File> doomed;
        for (int i = 0; i < table.getNumSelectedRows(); ++i)
        {
            const int row = table.getSelectedRow(i);
            if (row >= 0 && row < files.size()) doomed.add(files[row]);
        }
        if (doomed.isEmpty()) return;

        int links = 0;
        for (const auto& f : doomed) if (f.isSymbolicLink()) ++links;
        juce::String detail;
        if (links == doomed.size())
            detail = "These are links. The checkpoint files they point at are not touched.";
        else if (links > 0)
            detail = juce::String(links) + " are links (their checkpoints are not touched); the rest "
                     "go to the Trash.";
        else
            detail = "The checkpoint files go to the Trash.";

        juce::NativeMessageBox::showOkCancelBox(
            juce::MessageBoxIconType::WarningIcon,
            "Remove " + juce::String(doomed.size()) + " LoRA(s) from the library?",
            detail + "\n\nAny recipe that already recorded one keeps its filename either way.",
            this,
            juce::ModalCallbackFunction::create([this, doomed](int result) {
                if (result == 0) return;
                auto names = storedNames(database);
                auto* obj = names.getDynamicObject();
                for (const auto& f : doomed)
                {
                    if (obj != nullptr) obj->removeProperty(f.getFileName());
                    if (f.isSymbolicLink()) f.deleteFile(); // unlinks, does not follow
                    else f.moveToTrash();
                }
                if (obj != nullptr)
                    database.setSetting(kNamesKey, juce::JSON::toString(names, true).toStdString());
                refresh();
                if (onChanged) onChanged();
            }));
    }

    void promptRename(int row)
    {
        if (row < 0 || row >= files.size()) return;
        const auto file = files[row];
        auto aw = std::make_shared<juce::AlertWindow>("Name this LoRA",
                                                        file.getFileNameWithoutExtension(),
                                                        juce::MessageBoxIconType::NoIcon, this);
        aw->addTextEditor("name", displayNameFor(database, file));
        aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
        aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        aw->enterModalState(true, juce::ModalCallbackFunction::create([this, aw, file](int result) {
            if (result != 1) return;
            setName(file, aw->getTextEditorContents("name").trim());
        }), false);
    }

    void setName(const juce::File& file, const juce::String& name)
    {
        auto names = storedNames(database);
        auto* obj = names.getDynamicObject();
        if (obj == nullptr) return;
        // An emptied name REMOVES the key rather than storing "", so "never named" and
        // "named nothing" stay distinguishable -- the same discipline setSetting itself
        // follows for a cleared setting.
        if (name.isEmpty()) obj->removeProperty(file.getFileName());
        else obj->setProperty(file.getFileName(), name);
        database.setSetting(kNamesKey, juce::JSON::toString(names, true).toStdString());
        table.repaint();
        if (onChanged) onChanged();
    }

    void chooseFiles()
    {
        chooser = std::make_unique<juce::FileChooser>("Add LoRA checkpoints", juce::File(),
                                                        "*.safetensors");
        auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                     | juce::FileBrowserComponent::canSelectMultipleItems;
        chooser->launchAsync(flags, [this](const juce::FileChooser& fc) {
            int added = 0;
            for (const auto& picked : fc.getResults())
            {
                if (!picked.existsAsFile()) continue;
                auto target = loraDir.getChildFile(picked.getFileName());
                if (target == picked) { ++added; continue; } // already in the folder
                // A SYMLINK, not a copy: these are 38 MB each and the originals are on an
                // external drive. The generate window reads them through the link, and
                // nothing is duplicated onto the boot disk.
                if (target.existsAsFile() || picked.createSymbolicLink(target, true)) ++added;
            }
            refresh();
            if (added > 0 && onChanged) onChanged();
        });
    }

    const MiraLookAndFeel& laf;
    juce::File loraDir;
    mira::Database& database;
    juce::Array<juce::File> files;
    juce::TableListBox table;
    juce::TextButton addButton { "Add LoRA file..." };
    juce::TextButton revealButton { "Show in Finder" };
    juce::TextButton removeButton { "Remove..." };
    juce::Label hint;
    std::unique_ptr<juce::FileChooser> chooser;
};

class LoraLibraryWindow : public juce::DocumentWindow
{
public:
    LoraLibraryWindow(const MiraLookAndFeel& laf, juce::File loraDir, mira::Database& db)
        : juce::DocumentWindow("LoRA Library", MiraLookAndFeel::surface,
                                juce::DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        content = new LoraLibraryContent(laf, std::move(loraDir), db);
        setContentOwned(content, false);
        setResizable(true, false);
        centreWithSize(660, 460);
        setVisible(true);
        toFront(true);
    }

    LoraLibraryContent* content = nullptr;
    std::function<void()> onClosed;
    void closeButtonPressed() override { if (onClosed) onClosed(); }
};
