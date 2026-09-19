#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <map>
#include <vector>

#include "mira/db/Database.h"
#include "MiraLookAndFeel.h"

// Per-root scan state ("scanning is mira's job not the user's job" — TASKS.md Phase 5
// discussion): only roots carry this, not every subfolder, since a scan always runs
// against a whole root recursively. Idle covers both "never scanned yet" and "not
// currently scanning" — genuinely indistinguishable to the sidebar without also
// checking scan_complete in the DB, which paintItem doesn't have open; Complete is only
// set once MainComponent's orchestrator hears the scan actually finished successfully.
enum class FolderRootScanState
{
    Idle,
    Queued,   // waiting behind another root's scan — "can there be like a que and stuff"
    Scanning, // actively being scanned right now (only one root at a time)
    Complete,
    Error,
};

// TASKS.md Phase 5 — sidebar folder tree, third design. First attempt was a single
// live-filesystem tree rooted at Music (rejected: no way back to root/other drives).
// Second attempt used juce::FileTreeComponent, one per added root, each in its own
// fixed-height box (rejected on two counts): (1) FileTreeComponent's DirectoryScanner
// hardcodes `setDirectory(f, true, true)` for every subfolder you expand — it only
// respects the folders-only flag at the root level, so "audio files only, everywhere"
// silently broke one level deep; (2) N independent fixed-height boxes each scrolled on
// their own, not the one continuous list Soundly actually has.
//
// This is a hand-rolled TreeViewItem instead — FolderTreeItem below never asks for
// files at any depth (juce::File::findDirectories only, every level), and every added
// root lives as a sibling under one shared invisible super-root in a single TreeView,
// so the whole sidebar scrolls as one continuous list, exactly like Soundly's.
class FolderTreeView; // owner, declared below

class FolderTreeItem : public juce::TreeViewItem
{
public:
    // displayNameIn overrides folder.getFileName() when set — "so can we rename the
    // folder", a mira-side-only label (Database::FolderRootInfo::displayName); the real
    // path this item scopes to (getFolder()) is never affected by it. Only meaningful
    // when isRootIn is true — a non-root item is a real subfolder discovered by walking
    // disk, nothing to override.
    FolderTreeItem(juce::File folderIn, const MiraLookAndFeel& lafIn, bool isRootIn, FolderTreeView& ownerIn,
                    juce::String displayNameIn = {});

    bool mightContainSubItems() override;
    void itemOpennessChanged(bool isNowOpen) override;
    int getItemHeight() const override;
    void paintItem(juce::Graphics&, int width, int height) override;
    void itemClicked(const juce::MouseEvent&) override;

    const juce::File& getFolder() const { return folder; }
    bool getIsRoot() const { return isRoot; }

private:
    juce::File folder;
    const MiraLookAndFeel& laf;
    bool isRoot;
    juce::String displayName;
    bool childrenLoaded = false;
    // A live reference, not a std::function copy taken at construction time — the
    // std::function version had a real bug: FolderTreeView::rebuildRoots() runs inside
    // its own constructor, before MainComponent gets a chance to assign
    // onFolderSelected, so every item's copy was permanently baked in empty. Reading
    // through the owner instead means clicks always see whatever's currently assigned.
    FolderTreeView& owner;
};

// "can we group folders... a folder but actually a grouping and recall container inside
// mira" — a purely virtual node (Database::FolderGroup): never a real directory, just an
// organizational parent for a set of real added roots, which sit under it as ordinary
// FolderTreeItem children exactly as they would at the top level. Right-click for
// Rename/Delete Group (FolderTreeView.cpp's showGroupContextMenu).
class FolderGroupTreeItem : public juce::TreeViewItem
{
public:
    // category picks paintItem's icon -- "stems folder have icon with it like stems,
    // samples have that" -- one of "stems"/"samples"/"music" (Database::FolderGroup::
    // category) draws a distinct glyph; anything else (a user's own "New Group...") gets
    // the generic two-bar stack icon.
    FolderGroupTreeItem(int64_t groupIdIn, juce::String nameIn, juce::String categoryIn, const MiraLookAndFeel& lafIn,
                         FolderTreeView& ownerIn);

