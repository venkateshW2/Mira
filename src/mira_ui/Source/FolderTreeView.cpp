#include "FolderTreeView.h"

#include "mira/scan/Scanner.h"

#include <algorithm>

namespace {
constexpr int kToolbarHeight = 30;

// "still showing preset folders and stuff not needed" — Serum Presets etc have no
// audio anywhere in their subtree, so they're dead weight in a browser meant for
// finding sounds. Depth-limited (not unbounded) so a huge non-audio tree (node_modules-
// style folders that sneak into a library root) can't make every click hang; a folder
// deeper than this just gets shown rather than mis-hidden — same "false negatives over
// stalls" tradeoff as scanning a personal library at brute-force speed elsewhere in mira.
bool folderHasAudioInSubtree(const juce::File& folder, int depthRemaining = 6)
{
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFiles))
    {
        auto pathStd = entry.getFile().getFullPathName().toStdString();
        if (mira::hasSupportedAudioExtension(pathStd) && !mira::isAppleDoubleSidecar(pathStd))
            return true;
    }
    if (depthRemaining <= 0) return false;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findDirectories))
    {
        if (folderHasAudioInSubtree(entry.getFile(), depthRemaining - 1))
            return true;
    }
    return false;
}
} // namespace

namespace {
// Modal text-entry prompt, JUCE's own "owning AlertWindow via a shared_ptr captured by
// the modal callback" pattern — deleteWhenDismissed=false because we own the lifetime
// here (true would have JUCE delete the raw AlertWindow* itself while this shared_ptr
// still thinks it owns it too, a double free the moment this function's own copy of the
// shared_ptr goes out of scope).
void promptForText(const juce::String& title, const juce::String& message, const juce::String& initialValue,
                    juce::Component* associatedComponent, std::function<void(juce::String)> onConfirm)
{
    // associatedComponent anchors AlertWindow::updateLayout's own centreAroundComponent
    // call to mira's actual window — "the click sheet opens in some weird place" without
    // it, AlertWindow falls back to TopLevelWindow::getActiveTopLevelWindow(), which
    // should normally resolve to the same window anyway but isn't guaranteed to (e.g.
    // right after a PopupMenu closes, briefly a different "active" window).
    auto aw = std::make_shared<juce::AlertWindow>(title, message, juce::MessageBoxIconType::NoIcon,
                                                    associatedComponent);
    aw->addTextEditor("value", initialValue);
    aw->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    aw->enterModalState(true,
                         juce::ModalCallbackFunction::create([aw, onConfirm](int result) {
                             if (result == 1) onConfirm(aw->getTextEditorContents("value"));
                         }),
                         false);
}

// Modal multi-choice prompt with real, clearly separate buttons -- juce::AlertWindow
// only reliably supports about 3 buttons (its own doc comment: "generally up to 3
// buttons are supported... adding any more than this may have no effect"), which is
// exactly what happened using 5 (Stems/Samples/Music/No Group/Cancel): every button
// silently failed to appear at all -- "no choice here how to do it". This is a small,
// self-contained modal window instead, with one big stacked button per choice and no
// limit on how many it can hold.
class ChoiceDialogContent : public juce::Component
{
public:
    ChoiceDialogContent(const juce::String& message, const juce::StringArray& choices,
                         std::function<void(int)> onChoiceIn)
        : onChoice(std::move(onChoiceIn))
    {
        messageLabel.setText(message, juce::dontSendNotification);
        messageLabel.setJustificationType(juce::Justification::centredTop);
        messageLabel.setMinimumHorizontalScale(1.0f);
        messageLabel.setFont(juce::Font(juce::FontOptions(15.0f)));
        messageLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::text);
        addAndMakeVisible(messageLabel);

        for (auto choice : choices)
        {
            bool isCancel = choice == "Cancel";
            auto* b = buttons.add(new juce::TextButton(choice));
            b->setColour(juce::TextButton::buttonColourId, isCancel ? MiraLookAndFeel::surface3 : MiraLookAndFeel::accent);
            b->setColour(juce::TextButton::textColourOffId,
                         isCancel ? MiraLookAndFeel::textDim : juce::Colour(0xff1a1204));
            int index = buttons.size() - 1;
            b->onClick = [this, index] { if (onChoice) onChoice(index); };
            addAndMakeVisible(b);
        }
        setSize(320, 64 + buttons.size() * 48);
    }

