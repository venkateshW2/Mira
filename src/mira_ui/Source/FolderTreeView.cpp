#include "FolderTreeView.h"

#include "mira/scan/Scanner.h"
#include "mira/caption/TagVocabulary.h"

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
        // Rename/group/remove only make sense for a real added root: a subfolder isn't a
        // row in ui_folder_roots, it's something found by walking disk, so there is
        // nothing to rename or remove. But the *actions on audio* apply to any folder --
        // "subfolders dont get to be deleted or analysed i have to analyse to parent
        // folders". A subfolder used to get no menu at all, which meant analysing one
        // stem folder of twenty meant analysing the parent and waiting for all of them.
        if (isRoot) owner.showRootContextMenu(folder);
        else owner.showSubfolderContextMenu(folder);
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

    // A group created before categories existed (or before this one's category was added)
    // has none stored -- the real library's SAMPLES group is exactly that, which is why it
    // drew the generic mark while MUSIC drew a note. Fall back to the built-in's own name
    // so it still gets its glyph, rather than migrating the row from inside a paint call.
    // Display-level only: nothing that routes analysis reads this.
    auto effectiveCategory = category;
    if (effectiveCategory.isEmpty())
    {
        auto lowered = name.toLowerCase();
        if (lowered == "score stems") effectiveCategory = "stems_score";
        else if (lowered == "music stems") effectiveCategory = "stems_music";
        else if (lowered == "samples") effectiveCategory = "samples";
        else if (lowered == "music") effectiveCategory = "music";
        else if (lowered == "projects") effectiveCategory = "projects";
    }

    // "so now if u have a differtn icon for music lets do differnt icons for all the 4
    // categories will be nice" -- all four built-ins are now visually distinct. They were
    // not: both stem kinds shared one glyph, and SAMPLES fell through to the generic mark
    // entirely (see effectiveCategory above).
    if (effectiveCategory == "stems_music")
    {
        // Three parallel horizontal bars of different lengths -- separate stacked tracks,
        // the whole idea of a stem set. This is the plain stem glyph; score stems below
        // are this plus what makes them different.
        float barH = iconH * 0.22f;
        float widths[3] = { 15.0f, 10.0f, 13.0f };
        for (int i = 0; i < 3; ++i)
            g.fillRoundedRectangle(4.0f, iconY + static_cast<float>(i) * (barH + 2.0f), widths[static_cast<size_t>(i)],
                                    barH, 1.0f);
    }
    else if (effectiveCategory.startsWith("stems"))
    {
        // Score stems ("stems_score", or the legacy "stems" the SCORE STEMS group is
        // still stored under -- see MainComponent's isStemPath note). Same stacked tracks
        // as music stems, crossed by a cue marker: the difference between the two kinds is
        // that a score stem is a long reel carrying cues and segment markers, which is
        // exactly what the extra line says. Built from the music-stem glyph rather than a
        // wholly separate picture so the two still read as siblings.
        float barH = iconH * 0.22f;
        float widths[3] = { 13.0f, 9.0f, 11.0f };
        for (int i = 0; i < 3; ++i)
            g.fillRoundedRectangle(3.0f, iconY + static_cast<float>(i) * (barH + 2.0f), widths[static_cast<size_t>(i)],
                                    barH, 1.0f);
        // The marker itself: a full-height vertical line with a small flag at the top,
        // drawn brighter than the bars so it reads as laid *over* the tracks.
        g.setColour(MiraLookAndFeel::text);
        float markerX = 15.5f;
        g.fillRect(markerX, iconY - 1.0f, 1.4f, iconH + 2.0f);
        juce::Path flag;
        flag.addTriangle(markerX + 1.4f, iconY - 1.0f, markerX + 5.0f, iconY + 1.0f, markerX + 1.4f, iconY + 3.0f);
        g.fillPath(flag);
        g.setColour(neutral);
    }
    else if (effectiveCategory == "projects")
    {
        // A box with an arrow leaving it: a project is a set of cues that gets handed
        // over (MIRA-GENERATE.md §3.1 -- the folder IS the deliverable). Deliberately
        // not a folder glyph and deliberately not stem bars, the two things it could
        // otherwise be mistaken for.
        float boxW = iconH * 0.72f;
        g.drawRoundedRectangle(4.0f, iconY + iconH * 0.14f, boxW, iconH * 0.72f, 1.5f, 1.2f);
        float arrowY = iconY + iconH * 0.5f;
        float arrowX = 4.0f + boxW + 1.5f;
        g.fillRect(arrowX, arrowY - 0.7f, 5.0f, 1.4f);
        juce::Path head;
        head.addTriangle(arrowX + 4.0f, arrowY - 3.0f, arrowX + 8.0f, arrowY, arrowX + 4.0f, arrowY + 3.0f);
        g.fillPath(head);
    }
    else if (effectiveCategory == "collections")
    {
        // The same turned-corner sheets CollectionTreeItem draws, so the heading and its
        // children read as one family.
        g.fillRoundedRectangle(7.0f, iconY, 12.0f, iconH * 0.78f, 1.5f);
        g.setColour(neutral.withAlpha(0.55f));
        g.fillRoundedRectangle(4.0f, iconY + iconH * 0.26f, 12.0f, iconH * 0.78f, 1.5f);
        g.setColour(neutral);
    }
    else if (effectiveCategory == "samples")
    {
        // A 2x2 grid of small pads -- the common "sample pack" visual shorthand.
        float padSize = iconH * 0.42f;
        float gap = 2.5f;
        for (int row = 0; row < 2; ++row)
            for (int col = 0; col < 2; ++col)
                g.fillRoundedRectangle(4.0f + static_cast<float>(col) * (padSize + gap),
                                        iconY + static_cast<float>(row) * (padSize + gap), padSize, padSize, 1.5f);
    }
    else if (effectiveCategory == "music")
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
    // groupId 0 is the synthetic COLLECTIONS heading -- not a ui_folder_groups row, so
    // there is nothing to rename, delete or move roots into.
    if (groupId == 0)
    {
        if (!e.mods.isPopupMenu()) setOpen(!isOpen());
        return;
    }
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

    // Collections last, under their own heading: they are a different kind of thing from
    // everything above (files gathered by hand, not folders found on disk), and mixing
    // them into the same list would suggest they behave the same way -- they have no
    // subfolders to expand and no scan of their own.
    auto collections = database.listCollections();
    if (!collections.empty())
    {
        empty = false;
        auto* header = new FolderGroupTreeItem(0, "COLLECTIONS", "collections", laf, *this);
        superRoot.addSubItem(header);
        for (const auto& c : collections)
            header->addSubItem(new CollectionTreeItem(c.id, c.name, c.fileCount, laf, *this));
        header->setOpen(true);
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

// MIRA-GENERATE.md Phase 1. Name first, then where to put it: the name is the thing the
// user has in their head, and it becomes both the directory name and every exported
// filename's first token (§3.7), so asking for it first matches the order they think in.
void FolderTreeView::promptNewProject()
{
    promptForText("New Project", "Project name:", "", this, [this](juce::String name) {
        name = name.trim();
        if (name.isEmpty()) return; // cancelled, or nothing typed -- no silent "Untitled"
        folderChooser = std::make_unique<juce::FileChooser>(
            "Where should \"" + name + "\" live?",
            juce::File::getSpecialLocation(juce::File::userMusicDirectory));
        auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
        folderChooser->launchAsync(flags, [this, name](const juce::FileChooser& chooser) {
            auto parent = chooser.getResult();
            if (!parent.isDirectory()) return; // cancelled
            auto folder = parent.getChildFile(name);
            // An existing directory is adopted rather than refused: "New Project" over a
            // folder of takes from a previous session is a reasonable thing to want, and
            // registerProject is idempotent either way. Nothing inside is touched.
            if (!folder.isDirectory() && !folder.createDirectory().wasOk())
            {
                juce::AlertWindow::showMessageBoxAsync(juce::AlertWindow::WarningIcon, "New Project",
                    "Could not create:\n" + folder.getFullPathName());
                return;
            }
            registerProject(folder);
        });
    });
}

void FolderTreeView::promptOpenProject()
{
    folderChooser = std::make_unique<juce::FileChooser>(
        "Open a project folder", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;
    folderChooser->launchAsync(flags, [this](const juce::FileChooser& chooser) {
        auto folder = chooser.getResult();
        if (!folder.isDirectory()) return; // cancelled
        registerProject(folder);
    });
}

void FolderTreeView::registerProject(const juce::File& folder)
{
    auto path = folder.getFullPathName().toStdString();
    bool isNewRoot = true;
    for (const auto& info : database.listFolderRootInfos())
        if (info.path == path) { isNewRoot = false; break; }

    database.addFolderRoot(path); // idempotent (INSERT OR IGNORE)
    // "projects", not "music", even though a generated cue IS music (MIRA-GENERATE.md
    // §3.2 originally said music). findOrCreateCategoryGroup keys a group BY its
    // category, so a PROJECTS group filed under "music" could never be found -- the
    // older MUSIC group wins findFolderGroupByCategory's ORDER BY added_at -- and every
    // new project would have been filed into MUSIC instead. Nothing routes analysis off
    // "music" either way: only a `stems*` category is ever read (MainComponent::
    // isStemPath, rootWantsStemDeclaration), so a project folder gets exactly the
    // treatment §3.2 asked for -- router-decided content type, no stem declaration.
    database.setFolderRootGroup(path, findOrCreateCategoryGroup("projects", "Projects"));
    rebuildRoots();
    // Only scan a root mira has never seen. Reopening a project must not re-trigger a
    // full scan of a folder that is already indexed and being added to take by take.
    if (isNewRoot && onFolderAdded) onFolderAdded(folder);
    if (onProjectOpened) onProjectOpened(folder);
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

// A plain filesystem subfolder. Deliberately a subset of the root menu rather than a
// disabled-looking copy of it: only the things that mean something for a folder mira
// doesn't track as a root of its own.
CollectionTreeItem::CollectionTreeItem(int64_t idIn, juce::String nameIn, int fileCountIn,
                                        const MiraLookAndFeel& lafIn, FolderTreeView& ownerIn)
    : collectionId(idIn), name(std::move(nameIn)), fileCount(fileCountIn), laf(lafIn), owner(ownerIn)
{
}

void CollectionTreeItem::paintItem(juce::Graphics& g, int width, int height)
{
    auto bounds = juce::Rectangle<int>(0, 0, width, height);
    if (isSelected())
    {
        g.setColour(MiraLookAndFeel::accentSoft);
        g.fillRoundedRectangle(bounds.reduced(3, 1).toFloat(), 5.0f);
    }

    // A stack of sheets with one corner turned -- "files gathered by hand", distinct from
    // both the folder glyph (a real directory) and the group glyphs (containers of
    // folders). Nothing here is a directory, so nothing here should look like one.
    auto iconH = height * 0.40f;
    auto iconY = (height - iconH) * 0.5f;
    g.setColour(MiraLookAndFeel::textDim);
    g.fillRoundedRectangle(7.0f, iconY, 12.0f, iconH * 0.78f, 1.5f);
    g.setColour(MiraLookAndFeel::textFaint);
    g.fillRoundedRectangle(4.5f, iconY + iconH * 0.24f, 12.0f, iconH * 0.78f, 1.5f);

    auto textBounds = bounds.withTrimmedLeft(24);
    // The count sits on the right, dim -- a collection with nothing in it should say so
    // rather than looking like a folder that failed to load.
    auto countBounds = textBounds.removeFromRight(juce::jmin(44, textBounds.getWidth() / 3));
    g.setColour(MiraLookAndFeel::textFaint);
    g.setFont(laf.monoRegular(11.0f));
    g.drawText(juce::String(fileCount), countBounds, juce::Justification::centredRight, 1);

    g.setColour(isSelected() ? MiraLookAndFeel::accent : MiraLookAndFeel::text);
    g.setFont(laf.sansRegular(13.5f));
    g.drawText(name, textBounds, juce::Justification::centredLeft, 1);
}

void CollectionTreeItem::itemClicked(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        owner.showCollectionContextMenu(collectionId, name);
        return;
    }
    if (owner.onCollectionSelected) owner.onCollectionSelected(collectionId);
}

void FolderTreeView::showCollectionContextMenu(int64_t collectionId, const juce::String& currentName)
{
    juce::PopupMenu menu;
    menu.addItem("Rename...", [this, collectionId, currentName] {
        promptRenameCollection(collectionId, currentName);
    });
    menu.addSeparator();
    menu.addItem("Delete Collection...", [this, collectionId, currentName] {
        promptDeleteCollection(collectionId, currentName);
    });
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void FolderTreeView::promptRenameCollection(int64_t collectionId, const juce::String& currentName)
{
    promptForText("Rename Collection", "New name for \"" + currentName + "\":", currentName, this,
                   [this, collectionId](const juce::String& name) {
                       if (name.trim().isEmpty()) return;
                       database.renameCollection(collectionId, name.trim().toStdString());
                       rebuildRoots();
                   });
}

void FolderTreeView::promptDeleteCollection(int64_t collectionId, const juce::String& currentName)
{
    // Worth spelling out what is and isn't destroyed: a collection holds references, so
    // deleting one removes the grouping and nothing else. Same "nothing destructive by
    // default" discipline as Remove Folder from mira.
    juce::AlertWindow::showAsync(
        juce::MessageBoxOptions()
            .withIconType(juce::MessageBoxIconType::QuestionIcon)
            .withTitle("Delete Collection")
            .withMessage("Delete the collection \"" + currentName + "\"?\n\nThe files in it stay in your "
                         "library and on disk exactly where they are — only this grouping is removed.")
            .withButton("Delete")
            .withButton("Cancel"),
        [this, collectionId](int result) {
            if (result != 1) return;
            database.deleteCollection(collectionId);
            rebuildRoots();
        });
}

void FolderTreeView::promptAddFiles()
{
    folderChooser = std::make_unique<juce::FileChooser>(
        "Add files to mira", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
        "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.m4a;*.ogg;*.opus");

    auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                  | juce::FileBrowserComponent::canSelectMultipleItems;
    folderChooser->launchAsync(flags, [this](const juce::FileChooser& chooser) {
        std::vector<juce::String> paths;
        for (const auto& f : chooser.getResults())
            if (f.existsAsFile()) paths.push_back(f.getFullPathName());
        if (paths.empty()) return; // cancelled
        promptForCollectionThen(std::move(paths));
    });
}

// Which collection the picked files go into, asked before they are indexed so the whole
// operation is one decision rather than "they appeared somewhere, now go find them".
// Existing collections first, then New Collection... -- the common case after the first
// time is adding to one that already exists.
void FolderTreeView::promptForCollectionThen(std::vector<juce::String> paths)
{
    auto collections = database.listCollections();
    juce::StringArray choices;
    for (const auto& c : collections)
        choices.add(juce::String(c.name) + "  (" + juce::String(c.fileCount) + ")");
    choices.add("New Collection...");
    choices.add("Cancel");

    promptForChoice("Add " + juce::String(paths.size()) + (paths.size() == 1 ? " File" : " Files"),
                     "Which collection should these go into?", choices, this,
                     [this, paths = std::move(paths), collections](int index) mutable {
                         if (index < 0 || index >= static_cast<int>(collections.size()) + 1) return; // Cancel
                         if (index < static_cast<int>(collections.size()))
                         {
                             if (onFilesAdded) onFilesAdded(std::move(paths), collections[static_cast<size_t>(index)].id);
                             return;
                         }
                         promptForText("New Collection", "Name for the new collection:", "New Collection", this,
                                        [this, paths = std::move(paths)](const juce::String& name) mutable {
                                            auto trimmed = name.trim();
                                            if (trimmed.isEmpty()) return;
                                            auto id = database.createCollection(trimmed.toStdString());
                                            if (onFilesAdded) onFilesAdded(std::move(paths), id);
                                        });
                     });
}

void FolderTreeView::promptTagFolder(const juce::File& folder)
{
    const std::string folderPath = folder.getFullPathName().toStdString();

    // Read what this folder already has, so re-opening the dialog EDITS rather than
    // replaces. getFolderDefault is the exact-path getter, not findFolderDefaultsForPath
    // -- the latter answers "what applies to a file under here", which would show an
    // ancestor's tags as if they were this folder's own and then write them down a level.
    juce::String curMaterial, curWorld1, curWorld2, curHarmonic, curSignature;
    juce::StringArray extras;   // keywords that aren't in any vocabulary -- see below
    if (auto stored = database.getFolderDefault(folderPath))
    {
        for (const auto& kw : database.jsonStringArray(*stored, "$.keywords"))
        {
            juce::String w(kw);
            if (mira::inVocab(kw, mira::kMaterialVocab, mira::kMaterialVocabCount))
                curMaterial = w;
            else if (mira::inVocab(kw, mira::kWorldVocab, mira::kWorldVocabCount))
            {
                if (curWorld1.isEmpty()) curWorld1 = w;
                else if (curWorld2.isEmpty()) curWorld2 = w;
            }
            else if (mira::inVocab(kw, mira::kHarmonicVocab, mira::kHarmonicVocabCount))
                curHarmonic = w;
            else if (mira::inVocab(kw, mira::kSignatureVocab, mira::kSignatureVocabCount))
                curSignature = w;
            else
                // `mira tag-folder --keywords` can write free-form words this dialog has
                // no box for. Carry them through untouched rather than dropping them on
                // save -- a UI that silently deletes what the CLI wrote is worse than one
                // that can't edit it.
                extras.add(w);
        }
    }

    // AlertWindow's own combo boxes rather than a bespoke dialog: five dropdowns is
    // exactly what it is for, and it keeps the same anchoring/lifetime shape as
    // promptForText above. Its doc comment warns that more than about 3 BUTTONS may
    // silently fail -- Save/Clear/Cancel is three, deliberately.
    auto aw = std::make_shared<juce::AlertWindow>(
        "Tag Folder", "Applies to every file under \"" + folder.getFileName() +
        "\".\nA file's own tags still win over these.",
        juce::MessageBoxIconType::NoIcon, this);

    // "(none)" is index 0 in every list so a folder can leave any axis unset. Leaving one
    // unset is a real answer -- asserting "score" on a folder you haven't listened to is
    // worse than saying nothing, the same reason the measured fields omit rather than
    // guess a middle bucket.
    auto vocabItems = [](const char* const* v, size_t n) {
        juce::StringArray items;
        items.add("(none)");
        for (size_t i = 0; i < n; ++i) items.add(v[i]);
        return items;
    };
    auto addBox = [&](const char* id, const char* const* v, size_t n, const juce::String& label,
                       const juce::String& current) {
        aw->addComboBox(id, vocabItems(v, n), label);
        if (auto* cb = aw->getComboBoxComponent(id))
        {
            // Item ids are 1-based and "(none)" is id 1, so a vocabulary entry is its
            // index + 2. Found by searching rather than by arithmetic on the vocabulary
            // order, so reordering a list can never mis-select a stored tag.
            int id1 = 1;
            for (size_t i = 0; i < n; ++i)
                if (current == v[i]) { id1 = static_cast<int>(i) + 2; break; }
            cb->setSelectedId(id1, juce::dontSendNotification);
        }
    };

    addBox("material", mira::kMaterialVocab, mira::kMaterialVocabCount, "Material", curMaterial);
    addBox("world1", mira::kWorldVocab, mira::kWorldVocabCount, "World", curWorld1);
    addBox("world2", mira::kWorldVocab, mira::kWorldVocabCount, "World (2nd, optional)", curWorld2);
    addBox("harmonic", mira::kHarmonicVocab, mira::kHarmonicVocabCount, "Harmonic language", curHarmonic);
    addBox("signature", mira::kSignatureVocab, mira::kSignatureVocabCount, "Signature", curSignature);

    aw->addButton("Save", 1, juce::KeyPress(juce::KeyPress::returnKey));
    aw->addButton("Clear", 2);
    aw->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    aw->enterModalState(
        true,
        juce::ModalCallbackFunction::create([this, aw, folderPath, extras](int result) {
            if (result == 0) return;
            if (result == 2) {
                database.clearFolderDefault(folderPath);
                return;
            }

            auto chosen = [&aw](const char* box) -> juce::String {
                auto* cb = aw->getComboBoxComponent(box);
                if (cb == nullptr || cb->getSelectedItemIndex() <= 0) return {};
                return cb->getText();
            };

            juce::StringArray words;
            for (const char* box : {"material", "world1", "world2", "harmonic", "signature"})
            {
                auto v = chosen(box);
                // The two world boxes can name the same world; store it once.
                if (v.isNotEmpty() && !words.contains(v)) words.add(v);
            }
            for (const auto& e : extras)
                if (!words.contains(e)) words.add(e);

            // Nothing chosen means "clear", not "write an empty list" -- otherwise a
            // Save on an all-(none) dialog would leave an empty keywords array behind
            // that reads as a tagged folder with no tags.
            if (words.isEmpty()) {
                database.clearFolderDefault(folderPath);
                return;
            }

            juce::String json = "[";
            for (int i = 0; i < words.size(); ++i) {
                if (i) json += ",";
                json += "\"" + words[i].replace("\\", "\\\\").replace("\"", "\\\"") + "\"";
            }
            json += "]";
            database.setFolderDefaultField(folderPath, "$.keywords", json.toStdString());
        }),
        false);
}

void FolderTreeView::showSubfolderContextMenu(const juce::File& folder)
{
    juce::PopupMenu menu;
    menu.addItem("Analyze Folder", [this, folder] {
        if (onAnalyzeFolderRequested) onAnalyzeFolderRequested(folder);
    });
    menu.addItem("Tag Folder...", [this, folder] { promptTagFolder(folder); });
    menu.addItem("Prepare for Training...", [this, folder] {
        if (onPrepareFolderRequested) onPrepareFolderRequested(folder);
    });
    menu.addSeparator();
    // The way to "remove" a subfolder is to add it as a root in its own right and then
    // remove that -- said here rather than left to be discovered, since the absence of a
    // Remove item is exactly what prompted the question.
    menu.addItem("Add as Top-Level Folder...", [this, folder] { promptCategorizeNewFolder(folder); });
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void FolderTreeView::showRootContextMenu(const juce::File& folder)
{
    // "Move to Group..." opens its own popup (promptMoveToGroup) rather than a nested
    // PopupMenu::addSubMenu — it needs a live lookup of the current group and the full
    // group list at click time, which reads more naturally as a second top-level menu.
    juce::PopupMenu menu;
    menu.addItem("Rename...", [this, folder] { promptRenameRoot(folder); });
    menu.addItem("Tag Folder...", [this, folder] { promptTagFolder(folder); });
    menu.addItem("Prepare for Training...", [this, folder] {
        if (onPrepareFolderRequested) onPrepareFolderRequested(folder);
    });
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
