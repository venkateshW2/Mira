#include "ThumbnailStore.h"

#include <cstdlib>

namespace mira_ui::thumbnails
{
namespace {

// Must match WaveformView's own AudioThumbnail construction: the samples-per-pixel
// resolution is baked into the serialised data, so peaks written at one resolution and
// read back into a thumbnail built at another would be wrong, not just suboptimal.
constexpr int kSourceSamplesPerThumbnailSample = 512;

juce::String cacheKeyFor(const juce::File& audioFile)
{
    // Identity, not just location: a file edited in place keeps its path but changes its
    // modification time and (nearly always) its size, and must not read back the old
    // peaks. Not the sha256 the scanner computes -- that would mean a database lookup on
    // the click path this cache exists to make instant, to distinguish cases that
    // mtime+size already separates.
    auto identity = audioFile.getFullPathName() + "|"
                     + juce::String(audioFile.getLastModificationTime().toMilliseconds()) + "|"
                     + juce::String(audioFile.getSize());
    return juce::String::toHexString(identity.hashCode64()).paddedLeft('0', 16);
}

} // namespace

juce::File cacheDirectory()
{
    auto home = std::getenv("HOME");
    juce::File dir = home != nullptr ? juce::File(juce::String(home)).getChildFile(".mira")
                                      : juce::File::getCurrentWorkingDirectory().getChildFile(".mira");
    auto thumbs = dir.getChildFile("thumbnails");
    thumbs.createDirectory();
    return thumbs;
}

juce::File cacheFileFor(const juce::File& audioFile)
{
    return cacheDirectory().getChildFile(cacheKeyFor(audioFile) + ".mirathumb");
}

bool loadInto(juce::AudioThumbnail& thumbnail, const juce::File& audioFile)
{
    auto cacheFile = cacheFileFor(audioFile);
    if (!cacheFile.existsAsFile()) return false;

    juce::FileInputStream in(cacheFile);
    if (!in.openedOk()) return false;
    // loadFrom rejects data it doesn't recognise (a truncated write, a different JUCE
    // build's format) by returning false rather than throwing, which is exactly the
    // "treat it as a miss" behaviour wanted here.
    return thumbnail.loadFrom(in);
}

bool generateAndSave(juce::AudioFormatManager& formatManager, const juce::File& audioFile,
                      const std::function<bool()>& shouldAbort)
{
    if (cacheFileFor(audioFile).existsAsFile()) return true;
    if (!audioFile.existsAsFile()) return false;

    std::unique_ptr<juce::AudioFormatReader> reader(formatManager.createReaderFor(audioFile));
    if (reader == nullptr) return false;

    // A cache of 1 and its own TimeSliceThread: this object exists for the duration of
    // one file and is never shared with the UI's own thumbnail cache, so there's no
    // question of a background generation pass evicting the entry the waveform on screen
    // is currently drawing from.
    juce::AudioThumbnailCache cache(1);
    juce::AudioThumbnail thumbnail(kSourceSamplesPerThumbnailSample, formatManager, cache);
    thumbnail.setReader(reader.release(), audioFile.getFullPathName().hashCode64()); // takes ownership

    // Peaks are produced on the cache's own background thread, so this polls rather than
    // blocking on a condition it has no handle to. The timeout is generous because the
    // files this matters for are the long ones (a 40-minute stem), but it is bounded:
    // one unreadable-but-openable file must not wedge the whole precache pass.
    constexpr int kPollMs = 25;
    constexpr int kMaxWaitMs = 120000;
    for (int waited = 0; waited < kMaxWaitMs; waited += kPollMs)
    {
        if (shouldAbort && shouldAbort()) return false;
        if (thumbnail.isFullyLoaded()) break;
        juce::Thread::sleep(kPollMs);
    }
    if (!thumbnail.isFullyLoaded()) return false;

    // Written to a temp file and moved into place, so an interrupted write (app quit
    // mid-pass) can never leave a half-written entry that a later load would have to
    // detect.
    auto cacheFile = cacheFileFor(audioFile);
    auto temp = cacheFile.getSiblingFile(cacheFile.getFileName() + ".tmp");
    temp.deleteFile();
    {
        juce::FileOutputStream out(temp);
        if (!out.openedOk()) return false;
        thumbnail.saveTo(out);
    }
    return temp.moveFileTo(cacheFile);
}

} // namespace mira_ui::thumbnails