    void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface2); }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(20, 16);
        messageLabel.setBounds(bounds.removeFromTop(44));
        bounds.removeFromTop(8);
        for (auto* b : buttons)
        {
            b->setBounds(bounds.removeFromTop(40));
            bounds.removeFromTop(8);
        }
    }

private:
    juce::Label messageLabel;
    juce::OwnedArray<juce::TextButton> buttons;
    std::function<void(int)> onChoice;
};

class ChoiceDialogWindow : public juce::DocumentWindow
{
public:
    ChoiceDialogWindow(const juce::String& title, const juce::String& message, const juce::StringArray& choices,
                        juce::Component* associatedComponent, std::function<void(int)> onChoice)
        : DocumentWindow(title, MiraLookAndFeel::surface2, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar(true);
        auto* content = new ChoiceDialogContent(message, choices, [this, onChoice](int index) {
            if (onChoice) onChoice(index);
            exitModalState(0);
        });
        setContentOwned(content, true);
        setResizable(false, false);
        centreAroundComponent(associatedComponent, getWidth(), getHeight());
        setVisible(true);
        // Genuinely modal ("a bigger popup which blocks the app usage") -- the rest of
        // mira can't be interacted with until a button (or the native close control,
        // handled as Cancel below) resolves this. deleteWhenDismissed=true means this
        // object cleans itself up the moment exitModalState() runs; nothing else owns it.
        enterModalState(true, nullptr, true);
    }

    void closeButtonPressed() override { exitModalState(0); } // native close = same outcome as Cancel, no choice fired
};

// Owning pointer intentionally not kept anywhere -- ChoiceDialogWindow deletes itself
// via enterModalState's deleteWhenDismissed once a choice (or the native close control)
// resolves it, same lifetime pattern promptForText's AlertWindow uses via shared_ptr,
// just JUCE's own built-in version of it for a real Component/TopLevelWindow.
void promptForChoice(const juce::String& title, const juce::String& message, const juce::StringArray& choices,
                      juce::Component* associatedComponent, std::function<void(int)> onChoice)
{
    new ChoiceDialogWindow(title, message, choices, associatedComponent, std::move(onChoice));
}
} // namespace

FolderTreeItem::FolderTreeItem(juce::File folderIn, const MiraLookAndFeel& lafIn, bool isRootIn,
                                FolderTreeView& ownerIn, juce::String displayNameIn)
    : folder(std::move(folderIn)), laf(lafIn), isRoot(isRootIn), displayName(std::move(displayNameIn)),
      owner(ownerIn)
{
}

bool FolderTreeItem::mightContainSubItems()
{
    // A real filesystem check per item (not cached beyond childrenLoaded, which only
    // covers *this* item once opened) — fine at personal-library folder counts, same
    // "brute force is fine at this scale" judgment already made elsewhere in mira.
    // Only counts a subfolder if it (or something under it) actually holds audio —
    // "still showing preset folders and stuff not needed" — so a dead-weight folder
    // like Serum Presets doesn't even get an expand arrow, root folders excepted (a
    // root was explicitly added by the user, so it always shows even if briefly empty).
    for (const auto& child : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findDirectories))
    {
        if (folderHasAudioInSubtree(child.getFile()))
            return true;
    }
    return false;
}

void FolderTreeItem::itemOpennessChanged(bool isNowOpen)
{
    if (!isNowOpen || childrenLoaded) return;
    childrenLoaded = true;

    // Immediate subdirectories only (non-recursive) — folders only, every level, the
    // whole reason this is a hand-rolled TreeViewItem instead of juce::FileTreeComponent.
    // Skip any subfolder with no audio anywhere in its own subtree (same filter as
    // mightContainSubItems above) — this is what actually makes Serum Presets etc.
    // disappear, since mightContainSubItems only gates the expand arrow.
    std::vector<juce::File> subfolders;
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findDirectories))
        if (folderHasAudioInSubtree(entry.getFile()))
            subfolders.push_back(entry.getFile());
    std::sort(subfolders.begin(), subfolders.end(),
              [](const juce::File& a, const juce::File& b) { return a.getFileName() < b.getFileName(); });

    for (const auto& sub : subfolders)
        addSubItem(new FolderTreeItem(sub, laf, false, owner));
}

