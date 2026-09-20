#include "CanvasWindow.h"
#include "VideoWindow.h"
#include <iostream>
#include "mira/db/PathNormalise.h"
#include "NativeWindowChrome.h"

// The picture track's colour, and the reference track's with it. Deliberately NOT one of
// laneColour's eight: every track colour in this canvas is a desaturated mid-tone, because
// eight of them have to sit side by side without any one shouting. A saturated violet
// belongs to none of that family, so the two rows that came from a FILM read as a different
// kind of thing before the words PICTURE and REFERENCE have been read at all.
static const juce::Colour kPictureColour { 0xff9b6fd8 };

namespace mira::canvas {

// A generator with nothing in it. Not the same as "no settings": no settings means we have
// not looked yet and something else might know, and an empty recipe means there is nothing
// to know -- which is what a new block is.
// What a block IS, in two words, read off its own prompt. SA3 prompts are
// "Key: value, Key: value" so the facts are already in there -- "Keyscale: C minor,
// BPM: 64" -- and a block that says "C minor . 64" in its corner is one you can arrange
// against without opening anything.
//
// From the PROMPT rather than from mira's analysis on purpose: the prompt is what the
// block was asked for, it is there before a single sample exists, and an empty block can
// carry it. Analysis of the audio is the better answer once there IS audio, and it can
// replace this later without moving anything.
static juce::String promptField (const juce::String& prompt, const juce::String& key)
{
    const int at = prompt.indexOfIgnoreCase (key + ":");
    if (at < 0) return {};
    auto rest = prompt.substring (at + key.length() + 1);
    const int end = rest.indexOfChar (',');
    return (end >= 0 ? rest.substring (0, end) : rest).trim();
}

static juce::String keyAndTempoOf (const juce::var& settings)
{
    if (!settings.isObject()) return {};
    const auto prompt = settings.getProperty ("prompt", "").toString();
    if (prompt.isEmpty()) return {};
    const auto key = promptField (prompt, "Keyscale");
    const auto bpm = promptField (prompt, "BPM");
    if (key.isEmpty() && bpm.isEmpty()) return {};
    if (key.isEmpty()) return bpm + " bpm";
    if (bpm.isEmpty()) return key;
    return key + juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  ")) + bpm;
}

// The same line, from the BLOCK rather than from the prompt: "A minor  .  95". This is
// what the header shows once a block has a tempo of its own, and below the height a grid
// footer needs it is the whole of the summary.
static juce::String musicLabelOf (const mira::canvas::Block& b)
{
    juce::String tempo;
    if (b.tempo > 0.0)
        tempo = (std::abs(b.tempo - std::round(b.tempo)) < 0.05
                     ? juce::String((int) std::round(b.tempo))
                     : juce::String(b.tempo, 1)) + " bpm";
    if (b.key.isEmpty()) return tempo;
    if (tempo.isEmpty()) return b.key;
    return b.key + juce::String (juce::CharPointer_UTF8 ("  \xc2\xb7  ")) + tempo;
}

static juce::var emptyRecipe()
{
    auto* o = new juce::DynamicObject();
    o->setProperty("prompt", "");
    o->setProperty("negative_prompt", "");
    o->setProperty("loras", juce::var(juce::Array<juce::var>{}));
    return juce::var(o);
}


namespace {
juce::String formatTime(double seconds)
{
    if (seconds < 0.0) seconds = 0.0;
    const int total = static_cast<int>(seconds);
    return juce::String(total / 60) + ":" + juce::String(total % 60).paddedLeft('0', 2);
}
} // namespace

CanvasView::CanvasView(const MiraLookAndFeel& lafIn, juce::AudioFormatManager& formatsIn,
                       juce::AudioThumbnailCache& cacheIn)
    : laf(lafIn), formats(formatsIn), cache(cacheIn)
{
    setWantsKeyboardFocus(true);
    startTimerHz(30);
}

CanvasView::~CanvasView() { player.detach(); }

void CanvasView::setProject(const juce::File& project)
{
    commitRename();
    if (!project.isDirectory()) return;

    // Opening a FOLDER is the legacy path and stays only so an existing canvas.json is not
    // stranded: the first .mira in the folder wins, otherwise a canvas.json is read and
    // written back out as one. New work goes through New/Open.
    auto found = project.findChildFiles(juce::File::findFiles, false, juce::String("*") + kExtension);
    if (!found.isEmpty()) { openDocument(found[0]); return; }

    const auto legacy = project.getChildFile("canvas.json");
    if (legacy.existsAsFile())
    {
        projectFolder = project;
        readFrom(legacy);
        documentFile = project.getChildFile(project.getFileName() + kExtension);
        writeTo(documentFile);
        legacy.moveToTrash();
        dirty = false;
        if (onDocumentChanged) onDocumentChanged();
        return;
    }

    // A REAL FOLDER WITH NO DOCUMENT IN IT YET. It still becomes the project: a block has
    // to have somewhere to put its audio, and without a project folder blockFolderFor
    // returned nothing, pointPanelAt gave up silently, and the panel sat there saying "no
    // block selected" with Generate greyed out over a block that was plainly selected.
    projectFolder = project;
    documentFile = project.getChildFile(project.getFileName() + kExtension);
    writeTo(documentFile);
    dirty = false;
    if (onDocumentChanged) onDocumentChanged();
}

double CanvasView::contentEnd() const
{
    double end = 0.0;
    for (const auto& i : items) end = juce::jmax(end, i->block.end());
    return end;
}

void CanvasView::fit()
{
    const double end = contentEnd();
    const int w = juce::jmax(200, getWidth() - kHeaderWidth + 16);
    viewStart = 0.0;
    pixelsPerSecond = end > 0.0 ? juce::jlimit(0.5, 400.0, w / end) : 40.0;
    repaint();
}

void CanvasView::rebuildAudio()
{
    std::vector<Block> blocks;
    blocks.reserve(items.size());
    for (const auto& i : items) blocks.push_back(i->block);
    player.rebuild(blocks, formats);
    if (onStateChanged) onStateChanged();
}

// The header is a mixer strip: the name across the top, M/S and the fader under it, the
// meter down the right edge. The name used to start at x=60 with nothing to its left,
// because it shared a row with chips that were vertically centred somewhere else -- so it
// read as floating rather than as a title, and the rename box landed in the same odd spot.
// M and S sit to the RIGHT of the fader column, not above it. Stacked above, they ate the
// vertical space the fader needs -- and vertical space is the only thing a fader has.
juce::Rectangle<int> CanvasView::muteBoxFor(int lane) const
{
    return laneHeightOf(lane) >= 46
               ? juce::Rectangle<int>(kHeaderWidth - 62, laneToY(lane) + 24, 26, 18)
               : juce::Rectangle<int>(kHeaderWidth - 74, laneToY(lane) + laneHeightOf(lane) / 2 - 9, 22, 18);
}

juce::Rectangle<int> CanvasView::soloBoxFor(int lane) const
{
    return laneHeightOf(lane) >= 46
               ? juce::Rectangle<int>(kHeaderWidth - 32, laneToY(lane) + 24, 26, 18)
               : juce::Rectangle<int>(kHeaderWidth - 48, laneToY(lane) + laneHeightOf(lane) / 2 - 9, 22, 18);
}

// One warped scale, shared by the fader and the meter. Linear-in-dB spends half the
// travel between -60 and -30, where nothing you care about happens; a console spends it at
// the top. Both controls use this, which is the whole point -- a fader at -12 lines up
// with a meter reading -12.
double CanvasView::dbToNorm(double db)
{
    db = juce::jlimit(kFaderBottomDb, kFaderTopDb, db);
    if (db >= -12.0) return 0.55 + 0.45 * (db + 12.0) / 18.0;    // +6..-12 over the top 45%
    if (db >= -30.0) return 0.25 + 0.30 * (db + 30.0) / 18.0;    // -12..-30 over 30%
    return 0.25 * (db + 60.0) / 30.0;                             // -30..-60 over 25%
}

double CanvasView::normToDb(double n)
{
    n = juce::jlimit(0.0, 1.0, n);
    if (n >= 0.55) return -12.0 + (n - 0.55) / 0.45 * 18.0;
    if (n >= 0.25) return -30.0 + (n - 0.25) / 0.30 * 18.0;
    return -60.0 + n / 0.25 * 30.0;
}

// ONE CONTROL, the way a DAW mixer has it: the meter and the fader are a single tall
// widget on one scale, not two things side by side that happen to line up. The cap spans
// the whole width, so it reads as the handle of the thing the meter is part of.
juce::Rectangle<int> CanvasView::stripBoxFor(int lane) const
{
    if (laneHeightOf(lane) < 46) return {};
    const int top = laneToY(lane) + 22;
    return { 10, top, 30, juce::jmax(16, laneHeightOf(lane) - 30) };
}

juce::Rectangle<int> CanvasView::faderBoxFor(int lane) const
{
    auto strip = stripBoxFor(lane);
    if (strip.isEmpty()) return {};
    return { strip.getX() + 15, strip.getY(), 13, strip.getHeight() };
}

// Muted and deliberately NOT the accent: the accent means "selected" and "playing"
// everywhere else in mira, so a column of amber faders made every track look active at
// once. That is most of what "the yellow fader kills the ui" was.
juce::Colour CanvasView::laneColour(int lane)
{
    static const juce::Colour palette[] = {
        juce::Colour(0xff6f9bd1),   // slate blue
        juce::Colour(0xffc98a7a),   // clay
        juce::Colour(0xff7fae8c),   // sage
        juce::Colour(0xffb79bd0),   // lilac
        juce::Colour(0xffd0b06a),   // brass
        juce::Colour(0xff6fb0b5),   // teal
        juce::Colour(0xffd08f9f),   // rose
        juce::Colour(0xff9aa8c4)    // steel
    };
    return palette[(size_t) juce::jmax(0, lane) % (sizeof(palette) / sizeof(palette[0]))];
}

// The meter half of the same widget: two stereo bars down its left side. A mono meter
// cannot show the fault a meter exists to catch -- a take with a dead side.
juce::Rectangle<int> CanvasView::meterBoxFor(int lane) const
{
    if (laneHeightOf(lane) < 46)
        return { kHeaderWidth - 16, laneToY(lane) + 4, 9, juce::jmax(10, laneHeightOf(lane) - 8) };
    auto strip = stripBoxFor(lane);
    return { strip.getX() + 2, strip.getY(), 11, strip.getHeight() };
}

juce::Rectangle<int> CanvasView::nameBoxFor(int lane) const
{
    // The name stops short of the padlock rather than running under it.
    return laneHeightOf(lane) >= 46
               ? juce::Rectangle<int>(8, laneToY(lane) + 2, kHeaderWidth - 36, 18)
               : juce::Rectangle<int>(8, laneToY(lane), kHeaderWidth - 84, laneHeightOf(lane));
}

// RENAMING A BLOCK RENAMES ITS FOLDER. The name is not a label: blockFolderFor builds the
// take folder out of it, so a block called "block 3" keeps its audio in "<project>/block 3"
// and the generator writes there. Rename the block without moving the folder and the next
// generation goes somewhere new while the takes it already has stay behind under the old
// name -- which is the split-brain the folder-is-the-name rule exists to prevent.
//
// So the folder moves with it, and if it cannot, the rename does not happen.
// ---- export ---------------------------------------------------------------------------
//
// WHAT YOU HEAR, NOT WHAT IS ON DISK. A block's take knows nothing about the trim, the
// fades, the gain, the cut, or the crossfade with the block overlapping it -- all of that
// lives on the canvas. Handing over the take file would hand over something that is not
// the piece.
//
// So export renders through the PLAYER, at the timeline's 44,100: one mixer for the
// speakers and the file, which is the only way the two cannot drift apart. A track is a
// solo, not a second filter -- the lane masks already exist and already work.
juce::String CanvasView::exportNameFor(const Visual* v, const juce::String& suffix) const
{
    // Name, key and tempo, with a missing field dropping its token rather than guessing
    // one (convention 1, and the same rule Export.h follows for takes).
    juce::String name = documentFile != juce::File() ? documentFile.getFileNameWithoutExtension()
                                                     : juce::String("canvas");
    if (v != nullptr && v->block.name.isNotEmpty()) name += "_" + v->block.name;
    if (suffix.isNotEmpty()) name += "_" + suffix;
    if (v != nullptr)
    {
        const auto field = promptField(v->settings, "Keyscale");
        const auto bpm   = promptField(v->settings, "BPM");
        if (field.isNotEmpty()) name += "_" + field.replace(" ", "").replace("#", "s");
        if (bpm.isNotEmpty())   name += "_" + bpm + "bpm";
    }
    return juce::File::createLegalFileName(name);
}

bool CanvasView::renderToFile(const juce::File& dest, int lane, double fromSeconds,
                              double toSeconds, juce::String& errorOut)
{
    const double rate = CanvasPlayer::getTimelineRate();
    const auto from = (juce::int64) std::llround(juce::jmax(0.0, fromSeconds) * rate);
    const auto to   = (juce::int64) std::llround(juce::jmax(fromSeconds, toSeconds) * rate);
    const auto total = to - from;
    if (total <= 0) { errorOut = "nothing to export"; return false; }

    // One lane means SOLO that lane, using the mask the mixer already honours. Restored in
    // every exit path below -- leaving a solo latched after an export would silence the
    // canvas and look like a playback bug.
    const auto savedMute = muteMask, savedSolo = soloMask;
    if (lane >= 0 && lane < CanvasAudioSource::kMaxLanes)
        player.setLaneMasks(muteMask, juce::uint64(1) << lane);

    struct Restore {
        CanvasPlayer& p; juce::uint64 m, s;
        ~Restore() { p.setLaneMasks(m, s); }
    } restore { player, savedMute, savedSolo };

    dest.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream (dest.createOutputStream());
    if (stream == nullptr) { errorOut = "could not write " + dest.getFullPathName(); return false; }
    std::unique_ptr<juce::AudioFormatWriter> writer (wav.createWriterFor(stream.get(), rate, 2, 24, {}, 0));
    if (writer == nullptr) { errorOut = "could not create a writer"; return false; }
    stream.release();

    juce::AudioBuffer<float> buffer (2, 8192);
    for (juce::int64 done = 0; done < total;)
    {
        const int n = (int) juce::jmin<juce::int64>(buffer.getNumSamples(), total - done);
        buffer.clear();
        player.renderOffline(buffer, from + done, n);
        if (!writer->writeFromAudioSampleBuffer(buffer, 0, n)) { errorOut = "write failed"; return false; }
        done += n;
    }
    return true;
}

void CanvasView::promptExport(int what, juce::int64 id)
{
    const Visual* v = nullptr;
    for (const auto& i : items) if (i->block.id == id) { v = i.get(); break; }
    if (what == 0 && v == nullptr) return;

    // Exporting while the transport runs would have two things pulling on one mixer.
    if (player.isPlaying()) togglePlay();

    const auto suggested = documentFile != juce::File()
                               ? documentFile.getParentDirectory()
                               : juce::File::getSpecialLocation(juce::File::userMusicDirectory);
    auto chooser = std::make_shared<juce::FileChooser>(
        what == 2 ? "Export every track into a folder" : "Export to", suggested,
        what == 2 ? juce::String() : juce::String("*.wav"));
    const int flags = what == 2
                          ? (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectDirectories)
                          : (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting);

    juce::Component::SafePointer<CanvasView> safe (this);
    chooser->launchAsync(flags, [safe, chooser, what, id](const juce::FileChooser& fc) {
        if (safe == nullptr) return;
        auto& self = *safe;
        const auto picked = fc.getResult();
        if (picked == juce::File()) return;

        const Visual* v = nullptr;
        for (const auto& i : self.items) if (i->block.id == id) { v = i.get(); break; }

        juce::String error;
        juce::StringArray written;
        // PHASE 2.3. The reference is the film's own audio -- dialogue, effects, whatever
        // the cut arrived with -- and it is not yours to deliver. Refused at the two
        // targeted exports and skipped by the one that walks every lane.
        if ((what == 0 || what == 1) && v != nullptr && self.isReferenceLane(v->block.lane))
        {
            if (self.onTakeNote)
                self.onTakeNote("the reference track is the film's audio - it is never exported");
            return;
        }
        int excluded = 0;
        if (what == 0 && v != nullptr)
        {
            // The block alone, over its own span -- not the whole timeline with silence
            // either side of it.
            if (self.renderToFile(picked.withFileExtension("wav"), v->block.lane,
                                  v->block.start, v->block.end(), error))
                written.add(picked.getFileName());
        }
        else if (what == 1 && v != nullptr)
        {
            if (self.renderToFile(picked.withFileExtension("wav"), v->block.lane,
                                  0.0, self.contentEnd(), error))
                written.add(picked.getFileName());
        }
        else if (what == 2)
        {
            auto folder = picked.existsAsFile() ? picked.getParentDirectory() : picked;
            folder.createDirectory();
            const double end = self.contentEnd();
            for (int lane = 0; lane < self.laneCount && error.isEmpty(); ++lane)
            {
                if (self.isReferenceLane(lane)) { ++excluded; continue; }
                // Lanes with nothing on them are not silent files nobody asked for.
                const Visual* first = nullptr;
                for (const auto& i : self.items)
                    if (i->block.lane == lane && i->block.hasAudio()) { first = i.get(); break; }
                if (first == nullptr) continue;
                const auto name = self.exportNameFor(first, self.laneNames[lane].isNotEmpty()
                                                                ? self.laneNames[lane]
                                                                : "track" + juce::String(lane + 1));
                if (self.renderToFile(folder.getChildFile(name + ".wav"), lane, 0.0, end, error))
                    written.add(name + ".wav");
            }
        }

        if (self.onTakeNote)
            self.onTakeNote(error.isNotEmpty()
                                ? "export failed: " + error
                                : "exported " + juce::String(written.size()) + " file"
                                      + (written.size() == 1 ? "" : "s") + " - "
                                      + written.joinIntoString(", ")
                                      + (excluded > 0 ? "   (reference track excluded)"
                                                      : juce::String()));
    });
}

// Every wav in the block's own folder, newest first. The folder IS the take list -- there
// is no index to keep in step with it, so a take dropped in or removed from outside mira
// is simply seen next time.
juce::Array<juce::File> CanvasView::takesOf(const Visual& v) const
{
    juce::Array<juce::File> out;
    const auto folder = blockFolderFor(v);
    if (!folder.isDirectory()) return out;
    out = folder.findChildFiles(juce::File::findFiles, false, "*.wav");
    std::sort(out.begin(), out.end(), [](const juce::File& a, const juce::File& b) {
        return a.getLastModificationTime() > b.getLastModificationTime();
    });
    return out;
}

// 100+i shows take i; 200+i trashes it. One menu, two verbs.
void CanvasView::chooseTake(juce::int64 blockId, int menuId)
{
    Visual* v = nullptr;
    for (auto& i : items) if (i->block.id == blockId) { v = i.get(); break; }
    if (v == nullptr) return;

    const auto takes = takesOf(*v);
    const bool trash = menuId >= 200;
    const int index = menuId - (trash ? 200 : 100);
    if (!juce::isPositiveAndBelow(index, takes.size())) return;
    const auto file = takes[index];

    if (!trash)
    {
        // Switching take is an EDIT, so it undoes. setFileOn clears contentSeconds -- a
        // cut made against the old take described where a DIFFERENT file went quiet.
        pushUndo();
        setFileOn(*v, file);
        rebuildAudio();
        markDirty();
        if (panelBlockId == blockId) pointPanelAt(v);
        announceSelection();
        repaint();
        return;
    }

    // Trashing the take a block is SHOWING would leave it pointing at nothing. Move to the
    // next one down the list first, so the block always holds something real.
    const bool isCurrent = v->block.hasAudio()
        && mira::pathsEquivalent(file.getFullPathName().toStdString(),
                                  v->block.file.getFullPathName().toStdString());

    juce::Component::SafePointer<CanvasView> safe (this);
    juce::AlertWindow::showOkCancelBox(
        juce::AlertWindow::QuestionIcon, "Move take to Trash",
        file.getFileName() + (isCurrent ? "\n\nThis is the take the block is showing."
                                        : juce::String())
            + "\n\nIt goes to the Trash, not away for good.",
        "Move to Trash", "Cancel", nullptr,
        juce::ModalCallbackFunction::create([safe, blockId, file, isCurrent](int result) {
            if (safe == nullptr || result == 0) return;
            auto& self = *safe;
            Visual* target = nullptr;
            for (auto& i : self.items) if (i->block.id == blockId) { target = i.get(); break; }
            if (target == nullptr) return;

            self.pushUndo();
            if (isCurrent)
            {
                juce::File next;
                for (const auto& t : self.takesOf(*target))
                    if (!mira::pathsEquivalent(t.getFullPathName().toStdString(),
                                               file.getFullPathName().toStdString()))
                        { next = t; break; }
                // No other take means the block goes back to being an empty frame, which
                // is a real state it already knows how to be -- not an error.
                if (next != juce::File()) self.setFileOn(*target, next);
                else { target->block.file = juce::File(); target->thumb.reset(); target->audioSeconds = 0.0; }
            }
            file.withFileExtension("json").moveToTrash();   // the recipe goes with its audio
            const bool gone = file.moveToTrash();
            self.rebuildAudio();
            self.markDirty();
            if (self.panelBlockId == blockId) self.pointPanelAt(target);
            self.announceSelection();
            self.repaint();
            if (self.onTakeNote)
                self.onTakeNote(gone ? "moved " + file.getFileName() + " to the Trash"
                                     : "could not move " + file.getFileName() + " to the Trash");
        }));
}

// ---- cleanup --------------------------------------------------------------------------
//
// A block keeps every take it ever generated -- that is deliberate, it is what makes
// "go back to the one before" possible at all. But nine takes in ten are never used, and
// at ~10 MB a minute a project fills a drive quickly.
//
// USED means "a block is pointing at it", nothing cleverer. Anything else is a take you
// tried and moved on from. Deliberately conservative in two ways: it only ever looks
// inside folders that belong to blocks ON THIS CANVAS, so a folder mira does not
// recognise is never touched; and it goes to the TRASH, not to oblivion, because a
// judgement about which audio you still want is not one a program should make final.
void CanvasView::promptCleanup()
{
    if (!projectFolder.isDirectory())
    {
        if (onTakeNote) onTakeNote("save the canvas first - there is no project folder to clean");
        return;
    }

    juce::StringArray keep;
    juce::Array<juce::File> doomed;
    juce::int64 bytes = 0;
    for (const auto& i : items)
        if (i->block.hasAudio()) keep.add(i->block.file.getFullPathName());

    for (const auto& i : items)
    {
        const auto folder = blockFolderFor(*i);
        if (!folder.isDirectory()) continue;
        for (const auto& f : folder.findChildFiles(juce::File::findFiles, false, "*.wav"))
        {
            // Convention 9: never compare paths with ==. A block named with an accent in
            // it gives one byte sequence from the document and another from the directory
            // walk, and the take would read as unused and be deleted.
            bool used = false;
            for (const auto& k : keep)
                if (mira::pathsEquivalent(k.toStdString(), f.getFullPathName().toStdString()))
                    { used = true; break; }
            if (used) continue;
            if (doomed.contains(f)) continue;
            doomed.add(f);
            bytes += f.getSize();
        }
    }

    if (doomed.isEmpty())
    {
        if (onTakeNote) onTakeNote("nothing to clean up - every take in this project is in use");
        return;
    }

    const auto mb = juce::String(bytes / (1024.0 * 1024.0), 1);
    juce::Component::SafePointer<CanvasView> safe (this);
    juce::AlertWindow::showOkCancelBox(
        juce::AlertWindow::QuestionIcon, "Clean up unused takes",
        juce::String(doomed.size()) + " take" + (doomed.size() == 1 ? "" : "s")
            + " (" + mb + " MB) are not on the canvas.\n\nThey go to the Trash, not away for good.",
        "Move to Trash", "Cancel", nullptr,
        juce::ModalCallbackFunction::create([safe, doomed](int result) {
            if (safe == nullptr || result == 0) return;
            int gone = 0;
            for (const auto& f : doomed)
            {
                // The sidecar goes with its audio, or the folder fills with recipes for
                // takes that no longer exist.
                f.withFileExtension("json").moveToTrash();
                if (f.moveToTrash()) ++gone;
            }
            if (safe->onTakeNote)
                safe->onTakeNote("moved " + juce::String(gone) + " unused take"
                                 + (gone == 1 ? "" : "s") + " to the Trash");
        }));
}

void CanvasView::beginRenameBlock(juce::int64 id)
{
    commitBlockRename();
    Visual* v = nullptr;
    for (auto& i : items) if (i->block.id == id) { v = i.get(); break; }
    if (v == nullptr) return;

    const auto r = boundsOf(*v);
    if (r.getHeight() < 46) return;
    renamingBlock = id;
    blockRenameEditor = std::make_unique<juce::TextEditor>();
    blockRenameEditor->setText(v->block.name, juce::dontSendNotification);
    blockRenameEditor->setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
    auto box = r.reduced(6, 2).removeFromTop(14);
    if (const auto gb = blockGainBox(*v); !gb.isEmpty())
        box = box.withTrimmedLeft(gb.getRight() - r.getX() - 2);
    blockRenameEditor->setBounds(box.withWidth(juce::jmin(220, box.getWidth())));
    blockRenameEditor->setBorder(juce::BorderSize<int>(0));
    blockRenameEditor->setIndents(2, 1);
    blockRenameEditor->setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::surface.darker(0.2f));
    blockRenameEditor->setColour(juce::TextEditor::outlineColourId, MiraLookAndFeel::accent);
    blockRenameEditor->setColour(juce::TextEditor::textColourId, MiraLookAndFeel::text);
    blockRenameEditor->onReturnKey  = [this] { commitBlockRename(); };
    blockRenameEditor->onEscapeKey  = [this] { renamingBlock = 0; blockRenameEditor.reset(); repaint(); };
    blockRenameEditor->onFocusLost  = [this] { commitBlockRename(); };
    addAndMakeVisible(*blockRenameEditor);
    blockRenameEditor->selectAll();
    blockRenameEditor->grabKeyboardFocus();
}

void CanvasView::commitBlockRename()
{
    if (blockRenameEditor == nullptr || renamingBlock == 0) { renamingBlock = 0; return; }
    const auto wanted = blockRenameEditor->getText().trim();
    const auto id = renamingBlock;
    renamingBlock = 0;
    blockRenameEditor.reset();

    Visual* v = nullptr;
    for (auto& i : items) if (i->block.id == id) { v = i.get(); break; }
    if (v == nullptr || wanted.isEmpty() || wanted == v->block.name) { repaint(); return; }

    // A name that is not a usable folder name is not a usable block name.
    const auto legal = juce::File::createLegalFileName(wanted);
    if (legal.isEmpty()) { if (onTakeNote) onTakeNote("that name cannot be a folder"); repaint(); return; }
    for (const auto& i : items)
        if (i->block.id != id && i->block.name.equalsIgnoreCase(legal))
        {
            if (onTakeNote) onTakeNote("there is already a block called " + legal);
            repaint();
            return;
        }

    pushUndo();
    const auto from = blockFolderFor(*v);
    v->block.name = legal;
    const auto to = blockFolderFor(*v);
    if (from.isDirectory() && from != to)
    {
        if (from.moveFileTo(to))
        {
            // The block's file lived under the old folder, so it has to be re-pointed or
            // the block is left holding a path that no longer exists.
            if (v->block.hasAudio())
                v->block.file = to.getChildFile(v->block.file.getFileName());
        }
        else
        {
            v->block.name = from.getFileName();   // put it back rather than split the two
            if (onTakeNote) onTakeNote("could not rename the folder - the block keeps its name");
            repaint();
            return;
        }
    }
    // The panel points at this block by id, so it has to be told the folder moved.
    if (panelBlockId == id) pointPanelAt(v);
    rebuildAudio();
    markDirty();
    repaint();
}

// THE GENERATION HAPPENS ON THIS BLOCK, so the progress belongs on it -- not in the side
// panel's status line, which is the far side of the window from the thing being filled in
// and invisible entirely when the panel is folded away.
//
// Taller and inset rather than a hairline at the very bottom edge: at three pixels under
// the block's own border it was easy to miss even when it did draw.
void CanvasView::paintGenerationStrip(juce::Graphics& g, const Visual& v,
                                      juce::Rectangle<int> r) const
{
    if (genFraction < 0.0 || v.block.id != panelBlockId || r.getWidth() < 24) return;
    auto strip = r.reduced(6, 0).removeFromBottom(10).withTrimmedBottom(4);
    g.setColour(MiraLookAndFeel::surface.withAlpha(0.8f));
    g.fillRoundedRectangle(strip.toFloat(), 3.0f);
    g.setColour(MiraLookAndFeel::accent);
    g.fillRoundedRectangle(strip.toFloat().withWidth(
        juce::jmax(6.0f, (float) strip.getWidth() * (float) genFraction)), 3.0f);
}

void CanvasView::setGenerationProgress(double fraction)
{
    const double next = fraction < 0.0 ? -1.0 : juce::jlimit(0.0, 1.0, fraction);
    // Only when it MOVED enough to see. This is called from a 30 Hz timer over every
    // block on the canvas, and repainting the world for a thousandth of a bar is how a
    // progress indicator ends up costing more than the thing it is reporting on.
    if (std::abs(next - genFraction) < 0.004 && (next < 0.0) == (genFraction < 0.0)) return;
    genFraction = next;
    repaint();
}

void CanvasView::beginRename(int lane)
{
    if (isReferenceLane(lane))
    {
        if (onTakeNote) onTakeNote("the reference track is named by its film, not by you");
        return;
    }
    commitRename();
    renamingLane = lane;
    renameEditor = std::make_unique<juce::TextEditor>();
    renameEditor->setText(laneNames[lane], juce::dontSendNotification);
    // IN PLACE: exactly the name's rectangle, the same font, no border and no box. The
    // editor used to be a plain TextEditor at a slightly different size, in a name box
    // that was itself offset from where the name appeared to be -- so renaming looked
    // like a text field opening somewhere else rather than the name becoming editable.
    renameEditor->setFont(laf.sansSemiBold(MiraLookAndFeel::textSize(12.5f)));
    renameEditor->setBounds(nameBoxFor(lane));
    renameEditor->setBorder(juce::BorderSize<int>(0));
    renameEditor->setIndents(0, 1);
    renameEditor->setJustification(juce::Justification::centredLeft);
    renameEditor->setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::surface.darker(0.2f));
    renameEditor->setColour(juce::TextEditor::outlineColourId, laneColour(lane).withAlpha(0.5f));
    renameEditor->setColour(juce::TextEditor::focusedOutlineColourId, laneColour(lane));
    renameEditor->setColour(juce::TextEditor::textColourId, laneColour(lane).brighter(0.2f));
    renameEditor->setColour(juce::TextEditor::highlightColourId, laneColour(lane).withAlpha(0.3f));
    renameEditor->onReturnKey = [this] { commitRename(); };
    renameEditor->onEscapeKey = [this] { renamingLane = -1; renameEditor.reset(); repaint(); };
    renameEditor->onFocusLost = [this] { commitRename(); };
    addAndMakeVisible(*renameEditor);
    renameEditor->selectAll();
    renameEditor->grabKeyboardFocus();
}

void CanvasView::commitRename()
{
    if (renameEditor == nullptr || renamingLane < 0) { renameEditor.reset(); renamingLane = -1; return; }
    if (laneNames[renamingLane] != renameEditor->getText().trim())
    {
        laneNames.set(renamingLane, renameEditor->getText().trim());
        markDirty();
    }
    renamingLane = -1;
    renameEditor.reset();
    grabKeyboardFocus();
    repaint();
}

void CanvasView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (e.y >= topRuler && e.y < videoStripTop())
    {
        if (const int hit = markerAtStripX(e.x); hit >= 0) renameMarker(hit);
        return;
    }
    // Double-click a lane's NAME to rename it. "takes / 3" says what the file was called,
    // not what the lane is for, and a lane you cannot name is one you have to identify by
    // its waveform every time.
    if (e.x < kHeaderWidth && e.y >= lanesTop())
    {
        const int lane = yToLane(e.y);
        if (nameBoxFor(lane).contains(e.getPosition())) beginRename(lane);
        return;
    }

    // Double-click a block opens its generator -- which now means EXPANDING the panel if
    // it is folded, since the panel no longer opens as a window. Single click selects and
    // points the panel at it, which is what you do ninety times for every once you want
    // to go and look at the settings.
    Drag what = Drag::None;
    if (auto* hit = hitTest(e.getPosition(), what); hit != nullptr)
    {
        // Double-click the gain box for unity -- the one gain value worth having an exact
        // way back to.
        if (blockGainBox(*hit).contains(e.getPosition()))
        {
            pushUndo();
            hit->block.gainDb = 0.0;
            rebuildAudio(); markDirty(); repaint();
            return;
        }
        // Double-click the TEMPO box puts back what the RECIPE said -- the exact
        // counterpart of double-clicking the gain box for unity, and the only way back
        // from a dragged number that is worth having an exact route to. If the recipe said
        // nothing, that is the answer too: the tempo goes away and the grid with it.
        // Tested before the name, because the tag box is inside the same header row.
        if (auto tb = blockTagBox(*hit); !tb.isEmpty() && tb.contains(e.getPosition()))
        {
            selected.clear();
            selected.insert(hit->block.id);
            pushUndo();
            musicFromTake(*hit, true);
            markDirty(); repaint();
            return;
        }
        // Double-click the NAME to rename the block, the same gesture a lane already had.
        const auto r = boundsOf(*hit);
        if (r.getHeight() >= 46 && e.y - r.getY() <= 18
            && e.x > blockGainBox(*hit).getRight())
        {
            selected.clear();
            selected.insert(hit->block.id);
            beginRenameBlock(hit->block.id);
            return;
        }
        selected.clear();
        selected.insert(hit->block.id);
        if (onRevealGenerator) onRevealGenerator();
        pointPanelAt(hit);
        repaint();
    }
}