    bool mightContainSubItems() override { return true; }
    int getItemHeight() const override { return 26; }
    void paintItem(juce::Graphics&, int width, int height) override;
    void itemClicked(const juce::MouseEvent&) override;

    int64_t getGroupId() const { return groupId; }

private:
    int64_t groupId;
    juce::String name;
    juce::String category;
    const MiraLookAndFeel& laf;
    FolderTreeView& owner;
};

// A mira-side folder of individual FILES (Database::Collection), as opposed to
// FolderGroupTreeItem's container of folder roots -- "allow me to add files and then i
// can make a folder inside mira and organise it". Like a group it is never a real
// directory, and it has no children in the tree: clicking it scopes the file list to its
// members, which are listed there rather than nested here (a collection can hold files
// from a dozen unrelated folders, so a tree of them would say nothing).
class CollectionTreeItem : public juce::TreeViewItem
{
public:
    CollectionTreeItem(int64_t idIn, juce::String nameIn, int fileCountIn, const MiraLookAndFeel& lafIn,
                        FolderTreeView& ownerIn);

    bool mightContainSubItems() override { return false; }
    int getItemHeight() const override { return 24; }
    void paintItem(juce::Graphics&, int width, int height) override;
    void itemClicked(const juce::MouseEvent&) override;

    int64_t getCollectionId() const { return collectionId; }

private:
    int64_t collectionId;
    juce::String name;
    int fileCount;
    const MiraLookAndFeel& laf;
    FolderTreeView& owner;
};

// Invisible container so the TreeView can show multiple sibling roots/groups at once
// (TreeView itself only ever has one root item) — never painted, never selectable.
class FolderTreeSuperRoot : public juce::TreeViewItem
{
public:
    bool mightContainSubItems() override { return true; }
};

class FolderTreeView : public juce::Component
{
public:
    FolderTreeView(mira::Database& databaseIn, const MiraLookAndFeel& lafIn);
    ~FolderTreeView() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    // Opens the same "Add a folder to mira" picker the sidebar's own "+" button does —
    // public so the real macOS menu bar's File > Add Folder... item (Main.cpp) can
    // trigger the identical flow instead of duplicating it.
    void promptAddFolder() { addFolderClicked(); }

    // MIRA-GENERATE.md Phase 1. A project is a real directory registered as an ordinary
    // folder root under the PROJECTS group (§3.1) -- not a collection, so "deliver the
    // cue folders" is a folder copy and Show in Finder points at something real. Both
    // flows end in the same registerProject() below; New also creates the directory.
    void promptNewProject();
    void promptOpenProject();

    // Fires once a project has been created or opened, with its folder. MainComponent
    // stores it as the current project (window title, and the generate window's output
    // folder once Phase 2 lands) -- this class knows how to make one, not what being
    // "current" means.
    // The folder, and the .mira document inside it when Open chose one (an invalid File
    // for New Project, which has no document yet).
    std::function<void(const juce::File&, const juce::File&)> onProjectOpened;

    // Fires with every click on a folder row (left-click; right-click still opens the
    // "Scan this folder..." menu independently) — TASKS.md Phase 5 discussion: clicking
    // a folder shows everything under it, recursively; clicking a subfolder narrows
    // further — "more like a filter" than a separate browse mode. No "Show All" button:
    // clicking any root already shows everything under that root, and a second,
    // un-scoped "everything across every root" view was judged confusing, not useful.
    std::function<void(const juce::File&)> onFolderSelected;