int FolderTreeItem::getItemHeight() const { return isRoot ? 30 : 27; }

void FolderTreeItem::paintItem(juce::Graphics& g, int width, int height)
{
    auto bounds = juce::Rectangle<int>(0, 0, width, height);
    if (isSelected())
    {
        // "cant we do curves" -- an inset rounded selection pill (Finder/VS Code's own
        // convention) rather than a full-bleed rectangle flush with the row edges.
        g.setColour(MiraLookAndFeel::accentSoft);
        g.fillRoundedRectangle(bounds.reduced(3, 1).toFloat(), 5.0f);
    }

    // Mira's own flat folder glyph — not a system/Finder icon, not Soundly's blue, and
    // not colour-coded open/closed either (a loud accent tint read as "too much" —
    // shape carries the state instead, same convention Finder/most file browsers use:
    // closed is one solid silhouette, open is a two-tone back-panel-plus-lifted-flap).
    // Both states use the same neutral colour throughout.
    bool open = isOpen();
    auto iconH = height * 0.42f;
    auto iconY = (height - iconH) * 0.5f;
    juce::Rectangle<float> body(6.0f, iconY, 15.0f, iconH);
    float tabW = body.getWidth() * 0.5f;
    float tabH = iconH * 0.28f;
    juce::Colour neutral = MiraLookAndFeel::textDim.withAlpha(isRoot ? 1.0f : 0.85f);

    juce::Path back;
    back.addRoundedRectangle(body.getX(), body.getY() - tabH + 1.0f, tabW, tabH, 1.0f, 1.0f, true, true, false, false);
    back.addRoundedRectangle(body, 2.0f);

    if (!open)
    {
        g.setColour(neutral);
        g.fillPath(back);
    }
    else
    {
        // Back panel a touch dimmer, front flap (the part that's "lifted") brighter —
        // the two-tone read that says "open" without needing a second colour.
        g.setColour(neutral.withAlpha(neutral.getFloatAlpha() * 0.55f));
        g.fillPath(back);

        juce::Path flap;
        float flapTop = body.getY() + body.getHeight() * 0.32f;
        float overhang = 1.5f;
        flap.startNewSubPath(body.getX() - overhang, flapTop);
        flap.lineTo(body.getRight() + overhang, flapTop);
        flap.lineTo(body.getRight() + overhang * 0.5f, body.getBottom() + 1.5f);
        flap.lineTo(body.getX() - overhang * 0.5f, body.getBottom() + 1.5f);
        flap.closeSubPath();
        g.setColour(neutral);
        g.fillPath(flap);
    }

    auto textBounds = bounds.withTrimmedLeft(static_cast<int>(body.getRight()) + 8);
    g.setColour(isRoot ? MiraLookAndFeel::text : MiraLookAndFeel::text.withAlpha(0.85f));
    g.setFont(isRoot ? laf.sansMedium(14.0f) : laf.sansRegular(13.5f));

    // Root-only scan-state badge — "scanning is mira's job not the user's job": no Scan
    // button anywhere in the UI any more, this is purely a readout of what mira is
    // already doing on its own. Rescan (if a root's Error, or just to force a refresh)
    // lives in the real macOS menu bar instead, not here (Main.cpp's MiraMenuBarModel) —
    // "rescan can be in the osx toolbar... not in the ui, its confusing".
    if (isRoot)
    {
        auto state = owner.getRootScanState(folder.getFullPathName());
        if (state == FolderRootScanState::Scanning)
        {
            auto bullet = juce::CharPointer_UTF8("\xe2\x80\xa2"); // •
            auto badge = juce::String("scanning ") + juce::String(bullet) + " "
                       + juce::String(owner.getRootScanFilesSeen(folder.getFullPathName())) + " files";
            auto badgeBounds = textBounds.removeFromRight(juce::jmin(textBounds.getWidth(), 150));
            g.setColour(MiraLookAndFeel::accent);
            g.setFont(laf.monoMedium(11.5f)); // bold-ish, matches the status bar's own emphasis while scanning
            g.drawText(badge, badgeBounds, juce::Justification::centredRight, 1);
        }
        else if (state == FolderRootScanState::Queued)
        {
            auto badgeBounds = textBounds.removeFromRight(juce::jmin(textBounds.getWidth(), 90));
            g.setColour(MiraLookAndFeel::textFaint);
            g.setFont(laf.monoRegular(11.5f));
            g.drawText("queued", badgeBounds, juce::Justification::centredRight, 1);
        }
        else if (state == FolderRootScanState::Error)
        {
            auto badgeBounds = textBounds.removeFromRight(juce::jmin(textBounds.getWidth(), 110));
            g.setColour(MiraLookAndFeel::warn);
            g.setFont(laf.monoRegular(11.5f));
            g.drawText("rescan needed", badgeBounds, juce::Justification::centredRight, 1);
        }
    }

    g.drawText(displayName.isNotEmpty() ? displayName : folder.getFileName(), textBounds.reduced(0, 0),
                      juce::Justification::centredLeft, 1);
}