// Where a click on the fader lands, in dB, through the SAME warped scale the fader and
// meter are drawn with -- so the cap arrives under the pointer instead of near it.
double CanvasView::faderDbAtY(int lane, int y) const
{
    auto f = faderBoxFor(lane);
    if (f.isEmpty() || f.getHeight() <= 0) return laneDbAt(lane);
    const double norm = 1.0 - (double) (y - f.getY()) / (double) f.getHeight();
    return normToDb(norm);
}

void CanvasView::setLaneDb(int lane, double db)
{
    if (lane < 0 || lane >= CanvasAudioSource::kMaxLanes) return;
    if ((int) laneDb.size() <= lane) laneDb.resize((size_t) lane + 1, 0.0);
    laneDb[(size_t) lane] = juce::jlimit(-60.0, 6.0, db);
    player.setLaneGain(lane, laneDb[(size_t) lane] <= -60.0
                                 ? 0.0f
                                 : juce::Decibels::decibelsToGain((float) laneDb[(size_t) lane]));
}

juce::Rectangle<int> CanvasView::blockMuteBox(const Visual& v) const
{
    auto r = boundsOf(v);
    // Only where the header strip is actually drawn. A button you can hit but cannot see
    // is worse than no button.
    if (r.getHeight() < 46 || r.getWidth() < 52) return {};
    return { r.getX() + 5, r.getY() + 3, 15, 13 };
}

// The block's own gain, beside its mute. CANVAS.md listed "a gain handle on a block" as
// open; the field and the mixing were already there (Block::gainDb, applied per voice in
// CanvasEngine) with nothing on screen to move it.
//
// Drag, rather than a slider: a block is small and a real fader would cost more of it than
// the waveform can spare. Double-click returns to unity, which is the only value anyone
// ever wants to get back to exactly.
juce::Rectangle<int> CanvasView::blockGainBox(const Visual& v) const
{
    auto mb = blockMuteBox(v);
    if (mb.isEmpty()) return {};
    auto r = boundsOf(v);
    if (r.getWidth() < 96) return {};       // not at the cost of the name
    return { mb.getRight() + 3, mb.getY(), 30, mb.getHeight() };
}

// ANALYSE (MIRA-BLOCKS.md 2.1), third in the header row after M and the gain box.
//
// A chip on the block rather than only a menu item, for the same reason M is one: a menu
// is where you go to find something, a button on the thing itself is where you go to DO
// it -- and analysing a take is something you do over and over while listening, then
// watch, because it takes a minute on a long one.
//
// Only where there is audio to analyse. A chip on an empty block would be an affordance
// with nothing behind it, which is the fault blockMuteBox's own comment names.
juce::Rectangle<int> CanvasView::blockAnalyseBox(const Visual& v) const
{
    if (!v.block.hasAudio() || isReferenceLane(v.block.lane)) return {};
    auto gb = blockGainBox(v);
    if (gb.isEmpty()) return {};
    auto r = boundsOf(v);
    // Wider than the gain box needs, because the tempo box and the name both come out of
    // what is left. Below this the block says its name and nothing else, which is the
    // padlock's rule again: a control that does not fit is replaced by words.
    if (r.getWidth() < 128) return {};
    return { gb.getRight() + 3, gb.getY(), 15, gb.getHeight() };
}

// Tempo and key, on the RIGHT of the header row. Drawn there, double-clicked there, and
// the tempo editor opens there -- one definition, because three copies of this arithmetic
// is how a control ends up drawn in one place and clickable in another.
juce::Rectangle<int> CanvasView::blockTagBox(const Visual& v) const
{
    auto r = boundsOf(v);
    // Not on the REFERENCE lane. Its block is locked -- hitTest refuses to hand it to any
    // gesture -- so drawing a tempo box on it would be an affordance that does nothing,
    // and the film's dialogue has no tempo worth claiming anyway.
    if (r.getHeight() < 46 || !v.block.hasAudio() || isReferenceLane(v.block.lane)) return {};
    auto row = r.reduced(6, 2).removeFromTop(14);
    // Whichever chip is furthest right, so adding one never lands the tempo box on top of
    // it. Asked in order rather than assumed, because each chip has its own width gate and
    // a block wide enough for gain is not always wide enough for Analyse.
    if (const auto ab = blockAnalyseBox(v); !ab.isEmpty())
        row = row.withTrimmedLeft(ab.getRight() - r.getX() - 2);
    else if (const auto gb = blockGainBox(v); !gb.isEmpty())
        row = row.withTrimmedLeft(gb.getRight() - r.getX() - 2);
    else if (const auto mb = blockMuteBox(v); !mb.isEmpty())
        row = row.withTrimmedLeft(mb.getWidth() + 4);
    if (row.getWidth() <= 150) return {};
    return row.removeFromRight(juce::jmin(130, row.getWidth() / 2));
}

juce::File CanvasView::blockFolderFor(const Visual& v) const
{
    if (!projectFolder.isDirectory() || v.block.name.isEmpty()) return {};
    return projectFolder.getChildFile(juce::File::createLegalFileName(v.block.name));
}

CanvasView::Visual* CanvasView::singleSelection()
{
    if (selected.size() != 1) return nullptr;
    for (auto& i : items) if (selected.count(i->block.id)) return i.get();
    return nullptr;
}

// Where a take actually STOPS SOUNDING, which is not the same as where the file ends.
// SA3 pads to the requested duration whether or not it had that much music in it, and for
// some LoRA pairs it stops well short -- measured across one project's takes, a gsl+ams
// pair filled 55-79% of what was asked for while every single-LoRA take filled 96-100%.
//
// Silence at the end is not merely wasted block: it is what the NEXT extend continues
// from, so an unnoticed early stop makes every extension after it continue from nothing.
// Saying so is the whole point -- the canvas already has Cmd-E to cut where the audio
// really ends, and this is what tells you to use it.
//
// -60 dBFS, on the mixed channels, scanning backwards: the first block that holds anything
// audible ends the search, so a take that fills its length costs one block read.
double audibleEndOf(juce::AudioFormatManager& formats, const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor(f));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0) return 0.0;
    const int block = (int) juce::jmin<juce::int64>(reader->sampleRate, reader->lengthInSamples);
    juce::AudioBuffer<float> buffer ((int) juce::jmax (1u, reader->numChannels), block);
    for (juce::int64 pos = reader->lengthInSamples; pos > 0;)
    {
        const int want = (int) juce::jmin<juce::int64>(block, pos);
        pos -= want;
        reader->read (&buffer, 0, want, pos, true, true);
        for (int i = want; --i >= 0;)
        {
            float peak = 0.0f;
            for (int c = 0; c < buffer.getNumChannels(); ++c)
                peak = juce::jmax (peak, std::abs (buffer.getSample (c, i)));
            if (peak > 0.001f)          // -60 dBFS
                return (double) (pos + i) / reader->sampleRate;
        }
    }
    return 0.0;
}

// The take's OWN recipe sets the block's tempo and key (MIRA-BLOCKS.md 1.2), and it comes
// from the `.json` sidecar beside the wav rather than from whatever recipe is on screen.
// That distinction is not pedantry: a block's `settings` is the recipe you are ABOUT to
// generate with, and reading it here would let a tempo you have just typed into the prompt
// describe audio made before you typed it -- the same confusion that once had two blocks
// with different audio showing one identical prompt (see pointPanelAt).
//
// The grid is drawn, never enforced, so being close is enough: SA3 returns near what was
// asked for and not equal to it, and the measured answer is step 2's job.
// The prompt that MADE this take, off the `.json` sidecar beside it. Empty for a dropped
// file, for the film's reference audio, and for a take whose sidecar has been deleted --
// all of which are "no tempo", not "some other tempo".
static juce::String takePromptOf (const mira::canvas::Block& b)
{
    if (!b.hasAudio()) return {};
    auto sidecar = b.file.withFileExtension("json");
    if (!sidecar.existsAsFile()) return {};
    auto parsed = juce::JSON::parse(sidecar.loadFileAsString());
    return parsed.isObject() ? parsed.getProperty("prompt", "").toString() : juce::String();
}

// What the recipe asked for, or 0. The gesture that starts a tempo from nothing borrows
// this before it falls back to a round number.
static double promptTempoOf (const mira::canvas::Block& b)
{
    const double bpm = promptField(takePromptOf(b), "BPM").getDoubleValue();
    return (bpm >= 20.0 && bpm <= 400.0) ? bpm : 0.0;
}

void CanvasView::musicFromTake(Visual& v, bool force)
{
    // Convention 5, applied to time: a tempo you typed or measured is NOT overwritten by a
    // take landing. Only a grid that came from a prompt -- or from nothing at all -- is the
    // prompt's to set. `force` is the one deliberate exception: double-clicking the tempo
    // box asks for the recipe's answer back, and a request is not an overwrite.
    if (!force && v.block.tempoSource.isNotEmpty() && v.block.tempoSource != "prompt") return;

    const auto prompt = takePromptOf(v.block);
    const auto keyText = promptField(prompt, "Keyscale");
    const double bpm = promptField(prompt, "BPM").getDoubleValue();
    // A number outside this is not a tempo, it is a field that held something else. Omit
    // rather than guess (convention 1) -- a block with no tempo draws no grid, which is a
    // readable answer, where a grid at 4 BPM is not.
    const bool usable = bpm >= 20.0 && bpm <= 400.0;

    // Cleared when the new take says nothing, rather than left holding the old take's
    // numbers. State that outlives the thing it described is convention 12's bug: a grid
    // drawn over audio it was never about is worse than no grid.
    v.block.tempo = usable ? bpm : 0.0;
    v.block.key = keyText;
    v.block.tempoSource = (usable || keyText.isNotEmpty()) ? juce::String("prompt")
                                                           : juce::String();
    v.block.tempoConfidence = 0.0;   // nothing has been measured; step 2 is what earns this
    // Bar 1 stays where it is when a human put it there (convention 5). Otherwise it goes
    // back to the start of the audio, because the take it was aligned against is gone.
    // Asked for explicitly, it goes back too -- "put the recipe's answer back" means all
    // of it, or it is a button whose effect you have to remember the limits of.
    if (force) { v.block.barOnePos = 0.0; v.block.barOneIsHuman = false; }
    else if (!v.block.barOneIsHuman) v.block.barOnePos = 0.0;
}

// ---- MIRA-BLOCKS.md step 2: the grid it actually GOT --------------------------------

void CanvasView::analyseSelection()
{
    if (!onAnalyseRequested)
    {
        // Convention 6, at the outermost edge: the canvas can be built without an owner
        // that knows about the library (it is a separate window by design), and a chip
        // that silently does nothing in that build is worse than one that says why.
        if (onTakeNote) onTakeNote("this canvas has no library to analyse into");
        return;
    }

    int asked = 0, running = 0, empty = 0;
    for (auto& i : items)
    {
        if (selected.count(i->block.id) == 0) continue;
        if (isReferenceLane(i->block.lane)) continue;   // the film's audio is not ours to measure
        if (!i->block.hasAudio() || !i->block.file.existsAsFile()) { ++empty; continue; }
        // Asking twice for the same block queues a second identical run behind the first,
        // and the second one's answer is the first one's answer -- the same reasoning
        // enqueueAnalyze's own duplicate guard is built on.
        if (i->analysis == Visual::Analysis::Running) { ++running; continue; }

        i->analysis = Visual::Analysis::Running;
        i->analysisNote = "analysing" + juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"));
        ++asked;
        onAnalyseRequested(i->block.file, i->block.id);
    }
    repaint();

    if (onTakeNote)
    {
        if (asked > 0)
            onTakeNote("analysing " + juce::String(asked) + " take"
                        + (asked == 1 ? "" : "s") + juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"))
                        + " this takes about a minute each");
        else if (running > 0) onTakeNote("already analysing - wait for it to finish");
        else if (empty > 0)   onTakeNote("nothing to analyse - that block has no take yet");
        else                  onTakeNote("select a block to analyse");
    }
}

// THE CONFIDENCE GATE (2.4) lives here, and it is the reason this function reads the way
// it does: the measurement arrives whole, and then each part of it is asked separately
// whether it has earned the right to overwrite what the block already believes.
//
// Nothing here is a fallback. A measurement that does not clear the gate leaves the grid
// exactly as it was and SAYS SO -- a wrong grid imposed confidently is the failure this
// project has already had twice (MIRA-BLOCKS.md §10), and an unexplained refusal is the
// one that took three rounds of theories to find (convention 6, convention 10).
void CanvasView::analysisArrived(juce::int64 blockId, const juce::File& take,
                                 const Measurement& m)
{
    Visual* v = nullptr;
    for (auto& i : items) if (i->block.id == blockId) { v = i.get(); break; }
    // The block was deleted, or it is showing a different take now -- you chose another
    // one while this ran. Either way the answer is about audio this block is no longer
    // made of, and applying it would draw a grid measured from a file nobody is hearing.
    if (v == nullptr) return;
    if (!mira::pathsEquivalent(v->block.file.getFullPathName().toStdString(),
                                take.getFullPathName().toStdString()))
    {
        if (onTakeNote)
            onTakeNote("analysed " + take.getFileName() + " - that block has moved on to "
                        + "another take, so its grid is unchanged");
        if (v->analysis == Visual::Analysis::Running) v->analysis = Visual::Analysis::None;
        repaint();
        return;
    }

    if (!m.ok)
    {
        v->analysis = Visual::Analysis::Failed;
        v->analysisNote = m.note.isNotEmpty() ? m.note : juce::String("analysis produced no grid");
        if (onTakeNote) onTakeNote(v->block.name + ": " + v->analysisNote);
        repaint();
        return;
    }

    // Onsets first, and OUTSIDE the gate. An onset is a measurement of the audio; whether
    // the beat grid is trustworthy says nothing about whether a transient is where it is
    // -- and on a take whose grid is refused, the onsets are the only honest thing on
    // screen about its timing.
    v->onsets = m.onsets;

    const auto bpmText = juce::String(m.bpm, m.bpm < 100.0 ? 2 : 1);
    const auto confText = juce::String(m.stability, 2);

    if (m.stability < kGridConfidenceGate)
    {
        v->analysis = Visual::Analysis::Refused;
        // The number AND the reason, because the point of the gate is that you can
        // overrule it: the tempo box is still a drag box, and now you know what the
        // machine thought before you decide it was wrong.
        v->analysisNote = "measured " + bpmText + " bpm at confidence " + confText
                        + " - below " + juce::String(kGridConfidenceGate, 2)
                        + ", so the grid is left as it was";
        if (onTakeNote) onTakeNote(v->block.name + ": " + v->analysisNote);
        repaint();
        return;
    }

    pushUndo();   // one snapshot for one answer, so adopting a grid is one Cmd-Z to undo

    juce::StringArray adopted;
    v->block.tempo = m.bpm;
    v->block.tempoSource = "measured";
    v->block.tempoConfidence = m.stability;
    adopted.add(bpmText + " bpm");

    // The meter has a gate of its own and it is not this one. `beat_grid_stability` says
    // the BEATS are steady; `meter_bar_spread` says whether they group into a bar the same
    // way twice. A steady grid with a drifting bar is exactly the case where 4 is a guess.
    if (m.meter >= 2 && m.barSpread > 0.0 && m.barSpread <= kMeterSpreadLimit)
    {
        v->block.meter = m.meter;
        adopted.add(juce::String(m.meter) + "/4");
    }

    // Key comes from libKeyFinder with its own harmonicity gate, so a key that arrives at
    // all has already been judged. It replaces the prompt's key because one describes the
    // audio and the other describes what was asked for -- and there is no way to type a
    // key by hand yet, so nothing human is being overwritten.
    if (m.key.isNotEmpty() && m.key != v->block.key)
    {
        v->block.key = m.key;
        adopted.add(m.key);
    }

    // BAR 1 (2.3). Convention 5: a downbeat you put there by ear outranks a model, and it
    // survives every analysis after it. The measured one is in SOURCE time already, which
    // is the same clock barOnePos is kept in.
    if (m.firstDownbeat >= 0.0)
    {
        if (v->block.barOneIsHuman)
            adopted.add("bar 1 kept where you put it");
        else
        {
            v->block.barOnePos = m.firstDownbeat;
            adopted.add("bar 1 at " + juce::String(m.firstDownbeat, 2) + "s");
        }
    }

    v->analysis = Visual::Analysis::Measured;
    v->analysisNote = adopted.joinIntoString(", ") + " (confidence " + confText + ")";
    if (onTakeNote) onTakeNote(v->block.name + ": " + v->analysisNote);
    markDirty();
    repaint();
}

void CanvasView::setFileOn(Visual& v, const juce::File& f)
{
    v.block.file = f;
    v.thumb.reset();
    if (f.existsAsFile())
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor(f));
        if (reader != nullptr && reader->sampleRate > 0.0)
        {
            v.audioSeconds = reader->lengthInSamples / reader->sampleRate;
            // A new take is new audio, so a cut made against the old one is meaningless --
            // it described where a different file went quiet.
            v.block.contentSeconds = 0.0;
            // THE BLOCK'S LENGTH IS WHAT YOU ASKED FOR, and a take fills it. Only a block
            // that has never had a length takes it from the file.
            //
            // The old rule clamped the block to the file, which made an empty tail
            // impossible -- and that tail is the whole extend gesture: drag the block out
            // past the end of its audio and the gap is the range to fill in.
            if (v.block.length <= 0.0) v.block.length = v.audioSeconds;
        }
        v.thumb = std::make_unique<juce::AudioThumbnail>(512, formats, cache);
        v.thumb->setSource(new juce::FileInputSource(f));
    }
    // A measurement describes ONE file. The take just changed, so last take's onsets,
    // state and note are now claims about audio nobody is hearing -- convention 12's bug
    // exactly, and the reason musicFromTake clears rather than keeps.
    v.analysis = Visual::Analysis::None;
    v.analysisNote.clear();
    v.onsets.clear();

    // Every route a take can arrive by goes through here -- a generation adopted, a take
    // chosen, a file dropped, a block split or duplicated, a document loaded -- so this is
    // the one place that has to know the audio changed.
    musicFromTake(v);

    // What the library ALREADY knows, if anything: a take analysed in a previous session
    // gets its onsets back on reopen without measuring anything again. It sets nothing
    // else -- the document already holds whatever grid was adopted, and re-adopting it
    // here would let a measurement outrank a tempo you typed after it.
    if (onMeasurementLookup && v.block.hasAudio() && f.existsAsFile())
    {
        const auto known = onMeasurementLookup(f);
        if (known.ok)
        {
            v.onsets = known.onsets;
            // Only claim "measured" when the DOCUMENT says the measurement was adopted.
            // A row exists for every analysed file, including ones whose grid the gate
            // refused, and showing those as measured would be the chip lying about the
            // one thing it is there to report.
            if (v.block.tempoSource == "measured")
            {
                v.analysis = Visual::Analysis::Measured;
                v.analysisNote = "measured in an earlier session (confidence "
                               + juce::String(v.block.tempoConfidence, 2) + ")";
            }
        }
    }
}

// One place that names a block, so a dropped file and a "+ Block" cannot end up in
// different naming schemes -- which is exactly how the canvas grew two kinds of block.
juce::String CanvasView::nextBlockName() const
{
    int highest = 0;
    for (const auto& i : items)
    {
        const auto n = i->block.name;
        if (n.startsWith("block ")) highest = juce::jmax(highest, n.substring(6).getIntValue());
    }

    // Skip past any name whose FOLDER already exists on disk. Removing a block takes it
    // off the canvas but leaves its folder -- so the next "block 3" reused that folder and
    // opened showing the deleted block's files. A new block has to be new all the way
    // down, not just in the arrangement.
    for (int n = highest + 1; n < highest + 1000; ++n)
    {
        const auto name = "block " + juce::String(n);
        if (!projectFolder.isDirectory()) return name;
        if (!projectFolder.getChildFile(juce::File::createLegalFileName(name)).exists()) return name;
    }
    return "block " + juce::String(highest + 1);
}

void CanvasView::addLane()
{
    if (laneCount >= CanvasAudioSource::kMaxLanes) return;
    pushUndo();
    ++laneCount;
    if (laneNames[laneCount - 1].isEmpty()) laneNames.set(laneCount - 1, "track " + juce::String(laneCount));
    markDirty();
    repaint();
}

void CanvasView::moveLane(int from, int to)
{
    from = juce::jlimit(0, laneCount - 1, from);
    to   = juce::jlimit(0, laneCount - 1, to);
    if (from == to || laneCount < 2) return;

    pushUndo();
    UndoGuard oneEdit (*this);

    // `order[newIndex] = oldIndex` -- ONE permutation, applied to every list that is keyed
    // on the lane index. Writing the remap once and reusing it is the whole point: the
    // blocks, the names, the faders, the meters, the mute and solo bits and the reference
    // lane all have to agree, and six separate shift expressions would be six chances to
    // disagree. A reorder that moved the blocks but left the faders behind would be worse
    // than no reorder at all.
    std::vector<int> order;
    order.reserve((size_t) laneCount);
    for (int i = 0; i < laneCount; ++i) if (i != from) order.push_back(i);
    order.insert(order.begin() + to, from);

    std::vector<int> newIndexOf((size_t) laneCount, 0);
    for (int n = 0; n < laneCount; ++n) newIndexOf[(size_t) order[(size_t) n]] = n;

    auto remap = [&](int oldLane) {
        return juce::isPositiveAndBelow(oldLane, laneCount) ? newIndexOf[(size_t) oldLane] : oldLane;
    };

    for (auto& v : items)
    {
        // The colour travels with the TRACK: a block that is blue because it sits on
        // track 1 stays blue when track 1 moves, rather than turning into whatever colour
        // row 1 now is.
        v->block.colour = remap(v->block.colour);
        v->block.lane = remap(v->block.lane);
    }

    juce::StringArray names;
    for (int n = 0; n < laneCount; ++n)
        names.add(order[(size_t) n] < laneNames.size() ? laneNames[order[(size_t) n]] : juce::String());
    laneNames = names;

    auto reorderVector = [&](auto& v, auto fallback) {
        std::decay_t<decltype(v)> out;
        for (int n = 0; n < laneCount; ++n)
        {
            const auto o = (size_t) order[(size_t) n];
            out.push_back(o < v.size() ? v[o] : fallback);
        }
        v = out;
    };
    reorderVector(laneDb, 0.0);
    ensureLaneArrays();
    reorderVector(laneH, 0);
    reorderVector(laneMeter, std::array<float, 2>{ 0.0f, 0.0f });
    reorderVector(laneHold,  std::array<float, 2>{ 0.0f, 0.0f });
    reorderVector(laneClipped, false);

    auto reorderMask = [&](juce::uint64 mask) {
        juce::uint64 out = 0;
        for (int n = 0; n < juce::jmin(laneCount, CanvasAudioSource::kMaxLanes); ++n)
            if ((mask >> order[(size_t) n]) & 1u) out |= juce::uint64(1) << n;
        return out;
    };
    muteMask = reorderMask(muteMask);
    soloMask = reorderMask(soloMask);
    applyMasks();

    if (referenceLane >= 0) referenceLane = remap(referenceLane);
    if (selectedLane >= 0)  selectedLane  = remap(selectedLane);

    markDirty();
    rebuildAudio();
    repaint();
}

void CanvasView::moveSelectedLane(int delta)
{
    if (selectedLane < 0)
    {
        if (onTakeNote) onTakeNote("select a track first - click its header");
        return;
    }
    const int to = selectedLane + delta;
    if (to < 0 || to >= laneCount)
    {
        if (onTakeNote) onTakeNote(delta < 0 ? "that track is already at the top"
                                             : "that track is already at the bottom");
        return;
    }
    moveLane(selectedLane, to);
}

void CanvasView::removeLane(int lane)
{
    if (lane < 0 || lane >= laneCount || laneCount <= 1) return;
    if (isReferenceLane(lane))
    {
        // The reference belongs to the picture. Closing the film is what removes it, and
        // saying so is better than a delete that silently does nothing (convention 6).
        if (onTakeNote) onTakeNote("the reference track goes with its film - close the video to remove it");
        return;
    }
    pushUndo();
    UndoGuard oneEdit (*this);

    items.erase(std::remove_if(items.begin(), items.end(),
                                [lane](const std::unique_ptr<Visual>& v) {
                                    return v->block.lane == lane;
                                }),
                 items.end());
    // Everything below moves UP. A track numbered 4 with nothing above it is not a hole
    // anyone meant to leave, and the lane index is what mute, solo and the faders are
    // keyed on -- a gap in it is a gap in the mixer.
    for (auto& v : items)
        if (v->block.lane > lane) --v->block.lane;

    laneNames.remove(lane);
    if (lane < (int) laneDb.size()) laneDb.erase(laneDb.begin() + lane);
    if (lane < (int) laneH.size())  laneH.erase(laneH.begin() + lane);

    // The mask bits above the removed lane shift down with it, or mute and solo would
    // apply to whichever track happened to slide into the slot.
    auto shift = [lane](juce::uint64 mask) {
        const juce::uint64 below = mask & ((juce::uint64 (1) << lane) - 1);
        const juce::uint64 above = mask >> (lane + 1);
        return below | (above << lane);
    };
    muteMask = shift(muteMask);
    soloMask = shift(soloMask);
    applyMasks();

    --laneCount;
    // Everything below the removed lane moved up, and the reference is something below.
    if (referenceLane > lane) --referenceLane;
    selectedLane = juce::jlimit(-1, laneCount - 1, selectedLane >= laneCount ? laneCount - 1 : selectedLane);
    selected.clear();
    pointPanelAt(nullptr);
    markDirty();
    rebuildAudio();
    repaint();
}

void CanvasView::addEmptyBlock()
{
    // Dropped on the first lane with nothing under the playhead, at the playhead. An
    // empty block is a FRAME: a length you meant, with nothing in it yet.
    const double at = juce::jmax(0.0, player.getPositionSeconds());
    // Short. A new block is a placeholder you will resize, not a claim that the part is
    // thirty seconds long -- and a full-window frame on an empty canvas reads as an error
    // rather than as an invitation.
    // 30 seconds, because that is the generator's default duration and the block's length
    // IS the duration now: a new block is a 30 second frame you will resize, and resizing
    // it is how you ask for a different length.
    constexpr double kNewBlockSeconds = 30.0;
    pushUndo();
    UndoGuard oneEdit (*this);
    // A NEW BLOCK ALWAYS OPENS ON A NEW TRACK. Hunting for a free gap on an existing
    // track put two unrelated blocks on one fader, and a track is the thing you mix with
    // -- so a block that arrives sharing one arrives already mixed into something else.
    // The exception is the very first block on an empty canvas, which has a track waiting.
    int lane = 0;
    if (!items.empty()) { addLane(); lane = laneCount - 1; }

    auto v = std::make_unique<Visual>();
    v->block.lane = lane;
    v->block.start = at;
    v->block.length = kNewBlockSeconds;
    v->block.id = nextId++;
    v->block.name = nextBlockName();
    v->block.colour = lane;    // born here, and it keeps this colour wherever it goes
    v->settings = emptyRecipe();   // a new block generates nothing until you tell it what
    if (laneNames[lane].isEmpty()) laneNames.set(lane, "track " + juce::String(lane + 1));
    laneCount = juce::jmax(laneCount, lane + 1);
    selected.clear();
    selected.insert(v->block.id);
    items.push_back(std::move(v));
    announceSelection();
    pointPanelAt(items.back().get());
    markDirty();
    repaint();
}

void CanvasView::duplicateSelection()
{
    if (selected.empty()) return;
    pushUndo();

    // Placed after the rightmost of what was copied, on the same lanes, so a duplicate
    // lands where you would have dragged it rather than on top of the original.
    double rightmost = 0.0;
    double leftmost = 1e12;
    for (const auto& i : items)
        if (selected.count(i->block.id))
        { rightmost = juce::jmax(rightmost, i->block.end()); leftmost = juce::jmin(leftmost, i->block.start); }
    const double shift = juce::jmax(0.25, rightmost - leftmost);

    // The sources, resolved before anything is added -- pushing into `items` while
    // iterating it is how this loop would eat itself.
    std::vector<Visual*> sources;
    for (const auto& i : items)
        if (selected.count(i->block.id)) sources.push_back(i.get());

    selected.clear();
    for (auto* src : sources)
    {
        auto v = std::make_unique<Visual>();
        v->block = src->block;               // trim, fades, gain and lane all come too
        v->block.id = nextId++;
        v->block.start = src->block.start + shift;
        v->settings = src->settings;
        // A NEW NAME, and therefore a new folder. The name is what says where a generation
        // lands: `adoptTake` finds the block whose folder the take was written into, so a
        // duplicate that kept its original's name meant generating on the duplicate put the
        // audio on the ORIGINAL -- the first block with that folder wins. The colour does
        // not change, because colour comes from the TRACK and the duplicate is on the same
        // one; what makes them tell apart is the name, which is the thing that has to differ
        // anyway.
        v->block.name = nextBlockName();
        // The SAME take, not a new one. A duplicate that regenerated would be a different
        // piece of audio wearing the same name.
        setFileOn(*v, src->block.file);
        v->block.length = src->block.length; // setFileOn may have reset it to the file's
        v->block.sourceOffset = src->block.sourceOffset;
        selected.insert(v->block.id);
        // Pushed one at a time so nextBlockName() can see the previous copy -- otherwise
        // duplicating three blocks gives all three the same name and the same folder,
        // which is the bug this whole function was fixing.
        items.push_back(std::move(v));
    }

    // AND point the panel at the copy. Generating is aimed by whatever the panel is
    // showing, so leaving it on the original is the other half of why "duplicate, then
    // generate" put the new audio back on the block you had duplicated FROM.
    pointPanelAt(items.empty() ? nullptr : items.back().get());
    markDirty();
    rebuildAudio();
    repaint();
}

void CanvasView::applySettingsToSelection(const juce::var& settings)
{
    if (auto* v = singleSelection()) { v->settings = settings; markDirty(); }
}

void CanvasView::chooseTakeForSelection(const juce::File& take)
{
    if (auto* v = singleSelection())
    {
        pushUndo();
        setFileOn(*v, take);
        rebuildAudio();
        markDirty();
        repaint();
    }
}

void CanvasView::setSelectionMuted(bool muted)
{
    if (selected.empty()) return;
    pushUndo();
    for (auto& i : items)
        if (selected.count(i->block.id)) i->block.muted = muted;
    // A rebuild rather than an atomic bit, unlike the TRACK mute. A muted block is left
    // out of the arrangement entirely, which is also what keeps it from crossfading with
    // the block next to it -- an inaudible block pulling its neighbour down is worse than
    // no mute at all. Readers are cached, so the rebuild costs nothing on disk.
    rebuildAudio();
    markDirty();
    repaint();
}

