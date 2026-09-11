// TASKS.md Phase 5, build-order step 1: "promote spike/03_dragout into a real target in
// src/CMakeLists.txt, wired to mira's actual SQLite DB, not the spike's toy fixture."
//
// Drag-out mechanics (shouldDropFilesWhenDraggedExternally + DragAndDropContainer) were
// already proven working into Ableton/Logic/Finder in spike/03_dragout (Phase 0 day 4) —
// this is that same foundation, generalized from one hardcoded fixture file to a
// scrollable list backed by mira_core's real Database. Everything else Phase 5's build
// order calls for (LookAndFeel/palette, progress queue, persistent folder tree,
// paintCell-virtualized file table, waveform panel) comes in later steps — this step is
// specifically "prove the real app target + real data source", nothing more.

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_formats/juce_audio_formats.h>

#include "mira/db/Database.h"

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

class DraggableFileRow : public juce::Component
{
public:
    DraggableFileRow(juce::String pathIn, juce::String contentTypeIn)
        : filePath(std::move(pathIn)), contentType(std::move(contentTypeIn))
    {
        setSize(400, 32);
    }

    const juce::String& getFilePath() const { return filePath; }

    void paint(juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced(1.0f);
        g.setColour(isMouseOver() ? juce::Colours::darkslateblue.brighter(0.15f)
                                   : juce::Colours::darkslateblue);
        g.fillRoundedRectangle(bounds, 4.0f);

        g.setColour(juce::Colours::white);
        g.setFont(juce::FontOptions(13.0f));
        auto textBounds = getLocalBounds().reduced(8, 0);
        g.drawFittedText(juce::File(filePath).getFileName(), textBounds.removeFromLeft(280),
                          juce::Justification::centredLeft, 1);
        g.setColour(juce::Colours::lightgrey);
        g.setFont(juce::FontOptions(11.0f));
        g.drawFittedText(contentType, textBounds, juce::Justification::centredRight, 1);
    }

    void mouseEnter(const juce::MouseEvent&) override { repaint(); }
    void mouseExit(const juce::MouseEvent&) override { repaint(); }
    void mouseDown(const juce::MouseEvent&) override {}

    void mouseDrag(const juce::MouseEvent& event) override
    {
        if (event.getDistanceFromDragStart() < 5)
            return;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this))
        {
            if (!container->isDragAndDropActive())
                container->startDragging("mira-file", this, juce::ScaledImage(), true);
        }
    }

private:
    juce::String filePath, contentType;
};

// The drag target: reads which row was dragged from SourceDetails::sourceComponent (not
// a single stored fixture path, the spike's simplification) so any row in the list can be
// dropped as its own real file — the actual generalization this step exists to prove.
class FileListComponent : public juce::Component, public juce::DragAndDropContainer
{
public:
    explicit FileListComponent(mira::Database& databaseIn) : database(databaseIn)
    {
        rebuild();
    }

    void rebuild()
    {
        rows.clear();
        content = std::make_unique<juce::Component>();

        // Every scanned file, newest-scanned first — no filter, no pagination yet
        // (TableListBox at drive scale is a later build-order step). Fine at today's
        // library sizes; revisit alongside the real file table.
        auto files = database.queryFiles("1=1 ORDER BY scanned_at DESC");
        for (const auto& f : files)
        {
            auto* row = new DraggableFileRow(f.path, f.contentType);
            rows.add(row);
            content->addAndMakeVisible(row);
        }
        content->setSize(400, juce::jmax(1, static_cast<int>(rows.size()) * 34));

        viewport.setViewedComponent(content.get(), false);
        addAndMakeVisible(viewport);
        layoutRows();
    }

    void resized() override
    {
        viewport.setBounds(getLocalBounds());
        layoutRows();
    }

    bool shouldDropFilesWhenDraggedExternally(const juce::DragAndDropTarget::SourceDetails& details,
                                               juce::StringArray& files, bool& canMoveFiles) override
    {
        if (details.description.toString() != "mira-file")
            return false;
        if (auto* row = dynamic_cast<DraggableFileRow*>(details.sourceComponent.get()))
        {
            files.add(row->getFilePath());
            canMoveFiles = false; // copy, never move — PRD §1: "no file ever moves"
            return true;
        }
        return false;
    }

private:
    void layoutRows()
    {
        int y = 0;
        for (auto* row : rows)
        {
            row->setBounds(0, y, content->getWidth(), 32);
            y += 34;
        }
    }

    mira::Database& database;
    juce::Viewport viewport;
    std::unique_ptr<juce::Component> content;
    juce::OwnedArray<DraggableFileRow> rows;
};

class MainComponent : public juce::Component
{
public:
    MainComponent()
    {
        auto dbFile = defaultDbFile();
        database = std::make_unique<mira::Database>(dbFile.getFullPathName().toStdString());

        auto count = database->queryFiles("1=1").size();
        statusLabel.setText(juce::String(count) + " file(s) in " + dbFile.getFullPathName()
                                 + (count == 0 ? "  — run `mira scan` first" : ""),
                             juce::dontSendNotification);
        statusLabel.setFont(juce::FontOptions(12.0f));
        addAndMakeVisible(statusLabel);

        fileList = std::make_unique<FileListComponent>(*database);
        addAndMakeVisible(*fileList);

        setSize(440, 500);
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced(12);
        statusLabel.setBounds(bounds.removeFromTop(24));
        bounds.removeFromTop(8);
        fileList->setBounds(bounds);
    }

private:
    std::unique_ptr<mira::Database> database;
    std::unique_ptr<FileListComponent> fileList;
    juce::Label statusLabel;
};

class MainWindow : public juce::DocumentWindow
{
public:
    explicit MainWindow(juce::String name)
        : DocumentWindow(name, juce::Colours::darkgrey, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(), true);
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

    void initialise(const juce::String&) override { mainWindow = std::make_unique<MainWindow>(getApplicationName()); }
    void shutdown() override { mainWindow = nullptr; }

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION(MiraUiApp)