    // Fires once right after a new root's been added (after the DB insert and the tree
    // rebuild) — MainComponent uses this to kick off that root's very first scan
    // automatically, rather than requiring a separate manual "Scan" click.
    std::function<void(const juce::File&)> onFolderAdded;

    // "right click folder and choose to analyse the folder so all the files go into
    // analysis" — MainComponent walks the folder for audio files and enqueues them; this
    // class has no analyze-queue concept of its own.
    std::function<void(const juce::File&)> onAnalyzeFolderRequested;
    // Opens the Prepare for Training window aimed at this folder (PrepareWindow.h).
    std::function<void(const juce::File&)> onPrepareFolderRequested;

    // Live per-root scan status, driven by MainComponent's scan orchestrator — updates
    // the little badge paintItem draws next to a root's name and repaints the tree.
    void setRootScanState(const juce::String& path, FolderRootScanState state, int64_t filesSeen = 0);
    FolderRootScanState getRootScanState(const juce::String& path) const;
    int64_t getRootScanFilesSeen(const juce::String& path) const;

    // Right-click menu actions, called by FolderTreeItem/FolderGroupTreeItem — public so
    // those TreeViewItems (which don't own any state themselves) can reach them via
    // `owner`, same pattern onFolderSelected already uses.
    void showRootContextMenu(const juce::File& folder);
    // CAPTION-TAGGING.md: the four things no analyzer can measure, chosen once per folder
    // and applied to every file under it. Vocabularies come from caption/TagVocabulary.h,
    // shared with `mira tag-folder` so the two can never offer different words.
    void promptTagFolder(const juce::File& folder);
    void showSubfolderContextMenu(const juce::File& folder);
    void showGroupContextMenu(int64_t groupId, const juce::String& currentName);
    void showCollectionContextMenu(int64_t collectionId, const juce::String& currentName);

    // Opens the "Add files to mira" picker -- multi-select, audio only. Public for the
    // same reason promptAddFolder is: File > Add Files... drives the identical flow.
    void promptAddFiles();

    // Fires when a collection row is clicked, with the collection's id. MainComponent
    // scopes the file list to it (FileTableModel's collection scope).
    std::function<void(int64_t)> onCollectionSelected;
    // Fires after files are picked, so MainComponent can scan them (it owns the scan
    // queue) and then call back to file them into a collection.
    std::function<void(std::vector<juce::String>, int64_t collectionId)> onFilesAdded;
    // Rebuilt from the database -- called by MainComponent once the files it scanned are
    // actually in the library and filed.
    void refresh() { rebuildRoots(); }

private:
    void rebuildRoots();
    void addFolderClicked();
    void promptCategorizeNewFolder(const juce::File& folder);
    void promptRecategorizeRoot(const juce::File& folder);
    int64_t findOrCreateCategoryGroup(const char* category, const char* displayName);
    // Adds `folder` as a root under the PROJECTS group and announces it. Shared by New
    // and Open so a reopened project is filed exactly like a freshly created one --
    // addFolderRoot is idempotent, so opening a project already in the library is a
    // no-op plus a regrouping, never a duplicate row.
    void registerProject(const juce::File& folder, const juce::File& document = {});
    void newGroupClicked();
    void promptRenameRoot(const juce::File& folder);
    void promptMoveToGroup(const juce::File& folder);
    void promptRemoveRoot(const juce::File& folder);
    void promptRenameCollection(int64_t collectionId, const juce::String& currentName);
    void promptDeleteCollection(int64_t collectionId, const juce::String& currentName);
    void promptForCollectionThen(std::vector<juce::String> paths);

    mira::Database& database;
    const MiraLookAndFeel& laf;
    juce::TextButton addButton;
    juce::TreeView tree;
    FolderTreeSuperRoot superRoot;
    std::unique_ptr<juce::FileChooser> folderChooser; // must outlive the async picker call
    bool empty = true;
    std::map<juce::String, std::pair<FolderRootScanState, int64_t>> rootScanState;
};
