#pragma once

#include <cmath>
#include <vector>

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

// How a fade gets from silence to full. Linear is what a drawn wedge LOOKS like; equal
// power is what a CROSSFADE needs, because two linear fades summing through their middle
// lose 3 dB and you hear the join as a dip.
enum class FadeShape { Linear = 0, EqualPower = 1, Exponential = 2 };

// ONE curve, shared by the mixer and the drawing. A wedge drawn as a straight line over a
// fade that is actually a sine is a picture of something the audio is not doing, and the
// take editor has already been caught doing exactly that.
inline float fadeGain (float t, FadeShape shape) noexcept
{
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    switch (shape)
    {
        case FadeShape::EqualPower:  return std::sin (t * 1.5707963267948966f);
        case FadeShape::Exponential: return t * t;
        case FadeShape::Linear:
        default:                     return t;
    }
}

// One region of a block's file, placed inside the block. MIRA-BLOCKS.md §8.
//
// The shape goes in NOW, before anything writes more than one entry, because it is the
// one part of that plan which is expensive to retrofit: it changes the document, and a
// document change made later has to cope with every file written before it.
//
// An EMPTY list means "the whole of the file from sourceOffset", which is every block
// that exists today -- so a block with no slices behaves exactly as it always has.
struct Slice
{
    double sourceStart = 0.0;    // seconds into the file
    double sourceLength = 0.0;
    double placeAt = 0.0;        // seconds from the block's start
    double gainDb = 0.0;
    bool   muted = false;
};

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
    // How much of the FILE this block uses, from `sourceOffset`. Not the same as `length`:
    // length is how long the block is on the timeline, and the difference between them is
    // the empty tail an extend fills in.
    //
    // It exists because a generated take often ends in silence. Cutting that silence off
    // has to be REMEMBERED -- otherwise dragging the right edge back out just reveals the
    // silence again, and there is no way to say "the audio ends here, now continue from
    // there", which is the whole point of extending.
    //
    // Zero means "all of it", so a block written before this existed still reads right.
    double contentSeconds = 0.0;
    double gainDb = 0.0;
    double fadeIn = 0.0;
    double fadeOut = 0.0;
    juce::String name;      // the BLOCK's name -- its folder, and what the track shows
    juce::int64 id = 0;
    // The block's OWN colour, taken from the track it was born on and kept when it moves.
    // Colour used to come from whatever track the block was sitting on, so dragging a
    // block to another track recoloured it -- and a block that changes colour when you
    // move it has no identity to follow down a stack.
    int colour = 0;
    // Muted PER BLOCK, as well as per track. A track mute answers "not this layer"; a
    // block mute answers "not this bar", which is the question you ask while arranging.
    bool muted = false;
    FadeShape fadeShape = FadeShape::Linear;
    // An empty block has no file yet: a frame you placed before you generated into it.
    // That is the point of it -- lay out the shape of the piece first, fill it after.
    bool hasAudio() const { return file != juce::File(); }

    // ---- musical time (MIRA-BLOCKS.md) --------------------------------------------
    // It lives on the BLOCK and not on the track, because a block moves between tracks
    // freely: anything musical held by a track is positional, and dragging a block one
    // row down would change what it means. A per-track tempo is also just the global
    // grid reintroduced one level down, which is the thing the canvas exists to avoid.
    //
    // The rule the whole feature follows: THE GRID IS DRAWN, NEVER ENFORCED. Nothing
    // here gates placing, trimming or generating; it is scaffolding for the eye.
    double tempo = 0.0;          // BPM. 0 = unknown -- from the prompt, typed, or measured
    int    meter = 4;            // beats per bar
    // Bar 1, in SOURCE time (seconds into the file), NOT timeline time. That is what
    // makes moving the block, dropping it on another track, trimming its left edge and
    // appending an extension all leave the phase where it was.
    double barOnePos = 0.0;
    juce::String key;            // "C minor", from the prompt or measured
    // Where `tempo` came from, because a grid that cannot say that is a grid you cannot
    // argue with: "prompt" | "typed" | "measured" | "block 1"
    juce::String tempoSource;
    // $.rhythm.beat_grid_stability -- 0 when nothing has been measured. Convention 6:
    // an unmeasured grid says so rather than borrowing a number it did not earn.
    double tempoConfidence = 0.0;
    juce::int64 followsBlockId = 0;   // 0 = independent
    // Whether a bar 1 set by hand survives the next analysis. Convention 5: `human`
    // outranks machine, so a nudge you made by ear is not overwritten by a model.
    bool barOneIsHuman = false;
    std::vector<Slice> slices;

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
        // SHARED, and cached across rebuilds by CanvasPlayer. Reopening twelve files on
        // every mouse-up -- from an external USB drive -- is where the lag on dragging a
        // block came from. Safe to share because reads happen only on the buffering
        // source's single background thread, and AudioFormatReader::read takes an explicit
        // start sample rather than carrying a position.
        std::shared_ptr<juce::AudioFormatReader> reader;
        int lane = 0;                    // so mute and solo can be atomic bitmasks
        juce::int64 startSample = 0;     // on the canvas timeline, at the DEVICE rate
        juce::int64 lengthSamples = 0;
        juce::int64 sourceStartSample = 0;
        juce::int64 fadeInSamples = 0;
        juce::int64 fadeOutSamples = 0;
        float gain = 1.0f;
        double rateRatio = 1.0;          // source rate / timeline rate; 1.0 for 44.1 on 44.1
        // Per EDGE, not per block: an automatic crossfade forces equal power on the two
        // edges that meet and leaves the block's other end alone.
        FadeShape fadeInShape = FadeShape::Linear;
        FadeShape fadeOutShape = FadeShape::Linear;
    };

    std::vector<Voice> voices;
    juce::int64 totalSamples = 0;
};