void CanvasView::showBlockMenu(Visual& v)
{
    // Right-clicking something you have not selected selects it first. Otherwise the menu
    // is about one block and the action lands on another.
    if (selected.count(v.block.id) == 0)
    {
        selected.clear();
        selected.insert(v.block.id);
        pointPanelAt(&v);
    }

    const bool muted = v.block.muted;
    const auto id = v.block.id;
    const bool hasFades = v.block.fadeIn > 0.0 || v.block.fadeOut > 0.0;
    const int shape = (int) v.block.fadeShape;

    juce::PopupMenu fades;
    fades.addItem(10, "Linear",      true, shape == 0);
    fades.addItem(11, "Equal power", true, shape == 1);
    fades.addItem(12, "Exponential", true, shape == 2);

    juce::PopupMenu m;
    m.addSectionHeader(v.block.name);
    m.addItem(1, muted ? "Unmute block" : "Mute block");
    m.addSubMenu("Fade shape", fades);
    m.addItem(2, "Clear fades", hasFades);
    m.addItem(6, "Restore full take", v.block.contentSeconds > 0.0 && v.block.hasAudio());
    m.addSeparator();
    m.addItem(3, "Duplicate");
    m.addItem(7, "Cut at playhead");
    m.addItem(4, "Split into two at playhead");
    m.addItem(5, "Remove");
    // EVERY TAKE THIS BLOCK EVER MADE. The block keeps them all -- that is what makes
    // "go back to the one before" possible -- and until now there was no way to see them,
    // switch between them, or bin a bad one. The canvas showed you one file and silently
    // held the rest.
    //
    // Newest first, the current one ticked. Alt held turns the list into a trash list:
    // one menu, two verbs, rather than a submenu per take.
    auto takes = takesOf(v);
    if (!takes.isEmpty())
    {
        juce::PopupMenu takeMenu;
        for (int i = 0; i < takes.size() && i < 40; ++i)
        {
            const bool current = v.block.hasAudio()
                && mira::pathsEquivalent(takes[i].getFullPathName().toStdString(),
                                          v.block.file.getFullPathName().toStdString());
            takeMenu.addItem(100 + i, takes[i].getFileNameWithoutExtension(), true, current);
        }
        takeMenu.addSeparator();
        juce::PopupMenu trashMenu;
        for (int i = 0; i < takes.size() && i < 40; ++i)
            trashMenu.addItem(200 + i, takes[i].getFileNameWithoutExtension());
        takeMenu.addSubMenu("Move a take to the Trash", trashMenu);
        m.addSeparator();
        m.addSubMenu("Takes (" + juce::String(takes.size()) + ")", takeMenu);
    }

    // A SPLIT KEEPS THE PARENT'S BAR NUMBERS -- bar 1 is stored in source time, so the
    // right-hand half of a cut at bar 9 goes on saying bar 9, wherever you then drag it.
    // That is the right default (you split it to move that section, and you talk about it
    // by where it came from), but it is not always what you want, so the other answer is
    // one item rather than an argument: renumber from here.
    m.addItem(23, "Bar 1 starts here", v.block.tempo > 0.0);
    // The same verb as the header chip, with room for a sentence. The chip is where you
    // press it; this is where you read what it said -- a 15-pixel square cannot hold
    // "measured 88.1 bpm at confidence 0.71, below 0.90, so the grid is left as it was",
    // and that sentence is the entire point of the gate.
    m.addItem(24, "Analyse take", v.block.hasAudio()
                                   && v.analysis != Visual::Analysis::Running);
    if (v.analysisNote.isNotEmpty())
        m.addItem(25, v.analysisNote, false);   // a readout, not a command
    m.addSeparator();
    m.addItem(8, "Rename block...");
    // EXPORT WHAT YOU HEAR. The take on disk is the raw generation -- it knows nothing
    // about the trim, the fades, the gain or where the audio was cut. Exporting the file
    // would hand over something different from what the canvas plays.
    m.addItem(20, "Export block...", v.block.hasAudio());
    m.addItem(21, "Export this track...");
    m.addItem(22, "Export every track...");

    juce::Component::SafePointer<CanvasView> safe (this);
    // AT THE MOUSE. withTargetComponent(this) anchors the menu to the whole canvas, which
    // is the size of the window -- so the menu opened at the canvas's top-left corner,
    // nowhere near the block you right-clicked.
    m.showMenuAsync(juce::PopupMenu::Options().withMousePosition(),
                     [safe, muted, id] (int result)
                     {
                         if (safe == nullptr || result == 0) return;
                         auto& self = *safe;
                         if (result == 1) { self.setSelectionMuted(!muted); return; }
                         if (result == 2)
                         {
                             self.pushUndo();
                             for (auto& i : self.items)
                                 if (self.selected.count(i->block.id))
                                     i->block.fadeIn = i->block.fadeOut = 0.0;
                             self.rebuildAudio(); self.markDirty(); self.repaint();
                             return;
                         }
                         if (result == 6)
                         {
                             self.pushUndo();
                             // Undoing a CUT, not an edit to the file: the take never
                             // changed, only the block's claim about where it ended.
                             for (auto& i : self.items)
                                 if (self.selected.count(i->block.id))
                                 {
                                     i->block.contentSeconds = 0.0;
                                     i->block.length = juce::jmax(i->block.length,
                                                                   self.availableSecondsOf(*i));
                                 }
                             self.rebuildAudio(); self.markDirty();
                             self.announceSelection(); self.repaint();
                             return;
                         }
                         if (result >= 100 && result < 300) { self.chooseTake(id, result); return; }
                         if (result == 8) { self.beginRenameBlock(id); return; }
                         if (result == 24) { self.analyseSelection(); return; }
                         if (result == 23)
                         {
                             self.pushUndo();
                             for (auto& i : self.items)
                                 if (self.selected.count(i->block.id) && i->block.tempo > 0.0)
                                 {
                                     // The block's own left edge, in SOURCE time -- which
                                     // is what `sourceOffset` already is. Marked human so
                                     // the next analysis does not quietly undo it.
                                     i->block.barOnePos = i->block.sourceOffset;
                                     i->block.barOneIsHuman = true;
                                 }
                             self.markDirty(); self.repaint();
                             return;
                         }
                         if (result >= 20 && result <= 22) { self.promptExport(result - 20, id); return; }
                         if (result == 3) { self.duplicateSelection(); return; }
                         if (result == 4) { self.splitAtPlayhead();   return; }
                         if (result == 7) { self.cutAtPlayhead();     return; }
                         if (result == 5) { self.removeSelected();    return; }
                         if (result >= 10 && result <= 12)
                         {
                             self.pushUndo();
                             for (auto& i : self.items)
                                 if (self.selected.count(i->block.id))
                                     i->block.fadeShape = (FadeShape) (result - 10);
                             self.rebuildAudio(); self.markDirty(); self.repaint();
                         }
                     });
}

// How much audio this block HAS to give, before its length is considered: the cut, if one
// was made with Cmd-E, otherwise whatever the file has left after the block's offset.
double CanvasView::availableSecondsOf(const Visual& v) const
{
    if (!v.block.hasAudio() || v.audioSeconds <= 0.0) return 0.0;
    const double inFile = juce::jmax(0.0, v.audioSeconds - v.block.sourceOffset);
    return v.block.contentSeconds > 0.0 ? juce::jmin(v.block.contentSeconds, inFile) : inFile;
}

// How much of this block actually sounds. CLAMPED BY THE BLOCK'S LENGTH, which is what
// makes trimming a hide rather than a cut: pull the right edge in and less of the file
// sounds, pull it back out and it is all there again.
//
// Without the clamp a trimmed block still claimed the whole file, and the waveform was
// drawn squashed -- thirty seconds of audio painted into twenty seconds of block.
double CanvasView::soundingSecondsOf(const Visual& v) const
{
    return juce::jmin(availableSecondsOf(v), v.block.length);
}

double CanvasView::tailSecondsOf(const Visual& v) const
{
    if (!v.block.hasAudio()) return 0.0;
    return juce::jmax(0.0, v.block.length - soundingSecondsOf(v));
}

CanvasView::Geometry CanvasView::selectionGeometry() const
{
    for (const auto& i : items)
        if (selected.size() == 1 && selected.count(i->block.id))
            return { i->block.length, tailSecondsOf(*i), i->block.hasAudio() };
    return { 0.0, 0.0, false };
}

void CanvasView::extendSelection(bool remix)
{
    auto* v = singleSelection();
    if (onExtendRequested == nullptr) return;
    // CONVENTION 6: NEVER SILENTLY FALL BACK. All three of these used to return with no
    // message at all, so a button that did nothing looked identical to a generation that
    // failed, and "extend does not work" had no way to become a reason. The panel's own
    // status line is where a generation's outcome already appears, so refusals go there.
    if (v == nullptr)
    {
        if (onExtendRefused) onExtendRefused("select one block to extend");
        return;
    }
    if (!v->block.hasAudio())
    {
        if (onExtendRefused) onExtendRefused("this block has no audio yet - generate first");
        return;
    }
    if (!remix && tailSecondsOf(*v) <= 0.05)
    {
        // The gesture, spelled out. The tail IS the range: without one there is nothing to
        // fill, and after an extension lands the block is full again -- which is exactly
        // the moment this fires and the moment it reads as "it stopped working".
        if (onExtendRefused)
            onExtendRefused("block is full at " + juce::String(v->block.length, 1)
                            + "s - drag its right edge out past the audio, and the gap is "
                              "what gets generated");
        return;
    }

    // THE PROMPT ON SCREEN IS THE PROMPT THAT RUNS. Extend used to re-apply the block's
    // stored recipe first, which overwrote whatever you had just typed -- you edited the
    // prompt, pressed the button, and watched your edit disappear and the old trigger
    // generate again. A visible, editable field that is silently ignored is worse than no
    // field at all.
    //
    // Capturing it into the block first is what keeps the block's record honest: what it
    // says it was made with is what it was actually made with.
    syncPanelSettings();

    // Where the audio runs out INSIDE the block. Trimming the left edge moves the offset
    // rather than the audio, so the sounding part is shorter than the file by exactly that
    // offset -- and the range has to start where you can hear it stop, not where the file
    // does.
    // Where the audio ends INSIDE the block -- the cut, when you made one. A take that
    // ended in ten seconds of silence used to hand the inpainter a range starting after
    // the silence, so the silence stayed baked in and the continuation began late.
    const double sounding = soundingSecondsOf(*v);
    onExtendRequested(v->block.file, sounding, v->block.length, remix);
}

void CanvasView::announceSelection()
{
    // Called at every selection change, which is what keeps Extend and Remix honest: they
    // are about ONE block with an empty tail, so a marquee that picks up three blocks has
    // to switch them off again.
    if (onBlockGeometry == nullptr) return;
    const auto g = selectionGeometry();
    onBlockGeometry(g.length, g.tail, g.hasAudio);
}

void CanvasView::syncPanelSettings()
{
    if (onCaptureSettings == nullptr || panelBlockId == 0) return;
    for (auto& i : items)
        if (i->block.id == panelBlockId) { i->settings = onCaptureSettings(); return; }
}

void CanvasView::pointPanelAt(const Visual* v)
{
    if (onOpenGenerator == nullptr) return;

    // Whatever is on screen belongs to the block we are LEAVING. Taken before anything is
    // replaced, or a prompt typed and then clicked away from is simply lost.
    syncPanelSettings();

    if (v == nullptr)
    {
        panelBlockId = 0;
        onOpenGenerator({}, {}, {});
        if (onBlockGeometry) onBlockGeometry(0.0, 0.0, false);
        return;
    }
    const auto folder = blockFolderFor(*v);
    if (folder == juce::File())
    {
        // NEVER SILENTLY. A block with no project has nowhere to put audio, and the panel
        // has to say that rather than leave Generate grey with no explanation.
        panelBlockId = v->block.id;
        onOpenGenerator(v->block.name + "  -  no project yet", {}, {});
        if (onBlockGeometry) onBlockGeometry(0.0, 0.0, false);
        return;
    }

    // A block the document has never carried settings for gets them from ITS OWN TAKE --
    // the `.json` sidecar written beside every generated wav, which is the recipe that
    // made exactly this sound. Inheriting whatever was on screen instead is what made two
    // blocks with completely different audio show one identical prompt: every block on a
    // freshly opened project had no stored settings, so every block copied the last one
    // looked at, and nothing ever appeared to change but the title.
    //
    // With no take and no sidecar the generator opens EMPTY. It used to inherit whatever
    // was on screen, which meant a brand-new block arrived carrying the last block's
    // prompt and LoRAs -- a recipe nobody chose for it, ready to generate from by
    // accident. Copying a previous block's settings is what Duplicate is for, and it
    // copies them explicitly.
    auto* mutableV = const_cast<Visual*>(v);
    if (mutableV->settings.isVoid() && v->block.hasAudio())
    {
        auto sidecar = v->block.file.withFileExtension("json");
        if (sidecar.existsAsFile())
        {
            auto parsed = juce::JSON::parse(sidecar.loadFileAsString());
            if (parsed.isObject()) mutableV->settings = parsed;
        }
    }
    if (mutableV->settings.isVoid()) mutableV->settings = emptyRecipe();

    panelBlockId = v->block.id;
    onOpenGenerator(v->block.name
                        + (v->block.hasAudio()
                               ? "  -  " + v->block.file.getFileNameWithoutExtension()
                               : juce::String("  -  empty")),
                    folder, v->settings);
    // AFTER the settings. applySettings restores the `seconds` the block was last
    // generated at, and the block's length is the newer answer -- you resized the frame
    // since then, and the frame is what says how long the part should be.
    if (onBlockGeometry) onBlockGeometry(v->block.length, tailSecondsOf(*v), v->block.hasAudio());
}

void CanvasView::adoptTake(const juce::File& folder, const juce::File& take)
{
    // A generation IS an edit. The wav stays on disk whatever happens, so undo here means
    // "put the block back on the take it was showing" -- which is exactly the answer to
    // "I tried an extend and I do not want to keep it".
    bool pushed = false;
    for (auto& i : items)
        // Convention 9: never `==` on paths. A block named with an accent in it produces
        // one byte sequence here and another from whatever handed us `folder`.
        if (mira::pathsEquivalent(blockFolderFor(*i).getFullPathName().toStdString(),
                                  folder.getFullPathName().toStdString()))
        {
            if (!pushed) { pushUndo(); pushed = true; }
            setFileOn(*i, take);
            // Said the moment it lands, not discovered later by looking at a flat line.
            if (onTakeNote != nullptr && i->audioSeconds > 0.0)
            {
                const double audible = audibleEndOf(formats, take);
                if (audible < i->audioSeconds * 0.95 - 0.5)
                    onTakeNote("audio ends at " + juce::String(audible, 1) + "s of "
                               + juce::String(i->audioSeconds, 1)
                               + "s - put the playhead there and Cmd-E to cut, then extend "
                                 "from real audio rather than from the silence");
            }
            rebuildAudio();
            markDirty();
            if (onBlockGeometry) onBlockGeometry(i->block.length, tailSecondsOf(*i), i->block.hasAudio());
            repaint();
            return;
        }
}

// --- the document ---------------------------------------------------------------------
//
// One .mira file per project, beside the block folders it names. Takes are NOT listed --
// they are whatever is in a block's folder, so a take added or removed outside mira is
// simply seen next time.

juce::String CanvasView::getDocumentName() const
{
    return documentFile != juce::File() ? documentFile.getFileNameWithoutExtension()
                                        : juce::String("Untitled");
}

void CanvasView::markDirty()
{
    if (dirty) return;
    dirty = true;
    if (onDocumentChanged) onDocumentChanged();
}

void CanvasView::writeTo(const juce::File& miraFile) const
{
    miraFile.replaceWithText(toJson(miraFile.getParentDirectory()));
}

juce::String CanvasView::toJson(const juce::File& base) const
{
    juce::Array<juce::var> blocks;
    for (const auto& i : items)
    {
        auto* o = new juce::DynamicObject();
        o->setProperty("name", i->block.name);
        o->setProperty("lane", i->block.lane);
        o->setProperty("start", i->block.start);
        o->setProperty("length", i->block.length);
        o->setProperty("offset", i->block.sourceOffset);
        o->setProperty("content", i->block.contentSeconds);
        o->setProperty("fadeIn", i->block.fadeIn);
        o->setProperty("fadeOut", i->block.fadeOut);
        o->setProperty("gainDb", i->block.gainDb);
        o->setProperty("colour", i->block.colour);
        o->setProperty("muted", i->block.muted);
        o->setProperty("fadeShape", (int) i->block.fadeShape);
        // Musical time (MIRA-BLOCKS.md §8), written only when a block HAS any. A document
        // whose blocks never learned a tempo grows no new keys, and one written before
        // this existed reads back exactly as it does today -- additive, the way `video`
        // was, not a migration.
        if (i->block.tempo > 0.0 || i->block.key.isNotEmpty() || i->block.barOneIsHuman
            || i->block.followsBlockId != 0 || !i->block.slices.empty())
        {
            o->setProperty("tempo", i->block.tempo);
            o->setProperty("meter", i->block.meter);
            o->setProperty("barOne", i->block.barOnePos);
            o->setProperty("key", i->block.key);
            o->setProperty("tempoSource", i->block.tempoSource);
            o->setProperty("tempoConfidence", i->block.tempoConfidence);
            o->setProperty("barOneIsHuman", i->block.barOneIsHuman);
            // The parent is written as its INDEX in this array, not as its id. Ids are
            // handed out fresh on every load (`nextId++` in fromJson), so a saved id
            // would point at whatever block happened to take that number next time --
            // the same trap `audioBlockId` is left unwritten to avoid. The index is
            // stable because this loop writes the blocks in the order fromJson reads
            // them, and it is remapped back to an id there.
            if (i->block.followsBlockId != 0)
            {
                int parent = -1, n = 0;
                for (const auto& j : items)
                {
                    if (j->block.id == i->block.followsBlockId) { parent = n; break; }
                    ++n;
                }
                // A link whose parent is gone is not written at all, rather than written
                // as -1 and resolved to "independent" on load: the two are the same
                // answer, and only one of them can be read as a real relationship.
                if (parent >= 0) o->setProperty("follows", parent);
            }
            if (!i->block.slices.empty())
            {
                juce::Array<juce::var> sl;
                for (const auto& s : i->block.slices)
                {
                    auto* so = new juce::DynamicObject();
                    so->setProperty("src", s.sourceStart);
                    so->setProperty("len", s.sourceLength);
                    so->setProperty("at", s.placeAt);
                    so->setProperty("gainDb", s.gainDb);
                    so->setProperty("muted", s.muted);
                    sl.add(juce::var(so));
                }
                o->setProperty("slices", juce::var(sl));
            }
        }
        if (i->block.hasAudio())
        {
            // Relative when it lives under the document, absolute when it does not. A
            // project you can rename or move to another drive is the difference between a
            // document and a folder with a pointer in it.
            const auto full = i->block.file.getFullPathName();
            const bool inside = full.startsWith(base.getFullPathName() + "/");
            o->setProperty("file", inside ? i->block.file.getRelativePathFrom(base) : full);
        }
        if (!i->settings.isVoid()) o->setProperty("settings", i->settings);
        blocks.add(juce::var(o));
    }

    juce::Array<juce::var> names;
    for (const auto& n : laneNames) names.add(n);
    juce::Array<juce::var> gains;
    for (auto g : laneDb) gains.add(g);

    auto* root = new juce::DynamicObject();
    root->setProperty("format", "mira-canvas");
    root->setProperty("version", 1);
    root->setProperty("blocks", juce::var(blocks));
    root->setProperty("laneNames", juce::var(names));
    root->setProperty("laneGainDb", juce::var(gains));
    root->setProperty("laneCount", laneCount);
    // A per-lane height, 0 meaning "follow the zoom". Written as an array beside the gains
    // because it belongs to the track exactly as much as its fader does.
    {
        juce::Array<juce::var> heights;
        for (int i = 0; i < laneCount; ++i)
            heights.add(i < (int) laneH.size() ? laneH[(size_t) i] : 0);
        root->setProperty("laneHeights", juce::var(heights));
    }
    root->setProperty("waveZoom", (double) waveZoom);
    root->setProperty("ruler", rulerMode == Ruler::Timecode ? "timecode" : "seconds");
    // Kept even with no clip: a canvas laid out against a timecode an editor read down the
    // phone should not lose it the moment the film is detached.
    root->setProperty("tcFps", fallbackFps);
    root->setProperty("tcDrop", fallbackDrop);
    root->setProperty("tcStart", fallbackStart);
    {
        juce::Array<juce::var> ms;
        for (const auto& m : markers)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty("at", m.seconds);
            o->setProperty("name", m.name);
            ms.add(juce::var(o));
        }
        if (!ms.isEmpty()) root->setProperty("markers", juce::var(ms));
    }
    root->setProperty("muteMask", juce::String(muteMask));

    // A document with no `video` array opens exactly as it does today -- that is what
    // makes picture additive rather than a migration. The array is written only when
    // there IS a clip, so nothing that never saw a video grows an empty one.
    if (!videoClips.empty())
    {
        juce::Array<juce::var> clips;
        for (const auto& c : videoClips)
        {
            auto* o = new juce::DynamicObject();
            const auto full = c.file.getFullPathName();
            const bool inside = full.startsWith(base.getFullPathName() + "/");
            o->setProperty("file", inside ? c.file.getRelativePathFrom(base) : full);
            o->setProperty("start", c.start);
            o->setProperty("length", c.length);
            o->setProperty("offset", c.sourceOffset);
            o->setProperty("fps", c.fps);
            o->setProperty("dropFrame", c.dropFrame);
            o->setProperty("startTimecode", c.startTimecode);
            // `audioBlockId` is NOT written. Block ids are handed out fresh on every load
            // (`nextId++` in fromJson), so a saved id would point at whatever block
            // happened to take that number next time. The link is rebuilt structurally on
            // load instead -- a block on the reference lane starting where the clip does.
            clips.add(juce::var(o));
        }
        root->setProperty("video", juce::var(clips));
    }
    // Which lane the film's audio is on. Written whether or not there is a clip right now,
    // because a document mid-edit can hold one without the other and -1 is a real answer.
    root->setProperty("referenceLane", referenceLane);
    return juce::JSON::toString(juce::var(root), false);
}

bool CanvasView::readFrom(const juce::File& miraFile)
{
    return fromJson(miraFile.loadFileAsString(), miraFile.getParentDirectory(), true);
}

bool CanvasView::fromJson(const juce::String& json, const juce::File& base, bool refit)
{
    items.clear();
    selected.clear();
    panelBlockId = 0;      // the block it was showing no longer exists
    // Remembered so the reload below can tell "a document brought a different film" from
    // "an undo left the same one in place". Reopening a 40-minute film to undo a fade
    // would be a three-minute undo.
    const auto previousVideo = videoClips.empty() ? juce::File() : videoClips.front().file;
    videoClips.clear();
    referenceLane = -1;
    laneNames.clear();
    laneDb.clear();
    muteMask = soloMask = 0;
    laneCount = 1;

    const auto root = juce::JSON::parse(json);
    if (!root.isObject()) return false;

    if (auto* names = root.getProperty("laneNames", {}).getArray())
        for (int i = 0; i < names->size(); ++i) laneNames.set(i, (*names)[i].toString());
    if (auto* gains = root.getProperty("laneGainDb", {}).getArray())
        for (int i = 0; i < gains->size(); ++i) setLaneDb(i, (double) (*gains)[i]);
    laneH.clear();
    // Whether the KEY was there, not whether the array has a value: a document written
    // before per-lane heights existed has no opinion about them, while one written after
    // has said 0 on purpose. Without that distinction, unlocking the reference would be
    // undone by the next reload -- a setting that will not stay off.
    const bool documentKnowsHeights = root.getProperty("laneHeights", {}).isArray();
    if (auto* heights = root.getProperty("laneHeights", {}).getArray())
        for (int i = 0; i < heights->size(); ++i)
        {
            const int h = (int) (*heights)[i];
            laneH.push_back(h > 0 ? juce::jlimit(kLaneMin, kLaneMax, h) : 0);
        }
    muteMask = (juce::uint64) root.getProperty("muteMask", "0").toString().getLargeIntValue();
    laneCount = juce::jlimit(1, CanvasAudioSource::kMaxLanes, (int) root.getProperty("laneCount", 1));
    referenceLane = (int) root.getProperty("referenceLane", -1);
    if (referenceLane >= laneCount) referenceLane = -1;   // a document that lost its film
    waveZoom = juce::jlimit(0.15f, 16.0f, (float) (double) root.getProperty("waveZoom", 1.0));
    rulerMode = root.getProperty("ruler", "seconds").toString() == "timecode" ? Ruler::Timecode
                                                                              : Ruler::Seconds;
    fallbackFps = juce::jlimit(1.0, 240.0, (double) root.getProperty("tcFps", 25.0));
    fallbackDrop = (bool) root.getProperty("tcDrop", false);
    fallbackStart = juce::jmax(0.0, (double) root.getProperty("tcStart", 0.0));
    markers.clear();
    if (auto* ms = root.getProperty("markers", {}).getArray())
        for (const auto& mv : *ms)
        {
            Marker m;
            m.seconds = juce::jmax(0.0, (double) mv.getProperty("at", 0.0));
            m.name = mv.getProperty("name", "marker").toString();
            markers.push_back(m);
        }
    std::sort(markers.begin(), markers.end(),
               [](const Marker& a, const Marker& b) { return a.seconds < b.seconds; });

    // One entry per loaded block, in the same order, holding the index of the block it
    // follows (-1 for none). Resolved to real ids after the loop, when they all exist.
    std::vector<int> followsIndex;

    if (auto* blocks = root.getProperty("blocks", {}).getArray())
        for (const auto& b : *blocks)
        {
            auto v = std::make_unique<Visual>();
            v->block.name = b.getProperty("name", "block").toString();
            v->block.lane = (int) b.getProperty("lane", 0);
            v->block.start = (double) b.getProperty("start", 0.0);
            v->block.length = (double) b.getProperty("length", 8.0);
            v->block.sourceOffset = (double) b.getProperty("offset", 0.0);
            v->block.contentSeconds = (double) b.getProperty("content", 0.0);
            v->block.fadeIn = (double) b.getProperty("fadeIn", 0.0);
            v->block.fadeOut = (double) b.getProperty("fadeOut", 0.0);
            v->block.gainDb = (double) b.getProperty("gainDb", 0.0);
            // Documents written before blocks owned a colour fall back to their track,
            // which is exactly what they looked like when they were saved.
            v->block.colour = (int) b.getProperty("colour", v->block.lane);
            v->block.muted = (bool) b.getProperty("muted", false);
            v->block.fadeShape = (FadeShape) juce::jlimit(0, 2, (int) b.getProperty("fadeShape", 0));
            // Musical time. Every default here is what a document written before
            // MIRA-BLOCKS existed means: no tempo, no measurement, no parent, no slices.
            v->block.tempo = juce::jmax(0.0, (double) b.getProperty("tempo", 0.0));
            v->block.meter = juce::jlimit(1, 32, (int) b.getProperty("meter", 4));
            v->block.barOnePos = juce::jmax(0.0, (double) b.getProperty("barOne", 0.0));
            v->block.key = b.getProperty("key", "").toString();
            v->block.tempoSource = b.getProperty("tempoSource", "").toString();
            v->block.tempoConfidence = juce::jlimit(0.0, 1.0,
                                                    (double) b.getProperty("tempoConfidence", 0.0));
            v->block.barOneIsHuman = (bool) b.getProperty("barOneIsHuman", false);
            if (auto* sl = b.getProperty("slices", {}).getArray())
                for (const auto& sv : *sl)
                {
                    Slice s;
                    s.sourceStart = juce::jmax(0.0, (double) sv.getProperty("src", 0.0));
                    s.sourceLength = juce::jmax(0.0, (double) sv.getProperty("len", 0.0));
                    s.placeAt = (double) sv.getProperty("at", 0.0);
                    s.gainDb = (double) sv.getProperty("gainDb", 0.0);
                    s.muted = (bool) sv.getProperty("muted", false);
                    if (s.sourceLength > 0.0) v->block.slices.push_back(s);
                }
            // Held as the written INDEX and turned into an id once every block has one --
            // see the note in toJson. Stored in the block's own field meanwhile because
            // the ids handed out below are all >= 1, so a small index can never be
            // mistaken for one once the remap has run.
            followsIndex.push_back((int) b.getProperty("follows", -1));
            v->block.id = nextId++;
            v->settings = b.getProperty("settings", {});
            laneCount = juce::jmax(laneCount, v->block.lane + 1);

            const auto path = b.getProperty("file", "").toString();
            // A take that has been moved or deleted leaves the block EMPTY rather than
            // silently vanishing: the frame and its generator are still what you meant.
            if (path.isNotEmpty())
                setFileOn(*v, juce::File::isAbsolutePath(path) ? juce::File(path)
                                                               : base.getChildFile(path));
            items.push_back(std::move(v));
        }

    for (size_t n = 0; n < followsIndex.size() && n < items.size(); ++n)
    {
        const int parent = followsIndex[n];
        // A parent that points at itself or outside the array is dropped rather than
        // stored: a self-link is the shortest possible cycle, and step 4.2 refuses cycles
        // at the point they are made. Nothing should be able to get in through the door.
        if (parent >= 0 && parent < (int) items.size() && (size_t) parent != n)
            items[n]->block.followsBlockId = items[(size_t) parent]->block.id;
    }

    if (auto* clips = root.getProperty("video", {}).getArray())
        for (const auto& c : *clips)
        {
            VideoClip v;
            const auto path = c.getProperty("file", "").toString();
            if (path.isEmpty()) continue;
            v.file = juce::File::isAbsolutePath(path) ? juce::File(path) : base.getChildFile(path);
            v.start = (double) c.getProperty("start", 0.0);
            v.length = (double) c.getProperty("length", 0.0);
            v.sourceOffset = (double) c.getProperty("offset", 0.0);
            v.fps = (double) c.getProperty("fps", 0.0);
            v.dropFrame = (bool) c.getProperty("dropFrame", false);
            v.startTimecode = (double) c.getProperty("startTimecode", 0.0);
            videoClips.push_back(v);   // audioBlockId is relinked below, not read
        }

    // Convention 9: never compare two paths with ==. The document holds the path as it
    // was written; the window holds the path as the chooser gave it.
    if (videoClips.empty())
    {
        if (previousVideo.getFullPathName().isNotEmpty() && onVideoCleared) onVideoCleared();
    }
    else
    {
        const bool differentFilm =
            !mira::pathsEquivalent(videoClips.front().file.getFullPathName().toStdString(),
                                    previousVideo.getFullPathName().toStdString());
        // `|| referenceLane < 0` is not belt and braces -- it is a bug that was on screen.
        // A canvas opens its project TWICE at launch (showCanvasWindow loads the current
        // project, then the launch window's handler loads the chosen one), and the second
        // pass wiped the reference lane this pass had just built while the "same film,
        // do not reload" guard correctly said nothing had changed. The guard is about the
        // PICTURE, which is expensive to reopen; the reference is rebuilt state, and
        // whether it is missing is a different question from whether the film changed.
        // Any clip without a reference block, or a film that changed, means the listener
        // has work to do. With several clips the question is no longer "did the film
        // change" but "is anything missing", which is the same question the first fix made
        // it: `referenceLane < 0` was that question asked for exactly one clip.
        // Rebuild clip -> reference-block links from the geometry, because ids are not
        // stable across a load.
        for (auto& c : videoClips)
        {
            c.audioBlockId = 0;
            for (const auto& i : items)
                if (i->block.lane == referenceLane && std::abs(i->block.start - c.start) < 0.001)
                { c.audioBlockId = i->block.id; break; }
        }

        bool referenceMissing = referenceLane < 0;
        for (const auto& c : videoClips) referenceMissing = referenceMissing || c.audioBlockId == 0;
        if (differentFilm || referenceMissing)
            if (onVideoClipsChanged) onVideoClipsChanged();
    }

    // The reference lane is locked BY DEFAULT, and that has to hold for documents that
    // predate the setting too -- otherwise the one track the feature was asked for is the
    // one track that does not have it.
    if (referenceLane >= 0 && !documentKnowsHeights)
    {
        ensureLaneArrays();
        if (juce::isPositiveAndBelow(referenceLane, (int) laneH.size()))
            laneH[(size_t) referenceLane] = kReferenceHeight;
    }

    applyMasks();
    // An UNDO must not move the view. Refitting after every undone edit would answer a
    // question nobody asked -- you undid a trim, not a zoom -- and lose the place you were
    // looking at, which is the one thing undo is supposed to give back.
    if (refit && !items.empty()) fit();
    rebuildAudio();
    repaint();
    return true;
}

// ---- undo ---------------------------------------------------------------------------
//
// Snapshots, not a command log. The document already serialises to JSON and back, so the
// cheapest correct undo is to keep the JSON: no per-edit inverse to write, and no edit
// that can be added later and forgotten about here. A canvas of a few dozen blocks is a
// few kilobytes, which is nothing next to the audio it points at.
//
// What it CANNOT undo is a generation -- the wav is on disk and stays there. What it does
// instead is exactly what you want after a bad extend: the block goes back to the take it
// was showing, and the new one is still in the folder if you change your mind.

void CanvasView::pushUndo()
{
    if (undoSuppressed) return;

    // Selection by NAME. Ids are handed out fresh on every load, so an id snapshotted now
    // means nothing after a restore; names are what the document actually carries.
    Snapshot snap;
    snap.json = toJson(projectFolder);
    for (const auto& i : items)
        if (selected.count(i->block.id)) snap.selection.add(i->block.name);

    undoStack.push_back(std::move(snap));
    if ((int) undoStack.size() > kUndoDepth) undoStack.erase(undoStack.begin());
    redoStack.clear();      // a new edit is a new branch
}

void CanvasView::restore(const Snapshot& snap)
{
    fromJson(snap.json, projectFolder, false);
    selected.clear();
    for (auto& i : items)
        if (snap.selection.contains(i->block.name)) selected.insert(i->block.id);

    // The panel has to be repointed: every Visual is new, so the id it was holding is
    // gone. Pointing it at the restored selection keeps "undo, then look at what came
    // back" from needing a click.
    panelBlockId = 0;
    pointPanelAt(singleSelection());
    announceSelection();
    markDirty();
    repaint();
}

void CanvasView::undo()
{
    if (undoStack.empty()) return;
    Snapshot now;
    now.json = toJson(projectFolder);
    for (const auto& i : items)
        if (selected.count(i->block.id)) now.selection.add(i->block.name);
    redoStack.push_back(std::move(now));

    auto snap = undoStack.back();
    undoStack.pop_back();
    restore(snap);
}

void CanvasView::redo()
{
    if (redoStack.empty()) return;
    Snapshot now;
    now.json = toJson(projectFolder);
    for (const auto& i : items)
        if (selected.count(i->block.id)) now.selection.add(i->block.name);
    undoStack.push_back(std::move(now));

    auto snap = redoStack.back();
    redoStack.pop_back();
    restore(snap);
}

bool CanvasView::newDocument(const juce::File& folder, const juce::String& name)
{
    const auto clean = juce::File::createLegalFileName(name.trim());
    if (clean.isEmpty()) return false;
    const auto root = folder.getChildFile(clean);
    if (!root.isDirectory() && !root.createDirectory().wasOk()) return false;

    items.clear(); selected.clear(); laneNames.clear(); laneDb.clear();
    muteMask = soloMask = 0;
    laneCount = 1;
    projectFolder = root;
    documentFile = root.getChildFile(clean + kExtension);
    dirty = false;
    writeTo(documentFile);
    applyMasks();
    rebuildAudio();
    if (onDocumentChanged) onDocumentChanged();
    repaint();
    return true;
}

bool CanvasView::openDocument(const juce::File& miraFile)
{
    if (!miraFile.existsAsFile()) return false;
    projectFolder = miraFile.getParentDirectory();
    documentFile = miraFile;
    if (!readFrom(miraFile)) return false;
    dirty = false;
    if (onDocumentChanged) onDocumentChanged();
    return true;
}

bool CanvasView::saveDocument()
{
    if (documentFile == juce::File()) return false;   // caller has to ask where
    // What is on screen has not reached its block until now if you never clicked away.
    syncPanelSettings();
    writeTo(documentFile);
    dirty = false;
    if (onDocumentChanged) onDocumentChanged();
    return true;
}