void FolderTreeItem::itemClicked(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        // Rename/group actions only make sense for a real added root, not a plain
        // filesystem subfolder discovered by walking disk — TASKS.md Phase 5: "so can we
        // rename the folder or can we group folders".
        if (isRoot) owner.showRootContextMenu(folder);
        return;
    }

    // Left-click: filter the file list to this folder and everything under it —
    // Soundly's own "click a folder, see everything inside it" behaviour.
    if (owner.onFolderSelected) owner.onFolderSelected(folder);
}

FolderGroupTreeItem::FolderGroupTreeItem(int64_t groupIdIn, juce::String nameIn, juce::String categoryIn,
                                          const MiraLookAndFeel& lafIn, FolderTreeView& ownerIn)
    : groupId(groupIdIn), name(std::move(nameIn)), category(std::move(categoryIn)), laf(lafIn), owner(ownerIn)
{
}

void FolderGroupTreeItem::paintItem(juce::Graphics& g, int width, int height)
{
    auto bounds = juce::Rectangle<int>(0, 0, width, height);
    if (isSelected())
    {
        g.setColour(MiraLookAndFeel::accentSoft);
        g.fillRoundedRectangle(bounds.reduced(3, 1).toFloat(), 5.0f);
    }

    // "stems folder have icon with it like stems, samples have that" -- the three
    // built-in categories each get a glyph suggesting what they actually are, not the
    // generic mark below; deliberately never the folder glyph FolderTreeItem draws
    // either way -- a group is never a real directory.
    juce::Colour neutral = MiraLookAndFeel::textDim;
    auto iconH = height * 0.42f;
    auto iconY = (height - iconH) * 0.5f;
    g.setColour(neutral);

    if (category.startsWith("stems"))
    {
        // Both "stems_score" and "stems_music" get the same stacked-track glyph -- the
        // distinction between the two is behavioural (segment markers vs not, TASKS.md
        // Phase 5), not visual; the name label already says which one this group is.
        // Three parallel horizontal bars of different lengths -- separate stacked
        // tracks, the whole idea of a stem set.
        float barH = iconH * 0.22f;
        float widths[3] = { 15.0f, 10.0f, 13.0f };
        for (int i = 0; i < 3; ++i)
            g.fillRoundedRectangle(4.0f, iconY + static_cast<float>(i) * (barH + 2.0f), widths[static_cast<size_t>(i)],
                                    barH, 1.0f);
    }
    else if (category == "samples")
    {
        // A 2x2 grid of small pads -- the common "sample pack" visual shorthand.
        float padSize = iconH * 0.42f;
        float gap = 2.5f;
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 2; ++col)
                g.fillRoundedRectangle(4.0f + static_cast<float>(col) * (padSize + gap),
                                        iconY + static_cast<float>(row) * (padSize + gap), padSize, padSize, 1.5f);
    }
    else if (category == "music")
    {
        // A single flat eighth-note glyph: oval head + stem + flag.
        juce::Path note;
        float headY = iconY + iconH * 0.62f;
        note.addEllipse(4.0f, headY, iconH * 0.4f, iconH * 0.34f);
        float stemX = 4.0f + iconH * 0.4f - 1.0f;
        note.addRectangle(stemX, iconY, 1.6f, iconH * 0.62f);
        juce::Path flag;
        flag.startNewSubPath(stemX + 1.6f, iconY);
        flag.quadraticTo(stemX + iconH * 0.5f, iconY + iconH * 0.15f, stemX + 1.6f, iconY + iconH * 0.36f);
        note.addPath(flag);
        g.fillPath(note);
    }
    else
    {
        // Plain user-created group ("New Group...") -- the original generic mark: a
        // small stack of two offset bars, reading as "a set of things".
        float iconHOld = height * 0.4f;
        float iconYOld = (static_cast<float>(height) - iconHOld) * 0.5f;
        g.setColour(neutral.withAlpha(0.55f));
        g.fillRoundedRectangle(6.0f, iconYOld, 15.0f, iconHOld * 0.42f, 1.5f);
        g.setColour(neutral);
        g.fillRoundedRectangle(3.0f, iconYOld + iconHOld * 0.58f, 15.0f, iconHOld * 0.42f, 1.5f);
    }

    auto textBounds = bounds.withTrimmedLeft(24);
    g.setColour(MiraLookAndFeel::text);
    g.setFont(laf.sansSemiBold(14.0f));
    g.drawText(name, textBounds, juce::Justification::centredLeft, 1);
}

