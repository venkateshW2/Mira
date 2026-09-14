#include "PrepareWindow.h"

#include "mira/caption/TagVocabulary.h"

namespace {
constexpr int kRow = 26;
constexpr int kGap = 10;
constexpr int kLabelW = 110;

} // namespace

// Blocks on the child's stdout off the message thread and posts what it reads back.
class PrepareContent::OutputReader : public juce::Thread
{
public:
    OutputReader(PrepareContent& ownerIn, juce::ChildProcess& procIn)
        : juce::Thread("prepare-lora reader"), owner(&ownerIn), proc(procIn) {}

    void run() override
    {
        char chunk[2048];
        for (;;)
        {
            const int n = proc.readProcessOutput(chunk, sizeof(chunk));
            if (n <= 0) break;
            post(juce::String::fromUTF8(chunk, n));
            if (threadShouldExit()) return;
        }
        if (threadShouldExit()) return;

        // waitForProcessToFinish rather than isRunning(): the pipe can close a moment
        // before the exit status is readable, and reporting "exited with code 0" for a
        // failed run would be worse than reporting nothing.
        proc.waitForProcessToFinish(5000);
        const int code = proc.getExitCode();
        auto safe = owner;
        juce::MessageManager::callAsync([safe, code] {
            if (safe.getComponent() != nullptr) safe.getComponent()->onProcessFinished(code);
        });
    }

private:
    void post(const juce::String& text)
    {
        auto safe = owner;
        juce::MessageManager::callAsync([safe, text] {
            if (safe.getComponent() != nullptr) safe.getComponent()->appendLog(text);
        });
    }

    juce::Component::SafePointer<PrepareContent> owner;
    juce::ChildProcess& proc;
};

PrepareContent::PrepareContent(const MiraLookAndFeel& lafIn, juce::File studioRootIn,
                                mira::Database& databaseIn)
    : laf(lafIn), studioRoot(std::move(studioRootIn)), database(databaseIn)
{
    auto caption = [this](juce::Label& l, const juce::String& text) {
        l.setText(text, juce::dontSendNotification);
        l.setFont(laf.sansRegular(12.5f));
        l.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
        addAndMakeVisible(l);
    };
    caption(folderCaption, "Folder");
    caption(triggerCaption, "Trigger");
    caption(tagsCaption, "Tags");
    caption(maxDurCaption, "Max length");
    caption(hostCaption, "GPU host");

    folderValue.setText("(none chosen)", juce::dontSendNotification);
    folderValue.setFont(laf.monoRegular(12.0f));
    folderValue.setColour(juce::Label::textColourId, MiraLookAndFeel::text);
    addAndMakeVisible(folderValue);

    // Read-only: tags are set in the folder tree's Tag Folder dialog, and showing them
    // here is a check that the right folder is selected, not a second place to edit them.
    tagsValue.setText("--", juce::dontSendNotification);
    tagsValue.setFont(laf.monoRegular(12.0f));
    tagsValue.setColour(juce::Label::textColourId, MiraLookAndFeel::accent);
    addAndMakeVisible(tagsValue);

    addAndMakeVisible(chooseButton);
    chooseButton.onClick = [this] { chooseFolder(); };

    triggerField.setFont(laf.monoRegular(13.0f));
    triggerField.setTooltip("Three letters the LoRA learns as its name, e.g. lrt. "
                             "Must be unique per film: zvq xyr qsk vzx dkt are taken.");
    addAndMakeVisible(triggerField);

    maxDurField.setFont(laf.monoRegular(13.0f));
    maxDurField.setText("600", juce::dontSendNotification);
    maxDurField.setTooltip("Seconds. Files longer than this are cropped at encode time -- "
                            "8 of Dune's 38 tracks were truncated by the 600 s default.");
    addAndMakeVisible(maxDurField);

    hostField.setFont(laf.monoRegular(13.0f));
    hostField.setText(loadHost(), juce::dontSendNotification);
    hostField.setTooltip("user@host for the GPU box. Uses your ~/.ssh key -- mira never "
                          "stores a credential. Remembered between sessions.");
    addAndMakeVisible(hostField);

    encodeButton.setTooltip("Caption and encode locally. Stops at the zip; nothing leaves this Mac.");
    pushButton.setTooltip("Encode, then rsync to the GPU host and register the dataset.");
    addAndMakeVisible(encodeButton);
    addAndMakeVisible(pushButton);
    addAndMakeVisible(stopButton);
    encodeButton.onClick = [this] { run(false); };
    pushButton.onClick = [this] { run(true); };
    stopButton.onClick = [this] { stop(); };
    stopButton.setEnabled(false);

    log.setMultiLine(true);
    log.setReadOnly(true);
    log.setScrollbarsShown(true);
    log.setCaretVisible(false);
    log.setFont(laf.monoRegular(11.5f));
    log.setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::surface);
    log.setColour(juce::TextEditor::textColourId, MiraLookAndFeel::textDim);
    addAndMakeVisible(log);
}

PrepareContent::~PrepareContent()
{
    // Killing on close is deliberate: a half-finished encode leaves a latents dir the
    // verify step never checked, and leaving it running behind a closed window would hide
    // that from whoever opens the folder next.
    stop();
}

void PrepareContent::setFolder(const juce::File& f)
{
    folder = f;
    folderValue.setText(f.getFullPathName(), juce::dontSendNotification);
    refreshTagsLabel();
}