bool CanvasView::saveDocumentAs(const juce::File& miraFile)
{
    auto target = miraFile;
    if (!target.getFileName().endsWithIgnoreCase(kExtension))
        target = target.getParentDirectory().getChildFile(target.getFileName() + kExtension);
    projectFolder = target.getParentDirectory();
    documentFile = target;
    return saveDocument();
}

void CanvasView::clearAll()
{
    items.clear();
    selected.clear();
    laneNames.clear();
    muteMask = soloMask = 0;
    applyMasks();
    rebuildAudio();
    repaint();
}

// Whether this block draws a grid at all, and how much room it takes.
//
// Below ~5 px the lines sit closer together than they are wide and the footer becomes a
// grey band that says nothing -- so beats drop out first, then bars, then the footer
// itself. A grid you cannot count is not a smaller grid, it is noise.
int CanvasView::gridFooterHeight(const Visual& v, int blockHeight) const
{
    if (blockHeight < kGridFooterMin) return 0;
    // ONSETS KEEP THE FOOTER ALIVE WITH NO TEMPO (2.5). That is not a special case, it is
    // the case that matters most: a take whose grid the gate refused often has no tempo at
    // all, and its onsets are then the only honest thing on screen about its timing.
    if (onsetTicksVisible(v)) return kGridFooterHeight;
    if (v.block.tempo <= 0.0) return 0;
    const double pxPerBeat = (60.0 / v.block.tempo) * pixelsPerSecond;
    if (pxPerBeat < 5.0 && pxPerBeat * juce::jmax(1, v.block.meter) < 5.0) return 0;
    return kGridFooterHeight;
}

// Whether the onset ticks are worth drawing at this zoom -- the same kind of density
// decision the beats and bars already make, and asked in ONE place for the same reason:
// gridFooterHeight reserves the strip and paintBlockGrid fills it, and if they disagreed
// the block would grow an empty band or lose the ticks it made room for.
//
// Three pixels apart on average. Below that a run of transients is a grey smear that says
// "there is audio here", which the waveform three pixels above already says better.
bool CanvasView::onsetTicksVisible(const Visual& v) const
{
    if (v.onsets.empty()) return false;
    const double from = v.block.sourceOffset;
    const double to   = from + juce::jmax(0.0, v.block.length);
    int n = 0;
    for (const auto t : v.onsets) if (t >= from && t <= to) ++n;
    if (n == 0) return false;
    return (double) n * 3.0 <= v.block.length * pixelsPerSecond;
}

// The grid, drawn as a FOOTER along the bottom of the block (MIRA-BLOCKS.md 1.3).
//
// A footer and not an overlay across the waveform: the waveform is what you read to find
// a transient by eye, and beat lines through it are the thing that makes a drawn grid
// start to feel like a grid you have to obey. Along the bottom it is scaffolding you can
// glance at and ignore, which is the whole rule -- THE GRID IS DRAWN, NEVER ENFORCED.
//
// Bar 1 is held in SOURCE time, so the timeline position of a source second is
// `block.start + (s - block.sourceOffset)`. That indirection is the point: trimming the
// left edge moves `sourceOffset` and the grid stays on the music rather than sliding with
// the edge.
void CanvasView::paintBlockGrid(juce::Graphics& g, const Visual& v, juce::Rectangle<int> r,
                                juce::Colour tint, bool isSelected)
{
    if (gridFooterHeight(v, r.getHeight()) <= 0) return;

    // The footer can exist for the onsets alone, so the strip and its ticks are drawn
    // before anything asks about a tempo -- there may not be one.
    {
        auto foot = r.withTop(r.getBottom() - kGridFooterHeight).reduced(1, 0);
        g.setColour(tint.withAlpha(0.18f));
        g.fillRect(foot);

        if (onsetTicksVisible(v))
        {
            // ONSETS, drawn from the BOTTOM up and shorter than a beat line, so a tick
            // that happens to land on a beat reads as two marks rather than one longer
            // one. They are the slice points step 5 will cut at: seeing them is how you
            // know in advance whether slicing this take will work.
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion(foot);
            g.setColour(MiraLookAndFeel::accent.withAlpha(isSelected ? 0.55f : 0.38f));
            const double from = v.block.sourceOffset;
            const double to   = from + juce::jmax(0.0, v.block.length);
            const float bottom = (float) foot.getBottom();
            const float top    = bottom - foot.getHeight() * 0.38f;
            for (const auto t : v.onsets)
            {
                if (t < from || t > to) continue;
                const float x = (float) secondsToX(v.block.start + (t - from));
                if (x < (float) foot.getX() - 1.0f || x > (float) foot.getRight()) continue;
                g.drawLine(x, top, x, bottom, 1.0f);
            }
        }
    }

    if (v.block.tempo <= 0.0) return;
    const double spb = 60.0 / v.block.tempo;
    const double pxPerBeat = spb * pixelsPerSecond;
    const bool beats = pxPerBeat >= 5.0;
    const int meter = juce::jmax(1, v.block.meter);

    // The strip is already filled above; this is the grid drawn INTO it.
    auto foot = r.removeFromBottom(kGridFooterHeight).reduced(1, 0);
    juce::Graphics::ScopedSaveState clip (g);
    g.reduceClipRegion(foot);

    // Bar 1 on the TIMELINE, and the first beat at or left of the block's left edge. The
    // floor is done in beats rather than by walking from bar 1, so a block whose bar 1
    // sits far off screen costs the same as one whose does not.
    const double barOneOnTimeline = v.block.start + (v.block.barOnePos - v.block.sourceOffset);
    const double blockEnd = v.block.end();
    const long long firstBeat = (long long) std::floor((v.block.start - barOneOnTimeline) / spb);
    const long long lastBeat  = (long long) std::ceil((blockEnd - barOneOnTimeline) / spb);
    // A tempo typed as 0.01 would ask for millions of lines. The cap is a refusal to draw
    // rather than a clamp on the tempo: the number you typed is still the number shown.
    if (lastBeat - firstBeat > 20000) return;

    const float top = (float) foot.getY(), bottom = (float) foot.getBottom();
    const bool numbers = pxPerBeat * meter >= 26.0 && foot.getHeight() >= 11;

    for (long long n = firstBeat; n <= lastBeat; ++n)
    {
        // Floored division, so bars keep counting the right way to the LEFT of bar 1 --
        // a pickup before the downbeat is negative, not bar 1 twice.
        const long long bar = (n >= 0 ? n / meter : -(((-n) + meter - 1) / meter));
        const bool isBar = (n - bar * meter) == 0;
        if (!isBar && !beats) continue;

        const float x = (float) secondsToX(barOneOnTimeline + (double) n * spb);
        if (x < (float) foot.getX() - 1.0f || x > (float) foot.getRight()) continue;

        g.setColour(isBar ? tint.brighter(0.5f).withAlpha(isSelected ? 0.95f : 0.7f)
                          : tint.brighter(0.2f).withAlpha(isSelected ? 0.5f : 0.35f));
        g.drawLine(x, isBar ? top : top + foot.getHeight() * 0.45f, x, bottom, isBar ? 1.2f : 0.8f);

        // Bars BEFORE bar 1 are drawn but not numbered, the same way the waveform's bars
        // ruler leaves a pickup unnumbered: "bar 0" and "bar -1" are arithmetic, not
        // things anyone says out loud about music.
        if (isBar && numbers && bar >= 0)
        {
            g.setColour(MiraLookAndFeel::text.withAlpha(isSelected ? 0.7f : 0.45f));
            g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(8.5f)));
            g.drawText(juce::String((long long) (bar + 1)),
                        juce::Rectangle<float>(x + 2.0f, top, 22.0f, (float) foot.getHeight())
                            .toNearestInt(),
                        juce::Justification::centredLeft, false);
        }
    }
}

juce::Rectangle<int> CanvasView::boundsOf(const Visual& v) const
{
    const int x = secondsToX(v.block.start);
    const int w = juce::jmax(3, juce::roundToInt(v.block.length * pixelsPerSecond));
    return { x, laneToY(v.block.lane) + 3, w, laneHeightOf(v.block.lane) - 6 };
}

// ---- painting ----------------------------------------------------------------------

void CanvasView::paint(juce::Graphics& g)
{
    g.fillAll(MiraLookAndFeel::surface2);

    const int lanes = laneCount;
    for (int lane = 0; lane < lanes; ++lane)
    {
        // EACH TRACK IS ITS OWN SLAB, with a gap between it and the next. A row of
        // alternating tints separated by a hairline read as one striped surface -- "the
        // track looks joined with other track" -- and a track is the thing you mix with,
        // so it has to look like a thing.
        auto r = juce::Rectangle<int>(0, laneToY(lane), getWidth(), laneHeightOf(lane)).reduced(0, 2);
        const bool chosen = lane == selectedLane;
        g.setColour(chosen ? laneColour(lane).withAlpha(0.10f)
                           : MiraLookAndFeel::surface.withAlpha(0.55f));
        g.fillRect(r);
        // A colour rail down the left of the lane body, so which track a block is on is
        // answerable from the canvas as well as from the header.
        g.setColour(laneColour(lane).withAlpha(chosen ? 0.9f : 0.35f));
        g.fillRect(kHeaderWidth, r.getY(), 2, r.getHeight());
        if (chosen)
        {
            g.setColour(laneColour(lane).withAlpha(0.55f));
            g.drawRect(r, 1);
        }
    }

    // The loop region, under everything, so a block sitting in it still reads normally.
    if (loopEnd > loopStart)
    {
        const int a = secondsToX(loopStart), b = secondsToX(loopEnd);
        g.setColour(MiraLookAndFeel::accent.withAlpha(player.isLooping() ? 0.14f : 0.06f));
        g.fillRect(a, topRuler, juce::jmax(1, b - a), getHeight() - topRuler);
    }

    // --- ruler. SECONDS, not bars. There is no tempo on this canvas and inventing one to
    // have something to draw would be the exact lie Blockhead avoids: the grid comes from
    // the music, once there is music, or it does not come at all.
    {
        auto r = juce::Rectangle<int>(0, 0, getWidth(), topRuler);
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(r);
        g.setColour(MiraLookAndFeel::border);
        g.drawHorizontalLine(topRuler - 1, 0.0f, static_cast<float>(getWidth()));

        // A tick spacing that stays legible at any zoom, chosen from the 1-2-5 ladder
        // rather than a fixed number of seconds.
        static const double steps[] = { 0.1, 0.25, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600 };
        const bool asTimecode = rulerMode == Ruler::Timecode;
        // A timecode label is 11 characters against five, so it needs more room before the
        // labels start colliding. Same ladder, a wider gate.
        const double minGap = asTimecode ? 108.0 : 64.0;
        double step = 600.0;
        for (double s : steps) if (s * pixelsPerSecond >= minGap) { step = s; break; }

        g.setFont(laf.monoRegular(MiraLookAndFeel::textSize(9.5f)));
        const double first = std::floor(viewStart / step) * step;
        for (double t = first; secondsToX(t) < getWidth(); t += step)
        {
            const int x = secondsToX(t);
            if (x < kHeaderWidth) continue;
            g.setColour(MiraLookAndFeel::border);
            g.drawVerticalLine(x, 0.0f, static_cast<float>(topRuler));
            g.setColour(asTimecode ? MiraLookAndFeel::textDim.brighter(0.15f) : MiraLookAndFeel::textDim);
            g.drawText(formatPosition(t), x + 3, 0, asTimecode ? 96 : 60, topRuler,
                        juce::Justification::centredLeft, false);
        }
    }

    paintVideoStrip(g);

    // --- blocks
    for (const auto& item : items)
    {
        auto r = boundsOf(*item);
        if (r.getRight() < 0 || r.getX() > getWidth()) continue;
        const bool isSelected = selected.count(item->block.id) > 0;

        const bool laneMuted = item->block.lane < CanvasAudioSource::kMaxLanes
                            && ((muteMask & (juce::uint64 (1) << item->block.lane)) != 0
                                || (soloMask != 0
                                    && (soloMask & (juce::uint64 (1) << item->block.lane)) == 0));

        // An EMPTY block is a frame, not a slab: it is a length you meant with nothing in
        // it yet, and it has to read as waiting rather than as silent audio.
        if (!item->block.hasAudio())
        {
            g.setColour(MiraLookAndFeel::accent.withAlpha(0.06f));
            g.fillRoundedRectangle(r.toFloat(), 5.0f);
            juce::Path frame;
            frame.addRoundedRectangle(r.toFloat().reduced(1.0f), 5.0f);
            const float dashes[] = { 5.0f, 4.0f };
            juce::PathStrokeType(isSelected ? 2.0f : 1.2f).createDashedStroke(frame, frame, dashes, 2);
            g.setColour(MiraLookAndFeel::accent.withAlpha(isSelected ? 0.95f : 0.55f));
            g.fillPath(frame);
            g.setColour(MiraLookAndFeel::accent.withAlpha(0.9f));
            g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
            g.drawText(item->block.name + "  -  empty, generate into it",
                        r.reduced(8, 2), juce::Justification::centredLeft, true);
            // THE BAR HAS TO BE DRAWN HERE TOO, and this is the case that matters. An
            // empty block returns early -- it is a frame, not a slab -- so the strip at
            // the end of this loop was unreachable for the whole time a block was being
            // generated into, which is the only time anyone wants to see it. It appeared
            // only on a block that ALREADY had audio, i.e. an extend or a re-generate.
            paintGenerationStrip(g, *item, r);
            continue;
        }

        // The BLOCK's colour, not the track's. Dragging a block to another track used to
        // recolour it, so the one thing you were following down a stack changed identity
        // exactly when you moved it.
        const auto tint = isReferenceLane(item->block.lane) ? kPictureColour
                                                            : laneColour(item->block.colour);
        g.setColour(tint.withAlpha(laneMuted ? 0.07f : 0.17f));
        g.fillRoundedRectangle(r.toFloat(), 5.0f);

        if (item->thumb != nullptr && item->thumb->getTotalLength() > 0.0)
        {
            // The name strip only costs height while there is height to spare; below that
            // the waveform gets all of it, which is the point of zooming in vertically.
            const int nameStrip = r.getHeight() >= 46 ? 16 : 0;
            auto wave = r.reduced(4, 3).withTrimmedTop(nameStrip)
                         .withTrimmedBottom(gridFooterHeight(*item, r.getHeight()));
            // The waveform occupies only as much of the block as it actually fills. The
            // rest is the TAIL, and drawing the thumbnail across it would show empty space
            // as if it were silence someone recorded.
            const double tail = tailSecondsOf(*item);
            const double sounding = soundingSecondsOf(*item);
            if (tail > 0.0)
                wave = wave.withWidth(juce::jmax(2, juce::roundToInt(sounding * pixelsPerSecond)));
            g.setColour(MiraLookAndFeel::text.withAlpha(laneMuted ? 0.18f
                                                                  : (isSelected ? 0.85f : 0.6f)));
            item->thumb->drawChannels(g, wave, item->block.sourceOffset,
                                       item->block.sourceOffset + sounding, waveZoom);

            // The empty tail: what Extend or Remix would fill in. Dashed, because it is a
            // frame with nothing in it -- the same language an empty block speaks.
            if (tail > 0.02)
            {
                auto gap = r.withTrimmedLeft(juce::roundToInt(sounding * pixelsPerSecond))
                            .reduced(2, 3);
                if (gap.getWidth() > 3)
                {
                    juce::Path dash;
                    dash.addRoundedRectangle(gap.toFloat(), 3.0f);
                    const float pattern[] = { 4.0f, 3.0f };
                    juce::PathStrokeType(1.0f).createDashedStroke(dash, dash, pattern, 2);
                    g.setColour(MiraLookAndFeel::accent.withAlpha(0.7f));
                    g.fillPath(dash);
                    if (gap.getWidth() > 54 && gap.getHeight() > 16)
                    {
                        g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.5f)));
                        g.drawText(juce::String(tail, 1) + "s to fill", gap,
                                    juce::Justification::centred, false);
                    }
                }
            }
        }

        // The fades, drawn along the CURVE the mixer actually applies -- fadeGain() is the
        // same function CanvasEngine uses per sample. A straight wedge over a sine fade is
        // a picture of something the audio is not doing, and the take editor has already
        // been caught drawing a selection nobody could see.
        {
            const float top = (float) r.getY(), bottom = (float) r.getBottom();
            const float h = bottom - top;
            auto wedge = [&] (float edgeX, float w, bool rising)
            {
                if (w <= 0.5f) return;
                juce::Path p;
                p.startNewSubPath(edgeX, bottom);
                constexpr int kSteps = 24;
                for (int k = 0; k <= kSteps; ++k)
                {
                    const float t = (float) k / (float) kSteps;
                    const float gain = fadeGain(t, item->block.fadeShape);
                    p.lineTo(edgeX + (rising ? t * w : -t * w), bottom - gain * h);
                }
                p.lineTo(edgeX + (rising ? w : -w), top);
                p.lineTo(edgeX, top);
                p.closeSubPath();
                g.setColour(MiraLookAndFeel::surface.withAlpha(0.6f));
                g.fillPath(p);
                g.setColour(tint.brighter(0.5f).withAlpha(0.7f));
                g.strokePath(p, juce::PathStrokeType(1.0f));
            };
            wedge((float) r.getX(),     (float) (item->block.fadeIn  * pixelsPerSecond), true);
            wedge((float) r.getRight(), (float) (item->block.fadeOut * pixelsPerSecond), false);

            // The handles, on the selected block only. Always shown once selected, even at
            // zero fade -- a grab point you cannot see is a feature nobody finds.
            if (isSelected && r.getHeight() >= 26)
            {
                g.setColour(MiraLookAndFeel::text.withAlpha(0.9f));
                const int fi = r.getX() + juce::roundToInt(item->block.fadeIn * pixelsPerSecond);
                const int fo = r.getRight() - juce::roundToInt(item->block.fadeOut * pixelsPerSecond);
                for (int hx : { fi, fo })
                    g.fillRoundedRectangle((float) (juce::jlimit(r.getX(), r.getRight() - 7, hx - 3)),
                                            (float) (r.getY() + 3), 7.0f, 7.0f, 2.0f);
            }
        }

        // Under the muted hatch and under the selection outline: the grid is the least
        // important thing in the block, and it has to look it.
        paintBlockGrid(g, *item, r, tint, isSelected);

        // A MUTED BLOCK has to read as muted at a glance, not on inspection: hatched, so
        // it is distinguishable from a quiet one even in a screenshot.
        if (item->block.muted)
        {
            g.setColour(MiraLookAndFeel::surface.withAlpha(0.72f));
            g.fillRoundedRectangle(r.toFloat(), 5.0f);
            // CLIPPED to the block. Without this the hatching runs the full width of the
            // lane, so a muted block reads as a muted TRACK -- the one thing it is not.
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion(r);
            g.setColour(tint.withAlpha(0.35f));
            for (int x = r.getX() - r.getHeight(); x < r.getRight(); x += 9)
                g.drawLine((float) x, (float) r.getBottom(),
                            (float) (x + r.getHeight()), (float) r.getY(), 1.0f);
        }

        g.setColour(isSelected ? MiraLookAndFeel::text : tint.withAlpha(0.55f));
        g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 5.0f, isSelected ? 1.8f : 1.0f);

        if (r.getHeight() >= 46)
        {
            // The block's own header: an M you can hit, then the name. A right-click menu
            // is where you go to find something; a button on the thing itself is where you
            // go to DO it, and mute is the second kind.
            const auto mb = blockMuteBox(*item);
            if (!mb.isEmpty())
            {
                g.setColour(item->block.muted ? MiraLookAndFeel::accent.withAlpha(0.85f)
                                               : tint.withAlpha(0.35f));
                g.fillRoundedRectangle(mb.toFloat(), 2.5f);
                g.setColour(item->block.muted ? MiraLookAndFeel::surface
                                               : MiraLookAndFeel::text.withAlpha(0.75f));
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.0f)));
                g.drawText("M", mb, juce::Justification::centred, false);
            }

            if (const auto gb = blockGainBox(*item); !gb.isEmpty())
            {
                const bool unity = std::abs(item->block.gainDb) < 0.05;
                g.setColour(tint.withAlpha(unity ? 0.25f : 0.5f));
                g.fillRoundedRectangle(gb.toFloat(), 2.5f);
                g.setColour(unity ? MiraLookAndFeel::text.withAlpha(0.5f)
                                  : MiraLookAndFeel::text.withAlpha(0.9f));
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.0f)));
                g.drawText(unity ? juce::String("0.0")
                                 : juce::String(item->block.gainDb, 1).replace("-", juce::String(
                                       juce::CharPointer_UTF8("\xe2\x88\x92"))),
                           gb, juce::Justification::centred, false);
            }

            // ANALYSE (2.1). Four states and each one has to be legible at 15x13 px, so
            // it is the FILL that carries the state and the letter stays "A": running is
            // the accent colour, measured is a quiet tick of the track's own colour,
            // refused and failed are the warn colour. A chip that changed its letter
            // would be a chip you have to learn to read.
            if (const auto ab = blockAnalyseBox(*item); !ab.isEmpty())
            {
                const auto st = item->analysis;
                const bool running  = st == Visual::Analysis::Running;
                const bool measured = st == Visual::Analysis::Measured;
                const bool warn     = st == Visual::Analysis::Refused
                                   || st == Visual::Analysis::Failed;
                g.setColour(running  ? MiraLookAndFeel::accent.withAlpha(0.85f)
                            : warn   ? MiraLookAndFeel::warn.withAlpha(0.55f)
                            : measured ? tint.withAlpha(0.55f)
                                       : tint.withAlpha(0.25f));
                g.fillRoundedRectangle(ab.toFloat(), 2.5f);
                g.setColour(running ? MiraLookAndFeel::surface
                                    : MiraLookAndFeel::text.withAlpha(measured || warn ? 0.9f : 0.6f));
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.0f)));
                g.drawText("A", ab, juce::Justification::centred, false);
            }

            g.setColour(isSelected ? MiraLookAndFeel::text : tint.brighter(0.4f));
            g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
            // "block1 _ name of the file": the block's own name AND what is in it. The
            // block name alone says nothing about which take you chose, and the filename
            // alone loses which part of the piece this is.
            // 3.6 -- in TIMECODE, when that is what the ruler reads. A cue's in-point is
            // the number you say out loud about it, so it belongs on the block itself and
            // not only under the playhead.
            const auto label = (rulerMode == Ruler::Timecode
                                    ? formatPosition(item->block.start) + "   "
                                    : juce::String())
                             + item->block.name
                             + (item->block.hasAudio()
                                    ? "  -  " + item->block.file.getFileNameWithoutExtension()
                                    : juce::String());
            const auto gb = blockGainBox(*item);
            auto headerRow = r.reduced(6, 2).removeFromTop(14)
                               .withTrimmedLeft(gb.isEmpty() ? (mb.isEmpty() ? 0 : mb.getWidth() + 4)
                                                             : gb.getRight() - r.getX() - 2);

            // Key and tempo on the RIGHT of the same row, so the name can be as long as it
            // likes without pushing them off.
            // The BLOCK's tempo and key, falling back to the prompt's only when the block
            // has none. Reading `settings` here would show the recipe you are about to
            // generate with over audio that was made with a different one -- and now that
            // 1.5 lets you type a tempo, the number on screen has to be the one the grid
            // below it is drawn from.
            if (auto tagBox = blockTagBox(*item); !tagBox.isEmpty())
            {
                auto tags = musicLabelOf(item->block);
                // WHILE IT RUNS, the tempo box says so. It is the place your eye is
                // already on for a tempo and it is the thing about to change, so the
                // running state is reported where the answer will appear rather than only
                // on a 15-pixel chip and a status line one repaint can overwrite.
                const bool busy = item->analysis == Visual::Analysis::Running;
                if (busy) tags = "analysing" + juce::String(juce::CharPointer_UTF8("\xe2\x80\xa6"));
                // A block whose prompt said nothing about tempo still shows the BOX, faint
                // and with a dash in it. Drawing nothing would be honest about the tempo
                // and silent about the gesture: roughly a quarter of takes arrive with no
                // BPM in their recipe, and those are exactly the blocks someone needs to
                // be able to double-click and type one into.
                const bool placeholder = tags.isEmpty();   // busy always has text, so never here
                if (placeholder)
                    tags = keyAndTempoOf(item->settings).isNotEmpty()
                               ? keyAndTempoOf(item->settings)
                               : juce::String(juce::CharPointer_UTF8("\xe2\x80\x93 bpm"));
                headerRow = headerRow.withTrimmedRight(tagBox.getWidth());
                // It LOOKS like the gain box next to it, because it behaves like it: a
                // number you drag. A readout and a control that are dragged the same way
                // and drawn differently is how you get a control nobody finds.
                if (item->block.tempo > 0.0 || placeholder || busy)
                {
                    g.setColour(tint.withAlpha(placeholder ? 0.12f : 0.3f));
                    g.fillRoundedRectangle(tagBox.toFloat(), 2.5f);
                }
                g.setColour(isSelected ? MiraLookAndFeel::text.withAlpha(placeholder ? 0.35f : 0.9f)
                                       : MiraLookAndFeel::text.withAlpha(placeholder ? 0.3f : 0.75f));
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.5f)));
                g.drawText(tags, tagBox, juce::Justification::centredRight, true);
                g.setColour(isSelected ? MiraLookAndFeel::text : tint.brighter(0.4f));
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
            }

            g.drawText(label, headerRow, juce::Justification::centredLeft, true);
        }

        paintGenerationStrip(g, *item, r);
    }

    if (!marquee.isEmpty())
    {
        g.setColour(MiraLookAndFeel::accent.withAlpha(0.15f));
        g.fillRect(marquee);
        g.setColour(MiraLookAndFeel::accent.withAlpha(0.6f));
        g.drawRect(marquee, 1);
    }

    // --- lane headers, painted AFTER the blocks so a block scrolled left disappears
    // under them rather than over them.
    {
        auto strip = juce::Rectangle<int>(0, lanesTop(), kHeaderWidth, getHeight() - lanesTop());
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(strip);
        g.setColour(MiraLookAndFeel::border);
        g.drawVerticalLine(kHeaderWidth - 1, static_cast<float>(lanesTop()), static_cast<float>(getHeight()));

        for (int lane = 0; lane < lanes; ++lane)
        {
            const bool muted  = lane < CanvasAudioSource::kMaxLanes
                             && (muteMask & (juce::uint64 (1) << lane)) != 0;
            const bool soloed = lane < CanvasAudioSource::kMaxLanes
                             && (soloMask & (juce::uint64 (1) << lane)) != 0;

            auto drawChip = [&](juce::Rectangle<int> box, const char* letter, bool on, juce::Colour tint) {
                g.setColour(on ? tint : MiraLookAndFeel::surface3);
                g.fillRoundedRectangle(box.toFloat(), 3.5f);
                g.setColour(on ? MiraLookAndFeel::surface : MiraLookAndFeel::textDim);
                g.setFont(laf.sansMedium(MiraLookAndFeel::textSize(10.0f)));
                g.drawText(letter, box, juce::Justification::centred, false);
            };
            drawChip(muteBoxFor(lane), "M", muted,  MiraLookAndFeel::warn);
            drawChip(soloBoxFor(lane), "S", soloed, MiraLookAndFeel::accent);

            // The padlock: this track's height is its own, and shift-G/H will not move it.
            // NO chip behind it -- M and S are things you press constantly and earn a
            // background; this is set once and then read, so it is a glyph, and a faint one
            // until it means something.
            if (auto lock = lockBoxFor(lane); !lock.isEmpty())
            {
                const bool held = laneHeightLocked(lane);
                const auto tint = isReferenceLane(lane) ? kPictureColour : laneColour(lane);
                const auto c = lock.getCentre();
                const auto body = juce::Rectangle<float>(0, 0, 8.0f, 6.0f)
                                      .withCentre({ (float) c.x, (float) c.y + 2.0f });
                juce::Path shackle;
                const float r = 2.4f;
                shackle.addCentredArc((float) c.x, body.getY(), r, r, 0.0f,
                                       -juce::MathConstants<float>::halfPi,
                                       juce::MathConstants<float>::halfPi, true);
                // Drawn rather than lettered. "L" beside M and S reads as loop, or left.
                g.setColour(held ? tint : MiraLookAndFeel::textFaint.withAlpha(0.55f));
                g.strokePath(shackle, juce::PathStrokeType(1.3f));
                // An OPEN padlock when it is not locked: the shackle lifted clear of the
                // body is the difference you can read at eight pixels.
                g.fillRoundedRectangle(held ? body : body.translated(0.0f, 1.0f), 1.3f);
            }

            // The reference lane says WHAT IT IS, not "track 4" -- and it is not a name
            // you can edit, because it is not a name anyone chose.
            const bool referenceHere = isReferenceLane(lane);
            g.setColour(muted ? MiraLookAndFeel::textFaint
                              : (referenceHere ? kPictureColour : laneColour(lane).brighter(0.2f)));
            g.setFont(laf.sansSemiBold(MiraLookAndFeel::textSize(referenceHere ? 11.0f : 12.5f)));
            if (lane != renamingLane)
                g.drawText(referenceHere ? juce::String("REFERENCE")
                                         : (laneNames[lane].isNotEmpty() ? laneNames[lane]
                                                                         : juce::String(lane + 1)),
                            nameBoxFor(lane), juce::Justification::centredLeft, true);

            // A bar at the lane's bottom edge as well as the padlock, for lanes too short
            // to show one: "why is this one not zooming" must have an answer on screen at
            // every height.
            if (laneHeightLocked(lane) && lockBoxFor(lane).isEmpty())
            {
                g.setColour((referenceHere ? kPictureColour : laneColour(lane)).withAlpha(0.55f));
                g.fillRect(0, laneToY(lane) + laneHeightOf(lane) - 2, 14, 2);
            }

            auto fader = faderBoxFor(lane);
            auto meterBox = meterBoxFor(lane);
            auto stripBox = stripBoxFor(lane);
            const bool strip = !fader.isEmpty();

            // One well, containing both. This is what makes it read as a single mixer
            // control rather than as two neighbours: the meter is INSIDE the fader's
            // widget, on the fader's scale.
            if (strip)
            {
                g.setColour(MiraLookAndFeel::surface.darker(0.5f));
                g.fillRoundedRectangle(stripBox.toFloat(), 3.5f);
                g.setColour(MiraLookAndFeel::border.withAlpha(0.5f));
                g.drawRoundedRectangle(stripBox.toFloat().reduced(0.5f), 3.5f, 1.0f);
            }

            // ONE mapping, used by both. `norm` is 0 at -60 dB and 1 at +6, warped so the
            // working range gets the travel -- see dbToNorm.
            auto yFor = [&](juce::Rectangle<int> box, double db) {
                return (float) box.getBottom() - (float) box.getHeight() * (float) dbToNorm(db);
            };

            // --- the scale, drawn BETWEEN the fader and the meter so it reads for both.
            if (strip)
            {
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(8.0f)));
                // A scale with more marks than it has room for is a smear. Drop to the two
                // that matter -- unity and -12 -- when the lane is short, and label them
                // only when there is width to the right of the meter to label them in.
                const bool roomy = fader.getHeight() >= 70;
                const bool labels = kHeaderWidth - stripBox.getRight() >= 26;
                for (double tick : roomy ? std::vector<double>{ 6.0, 0.0, -6.0, -12.0, -24.0, -40.0 }
                                         : std::vector<double>{ 0.0, -12.0 })
                {
                    const float y = yFor(fader, tick);
                    if (y < fader.getY() + 4 || y > fader.getBottom() - 2) continue;
                    // The tick crosses BOTH controls. That line is the whole argument for
                    // one scale: you can see where the fader is against where the signal
                    // is, in one look, without reading two numbers.
                    g.setColour(MiraLookAndFeel::border.withAlpha(tick == 0.0 ? 0.9f : 0.45f));
                    g.fillRect((float) stripBox.getX() + 1.0f, y, (float) stripBox.getWidth() - 2.0f, 1.0f);
                    if (!labels) continue;
                    g.setColour(MiraLookAndFeel::textFaint.withAlpha(tick == 0.0 ? 0.9f : 0.6f));
                    g.drawText(tick > 0 ? "+" + juce::String((int) tick) : juce::String((int) tick),
                                juce::Rectangle<int>(stripBox.getRight() + 3, (int) y - 5, 22, 10),
                                juce::Justification::centredLeft, false);
                }
            }

            // --- the meter: two bars, left and right, against that same scale.
            if (!meterBox.isEmpty())
            {
                const auto lv = lane < (int) laneMeter.size() ? laneMeter[(size_t) lane]
                                                              : std::array<float, 2>{ 0.0f, 0.0f };
                const auto hd = lane < (int) laneHold.size()  ? laneHold[(size_t) lane]
                                                              : std::array<float, 2>{ 0.0f, 0.0f };
                const float barW = (meterBox.getWidth() - 3.0f) * 0.5f;
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float x = meterBox.getX() + 1.0f + ch * (barW + 1.0f);
                    if (lv[(size_t) ch] > 0.0005f)
                    {
                        const double db = juce::Decibels::gainToDecibels(lv[(size_t) ch]);
                        // Three bands, so the colour is where the LEVEL is rather than one
                        // colour for the whole column -- a peak touching red shows red at
                        // the top and green below it, the way a real meter does.
                        auto band = [&](double lo, double hi, juce::Colour c) {
                            if (db < lo) return;
                            const float y0 = yFor(meterBox, juce::jmin(hi, db)), y1 = yFor(meterBox, lo);
                            if (y0 >= y1) return;
                            g.setColour(c);
                            g.fillRect(juce::Rectangle<float>(x, y0, barW, y1 - y0));
                        };
                        band(kFaderBottomDb, -6.0, MiraLookAndFeel::active);
                        band(-6.0, -1.0, MiraLookAndFeel::accent);
                        band(-1.0, kFaderTopDb, MiraLookAndFeel::warn);
                    }
                    if (hd[(size_t) ch] > 0.0005f)
                    {
                        const double db = juce::Decibels::gainToDecibels(hd[(size_t) ch]);
                        g.setColour(db > -1.0 ? MiraLookAndFeel::warn
                                              : MiraLookAndFeel::text.withAlpha(0.85f));
                        g.fillRect(x, yFor(meterBox, db) - 1.0f, barW, 1.5f);
                    }
                }

                // CLIP, latched. A clip that shows for 200 ms is a clip you will miss, and
                // the canvas sums tracks -- overs are the failure mode it invites. Click
                // the strip to clear it.
                if (lane < (int) laneClipped.size() && laneClipped[(size_t) lane])
                {
                    g.setColour(MiraLookAndFeel::warn);
                    g.fillRect((float) stripBox.getX(), (float) stripBox.getY() - 4.0f,
                                (float) stripBox.getWidth(), 3.0f);
                }
            }

            // --- the fader: a groove with a proper cap, on the same scale.
            if (strip)
            {
                const double db = laneDbAt(lane);
                auto groove = fader.toFloat().withSizeKeepingCentre(3.0f, (float) fader.getHeight());
                g.setColour(MiraLookAndFeel::surface.darker(0.35f));
                g.fillRoundedRectangle(groove, 1.5f);

                const float capY = yFor(fader, db);
                g.setColour(muted ? MiraLookAndFeel::textFaint : laneColour(lane).withAlpha(0.75f));
                g.fillRoundedRectangle(groove.withTop(capY), 1.5f);

                // The cap spans the WHOLE widget, meter included -- that is what makes the
                // two halves one control rather than two. A centre line so its exact
                // position is readable against the scale; a filled bar alone cannot say
                // where the control is when the value is at the bottom.
                auto cap = juce::Rectangle<float>((float) stripBox.getX() + 1.0f, capY - 4.5f,
                                                   (float) stripBox.getWidth() - 2.0f, 9.0f);
                g.setColour(muted ? MiraLookAndFeel::surface3 : laneColour(lane).brighter(0.3f));
                g.fillRoundedRectangle(cap, 2.5f);
                g.setColour(MiraLookAndFeel::surface.darker(0.6f));
                g.drawRoundedRectangle(cap, 2.5f, 1.0f);
                g.fillRect(cap.getX() + 2.0f, cap.getCentreY() - 0.5f, cap.getWidth() - 4.0f, 1.0f);
            }

            // --- the number. A fader without one is a gesture you cannot repeat.
            if (strip)
            {
                const double db = laneDbAt(lane);
                g.setColour(muted ? MiraLookAndFeel::textFaint : MiraLookAndFeel::textDim);
                g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(9.5f)));
                g.drawText(db <= kFaderBottomDb ? juce::String("-inf")
                                                : juce::String(db, 1) + " dB",
                            juce::Rectangle<int>(kHeaderWidth - 62, laneToY(lane) + 44, 56, 12),
                            juce::Justification::centredLeft, false);
            }
        }
        // The ruler's own corner, so the seconds do not run under the headers.
        g.setColour(MiraLookAndFeel::surface2);
        g.fillRect(0, 0, kHeaderWidth, topRuler);
        g.setColour(MiraLookAndFeel::border);
        g.drawVerticalLine(kHeaderWidth - 1, 0.0f, static_cast<float>(topRuler));
    }

    // --- where a dragged track would land. Drawn after the headers and before the
    // playhead: it has to sit over the rows it is pointing between, and under the one
    // thing that is always readable.
    if (drag == Drag::LaneMove && laneDropTarget >= 0)
    {
        auto row = juce::Rectangle<int>(0, laneToY(laneDropTarget), getWidth(), laneHeightOf(laneDropTarget));
        g.setColour(MiraLookAndFeel::accent.withAlpha(0.10f));
        g.fillRect(row);
        // The line goes on the side the track is travelling TOWARDS, so it reads as
        // "it lands here" rather than "something is highlighted".
        const int edge = laneDropTarget <= dragOriginLane ? row.getY() : row.getBottom() - 2;
        g.setColour(MiraLookAndFeel::accent);
        g.fillRect(0, edge, getWidth(), 2);
    }

    paintMarkers(g);

    // --- playhead, over everything
    {
        const int x = secondsToX(player.getPositionSeconds());
        if (x >= kHeaderWidth && x < getWidth())
        {
            g.setColour(MiraLookAndFeel::accent);
            g.drawVerticalLine(x, 0.0f, static_cast<float>(getHeight()));
            juce::Path head;
            head.addTriangle((float) x - 5, 0.0f, (float) x + 5, 0.0f, (float) x, 8.0f);
            g.fillPath(head);
        }
    }

    if (items.empty())
    {
        g.setColour(MiraLookAndFeel::textFaint);
        g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(13.0f)));
        g.drawText("drop audio here - no tempo, no grid, put it where you want it",
                    getLocalBounds(), juce::Justification::centred, false);
    }
}

