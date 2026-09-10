// Phase 0, day 4 spike (PRD §9): system-level drag-out to a DAW/Finder.
// A founding requirement (PRD §2d), not a nice-to-have.
//
// A single draggable "sample" tile. Press and drag it out of the window onto
// Ableton, Logic, or Finder — it should drop as a real file, the same as
// dragging from Finder itself. That is what shouldDropFilesWhenDraggedExternally
// plus DragAndDropContainer::startDragging together provide on macOS (NSDraggingItem
// under the hood, per JUCE's juce_Windowing_mac.mm).

#include <juce_gui_extra/juce_gui_extra.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_audio_formats/juce_audio_formats.h>

// Phase 0, day 5 spike (PRD §9): "AudioThumbnailCache survives a relaunch".
// Generates a thumbnail from the fixture WAV, writes the cache to a file, then
// reloads it into a *fresh* cache instance and checks the thumbnail data survives —
// i.e. no re-scan of the audio is needed on the next launch. Run with --selftest
// to execute this headlessly and exit, instead of showing the drag-out window.
static bool runAudioThumbnailCachePersistenceTest()
{
    juce::File fixture (MIRA_FIXTURE_WAV);
    if (!fixture.existsAsFile())
    {
        std::cerr << "FAIL: fixture not found at " << fixture.getFullPathName() << std::endl;
        return false;
    }

    auto cacheFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("mira_thumbnail_cache_spike.dat");
    cacheFile.deleteFile();

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    // --- Pass 1: scan the file into a cache, then persist it to disk -----
    {
        juce::AudioThumbnailCache cache (10);
        juce::AudioThumbnail thumbnail (128, formatManager, cache);
        thumbnail.setSource (new juce::FileInputSource (fixture));

        // Force the background thumbnail scan to complete before saving.
        auto deadline = juce::Time::getMillisecondCounter() + 10000;
        while (thumbnail.isFullyLoaded() == false && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep (20);

        if (!thumbnail.isFullyLoaded())
        {
            std::cerr << "FAIL: thumbnail did not finish loading within 10s" << std::endl;
            return false;
        }

        juce::FileOutputStream out (cacheFile);
        if (!out.openedOk())
        {
            std::cerr << "FAIL: could not open " << cacheFile.getFullPathName() << " for writing" << std::endl;
            return false;
        }
        cache.writeToStream (out);
        std::cout << "pass 1: scanned " << fixture.getFileName()
                   << ", wrote cache (" << cacheFile.getSize() << " bytes) to "
                   << cacheFile.getFullPathName() << std::endl;
    }

    if (!cacheFile.existsAsFile() || cacheFile.getSize() == 0)
    {
        std::cerr << "FAIL: cache file missing or empty after pass 1" << std::endl;
        return false;
    }

    // --- Pass 2: fresh process-equivalent state — new cache, load from disk,
    //     confirm the thumbnail is available with NO new audio scan. ---------
    {
        juce::AudioThumbnailCache cache (10);
        {
            juce::FileInputStream in (cacheFile);
            if (!in.openedOk())
            {
                std::cerr << "FAIL: could not reopen " << cacheFile.getFullPathName() << std::endl;
                return false;
            }
            cache.readFromStream (in);
        }

        juce::AudioThumbnail thumbnail (128, formatManager, cache);
        // Same source identity (same file) as pass 1, so setSource should find cached
        // data (via the cache's stored hash) rather than starting a fresh background scan.
        bool opened = thumbnail.setSource (new juce::FileInputSource (fixture));

        auto deadline = juce::Time::getMillisecondCounter() + 500; // cache hit should be near-instant
        while (thumbnail.isFullyLoaded() == false && juce::Time::getMillisecondCounter() < deadline)
            juce::Thread::sleep (10);

        if (!opened || !thumbnail.isFullyLoaded())
        {
            std::cerr << "FAIL: reloaded cache did not immediately serve a fully-loaded thumbnail "
                       << "(opened=" << (int) opened << ")" << std::endl;
            return false;
        }

        std::cout << "pass 2: fresh AudioThumbnailCache loaded from disk, thumbnail for "
                   << fixture.getFileName() << " fully loaded within 500ms (cache hit, no full re-scan)" << std::endl;
    }

    cacheFile.deleteFile();
    return true;
}

class DraggableSample : public juce::Component
{
public:
    explicit DraggableSample (juce::File fileToOffer)
        : file (std::move (fileToOffer))
    {
        setSize (220, 60);
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat();
        g.setColour (juce::Colours::darkslateblue);
        g.fillRoundedRectangle (bounds, 8.0f);
        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (14.0f));
        g.drawFittedText ("drag me -> Ableton / Logic / Finder\n" + file.getFileName(),
                           getLocalBounds().reduced (8), juce::Justification::centred, 3);
    }

    void mouseDown (const juce::MouseEvent&) override {}

    void mouseDrag (const juce::MouseEvent& event) override
    {
        if (event.getDistanceFromDragStart() < 5)
            return;

        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            if (!container->isDragAndDropActive())
            {
                container->startDragging ("mira-sample", this, juce::ScaledImage(), true);
            }
        }
    }

private:
    juce::File file;
};

class MainComponent : public juce::Component,
                       public juce::DragAndDropContainer
{
public:
    MainComponent()
    {
        juce::File fixture (MIRA_FIXTURE_WAV);

        sample = std::make_unique<DraggableSample> (fixture);
        addAndMakeVisible (*sample);
        setSize (300, 150);

        statusLabel.setText ("fixture: " + fixture.getFullPathName()
                                  + (fixture.existsAsFile() ? "  [found]" : "  [MISSING]"),
                              juce::dontSendNotification);
        statusLabel.setJustificationType (juce::Justification::centred);
        statusLabel.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (statusLabel);

        fixtureFile = fixture;
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced (20);
        statusLabel.setBounds (bounds.removeFromBottom (40));
        sample->setCentrePosition (bounds.getCentre());
    }

    bool shouldDropFilesWhenDraggedExternally (const juce::DragAndDropTarget::SourceDetails& details,
                                                juce::StringArray& files, bool& canMoveFiles) override
    {
        if (details.description.toString() != "mira-sample")
            return false;

        files.add (fixtureFile.getFullPathName());
        canMoveFiles = false; // copy, never move — PRD §1: "no file ever moves"
        return true;
    }

private:
    std::unique_ptr<DraggableSample> sample;
    juce::Label statusLabel;
    juce::File fixtureFile;
};

class MainWindow : public juce::DocumentWindow
{
public:
    MainWindow (juce::String name)
        : DocumentWindow (name, juce::Colours::lightgrey, DocumentWindow::allButtons)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new MainComponent(), true);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        setAlwaysOnTop (true);
    }

    void closeButtonPressed() override
    {
        juce::JUCEApplication::getInstance()->systemRequestedQuit();
    }
};

class SpikeDragoutApp : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "mira drag-out spike"; }
    const juce::String getApplicationVersion() override { return "0.1"; }

    void initialise (const juce::String& commandLine) override
    {
        if (commandLine.contains ("--selftest"))
        {
            bool ok = runAudioThumbnailCachePersistenceTest();
            std::cout << (ok ? "SPIKE OK — AudioThumbnailCache survives a relaunch."
                              : "SPIKE FAILED") << std::endl;
            juce::JUCEApplication::getInstance()->setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        mainWindow = std::make_unique<MainWindow> (getApplicationName());
    }

    void shutdown() override { mainWindow = nullptr; }

private:
    std::unique_ptr<MainWindow> mainWindow;
};

START_JUCE_APPLICATION (SpikeDragoutApp)