void FolderGroupTreeItem::itemClicked(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu()) owner.showGroupContextMenu(groupId, name);
    // Left-click just opens/closes (TreeView's own default handling) — a group has no
    // real folder of its own to scope the file list to; its member roots do that.
}

FolderTreeView::FolderTreeView(mira::Database& databaseIn, const MiraLookAndFeel& lafIn)
    : database(databaseIn), laf(lafIn)
{
    addButton.setButtonText("+");
    addButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::accent);
    addButton.setColour(juce::TextButton::textColourOffId, juce::Colour(0xff1a1204));
    addButton.onClick = [this] {
        juce::PopupMenu menu;
        menu.addItem("Add Folder...", [this] { addFolderClicked(); });
        menu.addItem("New Group...", [this] { newGroupClicked(); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(addButton));
    };
    addAndMakeVisible(addButton);

    tree.setRootItem(&superRoot);
    tree.setRootItemVisible(false);
    tree.setColour(juce::TreeView::backgroundColourId, MiraLookAndFeel::surface);
    tree.setIndentSize(14);
    addAndMakeVisible(tree);

    rebuildRoots();
}

FolderTreeView::~FolderTreeView() { tree.setRootItem(nullptr); }

void FolderTreeView::rebuildRoots()
{
    superRoot.clearSubItems();
    auto infos = database.listFolderRootInfos();
    empty = infos.empty();

    // Groups first (each with its member roots nested inside, ordinary FolderTreeItems),
    // then every root with no group at the top level, same as before grouping existed —
    // "can we group folders... samples go to samples, stems and song can be grouped and
    // put into a new folder" reads as an opt-in organizational layer, not a mandatory one.
    for (const auto& group : database.listFolderGroups())
    {
        juce::String category = group.category ? juce::String(*group.category) : juce::String();
        auto* groupItem = new FolderGroupTreeItem(group.id, group.name, category, laf, *this);
        superRoot.addSubItem(groupItem);
        for (const auto& info : infos)
        {
            if (info.groupId != group.id) continue;
            juce::String displayName = info.displayName ? juce::String(*info.displayName) : juce::String();
            groupItem->addSubItem(new FolderTreeItem(juce::File(info.path), laf, true, *this, displayName));
        }
    }
    for (const auto& info : infos)
    {
        if (info.groupId) continue;
        juce::String displayName = info.displayName ? juce::String(*info.displayName) : juce::String();
        superRoot.addSubItem(new FolderTreeItem(juce::File(info.path), laf, true, *this, displayName));
    }
}