void CanvasView::resized() {}

// ---- interaction -------------------------------------------------------------------

CanvasView::Visual* CanvasView::hitTest(juce::Point<int> p, Drag& what)
{
    // Back to front, so the block drawn on top is the one you grab.
    for (auto it = items.rbegin(); it != items.rend(); ++it)
    {
        auto r = boundsOf(**it);
        if (!r.contains(p)) continue;
        // THE LOCK (Phase 2.2). The reference is not a block you can grab: no move, no
        // trim, no fade handle, no gain box, and therefore no selection, no duplicate, no
        // remove and no generator pointed at it. Enforced here, at the one place a gesture
        // finds a block, rather than by six separate checks that could each be forgotten.
        if (isReferenceLane((*it)->block.lane)) continue;
        const auto& b = (*it)->block;

        // The fade handles ride the TOP of the block, where the wedge meets the edge, and
        // they win over trimming there. Trim still has the whole height below the band, so
        // one corner is not asked to mean two things at the same y.
        if (r.getHeight() >= 26 && p.y - r.getY() <= kFadeBand)
        {
            const int fi = r.getX() + juce::roundToInt(b.fadeIn * pixelsPerSecond);
            const int fo = r.getRight() - juce::roundToInt(b.fadeOut * pixelsPerSecond);
            if (std::abs(p.x - fi) <= kFadeGrab) { what = Drag::FadeIn;  return it->get(); }
            if (std::abs(p.x - fo) <= kFadeGrab) { what = Drag::FadeOut; return it->get(); }
        }

        // The gain box wins over trimming and moving: it is a control sitting ON the
        // block, and a three-pixel miss that drags the whole block instead of nudging its
        // level is the kind of thing that makes a control not worth having.
        if (blockGainBox(**it).contains(p))     { what = Drag::Gain; return it->get(); }
        // The TEMPO box is dragged, not typed into (1.5). A text field that pops up over
        // the block is a modal moment in the middle of arranging; a number you push with
        // the mouse is the same gesture as the gain box three pixels to its left.
        if (auto tb = blockTagBox(**it); !tb.isEmpty() && tb.contains(p))
        { what = Drag::Tempo; return it->get(); }
        // THE GRID FOOTER drags bar 1 (1.4). Trimming keeps the edges: trim has only the
        // seven pixels at each end, and the footer has all the rest of its band, so the
        // cheaper gesture to lose is the one with a whole strip to spare.
        // A tempo is required, not just a footer: the footer can be there for the onsets
        // alone (2.5), and dragging "bar 1" on a block with no grid moves a number nothing
        // draws -- a gesture with no visible effect, which is the worst kind.
        if (const int foot = gridFooterHeight(**it, r.getHeight());
            foot > 0 && b.tempo > 0.0 && p.y >= r.getBottom() - foot
            && p.x - r.getX() > kEdgeGrab && r.getRight() - p.x > kEdgeGrab)
        { what = Drag::BarOne; return it->get(); }
        if (p.x - r.getX() <= kEdgeGrab)        what = Drag::TrimLeft;
        else if (r.getRight() - p.x <= kEdgeGrab) what = Drag::TrimRight;
        else                                     what = Drag::Move;
        return it->get();
    }
    what = Drag::None;
    return nullptr;
}

void CanvasView::mouseMove(const juce::MouseEvent& e)
{
    // The lane headers: the bottom edge of each one resizes it, so say so with the cursor.
    // An edge you cannot see and were never told about is a feature nobody finds.
    if (e.x < kHeaderWidth && e.y >= lanesTop())
    {
        const int lane = yToLane(e.y);
        if (lane < laneCount
            && std::abs(e.y - (laneToY(lane) + laneHeightOf(lane))) <= kLaneEdgeGrab)
        {
            setMouseCursor(juce::MouseCursor::UpDownResizeCursor);
            return;
        }
        setMouseCursor(juce::MouseCursor::NormalCursor);
        return;
    }

    Drag what = Drag::None;
    hitTest(e.getPosition(), what);
    setMouseCursor(what == Drag::TrimLeft || what == Drag::TrimRight
                       ? juce::MouseCursor::LeftRightResizeCursor
                   : what == Drag::BarOne
                       ? juce::MouseCursor::DraggingHandCursor
                   : what == Drag::Tempo
                       ? juce::MouseCursor::UpDownResizeCursor
                   : (what == Drag::FadeIn || what == Drag::FadeOut)
                       ? juce::MouseCursor::PointingHandCursor
                       : juce::MouseCursor::NormalCursor);
}

void CanvasView::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    dragFrom = e.getPosition();

    // The lane headers first: they sit over everything on the left, so a click there is
    // never a click on a block.
    if (e.x < kHeaderWidth && e.y >= lanesTop())
    {
        const int lane = yToLane(e.y);
        if (lane >= laneCount) return;
        // Any click on the strip acknowledges the clip. Latched indicators need a way to
        // be cleared or they stop meaning "this happened" and start meaning "this happened
        // at some point, once, maybe ages ago".
        if (lane < (int) laneClipped.size()) laneClipped[(size_t) lane] = false;
        // Clicking a header SELECTS the track. That is what makes "delete this track" a
        // thing you can ask for, and it costs nothing -- M, S and the fader all still do
        // their own jobs because they are tested before this takes effect.
        selectedLane = lane;
        if (e.mods.isPopupMenu()) { showLaneMenu(lane, e.getPosition()); repaint(); return; }
        // The padlock. Clicked, it captures the height the lane has right now and stops
        // the zoom touching it; clicked again, the lane follows the zoom as before.
        if (auto lock = lockBoxFor(lane); !lock.isEmpty() && lock.contains(e.getPosition()))
        {
            setLaneHeightLocked(lane, !laneHeightLocked(lane));
            return;
        }
        // The BOTTOM EDGE of the header resizes this lane alone. Setting a height by hand
        // is what locking means: you have said how tall you want it, so the global zoom
        // stops arguing with you about it.
        if (std::abs(e.y - (laneToY(lane) + laneHeightOf(lane))) <= kLaneEdgeGrab)
        {
            drag = Drag::LaneResize;
            resizingLane = lane;
            resizeOriginH = laneHeightOf(lane);
            dragStart = e.getPosition();
            return;
        }
        if (lane < CanvasAudioSource::kMaxLanes)
        {
            const juce::uint64 bit = juce::uint64 (1) << lane;
            if (muteBoxFor(lane).contains(e.getPosition())) muteMask ^= bit;
            else if (soloBoxFor(lane).contains(e.getPosition())) soloMask ^= bit;
            else if (auto fader = faderBoxFor(lane);
                     !fader.isEmpty() && fader.expanded(6, 4).contains(e.getPosition()))
            {
                faderLane = lane;
                if (e.mods.isCommandDown()) setLaneDb(lane, 0.0);   // cmd-click = unity
                else setLaneDb(lane, faderDbAtY(lane, e.y));
                repaint();
                return;
            }
            else
            {
                // Not a control, so this is the start of a possible REORDER. It costs
                // nothing if you do not drag: the move happens on mouse-up, and only when
                // the row actually changed.
                commitRename();
                drag = Drag::LaneMove;
                dragOriginLane = lane;
                laneDropTarget = lane;
                repaint();
                return;
            }
            applyMasks();
            repaint();
        }
        return;
    }

    if (e.y < topRuler)
    {
        // Right-click the ruler for what the ruler IS -- seconds or timecode, the frame
        // rate, drop-frame, the start. Settings about a clock belong on the clock.
        if (e.mods.isPopupMenu()) { showRulerMenu(e.getPosition()); return; }
        drag = Drag::Playhead;
        player.setPositionSeconds(juce::jmax(0.0, xToSeconds(e.x)));
        repaint();
        return;
    }
    // The MARKERS row. Grab one to move it; double-click to rename; right-click for the
    // list. A marker you cannot drag is a spotting note you have to delete and re-make
    // every time the cut changes by a frame.
    if (e.y < videoStripTop())
    {
        const int hit = markerAtStripX(e.x);
        if (e.mods.isPopupMenu()) { showRulerMenu(e.getPosition()); return; }
        if (hit >= 0)
        {
            drag = Drag::MarkerMove;
            dragTargetMarker = hit;
            dragGrabSeconds = markers[(size_t) hit].seconds - xToSeconds(e.x);
            pushUndo();
            repaint();
            return;
        }
        // Empty space in the row scrubs, like the ruler above it.
        drag = Drag::Playhead;
        player.setPositionSeconds(juce::jmax(0.0, xToSeconds(e.x)));
        repaint();
        return;
    }

    // The video track. Scrubs the playhead like the ruler does -- which is the gesture
    // you actually want over picture -- and is not a lane, so yToLane never sees it.
    if (e.y < lanesTop())
    {
        if (e.mods.isPopupMenu())
        {
            if (const int clip = videoClipAt(xToSeconds(e.x)); clip >= 0)
                showVideoClipMenu(clip, e.getPosition());
            return;
        }
        drag = Drag::Playhead;
        player.setPositionSeconds(juce::jmax(0.0, xToSeconds(e.x)));
        repaint();
        return;
    }
    if (e.mods.isMiddleButtonDown() || e.mods.isAltDown())
    {
        drag = Drag::Pan;
        panFromView = viewStart;
        return;
    }

    Drag what = Drag::None;
    auto* hit = hitTest(e.getPosition(), what);
    if (hit == nullptr)
    {
        selectedLane = -1;      // clicking the canvas is not about a track
        if (!e.mods.isShiftDown()) selected.clear();
        announceSelection();
        pointPanelAt(nullptr);
        drag = Drag::Marquee;
        marquee = { e.x, e.y, 0, 0 };
        repaint();
        return;
    }

    if (e.mods.isShiftDown() || e.mods.isCommandDown())
    {
        if (selected.count(hit->block.id)) selected.erase(hit->block.id);
        else selected.insert(hit->block.id);
    }
    else if (selected.count(hit->block.id) == 0)
    {
        selected.clear();
        selected.insert(hit->block.id);
    }

    if (auto mb = blockMuteBox(*hit); !mb.isEmpty() && mb.contains(e.getPosition())
                                       && !e.mods.isPopupMenu())
    {
        drag = Drag::None;
        pushUndo();
        hit->block.muted = !hit->block.muted;
        rebuildAudio();
        markDirty();
        repaint();
        return;
    }

    // ANALYSE (2.1). Checked here beside the mute chip and BEFORE the popup-menu branch
    // for the same reason: a control drawn on a block has to win over the gestures the
    // block itself offers, or a three-pixel miss starts a drag instead.
    //
    // No pushUndo -- asking a question changes nothing. The snapshot is taken in
    // analysisArrived, where an answer actually moves the grid.
    if (auto ab = blockAnalyseBox(*hit); !ab.isEmpty() && ab.contains(e.getPosition())
                                          && !e.mods.isPopupMenu())
    {
        drag = Drag::None;
        analyseSelection();
        return;
    }

    if (e.mods.isPopupMenu())
    {
        drag = Drag::None;
        repaint();
        showBlockMenu(*hit);
        return;
    }

    // ONE snapshot per gesture, taken as the drag begins -- not per mouse event, or
    // undoing a slow drag would take fifty presses to get back where you started.
    if (what == Drag::Move || what == Drag::TrimLeft || what == Drag::TrimRight
        || what == Drag::Gain || what == Drag::BarOne || what == Drag::Tempo
        || what == Drag::FadeIn || what == Drag::FadeOut) pushUndo();

    drag = what;
    dragTarget = hit->block.id;
    dragOriginFadeIn  = hit->block.fadeIn;
    dragOriginFadeOut = hit->block.fadeOut;
    announceSelection();
    // Selecting a block IS opening its generator now that the panel is always on screen.
    pointPanelAt(hit);
    dragOrigins.clear();
    for (const auto& i : items)
        if (selected.count(i->block.id)) dragOrigins[i->block.id] = { i->block.start, i->block.lane };
    dragGrabSeconds = xToSeconds(e.x);
    dragOriginStart = hit->block.start;
    dragOriginLength = hit->block.length;
    dragOriginOffset = hit->block.sourceOffset;
    dragOriginGain = hit->block.gainDb;
    dragOriginBarOne = hit->block.barOnePos;
    // Where a tempo drag starts from when the block has none: what the recipe asked for,
    // and only then a round number. 120 is a stated default, not a measurement -- which is
    // why the drag also stamps `tempoSource = "typed"`: the number came from your hand.
    dragOriginTempo = hit->block.tempo > 0.0 ? hit->block.tempo
                    : promptTempoOf(hit->block) > 0.0 ? promptTempoOf(hit->block)
                                                      : 120.0;
    dragStart = e.getPosition();
    dragOriginLane = hit->block.lane;
    repaint();
}

void CanvasView::mouseDrag(const juce::MouseEvent& e)
{
    if (drag == Drag::MarkerMove)
    {
        // No snapping to anything: a marker IS the thing other things snap to.
        setMarkerTime(dragTargetMarker, juce::jmax(0.0, xToSeconds(e.x) + dragGrabSeconds));
        return;
    }

    if (drag == Drag::LaneResize)
    {
        setLaneHeight(resizingLane, resizeOriginH + (e.y - dragStart.y));
        return;
    }

    if (drag == Drag::LaneMove)
    {
        const int t = juce::jlimit(0, laneCount - 1, yToLane(e.y));
        if (t != laneDropTarget) { laneDropTarget = t; repaint(); }
        return;
    }

    if (faderLane >= 0)
    {
        setLaneDb(faderLane, faderDbAtY(faderLane, e.y));
        repaint();
        return;
    }
    if (drag == Drag::Playhead)
    {
        player.setPositionSeconds(juce::jmax(0.0, xToSeconds(e.x)));
        repaint();
        return;
    }
    if (drag == Drag::Pan)
    {
        viewStart = juce::jmax(0.0, panFromView - (e.x - dragFrom.x) / pixelsPerSecond);
        repaint();
        return;
    }
    if (drag == Drag::Marquee)
    {
        marquee = juce::Rectangle<int>(dragFrom, e.getPosition());
        selected.clear();
        for (const auto& i : items)
            if (boundsOf(*i).intersects(marquee)) selected.insert(i->block.id);
        repaint();
        return;
    }
    if (drag == Drag::None) return;

    const double deltaSeconds = xToSeconds(e.x) - dragGrabSeconds;

    // BAR 1 moves on the DRAGGED block only, never across the selection: where the
    // downbeat falls is a fact about one piece of audio, and dragging a grid is a claim
    // about that audio and nothing else.
    if (drag == Drag::Tempo)
    {
        Visual* v = nullptr;
        for (auto& i : items) if (i->block.id == dragTarget) { v = i.get(); break; }
        if (v == nullptr) return;
        // Up is faster, the way up is louder on the gain box beside it. A quarter of a bpm
        // per pixel puts 20-400 inside one screen of travel while still landing on the
        // number you meant; shift divides that by five for the 87.3 case, which is the one
        // this whole feature exists for.
        const double perPixel = e.mods.isShiftDown() ? 0.05 : 0.25;
        const double raw = dragOriginTempo - (double) (e.y - dragStart.y) * perPixel;
        // Whole bpm unless you ask for finer, because a grid at 94.9983 is a grid nobody
        // typed and nobody wanted. The precision is available, it is just not the default.
        v->block.tempo = juce::jlimit(20.0, 400.0,
                                       e.mods.isShiftDown() ? std::round(raw * 10.0) / 10.0
                                                            : std::round(raw));
        v->block.tempoSource = "typed";
        // A dragged tempo is not a measured one -- see commitTempoEdit's reasoning, which
        // this gesture replaced: a stale confidence would let step 3 allow or refuse a
        // stretch on the strength of a number that no longer describes anything.
        v->block.tempoConfidence = 0.0;
        markDirty();
        repaint();
        return;
    }

    if (drag == Drag::BarOne)
    {
        Visual* v = nullptr;
        for (auto& i : items) if (i->block.id == dragTarget) { v = i.get(); break; }
        if (v == nullptr) return;
        // Stored in SOURCE time, so the delta goes straight in: a second of timeline is a
        // second of file, and that is exactly why trimming the left edge afterwards
        // cannot break the phase.
        v->block.barOnePos = juce::jmax(0.0, dragOriginBarOne + deltaSeconds);
        // You moved it by eye, so it is yours: an analysis will not put it back (1.4, and
        // convention 5). Set on the drag rather than on mouse-up so a drag abandoned
        // mid-gesture still counts -- the grid has already moved on screen.
        v->block.barOneIsHuman = true;
        markDirty();
        repaint();
        return;
    }

    for (auto& i : items)
    {
        if (selected.count(i->block.id) == 0) continue;
        auto& b = i->block;

        if (drag == Drag::Move)
        {
            // Every selected block moves by the SAME delta, from where it started, and
            // the lane shift is taken from the dragged block -- so a multi-block
            // selection keeps its shape instead of collapsing onto one lane.
            const auto origin = dragOrigins.find(b.id);
            if (origin == dragOrigins.end()) continue;
            const int laneShift = yToLane(e.y) - dragOriginLane;
            // 5.3 -- the START snaps to a marker when it lands near one. Only the block
            // being dragged, and only its start: snapping a whole selection would move
            // blocks whose starts are nowhere near a marker, and snapping the end as well
            // would mean two attractors fighting over one gesture.
            const double wanted = juce::jmax(0.0, origin->second.first + deltaSeconds);
            b.start = (b.id == dragTarget && !e.mods.isAltDown()) ? snapToMarker(wanted) : wanted;
            // CLAMPED TO TRACKS THAT EXIST. Dragging below the last track used to drop the
            // block onto empty space -- a lane with no header, no fader and no mute, which
            // is not a track, so the block was somewhere you could not mix it from.
            b.lane = juce::jlimit(0, juce::jmax(0, laneCount - 1),
                                  origin->second.second + laneShift);
        }
        else if (drag == Drag::TrimLeft && b.id == dragTarget)
        {
            // Trimming the left edge moves where in the FILE the block starts, so the
            // audio under the block stays put on the canvas instead of sliding.
            const double want = juce::jlimit(dragOriginStart - dragOriginOffset,
                                              dragOriginStart + dragOriginLength - 0.05,
                                              dragOriginStart + deltaSeconds);
            const double moved = want - dragOriginStart;
            b.start = want;
            b.sourceOffset = juce::jmax(0.0, dragOriginOffset + moved);
            b.length = juce::jmax(0.05, dragOriginLength - moved);
        }
        else if (drag == Drag::TrimRight && b.id == dragTarget)
        {
            // TRIMMING HIDES, IT DOES NOT CUT. Pulling the right edge in shows less of the
            // take; pulling it back out shows it again. An earlier version treated pulling
            // in as a cut and remembered it, so trimming a block destroyed its audio as
            // far as the canvas was concerned and there was no way back but a menu.
            //
            // Cutting is Cmd-E, and only Cmd-E: a deliberate "the audio ends here", which
            // is the thing an extend needs to know and a trim never meant to say.
            b.length = juce::jmax(0.05, dragOriginLength + deltaSeconds);
        }
        else if (drag == Drag::Gain && b.id == dragTarget)
        {
            // Vertical, and inverted the way every fader is: up is louder. 0.08 dB per
            // pixel gives the whole -24..+12 range in about 450 px of travel, which is
            // more than the window is tall -- so you can be precise without the drag
            // running out of screen.
            b.gainDb = juce::jlimit(-24.0, 12.0,
                                     dragOriginGain - (double) (e.y - dragStart.y) * 0.08);
        }
        else if (drag == Drag::FadeIn && b.id == dragTarget)
        {
            // A fade can reach the whole block but no further -- past that it would be
            // asked to fade for longer than there is audio to fade.
            b.fadeIn = juce::jlimit(0.0, b.length, dragOriginFadeIn + deltaSeconds);
        }
        else if (drag == Drag::FadeOut && b.id == dragTarget)
        {
            b.fadeOut = juce::jlimit(0.0, b.length, dragOriginFadeOut - deltaSeconds);
        }
    }

    repaint();
}

void CanvasView::mouseUp(const juce::MouseEvent&)
{
    faderLane = -1;
    if (drag == Drag::MarkerMove)
    {
        drag = Drag::None;
        dragTargetMarker = -1;
        // markerAfter, the cue sheet and the label truncation all read this list in time
        // order. Sorting on mouse-up rather than on every drag event keeps the marker you
        // are holding from changing index under your own hand.
        std::sort(markers.begin(), markers.end(),
                   [](const Marker& a, const Marker& b) { return a.seconds < b.seconds; });
        markDirty();
        if (onMarkersChanged) onMarkersChanged();
        repaint();
        return;
    }

    if (drag == Drag::LaneResize)
    {
        drag = Drag::None;
        resizingLane = -1;
        repaint();
        return;
    }

    if (drag == Drag::LaneMove)
    {
        const int from = dragOriginLane, to = laneDropTarget;
        drag = Drag::None;
        laneDropTarget = -1;
        if (to >= 0 && to != from) moveLane(from, to);
        repaint();
        return;
    }
    const bool changed = drag == Drag::Move || drag == Drag::TrimLeft || drag == Drag::TrimRight
                      || drag == Drag::Gain
                      || drag == Drag::FadeIn || drag == Drag::FadeOut;
    drag = Drag::None;
    marquee = {};
    if (changed)
    {
        rebuildAudio();
        markDirty();
        // Resizing the block IS setting the duration, so it has to reach the generator on
        // mouse-up rather than the next time you happen to reselect the block.
        if (onBlockGeometry)
        {
            const auto g = selectionGeometry();
            if (g.length > 0.0) onBlockGeometry(g.length, g.tail, g.hasAudio);
        }
    }
    repaint();
}

void CanvasView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // A MOUSE WHEEL HAS ONE AXIS, and that is the whole bug. A trackpad reports deltaX and
    // deltaY, so panning read deltaX and felt right; a wheel only ever reports deltaY, so
    // the pan branch was handed 0.0 on every event and the canvas simply would not move.
    // Everything below works off whichever axis actually moved instead of a fixed one.
    //
    // (macOS turns a shift-held wheel into deltaX itself, which is a second way the same
    // assumption broke: shift-zoom read deltaY and got nothing.)
    const float dx = wheel.deltaX, dy = wheel.deltaY;
    const float d  = std::abs (dx) > std::abs (dy) ? dx : dy;
    if (d == 0.0f) return;

    // A notched wheel sends a few big events; a trackpad sends a stream of small ones.
    // Scaling by the delta alone makes the wheel crawl, and a fixed step per event makes
    // the trackpad lurch a screen at a time. So: proportional when smooth, a step when not.
    const double step = wheel.isSmooth ? (double) d
                                       : (d > 0.0f ? 1.0 : -1.0) * 0.28;

    // Shift zooms VERTICALLY: taller lanes mean a taller waveform, which is the only way
    // to judge a quiet take against a loud one by eye. Separate from the horizontal zoom
    // because time and amplitude are separate questions.
    if (e.mods.isShiftDown())    { zoomVertical (step * 22.0); return; }
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
                                 { zoomBy (std::pow (1.6, step), e.x); return; }

    // Plain wheel pans the timeline. A wheel user has no other way to get there, and a
    // canvas that only scrolls for trackpads is a canvas half the input devices cannot
    // navigate.
    panBy (-step * 520.0 / pixelsPerSecond);
}

void CanvasView::panBy (double seconds)
{
    viewStart = juce::jmax (0.0, viewStart + seconds);
    repaint();
}

int CanvasView::laneToY(int lane) const
{
    int y = lanesTop();
    for (int i = 0; i < lane; ++i) y += laneHeightOf(i);
    return y;
}

int CanvasView::yToLane(int y) const
{
    int top = lanesTop();
    // Bounded rather than open: yToLane is asked about clicks well below the last track,
    // and a walk that only stops when it finds the row would never stop down there.
    for (int lane = 0; lane < CanvasAudioSource::kMaxLanes; ++lane)
    {
        const int h = juce::jmax(1, laneHeightOf(lane));
        if (y < top + h) return juce::jmax(0, lane);
        top += h;
    }
    return CanvasAudioSource::kMaxLanes - 1;
}

void CanvasView::ensureLaneArrays()
{
    if ((int) laneH.size() < laneCount) laneH.resize((size_t) laneCount, 0);
}

void CanvasView::setLaneHeight(int lane, int height)
{
    if (!juce::isPositiveAndBelow(lane, laneCount)) return;
    ensureLaneArrays();
    laneH[(size_t) lane] = juce::jlimit(kLaneMin, kLaneMax, height);
    markDirty();
    repaint();
}

void CanvasView::setLaneHeightLocked(int lane, bool locked)
{
    if (!juce::isPositiveAndBelow(lane, laneCount)) return;
    ensureLaneArrays();
    // Locking CAPTURES the height the lane has right now, so the lock never changes what
    // you are looking at -- it only stops it changing afterwards.
    laneH[(size_t) lane] = locked ? juce::jlimit(kLaneMin, kLaneMax, laneHeightOf(lane)) : 0;
    if (onTakeNote)
        onTakeNote(locked ? "track height locked at " + juce::String(laneHeightOf(lane)) + " px"
                          : "track height follows the zoom again");
    markDirty();
    repaint();
}

juce::Rectangle<int> CanvasView::lockBoxFor(int lane) const
{
    // THE TOP-RIGHT CORNER, on the name's row -- not a third chip under M and S. Under
    // them it landed on the gain readout and read as a third thing you press often, which
    // it is not: this is a property of the track, like its name, and it belongs up there
    // with it. Only when the lane is tall enough to have a name row of its own.
    if (laneHeightOf(lane) < 46) return {};
    return { kHeaderWidth - 21, laneToY(lane) + 4, 13, 13 };
}

void CanvasView::showLaneMenu(int lane, juce::Point<int> at)
{
    if (!juce::isPositiveAndBelow(lane, laneCount)) return;
    juce::PopupMenu m;
    const bool locked = laneHeightLocked(lane);
    m.addItem(1, locked ? "Let the height follow the zoom" : "Lock this height", true, false);
    m.addSeparator();
    m.addItem(3, "Move up", lane > 0, false);
    m.addItem(4, "Move down", lane < laneCount - 1, false);
    m.addSeparator();
    m.addItem(5, "Remove track", laneCount > 1 && !isReferenceLane(lane), false);

    juce::Component::SafePointer<CanvasView> safe (this);
    // AT THE POINTER, not centred on the canvas. withTargetComponent aims at the whole
    // component, which for a full-window canvas means the middle of the window -- a menu
    // that opens nowhere near the track it is about.
    const auto onScreen = localPointToGlobal(at);
    m.showMenuAsync(juce::PopupMenu::Options()
                        .withTargetScreenArea({ onScreen.x, onScreen.y, 1, 1 }),
                     [safe, lane](int id) {
        if (safe == nullptr || id == 0) return;
        auto& self = *safe;
        switch (id)
        {
            case 1: self.setLaneHeightLocked(lane, !self.laneHeightLocked(lane)); break;
            case 3: self.moveLane(lane, lane - 1); break;
            case 4: self.moveLane(lane, lane + 1); break;
            case 5: self.removeLane(lane); break;
            default: break;
        }
    });
}

void CanvasView::zoomVertical (double pixels)
{
    const int was = laneHeight;
    laneHeight = juce::jlimit (28, 320, laneHeight + juce::roundToInt (pixels));
    // A zoom that rounds to no change at all should still not repaint forever.
    if (laneHeight != was) repaint();
}

// Where a keyboard zoom should anchor. The playhead when you can see it -- that is the
// thing you are looking at -- and the middle of the view when you cannot, rather than the
// left edge, which throws away half the zoom.
int CanvasView::zoomAnchorX() const
{
    const int p = secondsToX (player.getPositionSeconds());
    if (p >= kHeaderWidth && p <= getWidth()) return p;
    return kHeaderWidth + (getWidth() - kHeaderWidth) / 2;
}

void CanvasView::zoomBy(double factor, int aroundX)
{
    // The second under the cursor stays under the cursor, which is the only zoom that
    // does not feel like the canvas jumped.
    const double anchor = xToSeconds(aroundX);
    pixelsPerSecond = juce::jlimit(0.5, 400.0, pixelsPerSecond * factor);
    viewStart = juce::jmax(0.0, anchor - (aroundX - kHeaderWidth) / pixelsPerSecond);
    repaint();
}

