// TASKS.md Phase 5. Build-order step 1 promoted spike/03_dragout into this real app
// target, reading mira_core's real Database instead of one hardcoded fixture file. Step
// 2 applied the real palette/type tokens (MiraLookAndFeel). This file now also carries
// step 4: FileTableComponent below replaces step 1's plain Viewport + stacked
// Components with the real paintCell-only TableListBox (FileTable.h/.cpp) — the
// virtualization that actually matters at drive scale. Progress queue and the
// waveform/detail panel are still open build-order steps.

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "mira/db/Database.h"
#include "MiraLookAndFeel.h"
#include "FileTable.h"

#include <cstdlib>

// Same convention as the CLI's defaultDbPath() (main.cpp) — duplicated rather than
// shared, since mira_ui doesn't link main.cpp (that would pull in the whole CLI's
// argument-parsing/command surface for one function).
static juce::File defaultDbFile()
{
    auto home = std::getenv("HOME");
    juce::File dir = home != nullptr ? juce::File(juce::String(home)).getChildFile(".mira")
                                      : juce::File::getCurrentWorkingDirectory().getChildFile(".mira");
    dir.createDirectory();
    return dir.getChildFile("library.db");
}

// The drag target: the dragged file's path travels as TableListBox's own drag
// description (FileTableModel::getDragSourceDescription), not via SourceDetails::
// sourceComponent — TableListBox owns its row components, so there's no app-defined row
// type to dynamic_cast back to the way step 1's DraggableFileRow allowed.
class FileTableComponent : public juce::Component, public juce::DragAndDropContainer
{
public:
    FileTableComponent(mira::Database& databaseIn, const MiraLookAndFeel& lafIn)
        : model(databaseIn, lafIn)
    {
        table.setModel(&model);
        table.setRowHeight(28);
        table.setHeaderHeight(24);
        table.setMultipleSelectionEnabled(false);
        FileTableModel::setupColumns(table.getHeader());
        addAndMakeVisible(table);
        table.updateContent();
    }

    void resized() override { table.setBounds(getLocalBounds()); }

    bool shouldDropFilesWhenDraggedExternally(const juce::DragAndDropTarget::SourceDetails& details,
                                               juce::StringArray& files, bool& canMoveFiles) override
    {
        auto path = details.description.toString();
        if (path.isEmpty()) return false;
        files.add(path);
        canMoveFiles = false; // copy, never move — PRD §1: "no file ever moves"
        return true;
    }

private:
    FileTableModel model;
    juce::TableListBox table;
};

class MainComponent : public juce::Component
{
public:
    explicit MainComponent(const MiraLookAndFeel& lafIn) : laf(lafIn)
    {
        auto dbFile = defaultDbFile();
        database = std::make_unique<mira::Database>(dbFile.getFullPathName().toStdString());

        auto count = database->queryFiles("1=1").size();
        statusLabel.setText(juce::String(count) + " file(s) in " + dbFile.getFullPathName()
                                 + (count == 0 ? "  — run `mira scan` first" : ""),
                             juce::dontSendNotification);
        statusLabel.setFont(laf.monoRegular(11.5f)); // mockup's .lib-stat: mono, tabular-nums
        statusLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        addAndMakeVisible(statusLabel);

        fileList = std::make_unique<FileTableComponent>(*database, laf);
        addAndMakeVisible(*fileList);

        setSize(600, 520); // wide enough for FileTable's six columns without cramping
    }

    void paint(juce::Graphics& g) override
    {
        MiraLookAndFeel::paintGlassPanel(g, getLocalBounds(), 0.0f, MiraLookAndFeel::surface);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(12);
        statusLabel.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(8);
        fileList->setBounds(bounds);
    }

private:
    const MiraLookAndFeel& laf;
    std::unique_ptr<mira::Database> database;
    std::unique_ptr<FileTableComponent> fileList;
    juce::Label statusLabel;
};

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow(juce::String name, const MiraLookAndFeel& lafIn)
        : DocumentWindow(name, MiraLookAndFeel::surface, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(lafIn), true);
        centreWithSize(getWidth(), getHeight());
        setResizable(true, true);
        setVisible(true);
    }

    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class MiraUiApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "mira"; }
    const juce::String getApplicationVersion() override { return "0.1"; }

    void initialise(const juce::String&) override
    {
        // Declared before mainWindow (member order below) so it outlives every component
        // that reads its colours/fonts; shutdown() below still clears the *default* LAF
        // pointer explicitly before either is torn down, rather than relying on
        // destruction order alone for that part.
        juce::LookAndFeel::setDefaultLookAndFeel(&lookAndFeel);
        mainWindow = std::make_unique<MainWindow>(getApplicationName(), lookAndFeel);
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
    }

private:
    MiraLookAndFeel lookAndFeel;
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(MiraUiApp)