// Sums every block that overlaps the current position. A PositionableAudioSource so it
// can sit under an AudioTransportSource and get play/stop/seek for free.
class CanvasAudioSource : public juce::PositionableAudioSource
{
public:
    CanvasAudioSource()
    {
        for (auto& g : laneGain) g.store(1.0f);
        for (auto& p : lanePeak) { p[0].store(0.0f); p[1].store(0.0f); }
        peak[0].store(0.0f); peak[1].store(0.0f);
    }

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

    // OFFLINE RENDER, on the calling thread. Export goes through this so the file is
    // mixed by exactly the code that plays it -- crossfades between overlapping blocks,
    // fade shapes, per-block gain, mute, solo and the per-voice resampler included --
    // rather than by a second implementation that would drift from the first the moment
    // either changed. Honours the lane masks, which is what lets "export this track"
    // be a solo rather than a filter written twice.
    void renderOffline(juce::AudioBuffer<float>& destination, juce::int64 from, int numSamples);

    // Mute and solo as BITMASKS, updated atomically, so toggling either takes effect on
    // the next block with no rebuild. Rebuilding would reopen every file on disk just to
    // silence one lane, and the gap while it did would be audible.
    void setLaneMasks(juce::uint64 muted, juce::uint64 soloed);
    static constexpr int kMaxLanes = 64;   // one bit each; past this, mute/solo is ignored

    // A fader per lane, atomic for the same reason the masks are: moving one must take
    // effect on the next block, not after a rebuild.
    void setLaneGain(int lane, float gain);
    float getLaneGain(int lane) const;

    // Per-lane peak since the last read, cleared by reading. This is what answers "which
    // lane is making that noise" -- the one question a stack of twelve waveforms cannot.
    //
    // PER CHANNEL, because a mono meter cannot show the one fault it exists to catch: a
    // stereo take with a dead side, or a mix leaning entirely one way. A single number
    // averaged over both is the number that hides it.
    float readAndClearLanePeak(int lane, int channel);