bool CanvasView::keyPressed(const juce::KeyPress& key)
{
    // The document shortcuts every app has. Handled here rather than in the app menu bar
    // because the canvas is the only thing that has a document, and a global Cmd+S that
    // sometimes meant the canvas and sometimes meant nothing would be worse than none.
    if (key.getModifiers().isCommandDown())
    {
        if (key.getKeyCode() == 'S' && onSaveRequested) { onSaveRequested(); return true; }
        if (key.getKeyCode() == 'O' && onOpenRequested) { onOpenRequested(); return true; }
        if (key.getKeyCode() == 'N' && onNewRequested)  { onNewRequested();  return true; }
        // Cmd-E cuts where the playhead stands; Cmd-shift-E makes two blocks of it. Paired
        // on purpose -- they are the same gesture with and without "and keep both halves".
        //
        // NOT plain `s`, which was asked for but is already solo (and `m` is mute) -- a
        // documented key that quietly starts doing something else is worse than a slightly
        // longer one. Say the word and they can swap.
        if (key.getKeyCode() == 'E')
        {
            if (key.getModifiers().isShiftDown()) splitAtPlayhead();
            else                                  cutAtPlayhead();
            return true;
        }
        // Cmd-up / Cmd-down move the SELECTED TRACK, which is what those keys move in
        // every arrangement window there has ever been. Plain up/down are left alone:
        // they are the obvious home for moving a BLOCK between tracks, later.
        if (key.getKeyCode() == juce::KeyPress::upKey)   { moveSelectedLane(-1); return true; }
        if (key.getKeyCode() == juce::KeyPress::downKey) { moveSelectedLane(1);  return true; }
        // Cmd-Z / Cmd-shift-Z, the two every app has. Handled here rather than in the menu
        // bar because the canvas is the only thing in mira with a document to undo.
        if (key.getKeyCode() == 'Z')
        {
            if (key.getModifiers().isShiftDown()) redo(); else undo();
            return true;
        }
    }

    // ZOOM FROM THE KEYBOARD. G and H horizontally, shift-G and shift-H vertically --
    // left-to-right reading as less-to-more, so H opens the view out under your eye and G
    // pulls it back. On the keyboard because scroll gestures are not the same on every
    // device and a shortcut is: it does the same thing on a trackpad, a wheel, and a
    // laptop with neither to hand.
    if (key.getKeyCode() == 'G' || key.getKeyCode() == 'H')
    {
        const bool in = key.getKeyCode() == 'H';
        if (key.getModifiers().isShiftDown()) zoomVertical (in ? 10.0 : -10.0);
        else                                  zoomBy (in ? 1.25 : 1.0 / 1.25, zoomAnchorX());
        return true;
    }
    if (key == juce::KeyPress::spaceKey)       { togglePlay(); return true; }
    if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
                                               { removeSelected(); return true; }
    if (key == juce::KeyPress::returnKey)      { player.setPositionSeconds(0.0); repaint(); return true; }
    if (key.getTextCharacter() == 'm' || key.getTextCharacter() == 's')
    {
        // Acts on the lanes of whatever is selected, so "mute this take" is one key after
        // clicking it rather than a trip to the header.
        juce::uint64 bits = 0;
        for (const auto& i : items)
            if (selected.count(i->block.id) && i->block.lane < CanvasAudioSource::kMaxLanes)
                bits |= juce::uint64 (1) << i->block.lane;
        if (bits == 0) return true;
        (key.getTextCharacter() == 'm' ? muteMask : soloMask) ^= bits;
        applyMasks();
        repaint();
        return true;
    }
    // K for a marK. NOT cmd-M, which is what this was first written as and which macOS
    // takes for Minimise before the app ever sees it -- the window shrank to the dock and
    // no marker appeared. And not plain M or S, which are mute and solo.
    if (key.getTextCharacter() == 'k')         { addMarkerAtPlayhead(); return true; }
    if (key.getTextCharacter() == 'K')         { addBlockToNextMarker(); return true; }
    if (key.getTextCharacter() == 'l')         { setLoopFromSelection(); return true; }
    if (key.getModifiers().isCommandDown() && key.getKeyCode() == 'D')
        { duplicateSelection(); return true; }
    if (key.getTextCharacter() == 'f')         { fit(); return true; }
    // WAVEFORM height, which is not block height. A quiet take is a flat line you cannot
    // edit against and a loud one fills the block and shows nothing; the peaks you are
    // looking for are in neither. Drawn taller or shorter without moving anything.
    if (key.getTextCharacter() == ']')
        { waveZoom = juce::jmin(16.0f, waveZoom * 1.35f); repaint(); return true; }
    if (key.getTextCharacter() == '[')
        { waveZoom = juce::jmax(0.15f, waveZoom / 1.35f); repaint(); return true; }
    if (key.getTextCharacter() == '=' || key.getTextCharacter() == '+')
        { laneHeight = juce::jmin(320, laneHeight + 8); repaint(); return true; }
    if (key.getTextCharacter() == '-')
        { laneHeight = juce::jmax(28, laneHeight - 8); repaint(); return true; }
    return false;
}

void CanvasView::cutAtPlayhead()
{
    // CUT, not split. Cmd-E ends the block at the playhead and leaves ONE block -- the
    // part before the cut -- ready to extend from there.
    //
    // It used to split into two, which made a second block with a second folder and an
    // empty generator in it: you asked to end a take and got a new empty thing to explain.
    // Splitting is still available, on the right-click menu, where it reads as the
    // deliberate two-block operation it is.
    //
    // The audio is not touched. Only the block's claim about where it ends moves, so
    // "Restore full take" brings it all back and undo is one keypress.
    const double at = player.getPositionSeconds();

    std::vector<Visual*> victims;
    for (const auto& i : items)
    {
        const bool spans = i->block.start < at - 1.0e-6 && i->block.end() > at + 1.0e-6;
        if (!spans) continue;
        if (selected.empty() || selected.count(i->block.id)) victims.push_back(i.get());
    }
    if (victims.empty()) return;
    pushUndo();

    for (auto* v : victims)
    {
        const double left = at - v->block.start;
        v->block.length = left;
        v->block.contentSeconds = left;      // the audio ends here too, not just the frame
        v->block.fadeIn  = juce::jmin(v->block.fadeIn,  left);
        v->block.fadeOut = juce::jmin(v->block.fadeOut, left);
    }

    markDirty();
    rebuildAudio();
    announceSelection();
    repaint();
}

void CanvasView::splitAtPlayhead()
{
    const double at = player.getPositionSeconds();

    // What to cut: the selection when there is one, and otherwise everything the playhead
    // is standing on. "The block at the cursor" is the second case, and having to select
    // first would make the shortcut two gestures instead of one.
    std::vector<Visual*> victims;
    for (const auto& i : items)
    {
        const bool spans = i->block.start < at - 1.0e-6 && i->block.end() > at + 1.0e-6;
        if (!spans) continue;
        if (selected.empty() || selected.count(i->block.id)) victims.push_back(i.get());
    }
    if (victims.empty()) return;
    pushUndo();

    selected.clear();
    for (auto* v : victims)
    {
        const double leftLength = at - v->block.start;

        auto right = std::make_unique<Visual>();
        right->block = v->block;
        right->block.id = nextId++;
        right->block.start = at;
        right->block.length = v->block.length - leftLength;
        // The right half starts further INTO the file. Trimming moves the offset rather
        // than the audio, which is the same rule the left-edge drag follows.
        right->block.sourceOffset = v->block.sourceOffset + leftLength;
        // A new name, and so a new folder -- for the same reason a duplicate gets one. The
        // name is the generation target, and two blocks sharing a folder is exactly the
        // bug where generating on one put the audio on the other.
        right->block.name = nextBlockName();
        right->settings = v->settings;
        // A cut is a statement about where the audio ends, on both halves. Without it the
        // left half still claims the whole file, and dragging its right edge out would
        // reveal exactly the audio the cut was meant to remove.
        right->block.contentSeconds = v->block.length - leftLength;
        // The fade-out belongs to the piece that still has the end of the sound; the
        // fade-in to the piece that still has the start. Splitting in the middle of a fade
        // would otherwise leave both halves fading the wrong way.
        right->block.fadeIn = 0.0;
        v->block.fadeOut = 0.0;
        right->block.fadeOut = juce::jmin (v->block.fadeOut, right->block.length);

        setFileOn (*right, v->block.file);
        right->block.length = v->block.length - leftLength;   // setFileOn may have reset it
        right->block.sourceOffset = v->block.sourceOffset + leftLength;

        v->block.length = leftLength;
        v->block.contentSeconds = leftLength;
        v->block.fadeIn = juce::jmin (v->block.fadeIn, leftLength);

        selected.insert (right->block.id);
        items.push_back (std::move (right));   // one at a time, so the namer sees the last
    }

    pointPanelAt (items.back().get());
    markDirty();
    rebuildAudio();
    repaint();
}

void CanvasView::togglePlay()
{
    if (player.isPlaying())
    {
        player.stop();
        if (returnOnStop) player.setPositionSeconds(playedFrom);
    }
    else
    {
        playedFrom = player.getPositionSeconds();
        player.play();
    }
    if (onStateChanged) onStateChanged();
    repaint();
}

void CanvasView::removeSelected()
{
    // A selected TRACK is what Remove is about when there is one: you clicked the header,
    // not a block, and deleting "the selection" has to mean the thing you selected.
    if (selected.empty() && selectedLane >= 0) { removeLane(selectedLane); return; }
    if (selected.empty()) return;
    pushUndo();
    items.erase(std::remove_if(items.begin(), items.end(),
                                [this](const std::unique_ptr<Visual>& v) {
                                    return selected.count(v->block.id) > 0;
                                }),
                 items.end());
    selected.clear();
    rebuildAudio();
    repaint();
}

void CanvasView::setLoopFromSelection()
{
    if (selected.empty()) { loopStart = loopEnd = 0.0; player.setLoop(false, 0, 0); repaint(); return; }
    double a = 1e12, b = 0.0;
    for (const auto& i : items)
        if (selected.count(i->block.id))
        { a = juce::jmin(a, i->block.start); b = juce::jmax(b, i->block.end()); }
    if (b <= a) return;
    loopStart = a; loopEnd = b;
    player.setLoop(true, a, b);
    if (onStateChanged) onStateChanged();
    repaint();
}

void CanvasView::toggleLoop()
{
    if (loopEnd <= loopStart) { setLoopFromSelection(); return; }
    player.setLoop(!player.isLooping(), loopStart, loopEnd);
    if (onStateChanged) onStateChanged();
    repaint();
}

// ---- dropping in --------------------------------------------------------------------

bool CanvasView::isInterestedInFileDrag(const juce::StringArray& files)
{
    for (const auto& f : files)
        if (formats.findFormatForFileExtension(juce::File(f).getFileExtension()) != nullptr)
            return true;
    return false;
}

void CanvasView::filesDropped(const juce::StringArray& files, int x, int y)
{
    juce::Array<juce::File> keep;
    for (const auto& f : files) keep.add(juce::File(f));
    // Clamped to ONE past the last track. Dropping low on an empty canvas used to create
    // every lane up to the cursor -- three tracks from one file, two of them empty.
    addFiles(keep, juce::jmax(0.0, xToSeconds(x)), juce::jmin(yToLane(y), laneCount));
}

void CanvasView::addFiles(const juce::Array<juce::File>& files, double atSeconds, int lane)
{
    // A DROPPED FILE BECOMES A BLOCK LIKE ANY OTHER, and that is the whole point of this
    // function now. The first version named the block after the file and left the audio
    // where it was, so the block's folder was empty, its generator showed "no takes yet",
    // and its label read "dun-s26-... - dun-s26-..." -- a block that looked like a take
    // and a generator with nothing in it. Two kinds of block, one of them broken.
    //
    // Now: the block is named "block N" like the rest, its folder is created, and the file
    // is COPIED into it as that block's first take. Drop it, and it is a take you can hear,
    // re-generate against, and keep beside alternatives -- the same object in every case.
    pushUndo();
    UndoGuard oneEdit (*this);
    double at = atSeconds;
    for (const auto& f : files)
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor(f));
        if (reader == nullptr) continue;
        const double len = reader->sampleRate > 0.0 ? reader->lengthInSamples / reader->sampleRate : 0.0;
        if (len <= 0.0) continue;
        reader.reset();

        auto v = std::make_unique<Visual>();
        // Each dropped file is its own block, so each gets its own track -- the same rule
        // "+ Block" follows. Dropping four stems used to lay them end to end on one fader.
        if (!items.empty()) { addLane(); lane = laneCount - 1; }
        v->block.lane = juce::jlimit(0, juce::jmax(0, laneCount - 1), lane);
        v->block.colour = v->block.lane;
        v->block.start = at;
        v->block.length = len;
        v->block.id = nextId++;
        v->block.name = nextBlockName();
        laneCount = juce::jlimit(1, CanvasAudioSource::kMaxLanes,
                                  juce::jmax(laneCount, v->block.lane + 1));
        if (laneNames[v->block.lane].isEmpty())
            laneNames.set(v->block.lane, "track " + juce::String(v->block.lane + 1));

        auto landed = f;
        if (auto folder = blockFolderFor(*v); folder != juce::File())
        {
            if (folder.createDirectory().wasOk())
            {
                auto target = folder.getChildFile(f.getFileName());
                for (int n = 2; target.existsAsFile(); ++n)
                    target = folder.getChildFile(f.getFileNameWithoutExtension() + "-" + juce::String(n)
                                                  + f.getFileExtension());
                // Copied, not moved or referenced: the file may be someone else's, and a
                // project that stops working because a sample was tidied up elsewhere is
                // not a project. Its sidecar comes too when it has one.
                if (f.copyFileTo(target))
                {
                    landed = target;
                    if (auto side = f.withFileExtension("json"); side.existsAsFile())
                        side.copyFileTo(target.withFileExtension("json"));
                }
            }
        }

        setFileOn(*v, landed);
        v->block.length = len;
        selected.clear();
        selected.insert(v->block.id);
        items.push_back(std::move(v));
        // NOT `at += len`. Four stems dropped together belong at the same moment on four
        // tracks, not one after another down a queue.
    }
    markDirty();
    rebuildAudio();
    announceSelection();
    // A dropped file IS a block, so the panel follows it like any other selection.
    if (!items.empty()) pointPanelAt(items.back().get());
    repaint();
}

void CanvasView::timerCallback()
{
    if (player.isPlaying())
    {
        const int lanes = juce::jmax(4, (getHeight() - lanesTop()) / laneHeight + 1);
        const std::array<float, 2> zero { 0.0f, 0.0f };
        if ((int) laneMeter.size()  < lanes) laneMeter.resize((size_t) lanes, zero);
        if ((int) laneHold.size()   < lanes) laneHold.resize((size_t) lanes, zero);
        if ((int) laneClipped.size() < lanes) laneClipped.resize((size_t) lanes, false);
        for (int lane = 0; lane < lanes && lane < CanvasAudioSource::kMaxLanes; ++lane)
            for (int ch = 0; ch < 2; ++ch)
            {
                const float hit = player.readAndClearLanePeak(lane, ch);
                auto& held = laneMeter[(size_t) lane][(size_t) ch];
                // Instant attack, slow release: a meter that falls as fast as it rises is
                // a flicker you cannot read at 30 fps.
                held = hit > held ? hit : held * 0.80f;
                auto& hold = laneHold[(size_t) lane][(size_t) ch];
                hold = hit > hold ? hit : hold * 0.985f;   // ~2 s to fall away
                // LATCHED. Full scale reached even once is the thing you need to know
                // about, and it is over before the meter has finished a frame.
                if (hit >= 0.999f) laneClipped[(size_t) lane] = true;
            }
        repaint();
        return;
    }
    // Let the meters fall to nothing after a stop rather than freezing mid-level.
    bool alive = false;
    for (auto& m : laneMeter)
        for (auto& c : m) { if (c > 0.0005f) { c *= 0.8f; alive = true; } else c = 0.0f; }
    for (auto& h : laneHold)
        for (auto& c : h) { if (c > 0.0005f) { c *= 0.9f; alive = true; } else c = 0.0f; }
    if (alive) { repaint(); return; }

    // Thumbnails load on a background thread and finish whenever they finish. Without
    // this the canvas draws whatever had arrived by the time the window opened and never
    // again -- which is why every block first appeared as a thin sliver of waveform with
    // the rest of it blank. Polled rather than listened to because the alternative is one
    // ChangeListener registration per block, unregistered on every delete.
    for (const auto& i : items)
        if (i->thumb != nullptr && !i->thumb->isFullyLoaded()) { repaint(); return; }
}

// ---- window -------------------------------------------------------------------------


// ---- the side panel's tabs ----------------------------------------------------------
//
// A vertical tab strip down the panel's inside edge, the way Blockhead has it: the panel
// is one column of window and the tabs say which tool is in it, rather than each tool
// getting a window of its own to arrange. Adding the next one is a row in an enum and a
// component -- which is the point of doing it this way rather than stacking panes.
enum class SideTab { Generate = 0, Master, Files, Count };

static const char* sideTabName (SideTab t)
{
    switch (t)
    {
        case SideTab::Generate: return "GENERATE";
        case SideTab::Master:   return "MASTER";
        case SideTab::Files:    return "FILES";
        default:                return "";
    }
}

class TabStrip : public juce::Component
{
public:
    explicit TabStrip (const MiraLookAndFeel& lafIn) : laf (lafIn) {}

    std::function<void(SideTab)> onTab;
    SideTab current = SideTab::Generate;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (MiraLookAndFeel::surface2);
        g.setColour (MiraLookAndFeel::border);
        g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());

        for (int i = 0; i < (int) SideTab::Count; ++i)
        {
            auto box = boxFor (i);
            const bool on = (SideTab) i == current;
            if (on)
            {
                g.setColour (MiraLookAndFeel::surface);
                g.fillRect (box);
                // The marker is on the INSIDE edge, against the content it selects, so the
                // tab reads as attached to the panel rather than as a button near it.
                g.setColour (MiraLookAndFeel::accent);
                g.fillRect (box.getRight() - 2, box.getY(), 2, box.getHeight());
            }

            // Rotated, because a vertical strip wide enough for horizontal words is not a
            // strip any more -- it is a second panel in front of the panel.
            juce::Graphics::ScopedSaveState state (g);
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi,
                                                              (float) box.getCentreX(),
                                                              (float) box.getCentreY()));
            g.setColour (on ? MiraLookAndFeel::text : MiraLookAndFeel::textFaint);
            g.setFont (laf.sansMedium (MiraLookAndFeel::textSize (10.0f)));
            g.drawText (sideTabName ((SideTab) i),
                        juce::Rectangle<int> (box.getCentreX() - box.getHeight() / 2,
                                               box.getCentreY() - box.getWidth() / 2,
                                               box.getHeight(), box.getWidth()),
                        juce::Justification::centred, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < (int) SideTab::Count; ++i)
            if (boxFor (i).contains (e.getPosition()))
            {
                current = (SideTab) i;
                if (onTab) onTab (current);
                repaint();
                return;
            }
    }

private:
    juce::Rectangle<int> boxFor (int i) const
    {
        constexpr int kTab = 86;
        return { 0, 6 + i * (kTab + 2), getWidth(), kTab };
    }

    const MiraLookAndFeel& laf;
};

// ---- MASTER --------------------------------------------------------------------------
//
// The same strip the tracks have, one size larger, on the same scale. It is the sum that
// leaves mira, so it is the one meter that can answer "is this going to clip" -- stacking
// N takes that each peak near full scale is N times full scale, and the tracks' own meters
// each say everything is fine.
class MasterStrip : public juce::Component, private juce::Timer
{
public:
    MasterStrip (const MiraLookAndFeel& lafIn, CanvasView& viewIn) : laf (lafIn), view (viewIn)
    {
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (MiraLookAndFeel::surface);

        // A SLIM CHANNEL: label, reading, fader. The readout sits above the strip rather
        // than beside it, so the panel is as wide as a fader and a scale and nothing else
        // -- and the fader gets the whole height, which is the part you actually aim at.
        // The old three-line "drag to set / cmd-click unity" tip is gone: it is a mixer
        // fader, it behaves like every other one, and it was costing more room than the
        // control.
        auto r = getLocalBounds().reduced (10, 12);
        g.setColour (MiraLookAndFeel::accent);
        g.setFont (laf.sansMedium (MiraLookAndFeel::textSize (10.0f)));
        g.drawText ("MASTER", r.removeFromTop (14), juce::Justification::centredLeft, false);

        {
            auto read = r.removeFromTop (19);
            g.setColour (MiraLookAndFeel::text);
            g.setFont (laf.sansMedium (MiraLookAndFeel::textSize (14.0f)));
            g.drawText (db() <= -60.0 ? juce::String ("-inf") : juce::String (db(), 1) + " dB",
                        read, juce::Justification::centredLeft, false);

            const float peakDb = juce::Decibels::gainToDecibels (juce::jmax (hold[0], hold[1], 1.0e-6f));
            g.setColour (clipped ? MiraLookAndFeel::warn : MiraLookAndFeel::textDim);
            g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (10.0f)));
            g.drawText (clipped ? "CLIP " + juce::String (peakDb, 1)
                                : juce::String (peakDb, 1) + " pk",
                        r.removeFromTop (14), juce::Justification::centredLeft, false);
        }
        r.removeFromTop (8);

        auto strip = r.removeFromLeft (juce::jmin (44, juce::jmax (30, r.getWidth() - 26)));

        g.setColour (MiraLookAndFeel::surface.darker (0.5f));
        g.fillRoundedRectangle (strip.toFloat(), 4.0f);
        g.setColour (MiraLookAndFeel::border.withAlpha (0.5f));
        g.drawRoundedRectangle (strip.toFloat().reduced (0.5f), 4.0f, 1.0f);

        auto yFor = [&] (double db) {
            return (float) strip.getBottom() - (float) strip.getHeight() * (float) CanvasView::dbToNorm (db);
        };

        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (9.0f)));
        for (double tick : { 6.0, 0.0, -6.0, -12.0, -24.0, -40.0 })
        {
            const float y = yFor (tick);
            g.setColour (MiraLookAndFeel::border.withAlpha (tick == 0.0 ? 0.9f : 0.45f));
            g.fillRect ((float) strip.getX() + 1.0f, y, (float) strip.getWidth() - 2.0f, 1.0f);
            g.setColour (MiraLookAndFeel::textFaint.withAlpha (tick == 0.0 ? 0.9f : 0.6f));
            g.drawText (tick > 0 ? "+" + juce::String ((int) tick) : juce::String ((int) tick),
                        juce::Rectangle<int> (strip.getRight() + 5, (int) y - 6, 28, 12),
                        juce::Justification::centredLeft, false);
        }

        // The stereo meter, inside the same well as the fader -- one control.
        auto meterArea = juce::Rectangle<int> (strip.getX() + 3, strip.getY(), 16, strip.getHeight());
        const float barW = (meterArea.getWidth() - 3.0f) * 0.5f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float x = meterArea.getX() + 1.0f + ch * (barW + 1.0f);
            if (level[(size_t) ch] > 0.0005f)
            {
                const double db = juce::Decibels::gainToDecibels (level[(size_t) ch]);
                auto band = [&] (double lo, double hi, juce::Colour c) {
                    if (db < lo) return;
                    const float y0 = yFor (juce::jmin (hi, db)), y1 = yFor (lo);
                    if (y0 >= y1) return;
                    g.setColour (c);
                    g.fillRect (juce::Rectangle<float> (x, y0, barW, y1 - y0));
                };
                band (-60.0, -6.0, MiraLookAndFeel::active);
                band (-6.0, -1.0, MiraLookAndFeel::accent);
                band (-1.0, 6.0, MiraLookAndFeel::warn);
            }
            if (hold[(size_t) ch] > 0.0005f)
            {
                const double db = juce::Decibels::gainToDecibels (hold[(size_t) ch]);
                g.setColour (db > -1.0 ? MiraLookAndFeel::warn : MiraLookAndFeel::text.withAlpha (0.85f));
                g.fillRect (x, yFor (db) - 1.0f, barW, 1.5f);
            }
        }

        // The fader groove and its cap, spanning the whole well.
        auto groove = juce::Rectangle<float> ((float) strip.getX() + 26.0f, (float) strip.getY(),
                                               3.0f, (float) strip.getHeight());
        g.setColour (MiraLookAndFeel::surface.darker (0.35f));
        g.fillRoundedRectangle (groove, 1.5f);
        const float capY = yFor (db());
        g.setColour (MiraLookAndFeel::accent.withAlpha (0.75f));
        g.fillRoundedRectangle (groove.withTop (capY), 1.5f);
        auto cap = juce::Rectangle<float> ((float) strip.getX() + 1.0f, capY - 5.0f,
                                            (float) strip.getWidth() - 2.0f, 10.0f);
        g.setColour (MiraLookAndFeel::accent.brighter (0.2f));
        g.fillRoundedRectangle (cap, 3.0f);
        g.setColour (MiraLookAndFeel::surface.darker (0.6f));
        g.drawRoundedRectangle (cap, 3.0f, 1.0f);
        g.fillRect (cap.getX() + 3.0f, cap.getCentreY() - 0.5f, cap.getWidth() - 6.0f, 1.0f);

        faderBox = strip;
    }

    void mouseDown (const juce::MouseEvent& e) override { drag (e); }
    void mouseDrag (const juce::MouseEvent& e) override { drag (e); }
    void mouseDoubleClick (const juce::MouseEvent&) override { clipped = false; repaint(); }

private:
    void drag (const juce::MouseEvent& e)
    {
        if (faderBox.isEmpty() || !faderBox.expanded (8, 4).contains (e.getPosition())) return;
        if (e.mods.isCommandDown()) { setDb (0.0); return; }
        setDb (CanvasView::normToDb (1.0 - (double) (e.y - faderBox.getY())
                                              / (double) faderBox.getHeight()));
    }

    double db() const
    {
        const float g = view.getMasterGain();
        return g <= 0.0f ? -60.0 : juce::jlimit (-60.0, 6.0, (double) juce::Decibels::gainToDecibels (g));
    }

    void setDb (double d)
    {
        d = juce::jlimit (-60.0, 6.0, d);
        view.setMasterGain (d <= -60.0 ? 0.0f : juce::Decibels::decibelsToGain ((float) d));
        repaint();
    }

    void timerCallback() override
    {
        bool moved = false;
        for (int ch = 0; ch < 2; ++ch)
        {
            const float hit = view.isPlaying() ? view.readAndClearPeak (ch) : 0.0f;
            auto& l = level[(size_t) ch];
            auto& h = hold[(size_t) ch];
            const float wasL = l, wasH = h;
            l = hit > l ? hit : l * 0.80f;
            h = hit > h ? hit : h * 0.995f;
            if (hit >= 0.999f) clipped = true;
            if (std::abs (l - wasL) > 0.0005f || std::abs (h - wasH) > 0.0005f) moved = true;
        }
        if (moved) repaint();
    }

    const MiraLookAndFeel& laf;
    CanvasView& view;
    juce::Rectangle<int> faderBox;
    std::array<float, 2> level { 0.0f, 0.0f }, hold { 0.0f, 0.0f };
    bool clipped = false;
};

// ---- FILES ----------------------------------------------------------------------------
//
// Every wav the project holds, across every block, in one list. The block folders ARE the
// pool -- there is no separate library to keep in step with them -- so this is a view of
// the filesystem rather than a second index that can disagree with it.
class FilesPanel : public juce::Component,
                   private juce::ListBoxModel,
                   private juce::Timer
{
public:
    FilesPanel (const MiraLookAndFeel& lafIn, CanvasView& viewIn,
                juce::AudioFormatManager& formatsIn)
        : laf (lafIn), view (viewIn), formats (formatsIn)
    {
        list.setModel (this);
        list.setRowHeight (34);
        list.setColour (juce::ListBox::backgroundColourId, MiraLookAndFeel::surface);
        addAndMakeVisible (list);
        startTimer (1500);      // the folder changes underneath us every generation
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 8);
        header = r.removeFromTop (34);
        list.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (MiraLookAndFeel::surface);
        g.setColour (MiraLookAndFeel::accent);
        g.setFont (laf.sansMedium (MiraLookAndFeel::textSize (11.0f)));
        g.drawText ("FILES", header.removeFromTop (16), juce::Justification::centredLeft, false);
        g.setColour (MiraLookAndFeel::textFaint);
        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (10.0f)));
        g.drawText (juce::String (rows.size()) + " takes in this project"
                     + juce::String (" - double-click to place on a new track"),
                    header, juce::Justification::centredLeft, false);
    }

    void refresh()
    {
        rows.clear();
        const auto root = view.getProjectFolder();
        if (root.isDirectory())
            for (const auto& f : root.findChildFiles (juce::File::findFiles, true, "*.wav"))
            {
                Row r;
                r.file = f;
                // The BLOCK it belongs to, which is its parent folder. That is the only
                // grouping this list needs, and it costs nothing to read.
                r.block = f.getParentDirectory() == root ? juce::String ("-")
                                                         : f.getParentDirectory().getFileName();
                rows.push_back (r);
            }
        std::sort (rows.begin(), rows.end(), [] (const Row& a, const Row& b) {
            return a.file.getLastModificationTime() > b.file.getLastModificationTime();
        });
        list.updateContent();
        repaint();
    }

private:
    struct Row { juce::File file; juce::String block; };

    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        const auto& r = rows[(size_t) row];
        if (selected) { g.setColour (MiraLookAndFeel::surface3); g.fillRect (0, 0, w, h); }

        g.setColour (MiraLookAndFeel::text);
        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (11.0f)));
        g.drawText (r.file.getFileNameWithoutExtension(),
                    juce::Rectangle<int> (8, 2, w - 16, 16), juce::Justification::centredLeft, true);

        g.setColour (MiraLookAndFeel::textFaint);
        g.setFont (laf.sansRegular (MiraLookAndFeel::textSize (9.5f)));
        g.drawText (r.block + "   " + juce::File::descriptionOfSizeInBytes (r.file.getSize()),
                    juce::Rectangle<int> (8, 17, w - 16, 14), juce::Justification::centredLeft, true);
        g.setColour (MiraLookAndFeel::border.withAlpha (0.4f));
        g.fillRect (0, h - 1, w, 1);
    }

    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override
    {
        if (row < 0 || row >= (int) rows.size()) return;
        // Placed the way a dropped file is: its own block, on its own track, at the
        // playhead. One rule for how audio arrives on the canvas.
        view.addFiles ({ rows[(size_t) row].file }, juce::jmax (0.0, view.getPositionSeconds()), 0);
    }

    juce::var getDragSourceDescription (const juce::SparseSet<int>& selectedRows) override
    {
        if (selectedRows.isEmpty()) return {};
        const int row = selectedRows[0];
        if (row < 0 || row >= (int) rows.size()) return {};
        return rows[(size_t) row].file.getFullPathName();
    }

    void timerCallback() override
    {
        // Polled rather than watched: a generation lands from another thread and a
        // directory watcher for one folder that changes every few minutes is more moving
        // parts than the problem has.
        const auto root = view.getProjectFolder();
        const auto stamp = root.isDirectory() ? root.getLastModificationTime() : juce::Time();
        if (stamp != lastStamp || (int) rows.size() == 0) { lastStamp = stamp; refresh(); }
    }

    const MiraLookAndFeel& laf;
    CanvasView& view;
    juce::AudioFormatManager& formats;
    juce::ListBox list;
    std::vector<Row> rows;
    juce::Rectangle<int> header;
    juce::Time lastStamp;
};

// ---- timecode (MIRA-VIDEO.md Phase 3) -----------------------------------------------

tc::Format CanvasView::timecodeFormat() const
{
    tc::Format f;
    if (!videoClips.empty())
    {
        const auto& c = videoClips.front();
        // A clip whose frame rate AVFoundation would not report falls back to the
        // canvas's own -- and the ruler menu says which one is in force, because a
        // timecode counted at the wrong rate is wrong in a way you cannot see.
        f.fps = c.fps > 0.0 ? c.fps : fallbackFps;
        f.dropFrame = c.dropFrame && tc::dropFrameIsPossible(f.fps);
        f.startSeconds = c.startTimecode;
    }
    else
    {
        f.fps = fallbackFps;
        f.dropFrame = fallbackDrop && tc::dropFrameIsPossible(f.fps);
        f.startSeconds = fallbackStart;
    }
    return f;
}

void CanvasView::setRulerMode(Ruler r)
{
    if (rulerMode == r) return;
    rulerMode = r;
    markDirty();
    if (onStateChanged) onStateChanged();
    repaint();
}

void CanvasView::setTimecodeFps(double fps)
{
    fps = juce::jlimit(1.0, 240.0, fps);
    if (!videoClips.empty()) videoClips.front().fps = fps;
    fallbackFps = fps;
    // Drop-frame only exists at 29.97 and 59.94. Leaving it set through a rate change
    // would leave a flag on that quietly renumbers a clock it has no business touching.
    if (!tc::dropFrameIsPossible(fps))
    {
        fallbackDrop = false;
        if (!videoClips.empty()) videoClips.front().dropFrame = false;
    }
    markDirty();
    if (onStateChanged) onStateChanged();
    repaint();
}

void CanvasView::setTimecodeDropFrame(bool drop)
{
    if (!videoClips.empty()) videoClips.front().dropFrame = drop;
    fallbackDrop = drop;
    markDirty();
    repaint();
}

void CanvasView::setTimecodeStart(double timecodeSeconds)
{
    timecodeSeconds = juce::jmax(0.0, timecodeSeconds);
    if (!videoClips.empty()) videoClips.front().startTimecode = timecodeSeconds;
    fallbackStart = timecodeSeconds;
    markDirty();
    repaint();
}

juce::String CanvasView::formatPosition(double seconds) const
{
    if (rulerMode == Ruler::Timecode) return tc::format(seconds, timecodeFormat());
    return formatTime(seconds);
}