void FolderTreeView::addFolderClicked()
{
    folderChooser = std::make_unique<juce::FileChooser>(
        "Add a folder to mira", juce::File::getSpecialLocation(juce::File::userMusicDirectory));

    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    folderChooser->launchAsync(flags, [this](const juce::FileChooser& chooser) {
        auto result = chooser.getResult();
        if (!result.isDirectory()) return; // cancelled
        promptCategorizeNewFolder(result);
    });
}

int64_t FolderTreeView::findOrCreateCategoryGroup(const char* category, const char* displayName)
{
    // "i had already made groups so now this is doubling" -- reuse an existing group by
    // category first, then by matching name (a pre-existing hand-made "Stems" group
    // adopts the category rather than getting a redundant sibling), only creating a new
    // one if genuinely nothing matches. Shared by both the Add Folder flow and
    // re-categorizing an existing root (promptRecategorizeRoot) -- one place that
    // resolves "category name" -> "group id" rather than two copies of this logic.
    auto existing = database.findFolderGroupByCategory(category);
    if (!existing) existing = database.findFolderGroupByName(displayName);
    if (existing)
    {
        if (!existing->category) database.setFolderGroupCategory(existing->id, category);
        return existing->id;
    }
    return database.createFolderGroup(displayName, std::string(category));
}

void FolderTreeView::promptCategorizeNewFolder(const juce::File& folder)
{
    // "i select a folder and then i dont clik the choice, the choice is lost and i dono
    // why" -- a PopupMenu silently dismisses on any outside click with NO callback at
    // all, which (since this used to add the folder only *after* a choice) meant the
    // folder just never got added, with no explanation. promptForChoice below is a real
    // modal window -- it blocks the rest of the app while it's up ("a bigger popup which
    // blocks the app usage") and every path out of it, including Cancel, is an explicit
    // button with its own handled outcome; nothing can vanish silently.
    auto path = folder.getFullPathName().toStdString();

    auto finishAdd = [this, path](std::optional<int64_t> groupId) {
        database.addFolderRoot(path);
        if (groupId) database.setFolderRootGroup(path, *groupId);
        rebuildRoots();
        if (onFolderAdded) onFolderAdded(juce::File(path));
    };

    auto assignToCategory = [this, finishAdd](const char* category, const char* displayName) {
        finishAdd(findOrCreateCategoryGroup(category, displayName));
    };

    promptForChoice("Categorize Folder", "What kind of folder is \"" + folder.getFileName() + "\"?",
                     { "Score Stems", "Music Stems", "Samples", "Music", "No Group", "Cancel" }, this,
                     [assignToCategory, finishAdd](int index) {
                         switch (index)
                         {
                             case 0: assignToCategory("stems_score", "Score Stems"); break;
                             case 1: assignToCategory("stems_music", "Music Stems"); break;
                             case 2: assignToCategory("samples", "Samples"); break;
                             case 3: assignToCategory("music", "Music"); break;
                             case 4: finishAdd(std::nullopt); break;
                             default: break; // Cancel (5) or the native close control -- folder is not added at all
                         }
                     });
}

void FolderTreeView::promptRecategorizeRoot(const juce::File& folder)
{
    // "film score stems -- or music stems is the division, not film mix stems" -- the
    // built-in categories are these four, not the earlier three: score stems (long-form,
    // will eventually want segment markers) and music stems (short per-track submixes of
    // a song) behave differently enough that they're distinct kinds, not one "Stems"
    // bucket. Both still route to the stem-tuned instrument algorithm (see
    // MainComponent::rootWantsStemDeclaration) -- "so the instrument algorithm shifts to
    // the correct algorithm" -- since that only cares whether it's *a* stem, not which kind.
    auto path = folder.getFullPathName().toStdString();
    promptForChoice("Categorize Folder", "What kind of folder is \"" + folder.getFileName() + "\"?",
                     { "Score Stems", "Music Stems", "Samples", "Music", "Remove from Group", "Cancel" }, this,
                     [this, path](int index) {
                         switch (index)
                         {
                             case 0: database.setFolderRootGroup(path, findOrCreateCategoryGroup("stems_score", "Score Stems")); break;
                             case 1: database.setFolderRootGroup(path, findOrCreateCategoryGroup("stems_music", "Music Stems")); break;
                             case 2: database.setFolderRootGroup(path, findOrCreateCategoryGroup("samples", "Samples")); break;
                             case 3: database.setFolderRootGroup(path, findOrCreateCategoryGroup("music", "Music")); break;
                             case 4: database.setFolderRootGroup(path, std::nullopt); break;
                             default: return; // Cancel or native close -- leave the current grouping untouched
                         }
                         rebuildRoots();
                     });
}