    // The canvas timeline's own rate, fixed. Every SA3 take is 44,100 and the transport
    // resamples to the device, so nothing here has to care what the device opened at.
    static constexpr double kTimelineRate = 44100.0;
    static constexpr int kScratchSamples = 16384;
    // Read-ahead. Was a full second, which is a second of prefill to sit through on every
    // edit; a third of that is still ample for a dozen files off an external drive.
    static constexpr int kReadAheadSamples = 16384;
    double getSampleRate() const { return kTimelineRate; }
    // Highest sample seen since the last read, and cleared by reading it. Summing N takes
    // that each peak near full scale is N times full scale, so a canvas that stacks
    // alternates WILL clip unless it says so. Per channel, for the same reason the lane
    // meters are.
    float readAndClearPeak(int channel = -1)
    {
        if (channel < 0) return juce::jmax(peak[0].exchange(0.0f), peak[1].exchange(0.0f));
        return peak[juce::jlimit(0, 1, channel)].exchange(0.0f);
    }

    // ONE master gain, applied after the sum and before the meter reads it -- so the
    // master meter shows what leaves mira rather than what the tracks added up to.
    void setMasterGain(float g) { masterGain.store(juce::jlimit(0.0f, 4.0f, g)); }
    float getMasterGain() const { return masterGain.load(); }

private:
    Arrangement::Ptr active;                 // read by the audio thread
    juce::ReferenceCountedArray<Arrangement> retired;  // freed on the message thread only

    juce::AudioBuffer<float> scratch;
    std::atomic<juce::int64> position { 0 };
    std::atomic<bool> looping { false };
    std::atomic<juce::int64> loopStart { 0 }, loopEnd { 0 };
    std::atomic<juce::uint64> muteMask { 0 }, soloMask { 0 };
    std::atomic<float> laneGain[kMaxLanes];
    std::atomic<float> lanePeak[kMaxLanes][2];
    std::atomic<float> peak[2];
    std::atomic<float> masterGain { 1.0f };
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
    // What the playhead should draw. The transport's position is linear and keeps
    // climbing past the out point while looping; where you are actually HEARING is the
    // mapped position, and a playhead that says otherwise is lying about the audio.
    double getPositionSeconds() const;
    void setPositionSeconds(double s) { transport.setPosition(s); }
    double getLengthSeconds() const { return transport.getLengthInSeconds(); }

    void setLoop(bool on, double startSeconds, double endSeconds);
    bool isLooping() const { return loopOn; }
    // What the interface is actually running at, as against the timeline's 44,100. The
    // difference is a resample, and a resample that nothing on screen admits to is the
    // difference between "mira sounds different from the preview" being a mystery and
    // being a fact you can point at.
    double getDeviceRate() const;
    static constexpr double getTimelineRate() { return CanvasAudioSource::kTimelineRate; }
    void setLaneMasks(juce::uint64 muted, juce::uint64 soloed) { canvasSource.setLaneMasks(muted, soloed); }
    void setLaneGain(int lane, float gain) { canvasSource.setLaneGain(lane, gain); }
    float readAndClearLanePeak(int lane, int channel) { return canvasSource.readAndClearLanePeak(lane, channel); }
    float readAndClearPeak(int channel = -1) { return canvasSource.readAndClearPeak(channel); }
    void setMasterGain(float g) { canvasSource.setMasterGain(g); }
    float getMasterGain() const { return canvasSource.getMasterGain(); }
    // For export. See CanvasAudioSource::renderOffline -- the point is that the file and
    // the speakers come out of one mixer.
    void renderOffline(juce::AudioBuffer<float>& destination, juce::int64 from, int numSamples)
    { canvasSource.renderOffline(destination, from, numSamples); }

private:
    juce::TimeSliceThread readThread { "canvas file reader" };
    CanvasAudioSource canvasSource;
    std::unique_ptr<juce::BufferingAudioSource> buffered;
    juce::AudioTransportSource transport;
    juce::AudioSourcePlayer player;
    juce::AudioDeviceManager* deviceManager = nullptr;
    bool loopOn = false;
    double loopFrom = 0.0, loopTo = 0.0;
    // Readers kept between rebuilds, keyed by path. Cleared of anything the new
    // arrangement did not claim, so deleting a block still closes its file.
    std::map<juce::String, std::shared_ptr<juce::AudioFormatReader>> readerCache;
};

} // namespace mira::canvas