void CanvasView::showRulerMenu(juce::Point<int> at)
{
    const auto f = timecodeFormat();
    const bool haveClip = !videoClips.empty() && videoClips.front().fps > 0.0;

    juce::PopupMenu rates;
    // The rates that exist in delivery, and nothing else. A free-text frame rate is a
    // field in which to make a typing mistake that then silently renumbers every cue.
    const double choices[] = { 23.976, 24.0, 25.0, 29.97, 30.0, 50.0, 59.94, 60.0 };
    for (int i = 0; i < (int) (sizeof(choices) / sizeof(choices[0])); ++i)
        rates.addItem(100 + i, juce::String(choices[i], choices[i] == std::floor(choices[i]) ? 0 : 3)
                                 + " fps", true, std::abs(f.fps - choices[i]) < 0.005);

    juce::PopupMenu m;
    // Markers first, and only the ones this click is actually about. A menu whose top item
    // changes with where you clicked is a menu that answers the question you asked.
    const int near = markerNear(at.x);
    if (near >= 0)
    {
        m.addSectionHeader(markers[(size_t) near].name + "  " + formatPosition(markers[(size_t) near].seconds));
        m.addItem(10, "Rename marker...", true, false);
        m.addItem(11, "Remove marker", true, false);
        m.addSeparator();
    }
    m.addItem(12, "Add marker at the playhead", true, false);
    m.addItem(13, "Block from here to the next marker", true, false);
    m.addSeparator();
    m.addItem(1, "Seconds", true, rulerMode == Ruler::Seconds);
    m.addItem(2, "Timecode", true, rulerMode == Ruler::Timecode);
    m.addSeparator();
    m.addSectionHeader(haveClip ? "Frame rate - from the film" : "Frame rate - no film loaded");
    m.addSubMenu("Frame rate", rates);
    // Offered ONLY where it exists. A drop-frame tick at 25 fps is a setting that can only
    // ever be wrong, and the fact that it is greyed out is itself the explanation.
    m.addItem(3, "Drop frame", tc::dropFrameIsPossible(f.fps), f.dropFrame);
    m.addSeparator();
    m.addItem(4, "Start timecode: " + tc::formatFrames((juce::int64) std::llround(
                     f.startSeconds * tc::nominalRate(f.fps)), tc::nominalRate(f.fps), f.dropFrame)
                     + "...", true, false);

    juce::Component::SafePointer<CanvasView> safe (this);
    const auto onScreen = localPointToGlobal(at);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea({ onScreen.x, onScreen.y, 1, 1 }),
                     [safe, choices, near](int id) {
        if (safe == nullptr || id == 0) return;
        auto& self = *safe;
        if (id >= 100 && id < 100 + (int) (sizeof(choices) / sizeof(choices[0])))
        { self.setTimecodeFps(choices[id - 100]); return; }
        switch (id)
        {
            case 1: self.setRulerMode(Ruler::Seconds); break;
            case 2: self.setRulerMode(Ruler::Timecode); break;
            case 3: self.setTimecodeDropFrame(!self.timecodeFormat().dropFrame); break;
            case 4: self.promptStartTimecode(); break;
            case 10: self.renameMarker(near); break;
            case 11: self.removeMarker(near); break;
            case 12: self.addMarkerAtPlayhead(); break;
            case 13: self.addBlockToNextMarker(); break;
            default: break;
        }
    });
}

void CanvasView::promptStartTimecode()
{
    const auto f = timecodeFormat();
    const int nominal = tc::nominalRate(f.fps);
    auto* window = new juce::AlertWindow("Start timecode",
                                          "The timecode of the film's first frame.",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("tc", tc::formatFrames((juce::int64) std::llround(f.startSeconds * nominal),
                                                  nominal, f.dropFrame));
    window->addButton("Set", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<CanvasView> safe (this);
    window->enterModalState(true, juce::ModalCallbackFunction::create([safe, window, nominal](int r) {
        std::unique_ptr<juce::AlertWindow> owned (window);
        if (safe == nullptr || r != 1) return;
        const auto typed = window->getTextEditorContents("tc");
        const double seconds = tc::parse(typed, nominal);
        // Convention 6 again: a timecode that will not parse says so. Silently keeping the
        // old one would leave every cue numbered from a start nobody chose.
        if (seconds < 0.0)
        {
            if (safe->onTakeNote) safe->onTakeNote("\"" + typed + "\" is not a timecode - try 01:00:00:00");
            return;
        }
        safe->setTimecodeStart(seconds);
        if (safe->onTakeNote) safe->onTakeNote("start timecode " + typed);
    }), false);
}

// ---- markers: the spotting notes (MIRA-VIDEO.md Phase 5) ----------------------------

void CanvasView::addMarkerAtPlayhead()
{
    const double at = juce::jmax(0.0, player.getPositionSeconds());
    // One marker per place. Dropping a second one on top of the first is a mis-click, not
    // a thing to keep, and two markers a frame apart cannot be told apart on screen.
    for (const auto& m : markers)
        if (std::abs(m.seconds - at) < 0.01)
        {
            if (onTakeNote) onTakeNote("there is already a marker there: " + m.name);
            return;
        }

    pushUndo();
    UndoGuard oneEdit (*this);
    Marker m;
    m.seconds = at;
    m.name = "marker " + juce::String((int) markers.size() + 1);
    markers.push_back(m);
    std::sort(markers.begin(), markers.end(),
               [](const Marker& a, const Marker& b) { return a.seconds < b.seconds; });
    markDirty();
    if (onMarkersChanged) onMarkersChanged();
    if (onTakeNote) onTakeNote("marker at " + formatPosition(at));
    resized();
    repaint();
}

void CanvasView::removeMarker(int index)
{
    if (!juce::isPositiveAndBelow(index, (int) markers.size())) return;
    pushUndo();
    UndoGuard oneEdit (*this);
    markers.erase(markers.begin() + index);
    markDirty();
    if (onMarkersChanged) onMarkersChanged();
    resized();
    repaint();
}

void CanvasView::renameMarker(int index)
{
    if (!juce::isPositiveAndBelow(index, (int) markers.size())) return;
    auto* window = new juce::AlertWindow("Marker", "What happens here?",
                                          juce::MessageBoxIconType::NoIcon);
    window->addTextEditor("name", markers[(size_t) index].name);
    window->addButton("Set", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));

    juce::Component::SafePointer<CanvasView> safe (this);
    window->enterModalState(true, juce::ModalCallbackFunction::create([safe, window, index](int r) {
        std::unique_ptr<juce::AlertWindow> owned (window);
        if (safe == nullptr || r != 1) return;
        const auto name = window->getTextEditorContents("name").trim();
        if (name.isEmpty()) return;
        if (!juce::isPositiveAndBelow(index, (int) safe->markers.size())) return;
        safe->setMarkerName(index, name);
    }), false);
}

int CanvasView::markerNear(int x) const
{
    int best = -1, bestDistance = 8;   // pixels, not seconds -- see snapToMarker
    for (int i = 0; i < (int) markers.size(); ++i)
    {
        const int distance = std::abs(secondsToX(markers[(size_t) i].seconds) - x);
        if (distance <= bestDistance) { bestDistance = distance; best = i; }
    }
    return best;
}

int CanvasView::markerAtStripX(int x) const
{
    if (x < kHeaderWidth) return -1;
    int best = -1;
    for (int i = 0; i < (int) markers.size(); ++i)
    {
        const int mx = secondsToX(markers[(size_t) i].seconds);
        if (mx <= x + 2 && (best < 0 || mx > secondsToX(markers[(size_t) best].seconds))) best = i;
    }
    return best;
}

int CanvasView::markerAfter(double seconds) const
{
    for (int i = 0; i < (int) markers.size(); ++i)      // kept sorted
        if (markers[(size_t) i].seconds > seconds + 0.001) return i;
    return -1;
}

double CanvasView::snapToMarker(double seconds) const
{
    // By PIXELS. What "close enough" means depends on the zoom, and a snap that is a
    // second wide zoomed out and a frame wide zoomed in is a snap you cannot predict.
    const int x = secondsToX(seconds);
    const int i = markerNear(x);
    return i >= 0 ? markers[(size_t) i].seconds : seconds;
}

void CanvasView::addBlockToNextMarker()
{
    const double at = juce::jmax(0.0, player.getPositionSeconds());
    const int next = markerAfter(at);
    if (next < 0)
    {
        // Convention 6: the reason, not a no-op. "Nothing happened" and "there is nothing
        // after the playhead to measure to" look identical from the outside.
        if (onTakeNote) onTakeNote("no marker after the playhead - add one where the cue has to be out");
        return;
    }
    const double length = markers[(size_t) next].seconds - at;
    if (length < 0.25) { if (onTakeNote) onTakeNote("that marker is too close to make a block to"); return; }

    addEmptyBlock();
    if (auto* v = singleSelection())
    {
        v->block.start = at;
        v->block.length = length;
        markDirty();
        rebuildAudio();
        announceSelection();
        if (onBlockGeometry) onBlockGeometry(v->block.length, tailSecondsOf(*v), v->block.hasAudio());
        if (onTakeNote)
            onTakeNote("block from " + formatPosition(at) + " to " + markers[(size_t) next].name
                        + " - " + juce::String(length, 1) + " s");
    }
    repaint();
}

void CanvasView::paintMarkers(juce::Graphics& g)
{
    if (markers.empty()) return;
    const int top = topRuler, h = markerStripH();

    // ITS OWN ROW. The first version drew the labels on the ruler, where they landed among
    // the time ticks and neither could be read.
    g.setColour(MiraLookAndFeel::surface);
    g.fillRect(0, top, getWidth(), h);
    g.setColour(MiraLookAndFeel::border);
    g.drawHorizontalLine(top + h - 1, 0.0f, (float) getWidth());

    g.setFont(laf.sansMedium(MiraLookAndFeel::textSize(9.5f)));
    for (int i = 0; i < (int) markers.size(); ++i)
    {
        const auto& m = markers[(size_t) i];
        const int x = secondsToX(m.seconds);
        if (x > getWidth()) continue;

        // The line still runs the full height, faint: a marker is a place on the TIMELINE,
        // and you need to see what it cuts through.
        if (x >= kHeaderWidth)
        {
            g.setColour(MiraLookAndFeel::active.withAlpha(0.22f));
            g.drawVerticalLine(x, (float) (top + h), (float) getHeight());
        }

        // The label runs to the NEXT marker and no further, so two close together truncate
        // instead of printing over each other.
        int until = getWidth();
        for (int j = 0; j < (int) markers.size(); ++j)
            if (markers[(size_t) j].seconds > m.seconds)
                until = juce::jmin(until, secondsToX(markers[(size_t) j].seconds));
        auto plate = juce::Rectangle<int>(x, top + 2, juce::jmax(14, until - x - 3), h - 5);

        juce::Graphics::ScopedSaveState clipped(g);
        g.reduceClipRegion(juce::Rectangle<int>(kHeaderWidth, top, getWidth() - kHeaderWidth, h));
        const bool dragging = drag == Drag::MarkerMove && i == dragTargetMarker;
        g.setColour(MiraLookAndFeel::active.withAlpha(dragging ? 0.42f : 0.22f));
        g.fillRoundedRectangle(plate.toFloat(), 2.5f);
        g.setColour(MiraLookAndFeel::active);
        g.fillRect(x - 1, top + 1, 2, h - 3);
        g.drawText(m.name, plate.reduced(6, 0), juce::Justification::centredLeft, true);
    }

    // The header says what the row is, like every other row on this canvas.
    g.setColour(MiraLookAndFeel::surface2);
    g.fillRect(0, top, kHeaderWidth, h);
    g.setColour(MiraLookAndFeel::border);
    g.drawVerticalLine(kHeaderWidth - 1, (float) top, (float) (top + h));
    g.setColour(MiraLookAndFeel::textFaint);
    g.setFont(laf.monoRegular(MiraLookAndFeel::textSize(9.0f)));
    g.drawText("MARKERS", juce::Rectangle<int>(12, top, kHeaderWidth - 18, h),
                juce::Justification::centredLeft, false);
}

void CanvasView::setMarkerName(int index, const juce::String& name)
{
    if (!juce::isPositiveAndBelow(index, (int) markers.size()) || name.trim().isEmpty()) return;
    pushUndo();
    markers[(size_t) index].name = name.trim();
    markDirty();
    if (onMarkersChanged) onMarkersChanged();
    repaint();
}

void CanvasView::setMarkerTime(int index, double seconds)
{
    if (!juce::isPositiveAndBelow(index, (int) markers.size())) return;
    markers[(size_t) index].seconds = juce::jmax(0.0, seconds);
    markDirty();
    repaint();
}

void CanvasView::gotoMarker(int index)
{
    if (!juce::isPositiveAndBelow(index, (int) markers.size())) return;
    const double at = markers[(size_t) index].seconds;
    player.setPositionSeconds(at);
    // Bring it on screen if it is not. A "go to" that leaves you looking somewhere else is
    // a go-to you have to follow up with a scroll.
    const double visible = (getWidth() - kHeaderWidth) / pixelsPerSecond;
    if (at < viewStart || at > viewStart + visible)
        viewStart = juce::jmax(0.0, at - visible * 0.35);
    repaint();
}

void CanvasView::promptExportCueSheet()
{
    if (items.empty()) { if (onTakeNote) onTakeNote("there are no blocks to write down"); return; }

    const auto suggested = documentFile != juce::File()
                               ? documentFile.getParentDirectory().getChildFile(getDocumentName() + " cues.csv")
                               : juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                                     .getChildFile("cues.csv");
    auto chooser = std::make_shared<juce::FileChooser>("Export the cue sheet", suggested, "*.csv");
    juce::Component::SafePointer<CanvasView> safe (this);
    chooser->launchAsync(juce::FileBrowserComponent::saveMode
                             | juce::FileBrowserComponent::canSelectFiles
                             | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, chooser](const juce::FileChooser& fc) {
        if (safe == nullptr) return;
        auto& self = *safe;
        const auto dest = fc.getResult();
        if (dest == juce::File()) return;

        // In TIMECODE whatever the ruler is set to. A cue sheet is paperwork for someone
        // else, and the someone else counts in timecode.
        const auto f = self.timecodeFormat();
        auto quote = [](juce::String v) { return "\"" + v.replace("\"", "\"\"") + "\""; };

        juce::StringArray lines;
        lines.add("cue,track,in,out,length,seconds,key,tempo,take");

        // In ORDER, which the canvas's own list is not -- items are in the order they were
        // made. A cue sheet out of order is a cue sheet nobody can read against picture.
        std::vector<const Visual*> ordered;
        for (const auto& i : self.items)
            if (!self.isReferenceLane(i->block.lane)) ordered.push_back(i.get());
        std::sort(ordered.begin(), ordered.end(), [](const Visual* a, const Visual* b) {
            return a->block.start < b->block.start;
        });

        for (const auto* v : ordered)
        {
            const auto tags = keyAndTempoOf(v->settings);
            juce::String key, tempo;
            // keyAndTempoOf renders "C minor - 120 BPM" for the header; split it back out
            // so a spreadsheet gets two columns rather than one it has to be taught to cut.
            if (tags.contains(" - "))
            {
                key = tags.upToFirstOccurrenceOf(" - ", false, false).trim();
                tempo = tags.fromFirstOccurrenceOf(" - ", false, false).trim();
            }
            else key = tags;

            lines.add(quote(v->block.name) + ","
                       + quote(self.laneNames[v->block.lane].isNotEmpty()
                                   ? self.laneNames[v->block.lane]
                                   : "track " + juce::String(v->block.lane + 1)) + ","
                       + quote(tc::format(v->block.start, f)) + ","
                       + quote(tc::format(v->block.end(), f)) + ","
                       + quote(tc::formatFrames((juce::int64) std::llround(v->block.length * f.fps),
                                                 tc::nominalRate(f.fps), f.dropFrame)) + ","
                       + juce::String(v->block.length, 3) + ","
                       + quote(key) + "," + quote(tempo) + ","
                       + quote(v->block.hasAudio() ? v->block.file.getFileName() : juce::String()));
        }

        // The markers too. A spotting note with no cue against it yet is exactly the row
        // you want to see on the sheet.
        for (const auto& m : self.markers)
            lines.add(quote(m.name) + ",\"(marker)\"," + quote(tc::format(m.seconds, f))
                       + ",,,\"" + juce::String(m.seconds, 3) + "\",,,");

        if (dest.replaceWithText(lines.joinIntoString("\n") + "\n"))
        {
            if (self.onTakeNote)
                self.onTakeNote("wrote " + juce::String(ordered.size()) + " cues and "
                                 + juce::String((int) self.markers.size()) + " markers to "
                                 + dest.getFileName());
        }
        else if (self.onTakeNote) self.onTakeNote("could not write " + dest.getFullPathName());
    });
}

// ---- picture on the timeline (MIRA-VIDEO.md Phase 1) --------------------------------

// (kPictureColour is defined near the top of this file -- paint() needs it.)
// The picture track's colour, and it is deliberately NOT one of laneColour's eight. Every
// track colour in this canvas is a desaturated mid-tone, because eight of them have to sit
// side by side without any one shouting; a saturated violet belongs to none of that family,
// so the video track reads as a DIFFERENT KIND OF THING before you have read the word
// PICTURE. That is the whole job: it is the one lane on the canvas that carries no audio,
// sums into nothing and exports nowhere, and it should not look like a track you could mix.

void CanvasView::paintVideoStrip(juce::Graphics& g)
{
    if (videoClips.empty()) return;

    const auto band = juce::Rectangle<int>(0, videoStripTop(), getWidth(), videoStripH());
    g.setColour(MiraLookAndFeel::surface);
    g.fillRect(band);

    // The clips first, then the header over them -- the same order the audio lanes use,
    // so a clip scrolled off the left disappears UNDER the header rather than over it.
    {
        juce::Graphics::ScopedSaveState clipped(g);
        g.reduceClipRegion(band.withTrimmedLeft(kHeaderWidth));
        for (const auto& c : videoClips)
        {
            const int x0 = secondsToX(c.start);
            const int x1 = secondsToX(c.start + juce::jmax(0.5, c.length));
            auto r = juce::Rectangle<int>(x0, videoStripTop() + 3, juce::jmax(2, x1 - x0), videoStripH() - 6);
            g.setColour(kPictureColour.withAlpha(0.18f));
            g.fillRoundedRectangle(r.toFloat(), 3.0f);
            g.setColour(kPictureColour.withAlpha(0.75f));
            g.drawRoundedRectangle(r.toFloat().reduced(0.5f), 3.0f, 1.2f);
            g.setColour(MiraLookAndFeel::text);
            g.setFont(laf.sansSemiBold(MiraLookAndFeel::textSize(10.5f)));
            // The length is said out loud in the strip, because a clip whose length
            // AVFoundation would not report is drawn at a made-up half second, and that
            // has to look wrong rather than look like a very short film.
            const auto label = c.file.getFileName()
                                 + (c.length > 0.0 ? "   " + juce::String(c.length, 1) + " s"
                                                   : juce::String("   length unknown"));
            g.drawText(label, r.reduced(7, 0), juce::Justification::centredLeft, true);
        }
    }

    // The header, tinted rather than plain surface2 -- the tracks below all share one
    // header colour and take their identity from a stripe, so a tinted header is itself
    // the signal that this row is not one of them.
    g.setColour(MiraLookAndFeel::surface2);
    g.fillRect(0, videoStripTop(), kHeaderWidth, videoStripH());
    g.setColour(kPictureColour.withAlpha(0.12f));
    g.fillRect(0, videoStripTop(), kHeaderWidth, videoStripH());
    // A solid edge of the colour down the left, the way a track's colour stripe runs.
    g.setColour(kPictureColour);
    g.fillRect(0, videoStripTop(), 3, videoStripH());
    g.setColour(MiraLookAndFeel::border);
    g.drawVerticalLine(kHeaderWidth - 1, static_cast<float>(videoStripTop()), static_cast<float>(lanesTop()));
    g.drawHorizontalLine(lanesTop() - 1, 0.0f, static_cast<float>(getWidth()));
    g.setColour(kPictureColour);
    g.setFont(laf.monoMedium(MiraLookAndFeel::textSize(10.0f)));
    g.drawText("PICTURE", juce::Rectangle<int>(12, videoStripTop(), kHeaderWidth - 18, videoStripH()),
               juce::Justification::centredLeft, false);
}

void CanvasView::addVideoClip(const juce::File& file, double lengthSeconds, double framesPerSecond)
{
    pushUndo();
    UndoGuard oneEdit (*this);

    VideoClip c;
    c.file = file;
    c.length = juce::jmax(0.0, lengthSeconds);
    c.fps = juce::jmax(0.0, framesPerSecond);
    // END TO END, after the last clip. A cut arrives in reels, and reel 2 starts where
    // reel 1 finished -- anywhere else and you would have to place it by hand before you
    // could watch it. Non-overlapping by construction, which is what makes "there is only
    // ever one thing to look at" true rather than a rule someone has to remember.
    double after = 0.0;
    for (const auto& existing : videoClips) after = juce::jmax(after, existing.start + existing.length);
    c.start = after;

    videoClips.push_back(c);
    markDirty();
    if (onVideoClipsChanged) onVideoClipsChanged();
    resized();
    repaint();
}

void CanvasView::removeVideoClip(int index)
{
    if (!juce::isPositiveAndBelow(index, (int) videoClips.size())) return;
    pushUndo();
    UndoGuard oneEdit (*this);

    // Its reference block goes with it. They are one object with two faces, and a
    // reference left behind for a film that is gone is dialogue with nothing to explain it.
    const auto blockId = videoClips[(size_t) index].audioBlockId;
    if (blockId != 0)
        items.erase(std::remove_if(items.begin(), items.end(),
                                    [blockId](const std::unique_ptr<Visual>& v) {
                                        return v->block.id == blockId;
                                    }),
                     items.end());

    videoClips.erase(videoClips.begin() + index);
    // The last clip takes the lane with it: an empty REFERENCE track is a row that can
    // only confuse.
    if (videoClips.empty()) detachReference();

    markDirty();
    if (onVideoClipsChanged) onVideoClipsChanged();
    rebuildAudio();
    resized();
    repaint();
}

int CanvasView::videoClipAt(double seconds) const
{
    for (int i = 0; i < (int) videoClips.size(); ++i)
    {
        const auto& c = videoClips[(size_t) i];
        if (seconds >= c.start && seconds < c.start + juce::jmax(0.5, c.length)) return i;
    }
    return -1;
}

void CanvasView::showVideoClipMenu(int index, juce::Point<int> at)
{
    if (!juce::isPositiveAndBelow(index, (int) videoClips.size())) return;
    const auto& c = videoClips[(size_t) index];

    juce::PopupMenu m;
    m.addSectionHeader(c.file.getFileName());
    // 4.3 -- each clip keeps its OWN rate. Two reels at different rates is a real thing,
    // so the rate is shown per clip rather than as one setting for the whole track.
    m.addItem(1, juce::String(c.fps, 3) + " fps, " + juce::String(c.length, 1) + " s", false, false);
    m.addItem(2, "Start timecode...", true, false);
    m.addSeparator();
    m.addItem(3, "Remove this clip", true, false);

    juce::Component::SafePointer<CanvasView> safe (this);
    const auto onScreen = localPointToGlobal(at);
    m.showMenuAsync(juce::PopupMenu::Options().withTargetScreenArea({ onScreen.x, onScreen.y, 1, 1 }),
                     [safe, index](int id) {
        if (safe == nullptr || id == 0) return;
        if (id == 2) safe->promptStartTimecode();
        if (id == 3) safe->removeVideoClip(index);
    });
}

// ---- the reference track (MIRA-VIDEO.md Phase 2) ------------------------------------

int CanvasView::ensureReferenceLane()
{
    if (referenceLane >= 0) return referenceLane;

    // Its own lane, at the bottom. Appending rather than inserting keeps every existing
    // block's lane index -- and so every mute bit, solo bit and fader -- exactly where it
    // was, which an insert at 0 would not.
    referenceLane = juce::jlimit(0, CanvasAudioSource::kMaxLanes - 1, laneCount);
    laneCount = juce::jlimit(1, CanvasAudioSource::kMaxLanes, referenceLane + 1);
    laneNames.set(referenceLane, "REFERENCE");
    setLaneDb(referenceLane, 0.0);
    // Locked, and shorter, from the moment it arrives. The reference is something you
    // glance at to find a cut, not something you read the waveform of -- and a track you
    // never edit should not grow every time you zoom the ones you do.
    ensureLaneArrays();
    laneH[(size_t) referenceLane] = kReferenceHeight;
    return referenceLane;
}

void CanvasView::attachReference(const juce::File& audio, double startOnTimeline)
{
    if (!audio.existsAsFile())
    {
        if (onTakeNote) onTakeNote("the film's audio is not where it was left: " + audio.getFullPathName());
        return;
    }

    pushUndo();
    UndoGuard oneEdit (*this);
    ensureReferenceLane();

    auto v = std::make_unique<Visual>();
    v->block.lane = referenceLane;
    v->block.colour = referenceLane;
    v->block.start = startOnTimeline;
    v->block.id = nextId++;
    v->block.name = audio.getFileNameWithoutExtension();
    // length 0 so setFileOn takes it from the file: the reference is exactly as long as
    // the film's audio, and there is no gesture that could have made it anything else.
    v->block.length = 0.0;
    setFileOn(*v, audio);
    // Tied to the clip it came from, by START: several clips mean several reference blocks
    // on the one lane, and removing a clip has to know which of them was its.
    for (auto& c : videoClips)
        if (std::abs(c.start - startOnTimeline) < 0.001 && c.audioBlockId == 0)
        { c.audioBlockId = v->block.id; break; }
    items.push_back(std::move(v));

    markDirty();
    rebuildAudio();
    repaint();
}

double CanvasView::referenceWaveformProgress() const
{
    if (referenceLane < 0) return -1.0;
    for (const auto& i : items)
        if (i->block.lane == referenceLane && i->thumb != nullptr)
        {
            const double p = i->thumb->getProportionComplete();
            // 0.999, not 1.0. getProportionComplete never quite reaches 1.0 -- the Phase 0
            // spike spent its whole 120 s timeout waiting for it to, on a read that had
            // finished in two seconds.
            return p >= 0.999 ? -1.0 : p;
        }
    return -1.0;
}

void CanvasView::detachReference()
{
    if (referenceLane < 0) return;
    const int lane = referenceLane;
    referenceLane = -1;       // cleared FIRST, or removeLane below refuses its own caller

    items.erase(std::remove_if(items.begin(), items.end(),
                                [lane](const std::unique_ptr<Visual>& v) { return v->block.lane == lane; }),
                 items.end());
    for (auto& v : items) if (v->block.lane > lane) --v->block.lane;
    laneNames.remove(lane);
    if (lane < (int) laneDb.size()) laneDb.erase(laneDb.begin() + lane);
    if (lane < (int) laneH.size())  laneH.erase(laneH.begin() + lane);
    auto shift = [lane](juce::uint64 mask) {
        const juce::uint64 below = mask & ((juce::uint64 (1) << lane) - 1);
        const juce::uint64 above = mask >> (lane + 1);
        return below | (above << lane);
    };
    muteMask = shift(muteMask);
    soloMask = shift(soloMask);
    applyMasks();
    laneCount = juce::jmax(1, laneCount - 1);
    if (!videoClips.empty()) videoClips.front().audioBlockId = 0;
    rebuildAudio();
    repaint();
}

void CanvasView::clearVideo()
{
    if (videoClips.empty()) return;
    pushUndo();
    detachReference();
    videoClips.clear();
    markDirty();
    if (onVideoCleared) onVideoCleared();
    resized();
    repaint();
}

// ---- the marker list (MIRA-VIDEO.md Phase 5) ----------------------------------------
//
// A spotting session produces a LIST -- twenty or thirty notes with timecodes -- and a
// list is not something you read off a timeline one screen at a time. This is the same
// shape as the LoRA library window: a table you keep open beside the work, where a
// double-click takes you to the thing.
class MarkerListWindow : public juce::DocumentWindow
{
public:
    MarkerListWindow(const MiraLookAndFeel& laf, CanvasView& viewIn)
        : juce::DocumentWindow("Markers", MiraLookAndFeel::surface, juce::DocumentWindow::closeButton)
    {
        content = std::make_unique<Content>(laf, viewIn);
        setUsingNativeTitleBar(true);
        setContentNonOwned(content.get(), false);
        setResizable(true, false);
        centreWithSize(360, 420);
        setVisible(true);
        mira_ui::chrome::applyDarkTitleBar(*this, MiraLookAndFeel::surface2);
    }
    ~MarkerListWindow() override { setContentNonOwned(nullptr, false); }

    void closeButtonPressed() override { if (onClosed) onClosed(); }
    std::function<void()> onClosed;
    void refresh() { content->refresh(); }
    juce::String geometryString() { return getWindowStateAsString(); }
    void restoreGeometry(const juce::String& s) { if (s.isNotEmpty()) restoreWindowStateFromString(s); }

private:
    struct Content : juce::Component, juce::ListBoxModel
    {
        Content(const MiraLookAndFeel& lafIn, CanvasView& viewIn) : laf(lafIn), view(viewIn)
        {
            // OWNER FIRST, THEN THE MODEL -- the launch window's recents list was empty for
            // a day because setModel ran before the thing the model reads was set.
            addAndMakeVisible(list);
            list.setRowHeight(24);
            list.setColour(juce::ListBox::backgroundColourId, MiraLookAndFeel::surface2);
            list.setModel(this);

            auto button = [this](juce::TextButton& b, const juce::String& text) {
                b.setButtonText(text);
                addAndMakeVisible(b);
            };
            button(addButton, "+ At Playhead");
            button(renameButton, "Rename");
            button(deleteButton, "Remove");
            addButton.onClick    = [this] { view.addMarkerAtPlayhead(); refresh(); };
            renameButton.onClick = [this] { if (selected() >= 0) view.renameMarker(selected()); };
            deleteButton.onClick = [this] { if (selected() >= 0) { view.removeMarker(selected()); refresh(); } };

            hint.setText("double-click a marker to put the playhead on it",
                          juce::dontSendNotification);
            hint.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
            hint.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
            addAndMakeVisible(hint);
            refresh();
        }

        int selected() const { return list.getSelectedRow(); }
        void refresh() { list.updateContent(); list.repaint(); }

        void paint(juce::Graphics& g) override { g.fillAll(MiraLookAndFeel::surface); }
        void resized() override
        {
            auto r = getLocalBounds().reduced(10);
            auto bar = r.removeFromBottom(30);
            addButton.setBounds(bar.removeFromLeft(110).reduced(2));
            renameButton.setBounds(bar.removeFromLeft(90).reduced(2));
            deleteButton.setBounds(bar.removeFromLeft(90).reduced(2));
            hint.setBounds(r.removeFromBottom(20));
            list.setBounds(r);
        }

        int getNumRows() override { return (int) view.getMarkers().size(); }

        void paintListBoxItem(int row, juce::Graphics& g, int width, int height, bool selectedRow) override
        {
            const auto& markers = view.getMarkers();
            if (!juce::isPositiveAndBelow(row, (int) markers.size())) return;
            if (selectedRow) { g.setColour(MiraLookAndFeel::accent.withAlpha(0.18f)); g.fillRect(0, 0, width, height); }

            // The time reads the way the RULER reads. A marker list in seconds beside a
            // timeline in timecode is two numbers for one place.
            g.setColour(MiraLookAndFeel::active);
            g.setFont(laf.monoRegular(MiraLookAndFeel::textSize(10.5f)));
            const auto when = view.formatPosition(markers[(size_t) row].seconds);
            g.drawText(when, 8, 0, 96, height, juce::Justification::centredLeft, false);

            g.setColour(MiraLookAndFeel::text);
            g.setFont(laf.sansRegular(MiraLookAndFeel::textSize(11.5f)));
            g.drawText(markers[(size_t) row].name, 110, 0, width - 118, height,
                        juce::Justification::centredLeft, true);
        }

        void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override
        {
            view.gotoMarker(row);
        }

        const MiraLookAndFeel& laf;
        CanvasView& view;
        juce::ListBox list;
        juce::TextButton addButton, renameButton, deleteButton;
        juce::Label hint;
    };
    std::unique_ptr<Content> content;
};