void FolderTreeView::newGroupClicked()
{
    promptForText("New Group", "Name for the new group:", "New Group", this, [this](juce::String name) {
        if (name.trim().isEmpty()) return;
        database.createFolderGroup(name.trim().toStdString());
        rebuildRoots();
    });
}

void FolderTreeView::promptRenameRoot(const juce::File& folder)
{
    promptForText("Rename Folder", "Display name in mira (the real folder on disk is never renamed):",
                   folder.getFileName(), this, [this, folder](juce::String name) {
                       name = name.trim();
                       auto path = folder.getFullPathName().toStdString();
                       // Empty clears the override, reverting to the folder's real name
                       // — matches setFolderRootDisplayName's own nullopt-means-default
                       // contract (Database.h).
                       if (name.isEmpty() || name == folder.getFileName())
                           database.setFolderRootDisplayName(path, std::nullopt);
                       else
                           database.setFolderRootDisplayName(path, name.toStdString());
                       rebuildRoots();
                   });
}

void FolderTreeView::promptMoveToGroup(const juce::File& folder)
{
    auto path = folder.getFullPathName().toStdString();
    juce::PopupMenu menu;
    // Find current group (if any) so "Remove from Group" only appears when relevant.
    std::optional<int64_t> currentGroup;
    for (const auto& info : database.listFolderRootInfos())
        if (info.path == path) { currentGroup = info.groupId; break; }

    for (const auto& group : database.listFolderGroups())
    {
        bool isCurrent = currentGroup && *currentGroup == group.id;
        menu.addItem(group.name + (isCurrent ? " (current)" : ""), !isCurrent, false, [this, path, group] {
            database.setFolderRootGroup(path, group.id);
            rebuildRoots();
        });
    }
    if (!database.listFolderGroups().empty()) menu.addSeparator();
    menu.addItem("New Group...", [this, path] {
        promptForText("New Group", "Name for the new group:", "New Group", this, [this, path](juce::String name) {
            if (name.trim().isEmpty()) return;
            auto groupId = database.createFolderGroup(name.trim().toStdString());
            database.setFolderRootGroup(path, groupId);
            rebuildRoots();
        });
    });
    if (currentGroup)
    {
        menu.addSeparator();
        menu.addItem("Remove from Group", [this, path] {
            database.setFolderRootGroup(path, std::nullopt);
            rebuildRoots();
        });
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void FolderTreeView::showRootContextMenu(const juce::File& folder)
{
    // "Move to Group..." opens its own popup (promptMoveToGroup) rather than a nested
    // PopupMenu::addSubMenu — it needs a live lookup of the current group and the full
    // group list at click time, which reads more naturally as a second top-level menu.
    juce::PopupMenu menu;
    menu.addItem("Rename...", [this, folder] { promptRenameRoot(folder); });
    // "we put it stems while loading and then regroup or change the group what happens
    // -- the type of folder should be marked" -- a quick re-run of the same Stems/
    // Samples/Music choice Add Folder shows, for a root that's already been added (its
    // category isn't fixed at add-time, it's just whichever group it's currently in).
    menu.addItem("Categorize...", [this, folder] { promptRecategorizeRoot(folder); });
    menu.addItem("Move to Group...", [this, folder] { promptMoveToGroup(folder); });
    menu.addSeparator();
    // "in the sidebar i want right [click] folder and choose to analyse the folder so
    // all the files go into analysis" — MainComponent does the actual filesystem walk +
    // enqueue, since it already owns the analyze queue; this just reports which folder.
    menu.addItem("Analyze Folder", [this, folder] {
        if (onAnalyzeFolderRequested) onAnalyzeFolderRequested(folder);
    });
    menu.addSeparator();
    // "we also need a delete from mira option -- remove from this database" -- only ever
    // removes the ui_folder_roots row (stops mira tracking/showing it); any files/
    // analysis already recorded for paths under it are left alone, same "no file ever
    // moves, nothing destructive by default" discipline the rest of mira follows. Asks
    // first since it's a deliberate structural change from a single right-click.
    menu.addItem("Remove Folder from mira...", [this, folder] { promptRemoveRoot(folder); });
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void FolderTreeView::promptRemoveRoot(const juce::File& folder)
{
    auto path = folder.getFullPathName().toStdString();
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::WarningIcon)
            .withTitle("Remove Folder")
            .withMessage("Remove \"" + folder.getFileName()
                         + "\" from mira? The real folder and its files are never touched -- this only stops "
                           "mira tracking it. Any scan/analysis data already recorded for its files stays in "
                           "the database until it's re-added.")
            .withButton("Remove")
            .withButton("Cancel")
            .withAssociatedComponent(this),
        [this, path](int result) {
            if (result != 1) return;
            database.removeFolderRoot(path);
            rebuildRoots();
        });
}

void FolderTreeView::showGroupContextMenu(int64_t groupId, const juce::String& currentName)
{
    juce::PopupMenu menu;
    menu.addItem("Rename Group...", [this, groupId, currentName] {
        promptForText("Rename Group", "Group name:", currentName, this, [this, groupId](juce::String name) {
            name = name.trim();
            if (name.isEmpty()) return;
            database.renameFolderGroup(groupId, name.toStdString());
            rebuildRoots();
        });
    });
    menu.addItem("Delete Group", [this, groupId] {
        // Ungroups members, never touches their real folders/files (Database::
        // deleteFolderGroup's own contract) — "delete group" here means "stop grouping
        // them", not "remove the folders".
        database.deleteFolderGroup(groupId);
        rebuildRoots();
    });
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void FolderTreeView::setRootScanState(const juce::String& path, FolderRootScanState state, int64_t filesSeen)
{
    rootScanState[path] = { state, filesSeen };
    tree.repaint(); // TreeView owns its rows' painting; repaint() on this Component alone wouldn't reach them
}

FolderRootScanState FolderTreeView::getRootScanState(const juce::String& path) const
{
    auto it = rootScanState.find(path);
    return it == rootScanState.end() ? FolderRootScanState::Idle : it->second.first;
}

int64_t FolderTreeView::getRootScanFilesSeen(const juce::String& path) const
{
    auto it = rootScanState.find(path);
    return it == rootScanState.end() ? 0 : it->second.second;
}

void FolderTreeView::paint(juce::Graphics& g)
{
    MiraLookAndFeel::paintGlassPanel(g, getLocalBounds(), 0.0f, MiraLookAndFeel::surface);

    auto toolbar = getLocalBounds().removeFromTop(kToolbarHeight).reduced(8, 4);
    toolbar.removeFromRight(24);
    g.setColour(MiraLookAndFeel::textFaint);
    g.setFont(laf.sansRegular(12.0f));
    g.drawText("MIRA FOLDERS", toolbar, juce::Justification::centredLeft, 1);

    if (empty)
    {
        g.setColour(MiraLookAndFeel::textFaint);
        g.setFont(laf.sansRegular(13.5f));
        g.drawFittedText("No folders added yet\nclick + to add one",
                          getLocalBounds().withTrimmedTop(kToolbarHeight).reduced(12),
                          juce::Justification::centredTop, 2);
    }
}

void FolderTreeView::resized()
{
    auto bounds = getLocalBounds();
    auto toolbar = bounds.removeFromTop(kToolbarHeight).reduced(4);
    addButton.setBounds(toolbar.removeFromRight(24));
    tree.setBounds(bounds);
}
