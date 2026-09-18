#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>

// ---- the canvas experiment: the audio half -----------------------------------------
//
// A Blockhead-shaped surface: blocks of audio placed freely on a time axis, with no
// tempo and no grid, several sounding at once. This file is the engine; CanvasWindow.h
// is the picture. Nothing here is wired into the generate window, the take stack or the
// browser -- the whole point of the experiment is that it can be thrown away.
//
// THE CONSTRAINT THAT SHAPES EVERYTHING BELOW: the audio thread may not allocate, may
// not lock, and may not touch the filesystem. So:
//
//   * the arrangement the audio thread reads is IMMUTABLE. Editing builds a new one and
//     swaps the pointer; the old one dies later, on the message thread, once nothing is
//     reading it.
//   * file reads happen on a BufferingAudioSource's background thread, never in the
//     device callback. CanvasAudioSource does the mixing; the buffering source in front
//     of it is what keeps that mixing off the real-time path.
//
// This is the JUCE demo's reference-counted handoff, which is adequate for an experiment
// and honest about what it is: the pointer assignment itself is not atomic, so a swap
// racing a block boundary can in principle be seen half-done. A lock-free FIFO handoff is
// the production answer. Said out loud here rather than discovered later as a click
// nobody can reproduce.
namespace mira::canvas {

// One block on the canvas. Times are in SECONDS on the canvas timeline; `sourceOffset` is
// where in the file the block starts, so trimming the left edge moves the offset rather
// than the audio.
struct Block
{
    juce::File file;
    int lane = 0;
    double start = 0.0;          // where it sits on the canvas
    double length = 0.0;         // how much of it sounds
    double sourceOffset = 0.0;   // where in the file `start` corresponds to
    double gainDb = 0.0;
    double fadeIn = 0.0;
    double fadeOut = 0.0;
    juce::String name;
    juce::int64 id = 0;

    double end() const { return start + length; }
};

// What the audio thread actually reads: blocks resolved to readers and sample counts, in
// one object that is never modified after it is published.
class Arrangement : public juce::ReferenceCountedObject
{
public:
    using Ptr = juce::ReferenceCountedObjectPtr<Arrangement>;

    struct Voice
    {
        std::unique_ptr<juce::AudioFormatReader> reader;
        int lane = 0;                    // so mute and solo can be atomic bitmasks
        juce::int64 startSample = 0;     // on the canvas timeline, at the DEVICE rate
        juce::int64 lengthSamples = 0;
        juce::int64 sourceStartSample = 0;
        juce::int64 fadeInSamples = 0;
        juce::int64 fadeOutSamples = 0;
        float gain = 1.0f;
        double rateRatio = 1.0;          // source rate / timeline rate; 1.0 for 44.1 on 44.1
    };

    std::vector<Voice> voices;
    juce::int64 totalSamples = 0;
};

// Sums every block that overlaps the current position. A PositionableAudioSource so it
// can sit under an AudioTransportSource and get play/stop/seek for free.
class CanvasAudioSource : public juce::PositionableAudioSource
{
public:
    CanvasAudioSource() = default;

    // Called on the MESSAGE thread. Builds readers, then publishes.
    void setArrangement(Arrangement::Ptr next);

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void releaseResources() override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo&) override;

    void setNextReadPosition(juce::int64 newPosition) override { position = newPosition; }
    juce::int64 getNextReadPosition() const override { return position; }
    juce::int64 getTotalLength() const override;
    bool isLooping() const override { return looping; }
    void setLooping(bool shouldLoop) override { looping = shouldLoop; }

    void setLoopRange(double startSeconds, double endSeconds);

    // Mute and solo as BITMASKS, updated atomically, so toggling either takes effect on
    // the next block with no rebuild. Rebuilding would reopen every file on disk just to
    // silence one lane, and the gap while it did would be audible.
    void setLaneMasks(juce::uint64 muted, juce::uint64 soloed);
    static constexpr int kMaxLanes = 64;   // one bit each; past this, mute/solo is ignored

    double getSampleRate() const { return deviceRate; }
    // Highest sample seen since the last read, and cleared by reading it. Summing N takes
    // that each peak near full scale is N times full scale, so a canvas that stacks
    // alternates WILL clip unless it says so.
    float readAndClearPeak() { return peak.exchange(0.0f); }

private:
    Arrangement::Ptr active;                 // read by the audio thread
    juce::ReferenceCountedArray<Arrangement> retired;  // freed on the message thread only

    juce::AudioBuffer<float> scratch;
    std::atomic<juce::int64> position { 0 };
    std::atomic<bool> looping { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };
    std::atomic<juce::uint64> muteMask { 0 }, soloMask { 0 };
    std::atomic<float> peak { 0.0f };
    double deviceRate = 44100.0;
    int blockSize = 512;

    void renderRange(const juce::AudioSourceChannelInfo& info, juce::int64 from, int numSamples);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CanvasAudioSource)
};

// Everything the window needs to make sound: the source, the buffering thread in front of
// it, a transport, and the join to the app's shared device.
class CanvasPlayer
{
public:
    CanvasPlayer();
    ~CanvasPlayer();

    void attachTo(juce::AudioDeviceManager& device);
    void detach();

    // Rebuilds the arrangement from blocks. Message thread only.
    void rebuild(const std::vector<Block>& blocks, juce::AudioFormatManager& formats);

    void play();
    void stop();
    bool isPlaying() const { return transport.isPlaying(); }
    double getPositionSeconds() const { return transport.getCurrentPosition(); }
    void setPositionSeconds(double s) { transport.setPosition(s); }
    double getLengthSeconds() const { return transport.getLengthInSeconds(); }

    void setLoop(bool on, double startSeconds, double endSeconds);
    bool isLooping() const { return loopOn; }
    void setLaneMasks(juce::uint64 muted, juce::uint64 soloed) { canvasSource.setLaneMasks(muted, soloed); }
    float readAndClearPeak() { return canvasSource.readAndClearPeak(); }

private:
    juce::TimeSliceThread readThread { "canvas file reader" };
    CanvasAudioSource canvasSource;
    std::unique_ptr<juce::BufferingAudioSource> buffered;
    juce::AudioTransportSource transport;
    juce::AudioSourcePlayer player;
    juce::AudioDeviceManager* deviceManager = nullptr;
    bool loopOn = false;
};

} // namespace mira::canvas