struct CanvasWindow::Content : juce::Component, private juce::Timer
{
    Content(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
            juce::AudioThumbnailCache& cache, GenerateContent* panelIn)
        : view(laf, formats, cache), panel(panelIn), tabs(laf), formatManager(formats), look(laf)
    {
        auto button = [this](juce::TextButton& b, const juce::String& text) {
            b.setButtonText(text);
            addAndMakeVisible(b);
        };
        button(playButton, "Play");
        button(loopButton, "Loop selection");
        button(fitButton, "Fit");
        button(deleteButton, "Remove");

        playButton.onClick   = [this] { view.togglePlay(); };
        loopButton.onClick   = [this] { view.toggleLoop(); };
        fitButton.onClick    = [this] { view.fit(); };
        deleteButton.onClick = [this] { view.removeSelected(); };

        hint.setText("space play - L loop - M/S mute solo - F fit - G/H zoom - [ ] wave height - cmd-E cut - cmd-Z undo",
                      juce::dontSendNotification);
        hint.setFont(laf.sansRegular(MiraLookAndFeel::textSize(10.5f)));
        hint.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        addAndMakeVisible(hint);

        button(newProjectButton, "New");
        button(openProjectButton, "Open...");
        button(saveProjectButton, "Save");
        button(addBlockButton, "+ Block");
        button(duplicateButton, "Duplicate");
        duplicateButton.onClick = [this] { view.duplicateSelection(); };

        // Extend and Remix used to live here. They are a BLOCK's actions, driven by the
        // block's own prompt, so they belong in the block's generator -- on the toolbar
        // they read as something the canvas does.
        if (panel != nullptr)
        {
            panel->onExtend = [this] { view.extendSelection(false); };
            panel->onRemix  = [this] { view.extendSelection(true);  };
        }
        button(addTrackButton, "+ Track");
        addAndMakeVisible(tabs);
        master = std::make_unique<MasterStrip>(laf, view);
        files  = std::make_unique<FilesPanel>(laf, view, formats);
        addChildComponent(*master);
        addChildComponent(*files);
        tabs.onTab = [this](SideTab t) {
            tab = t;
            // Switching tabs OPENS the panel. A tab you can click while the panel is
            // folded that then does nothing visible is a control that lies.
            if (panelCollapsed)
            {
                panelCollapsed = false;
                panelToggle.setButtonText("Panel >");
            }
            applyTab();
            resized();
        };

        button(panelToggle, "Panel >");
        panelToggle.onClick = [this] {
            panelCollapsed = !panelCollapsed;
            panelToggle.setButtonText(panelCollapsed ? "Panel <" : "Panel >");
            applyTab();
            blockLabel.setVisible(!panelCollapsed && tab == SideTab::Generate);
            resized();
        };
        newProjectButton.onClick  = [this] { promptNewProject(); };
        openProjectButton.onClick = [this] { promptOpenProject(); };
        saveProjectButton.onClick = [this] { saveOrAsk(); };
        view.onDocumentChanged = [this] { updateTitle(); };
        addBlockButton.onClick   = [this] { view.addEmptyBlock(); };
        addTrackButton.onClick   = [this] { view.addLane(); };

        // The generate pane, IN the side panel, in column mode: controls over takes, no
        // project tree and no second takes column. The canvas already is the arrangement
        // view, so those would be three views of the same project fighting over 340px.
        if (panel != nullptr)
        {
            panel->setPanelOnly(true);
            panel->setTrainingBenchVisible(false);
            // Starts with nothing selected, so it never opens pointed at the default
            // ~/Music/mira-generated it was constructed with.
            panel->setNoTarget();
            addAndMakeVisible(panel);
        }
        // Selecting a block points the panel at that block's folder. Nothing opens, moves
        // or floats -- the panel is always there and always about whatever is selected,
        // which is what "feels united" means in practice.
        view.onRevealGenerator = [this] {
            // Double-clicking a block is asking for its GENERATOR, so the tab follows.
            tabs.current = tab = SideTab::Generate;
            if (!panelCollapsed) { applyTab(); resized(); return; }
            panelCollapsed = false;
            panelToggle.setButtonText("Panel >");
            if (panel != nullptr) panel->setVisible(true);
            blockLabel.setVisible(true);
            resized();
        };
        view.onBlockGeometry = [this](double lengthSeconds, double tailSeconds, bool hasAudio) {
            // The block's length IS the duration. Resizing the frame is how you ask for a
            // different length, rather than typing a number somewhere else on screen.
            if (panel != nullptr) panel->setDuration(lengthSeconds);
            // Extend needs somewhere to put the audio; remix only needs audio to remix.
            if (panel != nullptr)
                panel->setBlockActions(tailSeconds > 0.05, hasAudio && lengthSeconds > 0.0);
        };
        view.onExtendRequested = [this](const juce::File& take, double rangeStart,
                                         double totalSeconds, bool remix) {
            if (panel == nullptr) return;
            // Neither one touches the prompt. What is on screen is what runs.
            if (remix) panel->generateRemix(take, totalSeconds);
            else       panel->generateExtension(take, rangeStart, totalSeconds);
        };
        view.onExtendRefused = [this](const juce::String& why) {
            if (panel != nullptr) panel->setStatus(why);
        };
        // Picture. The clip is the document's; the WINDOW is this session's, so opening
        // a project that carries a film opens the film with it.
        view.onVideoClipsChanged = [this] { syncPicture(); };
        // The list window, when there is one, follows the canvas rather than being told
        // by each of the five places that can change a marker.
        view.onMarkersChanged = [this] { if (markerWindow != nullptr) markerWindow->refresh(); };
        view.onVideoCleared = [this] {
            storeVideoGeometry();
            videoWindow.reset();
            if (owner != nullptr && owner->onVideoChanged) owner->onVideoChanged();
        };
        view.onTakeNote = [this](const juce::String& text) {
            note(text);
        };
        view.onCaptureSettings = [this]() -> juce::var {
            return panel != nullptr ? panel->captureSettings() : juce::var();
        };
        view.onOpenGenerator = [this](const juce::String& name, const juce::File& folder,
                                       const juce::var& settings) {
            if (panel == nullptr) return;
            // The same label the block carries on the canvas, so the panel and the track
            // cannot disagree about which block you are editing.
            blockLabel.setText(name.isEmpty() ? "no block selected" : name, juce::dontSendNotification);
            currentBlockFolder = folder;
            // No block means no target: an empty panel that says "select a block" rather
            // than 62 takes from a folder you never chose.
            if (folder != juce::File()) panel->setOutputFolder(folder);
            // The label already says which block and why; the status says what to do.
            else if (name.isNotEmpty())  panel->setNoTarget("New or Save the canvas first "
                                                             "- a block needs a project to generate into");
            else                         panel->setNoTarget();
            // The block's own recipe. Without this the panel changed its title and
            // nothing else, so every block appeared to share one prompt and one LoRA set.
            if (!settings.isVoid()) panel->applySettings(settings);
        };

        view.onStateChanged = [this] {
            playButton.setButtonText(view.isPlaying() ? "Stop" : "Play");
            loopButton.setToggleState(view.isLooping(), juce::dontSendNotification);
        };
        // A clock, and while the experiment is young an instrument: position over length
        // says in one glance whether a transport that reports "playing" is actually being
        // pulled by the device, which a playhead sitting at 0:00 cannot distinguish from a
        // paint bug.
        clock.setFont(laf.monoRegular(MiraLookAndFeel::textSize(11.0f)));
        clock.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
        clock.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(clock);
        meter.setFont(laf.monoRegular(MiraLookAndFeel::textSize(11.0f)));
        meter.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(meter);
        blockLabel.setText("no block selected", juce::dontSendNotification);
        blockLabel.setFont(laf.sansMedium(MiraLookAndFeel::textSize(12.0f)));
        blockLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::accent);
        addAndMakeVisible(blockLabel);

        emptyHint.setText("No blocks yet.\n\nA block is what the generator writes into -- it owns the "
                          "folder the take lands in and the prompt it is made from.\n\n"
                          "+ Block, or drop audio onto the canvas.",
                          juce::dontSendNotification);
        emptyHint.setFont(laf.sansRegular(MiraLookAndFeel::textSize(12.0f)));
        emptyHint.setColour(juce::Label::textColourId, MiraLookAndFeel::textDim);
        emptyHint.setJustificationType(juce::Justification::topLeft);
        addChildComponent(emptyHint);
        startTimerHz(10);
        addAndMakeVisible(view);
    }

    // The title says the document and whether it has unsaved changes, the way every DAW
    // does. Without it a window called "Canvas (experimental)" is the same window whatever
    // you have open, which is how you save over the wrong project.
    void updateTitle()
    {
        const juce::String t = "Canvas - " + view.getDocumentName() + (view.isDirty() ? " *" : "");
        if (auto* w = findParentComponentOfClass<juce::DocumentWindow>()) w->setName(t);
        saveProjectButton.setEnabled(view.isDirty() || view.getDocumentFile() == juce::File());
    }

    bool saveOrAsk()
    {
        if (view.saveDocument()) return true;
        promptSaveAs();
        return false;
    }

    void promptSaveAs()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Save project as", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            juce::String("*") + mira::canvas::CanvasView::kExtension);
        chooser->launchAsync(juce::FileBrowserComponent::saveMode
                                 | juce::FileBrowserComponent::warnAboutOverwriting,
                              [this, chooser](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            view.saveDocumentAs(f);
            updateTitle();
        });
    }

    void promptOpenProject()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Open a mira project", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
            juce::String("*") + mira::canvas::CanvasView::kExtension);
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                              [this, chooser](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            view.openDocument(f);
            updateTitle();
        });
    }

    void promptNewProject()
    {
        // A NAME, then a place to put it -- which is what makes a project a document
        // rather than a folder someone pointed at. The folder is created for you, and the
        // .mira lands inside it with the block folders as its siblings.
        auto* w = new juce::AlertWindow("New project", "Name this project.",
                                         juce::AlertWindow::NoIcon, this);
        w->addTextEditor("name", "Untitled", "Name");
        w->addButton("Create", 1, juce::KeyPress(juce::KeyPress::returnKey));
        w->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
        w->enterModalState(true, juce::ModalCallbackFunction::create([this, w](int result) {
            const auto name = w->getTextEditorContents("name").trim();
            delete w;
            if (result != 1 || name.isEmpty()) return;

            auto chooser = std::make_shared<juce::FileChooser>(
                "Where should \"" + name + "\" live?",
                juce::File::getSpecialLocation(juce::File::userMusicDirectory));
            chooser->launchAsync(juce::FileBrowserComponent::openMode
                                     | juce::FileBrowserComponent::canSelectDirectories,
                                  [this, chooser, name](const juce::FileChooser& fc) {
                const auto parent = fc.getResult();
                if (parent == juce::File()) return;
                if (!view.newDocument(parent, name))
                    juce::AlertWindow::showMessageBoxAsync(
                        juce::AlertWindow::WarningIcon, "New project",
                        "Could not create the project folder in " + parent.getFullPathName());
                updateTitle();
            });
        }), false);
    }

    void resized() override
    {
        auto r = getLocalBounds();

        // THE TOOLBAR IS A ROW IN THE WINDOW, not the title bar. It lived in the title bar
        // for exactly one commit: "the osx toolbar" meant the MENU BAR -- File, Edit,
        // Analyze -- and a full-size content view with buttons across it leaves nowhere to
        // grab the window. Every action here is also in the macOS Canvas menu (Main.cpp),
        // which is where it was asked to be; this row is the visible transport.
        auto bar = r.removeFromTop(34).reduced(8, 5);
        playButton.setBounds(bar.removeFromLeft(70));
        bar.removeFromLeft(6);
        loopButton.setBounds(bar.removeFromLeft(110));
        bar.removeFromLeft(6);
        fitButton.setBounds(bar.removeFromLeft(54));
        bar.removeFromLeft(6);
        deleteButton.setBounds(bar.removeFromLeft(80));
        bar.removeFromLeft(6);
        addTrackButton.setBounds(bar.removeFromLeft(74));
        bar.removeFromLeft(6);
        addBlockButton.setBounds(bar.removeFromLeft(74));
        bar.removeFromLeft(6);
        duplicateButton.setBounds(bar.removeFromLeft(86));
        bar.removeFromLeft(6);
        panelToggle.setBounds(bar.removeFromRight(96));
        bar.removeFromRight(8);
        newProjectButton.setBounds(bar.removeFromLeft(56));
        bar.removeFromLeft(4);
        openProjectButton.setBounds(bar.removeFromLeft(70));
        bar.removeFromLeft(4);
        saveProjectButton.setBounds(bar.removeFromLeft(56));
        bar.removeFromLeft(12);
        clock.setBounds(bar.removeFromRight(250));
        bar.removeFromRight(8);
        meter.setBounds(bar.removeFromRight(72));
        bar.removeFromRight(4);
        masterMeter = bar.removeFromRight(120).withSizeKeepingCentre(120, 10);
        hint.setBounds(bar);

        if (!panelCollapsed)
        {
            // EACH TAB ASKS FOR WHAT IT NEEDS. The master strip is a fader, a meter and
            // two numbers; giving it a third of the window because the generator wants one
            // is a third of the window spent on empty panel.
            const int wanted = tab == SideTab::Master
                                   ? 104
                               : tab == SideTab::Files
                                   ? juce::jmax(300, juce::roundToInt(r.getWidth() * panelFraction * 0.8))
                                   : juce::roundToInt(r.getWidth() * panelFraction);
            // The floor is per tab too, or the slim master strip is clamped straight back
            // up to the generator's minimum and the width it asked for means nothing.
            const int floorW = tab == SideTab::Master ? 120 : 300;
            auto side = r.removeFromRight(juce::jlimit(floorW,
                                                        juce::jmax(floorW + 20, r.getWidth() - 320),
                                                        wanted + kTabStripWidth));
            // The tab strip is part of the panel and sits on its INSIDE edge, against the
            // canvas -- so the tabs are next to the thing they change.
            tabs.setBounds(side.removeFromLeft(kTabStripWidth));
            if (tab == SideTab::Generate)
            {
                blockLabel.setBounds(side.removeFromTop(22).reduced(10, 0));
                emptyHint.setBounds(side.reduced(14, 8));
                if (panel != nullptr) panel->setBounds(side);
            }
            else if (tab == SideTab::Master) master->setBounds(side);
            else                             files->setBounds(side);
            sideDivider = r.removeFromRight(6);
        }
        else { tabs.setBounds({}); sideDivider = {}; }
        view.setBounds(r);
    }

    // One place that decides what the panel column is showing. Three setVisible calls in
    // three different handlers is how a panel ends up with two tools drawn over each other.
    // THE DOCUMENT SHORTCUTS BELONG TO THE WINDOW, not to the canvas view. They were in
    // CanvasView::keyPressed, which only ever fires when the CANVAS has keyboard focus --
    // so Cmd-S did nothing the moment you had clicked into the prompt field, which is
    // most of the time you would want to save. A key travels up from whatever is focused
    // through its parents, and everything in this window is a child of this.
    bool keyPressed(const juce::KeyPress& key) override
    {
        if (!key.getModifiers().isCommandDown()) return false;
        if (key.getKeyCode() == 'S') { saveOrAsk();        return true; }
        if (key.getKeyCode() == 'O') { promptOpenProject(); return true; }
        if (key.getKeyCode() == 'N') { promptNewProject();  return true; }
        return false;
    }

    void applyTab()
    {
        const bool open = !panelCollapsed;
        // NO BLOCK, NO GENERATOR. A block IS the generator's target -- it owns the folder
        // the take lands in and the settings the take is made from -- so a generate pane
        // over an empty canvas is a control with nothing behind it. It showed a prompt,
        // a duration and a Generate button and could not answer where the audio would go.
        // The tab stays in place and comes back the moment a block exists; what it shows
        // in the meantime says what to do rather than going blank.
        const bool haveBlocks = view.getBlockCount() > 0;
        const bool generate = open && tab == SideTab::Generate;
        if (panel != nullptr) panel->setVisible(generate && haveBlocks);
        blockLabel.setVisible(generate && haveBlocks);
        emptyHint.setVisible(generate && !haveBlocks);
        master->setVisible(open && tab == SideTab::Master);
        files->setVisible(open && tab == SideTab::Files);
        tabs.setVisible(open);
        if (open && tab == SideTab::Files) files->refresh();
        lastBlockCount = view.getBlockCount();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(MiraLookAndFeel::surface2);
        // The master meter, beside the number. Green up to -6, amber to -1, red over --
        // the same reading every console gives you, so the colour alone answers "am I
        // anywhere near clipping" without doing arithmetic on a decibel.
        if (!masterMeter.isEmpty())
        {
            g.setColour(MiraLookAndFeel::surface3);
            g.fillRoundedRectangle(masterMeter.toFloat(), 3.0f);
            if (held > 0.0005f)
            {
                const float db = juce::Decibels::gainToDecibels(held);
                const float frac = juce::jlimit(0.0f, 1.0f, (db + 48.0f) / 48.0f);
                g.setColour(db > -1.0f ? MiraLookAndFeel::warn
                                       : (db > -6.0f ? MiraLookAndFeel::accent
                                                     : MiraLookAndFeel::active));
                g.fillRoundedRectangle(masterMeter.toFloat().withWidth(
                    juce::jmax(3.0f, masterMeter.getWidth() * frac)), 3.0f);
            }
            // -6 dBFS marked, which is where you start caring.
            g.setColour(MiraLookAndFeel::text.withAlpha(0.3f));
            g.drawVerticalLine(masterMeter.getX() + juce::roundToInt(masterMeter.getWidth() * (42.0f / 48.0f)),
                                (float) masterMeter.getY(), (float) masterMeter.getBottom());
        }

        if (!sideDivider.isEmpty())
        {
            g.setColour(MiraLookAndFeel::border);
            const int cx = sideDivider.getCentreX(), cy = sideDivider.getCentreY();
            for (int i = -1; i <= 1; ++i)
                g.fillRect(cx - 1, cy + i * 10 - 6, 2, 12);
        }
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        setMouseCursor(sideDivider.expanded(3, 0).contains(e.getPosition())
                           ? juce::MouseCursor::LeftRightResizeCursor
                           : juce::MouseCursor::NormalCursor);
    }
    void mouseDown(const juce::MouseEvent& e) override
    {
        draggingSide = sideDivider.expanded(3, 0).contains(e.getPosition());
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!draggingSide) return;
        panelFraction = juce::jlimit(0.18, 0.6, (double) (getWidth() - e.x) / juce::jmax(1, getWidth()));
        resized();
    }
    void mouseUp(const juce::MouseEvent&) override { draggingSide = false; }

    ~Content() override { storeVideoGeometry(); }

    // ---- picture (MIRA-VIDEO.md Phase 1) ----------------------------------------------

    // ONE place that reconciles the canvas's video track with the picture window and the
    // reference lane. Called whenever the track changes -- a clip added, removed, or
    // brought in by a document -- because with several clips the question is never "what
    // happened to this one" but "does the whole track agree with itself".
    void syncPicture()
    {
        const auto& clips = view.getVideoClips();
        if (clips.empty())
        {
            storeVideoGeometry();
            videoWindow.reset();
            if (owner != nullptr && owner->onVideoChanged) owner->onVideoChanged();
            return;
        }

        ensureVideoWindow();
        std::vector<VideoWindow::Clip> forWindow;
        forWindow.reserve(clips.size());
        for (const auto& c : clips)
            forWindow.push_back({ c.file, c.start, c.length, c.sourceOffset });
        videoWindow->setClips(std::move(forWindow));
        videoWindow->toFront(false);

        // Any clip with no reference block yet gets one. Done after the window is up, so
        // the picture is watchable while a long extraction runs. The direct-read route is
        // synchronous and cheap, so every clip that can take it does; a clip that needs an
        // extraction waits its turn, because there is one extractor and it is busy.
        //
        // A COPY of the list, because attachReferenceFor mutates the clips it is iterating.
        const auto pending = clips;
        for (const auto& c : pending)
            if (c.audioBlockId == 0) attachReferenceFor(c);

        if (owner != nullptr && owner->onVideoChanged) owner->onVideoChanged();
    }

    void showMarkerList()
    {
        if (markerWindow != nullptr) { markerWindow->toFront(true); markerWindow->refresh(); return; }
        markerWindow = std::make_unique<MarkerListWindow>(look, view);
        if (owner != nullptr && owner->loadSetting)
            markerWindow->restoreGeometry(owner->loadSetting("canvas_marker_geometry"));
        markerWindow->onClosed = [this] {
            if (owner != nullptr && owner->saveSetting && markerWindow != nullptr)
                owner->saveSetting("canvas_marker_geometry", markerWindow->geometryString());
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Content>(this)] {
                if (safe != nullptr) safe->markerWindow.reset();
            });
        };
    }

    void promptOpenVideo()
    {
        auto chooser = std::make_shared<juce::FileChooser>(
            "Open a video", juce::File::getSpecialLocation(juce::File::userMoviesDirectory),
            "*.mp4;*.mov;*.m4v");
        chooser->launchAsync(juce::FileBrowserComponent::openMode
                                 | juce::FileBrowserComponent::canSelectFiles,
                              [this, chooser](const juce::FileChooser& fc) {
            const auto f = fc.getResult();
            if (f == juce::File()) return;
            note("opening " + f.getFileName() + "...");
            // PROBED BEFORE IT BECOMES A CLIP. A file that will not open says why and does
            // not land on the timeline as a clip showing nothing (convention 6) -- and the
            // length has to come from AVFoundation, not from getVideoDuration(), which
            // Phase 0.2 watched return 0.00 through 700 seconds of playback.
            double seconds = 0.0, fps = 0.0;
            juce::String probeError;
            if (!video_native::probe(f, seconds, fps, probeError) && seconds <= 0.0)
            {
                note("cannot open " + f.getFileName() + " - " + probeError);
                return;
            }
            if (fps <= 0.0)
                note("no frame rate reported for " + f.getFileName()
                      + " - timecode will count at 25 until you set it");
            view.addVideoClip(f, seconds, fps);
        });
    }

    // ---- the reference track (MIRA-VIDEO.md Phase 2.1) -------------------------------
    //
    // TWO ROUTES, and which one ran is always said out loud. Phase 0.1 measured JUCE's
    // CoreAudioFormat reading 4 of 4 `.mp4` files and 0 of 4 `.mov` files -- while
    // `canHandleFile` cheerfully claimed `.mov`, and `afinfo` opened every one. So the
    // direct read is tried first and Core Audio is asked directly when it fails.
    //
    // They cost wildly different amounts -- one is instant, the other reads and rewrites
    // the whole film -- and a user who cannot tell them apart cannot explain why one cut
    // opened at once and another took three minutes. That is the whole reason for the note.
    void attachReferenceFor(const VideoClip& c)
    {
        if (!c.file.existsAsFile())
        {
            note("the film is not where the project left it: " + c.file.getFullPathName());
            return;
        }

        {
            std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor(c.file));
            if (reader != nullptr && reader->sampleRate > 0.0 && reader->lengthInSamples > 0)
            {
                const auto rate = reader->sampleRate;
                const auto chans = (int) reader->numChannels;
                const double secs = reader->lengthInSamples / rate;
                reader.reset();
                view.attachReference(c.file, c.start);
                note("reference: read straight from " + c.file.getFileName() + " - "
                      + juce::String(rate / 1000.0, 1) + " kHz, " + juce::String(chans)
                      + " ch, " + juce::String(secs, 1) + " s");
                sawWaveformProgress = false;
                return;
            }
        }

        const auto home = view.getProjectFolder();
        if (home == juce::File())
        {
            // The extraction writes a file, and a file needs somewhere to live.
            note("save the canvas first - the film's audio has to be written beside the project");
            return;
        }

        const auto dest = home.getChildFile("reference")
                              .getChildFile(c.file.getFileNameWithoutExtension() + ".wav");
        if (dest.existsAsFile() && dest.getSize() > 1024)
        {
            view.attachReference(dest, c.start);
            note("reference: reusing the extraction in " + dest.getParentDirectory().getFileName()
                  + "/ (" + juce::String(dest.getSize() / (1024.0 * 1024.0), 1) + " MB)");
            sawWaveformProgress = false;
            return;
        }

        if (extractor != nullptr) { note("still extracting the last film's audio"); return; }
        note("JUCE cannot read this file's audio - extracting it with AVAssetReader...");
        extractStartMs = juce::Time::getMillisecondCounterHiRes();
        // WHERE it goes, remembered with the job. With several clips on the track, "the
        // first clip's start" is not the answer -- the reference has to land under the
        // reel it came out of.
        extractFor = c.start;
        extractor = std::make_unique<Extractor>(c.file, dest);
        extractor->startThread();
    }

    void pollExtractor()
    {
        if (extractor == nullptr) return;
        if (!extractor->done)
        {
            note("extracting the film's audio - " + juce::String(extractor->progress * 100.0, 0) + "%");
            return;
        }
        const bool ok = extractor->ok;
        const auto error = extractor->error;
        const auto dest = extractor->dest;
        const double took = (juce::Time::getMillisecondCounterHiRes() - extractStartMs) / 1000.0;
        extractor.reset();

        if (!ok) { note("could not extract the film's audio - " + error); return; }
        view.attachReference(dest, extractFor);
        // Another clip may have been waiting for this extractor to be free.
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Content>(this)] {
            if (safe != nullptr) safe->syncPicture();
        });
        note("reference: extracted with AVAssetReader in " + juce::String(took, 1) + " s -> "
              + dest.getParentDirectory().getFileName() + "/" + dest.getFileName());
        sawWaveformProgress = false;
    }

    void ensureVideoWindow()
    {
        if (videoWindow != nullptr) return;
        videoWindow = std::make_unique<VideoWindow>();
        // The transport is the clock, polled. MIRA-VIDEO.md §3.
        videoWindow->transportPosition = [this] { return view.getPositionSeconds(); };
        videoWindow->transportPlaying  = [this] { return view.isPlaying(); };
        videoWindow->onNote = [this](const juce::String& n) { note(n); };
        videoWindow->onClosed = [this] {
            storeVideoGeometry();
            // Deleted from the message queue rather than from inside its own callback.
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Content>(this)] {
                if (safe != nullptr) safe->videoWindow.reset();
            });
        };
        if (owner != nullptr && owner->loadSetting)
            videoWindow->restoreGeometry(owner->loadSetting(videoGeometryKey()));
    }

    // Phase 1.6 -- per PROJECT, because where the picture wants to sit depends on what
    // you are scoring, not on the app.
    juce::String videoGeometryKey() const
    {
        const auto doc = view.getDocumentFile();
        return "canvas_video_geometry:" + (doc.getFullPathName().isNotEmpty()
                                               ? doc.getFullPathName() : juce::String("untitled"));
    }

    void storeVideoGeometry()
    {
        if (videoWindow == nullptr || owner == nullptr || !owner->saveSetting) return;
        owner->saveSetting(videoGeometryKey(), videoWindow->geometryString());
    }

    // The canvas's own status line, plus a trace nobody has to be looking at the screen
    // to read. MIRA_TRACE_VIDEO=1 follows the MIRA_TRACE_ROWS precedent: the status line
    // is one line that anything else can overwrite, and "which route read this film, and
    // why did the other one fail" is exactly the kind of answer that must not depend on
    // having been watching at the right moment.
    void note(const juce::String& text)
    {
        static const bool trace = juce::SystemStats::getEnvironmentVariable("MIRA_TRACE_VIDEO", {}).isNotEmpty();
        if (trace) std::cerr << "[video] " << text << std::endl;
        if (panel != nullptr) panel->setStatus(text);
    }

    void timerCallback() override
    {
        // The peak matters MORE here than in a normal DAW. Stacking alternates means N
        // takes that each peak near full scale summing into one bus: two is +6 dB over,
        // four is +12. Nothing normalises that, so the number has to be on screen or the
        // first thing the canvas teaches you is that it distorts.
        // Only while the TRANSPORT is playing. A BufferingAudioSource fills its read-ahead
        // buffer whether or not the transport is running, so the source kept producing
        // peaks with nothing playing and the readout sat at some arbitrary level forever.
        // Stopped means silence, and the meter has to say so.
        const float p = view.isPlaying() ? view.readAndClearPeak() : 0.0f;
        if (!view.isPlaying()) held = 0.0f;
        if (p > held) held = p;
        held *= 0.92f;                                     // a slow fall, so a hit is readable
        const float dB = juce::Decibels::gainToDecibels(juce::jmax(held, 1.0e-6f));

        // The rates, said out loud. The timeline is 44,100 because that is all SA3
        // generates; the interface is whatever you set it to, and when they differ the
        // transport resamples the whole mix. That resample is the honest explanation for
        // "the canvas sounds different from the preview", and hiding it made the
        // difference feel like a fault rather than a conversion.
        const double devRate = view.getDeviceRate();
        auto khz = [](double r) { return juce::String(r / 1000.0, 1) + "k"; };
        const auto rates = devRate <= 0.0
                               ? juce::String("no device")
                               : (std::abs(devRate - CanvasView::getTimelineRate()) < 1.0
                                      ? khz(devRate)
                                      : khz(CanvasView::getTimelineRate()) + " -> " + khz(devRate));

        // The generation strip on the block. Polled rather than pushed: the panel already
        // owns the estimate and this timer already runs, so a callback would be a second
        // path to the same number.
        if (panel != nullptr) view.setGenerationProgress(panel->generationFraction());

        pollExtractor();
        // The waveform of a long film is the one part of loading that is O(length) --
        // 0.3 measured 200 seconds for a 40-minute reel off an external drive. Reported
        // while it runs, and said once when it lands, so a canvas that looks half-drawn
        // has a reason on screen.
        if (extractor == nullptr)
        {
            if (const double w = view.referenceWaveformProgress(); w >= 0.0)
            {
                sawWaveformProgress = true;
                note("reading the film's waveform - " + juce::String(w * 100.0, 0) + "%");
            }
            else if (sawWaveformProgress)
            {
                sawWaveformProgress = false;
                note("the film's waveform is drawn");
            }
        }

        // The generate pane appears and disappears with the first and last block, and
        // blocks arrive from a drop, a menu, an undo -- too many paths to notify from each
        // one. The count is already read here every tick; comparing it is free.
        if (view.getBlockCount() != lastBlockCount) { applyTab(); resized(); }

        // 3.5 -- the transport clock reads what the RULER reads. The number you say out
        // loud and the number on the screen have to be the same number.
        clock.setText(view.formatPosition(view.getPositionSeconds()) + " / "
                       + view.formatPosition(view.getLengthSeconds())
                       + "   " + juce::String(view.getBlockCount()) + " blocks   " + rates,
                      juce::dontSendNotification);
        meter.setText(held > 1.0f ? "CLIP +" + juce::String(dB, 1) + " dB"
                                  : juce::String(dB, 1) + " dB",
                      juce::dontSendNotification);
        meter.setColour(juce::Label::textColourId,
                        held > 1.0f ? MiraLookAndFeel::warn : MiraLookAndFeel::textFaint);
        // The labels are child components and repaint themselves; the master meter is
        // drawn by THIS component's paint, which nothing was asking to run again. The
        // number moved and the bar never did.
        repaint(masterMeter.expanded(2, 2));
    }

    float held = 0.0f;

    CanvasWindow* owner = nullptr;
    juce::AudioFormatManager& formatManager;
    const MiraLookAndFeel& look;
    std::unique_ptr<VideoWindow> videoWindow;
    std::unique_ptr<MarkerListWindow> markerWindow;

    // MIRA-VIDEO.md Phase 2.1's fallback route, on its own thread. A 40-minute reel off
    // the drive it arrived on is minutes of work, and minutes of work on the message
    // thread is a frozen app.
    struct Extractor : juce::Thread
    {
        Extractor(juce::File s, juce::File d)
            : juce::Thread("mira film audio"), source(std::move(s)), dest(std::move(d)) {}
        ~Extractor() override { stopThread(4000); }
        void run() override
        {
            juce::String err;
            const bool result = video_native::extractAudio(source, dest, err,
                [this](double p) { progress = p; return !threadShouldExit(); });
            error = err;
            ok = result;
            done = true;
        }
        juce::File source, dest;
        juce::String error;
        std::atomic<double> progress { 0.0 };
        std::atomic<bool> done { false }, ok { false };
    };
    std::unique_ptr<Extractor> extractor;
    double extractStartMs = 0.0, extractFor = 0.0;
    bool sawWaveformProgress = false;
    CanvasView view;
    GenerateContent* panel = nullptr;      // owned by the window, not by this
    // How much of the window the side panel takes. Dragged, not fixed -- a panel that is
    // always a third is wrong at 1100px and wrong again at 2400.
    double panelFraction = 0.30;
    juce::Rectangle<int> sideDivider;
    bool draggingSide = false;
    juce::File currentBlockFolder;
    juce::Label blockLabel, emptyHint;
    int lastBlockCount = -1;
    juce::TextButton playButton, loopButton, fitButton, deleteButton,
                     addBlockButton, addTrackButton, duplicateButton,
                     newProjectButton, openProjectButton, saveProjectButton, panelToggle;
    juce::Rectangle<int> masterMeter;
    bool panelCollapsed = false;
    juce::Label hint, clock, meter;
    // The side panel is one column with a tab strip, not a stack of panes fighting for
    // height. GENERATE is the block's generator; MASTER is the sum; FILES is every take
    // the project holds. Adding the next tool is an enum row and a component.
    TabStrip tabs;
    std::unique_ptr<MasterStrip> master;
    std::unique_ptr<FilesPanel> files;
    SideTab tab = SideTab::Generate;
    static constexpr int kTabStripWidth = 26;
};

CanvasWindow::CanvasWindow(const MiraLookAndFeel& laf, juce::AudioFormatManager& formats,
                           juce::AudioThumbnailCache& cache, GenerateContent* panel)
    : juce::DocumentWindow("Canvas", MiraLookAndFeel::surface,
                            juce::DocumentWindow::closeButton)
{
    content = std::make_unique<Content>(laf, formats, cache, panel);
    content->owner = this;
    view = &content->view;
    setUsingNativeTitleBar(true);
    setContentNonOwned(content.get(), false);
    setResizable(true, false);
    centreWithSize(1100, 640);
    setVisible(true);
    // The native title bar is OS-gray by default, which reads as a different app sitting
    // on top of mira's dark surface. surface2 is what Content paints its toolbar row, so
    // the bar and the row below it are one strip. After setVisible -- that is what creates
    // the peer this needs.
    mira_ui::chrome::applyDarkTitleBar(*this, MiraLookAndFeel::surface2);
}

CanvasWindow::~CanvasWindow() = default;

void CanvasWindow::saveProject() { if (content != nullptr) content->saveOrAsk(); }

void CanvasWindow::openVideo() { if (content != nullptr) content->promptOpenVideo(); }

void CanvasWindow::showMarkers() { if (content != nullptr) content->showMarkerList(); }

void CanvasWindow::showPicture()
{
    if (content == nullptr || view == nullptr) return;
    if (view->getVideoClips().empty()) return;
    content->syncPicture();
}

bool CanvasWindow::hasVideo() const { return view != nullptr && view->hasVideo(); }

} // namespace mira::canvas