void PrepareContent::refreshTagsLabel()
{
    if (!folder.isDirectory()) { tagsValue.setText("--", juce::dontSendNotification); return; }
    auto stored = database.getFolderDefault(folder.getFullPathName().toStdString());
    if (!stored) {
        tagsValue.setText("none set -- right-click the folder -> Tag Folder...",
                           juce::dontSendNotification);
        return;
    }
    juce::StringArray words;
    for (const auto& kw : database.jsonStringArray(*stored, "$.keywords")) words.add(juce::String(kw));
    tagsValue.setText(words.isEmpty() ? "none set" : words.joinIntoString(" \xc2\xb7 "),
                       juce::dontSendNotification);
}

void PrepareContent::chooseFolder()
{
    chooser = std::make_unique<juce::FileChooser>("Choose the audio folder to prepare",
                                                   folder.isDirectory() ? folder : juce::File());
    chooser->launchAsync(juce::FileBrowserComponent::openMode
                             | juce::FileBrowserComponent::canSelectDirectories,
                          [this](const juce::FileChooser& fc) {
                              auto f = fc.getResult();
                              if (f.isDirectory()) setFolder(f);
                          });
}

void PrepareContent::run(bool push)
{
    if (proc != nullptr) return;

    auto script = studioRoot.getChildFile("prepare-lora.sh");
    auto trigger = triggerField.getText().trim();
    auto host = hostField.getText().trim();

    // Fail here rather than let the script fail three steps in with a less obvious
    // message -- every one of these is a thing that has actually gone wrong once.
    if (!folder.isDirectory())      { appendLog("!! choose a folder first\n"); return; }
    if (!script.existsAsFile())     { appendLog("!! prepare-lora.sh not found at " + script.getFullPathName() + "\n"); return; }
    if (trigger.isEmpty())          { appendLog("!! a trigger is required\n"); return; }
    if (push && host.isEmpty())     { appendLog("!! a GPU host is required to push\n"); return; }

    juce::StringArray args;
    args.add("/bin/bash");
    args.add(script.getFullPathName());
    args.add(folder.getFullPathName());
    args.add(trigger);
    args.add(maxDurField.getText().trim().isEmpty() ? "600" : maxDurField.getText().trim());
    if (push) { args.add("--push"); args.add(host); saveHost(host); }

    log.clear();
    appendLog("$ " + args.joinIntoString(" ") + "\n\n");

    proc = std::make_unique<juce::ChildProcess>();
    if (!proc->start(args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        appendLog("!! could not start prepare-lora.sh\n");
        proc.reset();
        return;
    }
    setRunning(true);
    reader = std::make_unique<OutputReader>(*this, *proc);
    reader->startThread();
}

void PrepareContent::stop()
{
    // Kill first: the reader is parked inside a blocking read and only wakes when the
    // pipe closes, which killing the child is what causes.
    if (proc != nullptr) proc->kill();
    if (reader != nullptr) { reader->stopThread(2000); reader.reset(); }
    proc.reset();
    setRunning(false);
}

void PrepareContent::onProcessFinished(int exitCode)
{
    appendLog(exitCode == 0 ? "\n-- done --\n"
                            : "\n!! exited with code " + juce::String(exitCode) + "\n");
    if (reader != nullptr) { reader->stopThread(2000); reader.reset(); }
    proc.reset();
    setRunning(false);
}

void PrepareContent::setRunning(bool running)
{
    encodeButton.setEnabled(!running);
    pushButton.setEnabled(!running);
    chooseButton.setEnabled(!running);
    stopButton.setEnabled(running);
}

void PrepareContent::appendLog(const juce::String& text)
{
    log.moveCaretToEnd();
    log.insertTextAtCaret(text);
}

juce::File PrepareContent::hostSettingsFile() const
{
    return juce::File::getSpecialLocation(juce::File::userHomeDirectory)
        .getChildFile(".mira").getChildFile("gpu-host");
}

juce::String PrepareContent::loadHost() const
{
    auto f = hostSettingsFile();
    return f.existsAsFile() ? f.loadFileAsString().trim() : juce::String();
}

void PrepareContent::saveHost(const juce::String& host) const
{
    // Just the host string. Deliberately NOT a key, a password, or anything ssh itself
    // would not already have -- this file is a convenience, never a credential store.
    auto f = hostSettingsFile();
    f.getParentDirectory().createDirectory();
    f.replaceWithText(host);
}

void PrepareContent::paint(juce::Graphics& g)
{
    g.fillAll(MiraLookAndFeel::bg);
}

void PrepareContent::resized()
{
    auto r = getLocalBounds().reduced(kGap * 2, kGap * 2);

    auto row = [&](juce::Label& cap, juce::Component& field, int fieldW = 0) {
        auto line = r.removeFromTop(kRow);
        cap.setBounds(line.removeFromLeft(kLabelW));
        field.setBounds(fieldW > 0 ? line.removeFromLeft(fieldW) : line);
        r.removeFromTop(kGap);
    };

    auto folderLine = r.removeFromTop(kRow);
    folderCaption.setBounds(folderLine.removeFromLeft(kLabelW));
    chooseButton.setBounds(folderLine.removeFromRight(90));
    folderLine.removeFromRight(kGap);
    folderValue.setBounds(folderLine);
    r.removeFromTop(kGap);

    row(triggerCaption, triggerField, 90);
    row(tagsCaption, tagsValue);
    row(maxDurCaption, maxDurField, 90);
    row(hostCaption, hostField);

    auto buttons = r.removeFromTop(30);
    encodeButton.setBounds(buttons.removeFromLeft(130));
    buttons.removeFromLeft(kGap);
    pushButton.setBounds(buttons.removeFromLeft(140));
    buttons.removeFromLeft(kGap);
    stopButton.setBounds(buttons.removeFromLeft(80));
    r.removeFromTop(kGap * 2);

    log.setBounds(r);
}
