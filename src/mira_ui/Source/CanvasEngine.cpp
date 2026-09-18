#include "CanvasEngine.h"

namespace mira::canvas {

void CanvasAudioSource::setArrangement(Arrangement::Ptr next)
{
    // The old one is kept alive in `retired` rather than dropped here: the audio thread
    // may be inside getNextAudioBlock holding a reference to it right now. Anything whose
    // only remaining reference is this array is provably unreferenced elsewhere, so it is
    // safe to release -- and this runs on the message thread, where freeing is allowed.
    if (active != nullptr) retired.add(active);
    active = std::move(next);

    for (int i = retired.size(); --i >= 0;)
        if (retired.getObjectPointer(i)->getReferenceCount() == 1)
            retired.remove(i);
}

void CanvasAudioSource::prepareToPlay(int samplesPerBlockExpected, double sampleRate)
{
    deviceRate = sampleRate > 0.0 ? sampleRate : 44100.0;
    blockSize = juce::jmax(64, samplesPerBlockExpected);
    // Allocated once, here, because getNextAudioBlock may not allocate. Two channels is
    // what every SA3 take is; a mono source is read into both.
    scratch.setSize(2, blockSize + 8, false, true, true);
}

void CanvasAudioSource::releaseResources()
{
    scratch.setSize(0, 0);
}

juce::int64 CanvasAudioSource::getTotalLength() const
{
    if (auto a = active) return a->totalSamples;
    return 0;
}

void CanvasAudioSource::setLaneMasks(juce::uint64 muted, juce::uint64 soloed)
{
    muteMask.store(muted);
    soloMask.store(soloed);
}

void CanvasAudioSource::setLoopRange(double startSeconds, double endSeconds)
{
    loopStart.store(static_cast<juce::int64>(startSeconds * deviceRate));
    loopEnd.store(static_cast<juce::int64>(endSeconds * deviceRate));
}

void CanvasAudioSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    // One local copy for the whole block. Taking the reference once is what makes a swap
    // mid-block harmless: this callback finishes against the arrangement it started with.
    Arrangement::Ptr a = active;
    if (a == nullptr || a->voices.empty()) { position.fetch_add(info.numSamples); return; }

    juce::int64 from = position.load();

    if (looping.load())
    {
        const juce::int64 ls = loopStart.load(), le = loopEnd.load();
        if (le > ls)
        {
            // Wrapped here rather than by a timer, so the seam is sample-accurate: a
            // visual loop that drifts by a block every pass is worse than no loop.
            int done = 0;
            while (done < info.numSamples)
            {
                if (from >= le) from = ls;
                const int chunk = static_cast<int>(juce::jmin<juce::int64>(info.numSamples - done, le - from));
                if (chunk <= 0) break;
                juce::AudioSourceChannelInfo part (info.buffer, info.startSample + done, chunk);
                renderRange(part, from, chunk);
                from += chunk;
                done += chunk;
            }
            position.store(from);
            return;
        }
    }

    renderRange(info, from, info.numSamples);
    position.store(from + info.numSamples);
}

void CanvasAudioSource::renderRange(const juce::AudioSourceChannelInfo& info,
                                    juce::int64 from, int numSamples)
{
    Arrangement::Ptr a = active;
    if (a == nullptr) return;

    const juce::int64 to = from + numSamples;
    const int outChannels = info.buffer->getNumChannels();
    const juce::uint64 mutes = muteMask.load();
    const juce::uint64 solos = soloMask.load();

    for (auto& v : a->voices)
    {
        const juce::int64 voiceEnd = v.startSample + v.lengthSamples;
        if (voiceEnd <= from || v.startSample >= to) continue;   // not sounding in this block
        if (v.reader == nullptr) continue;

        if (v.lane < kMaxLanes)
        {
            const juce::uint64 bit = juce::uint64 (1) << v.lane;
            if (mutes & bit) continue;
            if (solos != 0 && !(solos & bit)) continue;   // any solo silences everything else
        }

        const juce::int64 overlapStart = juce::jmax(from, v.startSample);
        const juce::int64 overlapEnd   = juce::jmin(to, voiceEnd);
        const int n = static_cast<int>(overlapEnd - overlapStart);
        if (n <= 0) continue;

        const juce::int64 intoVoice = overlapStart - v.startSample;
        const juce::int64 readFrom  = v.sourceStartSample
                                    + static_cast<juce::int64>(intoVoice * v.rateRatio);

        if (n > scratch.getNumSamples()) continue;   // never grow the buffer here
        scratch.clear(0, n);
        // Reading on this thread is safe ONLY because a BufferingAudioSource sits in
        // front of this source: this runs on its background thread, not in the device
        // callback. Take that buffer away and this line becomes file I/O in the
        // real-time path.
        v.reader->read(&scratch, 0, n, readFrom, true, true);

        for (int ch = 0; ch < outChannels; ++ch)
        {
            const int srcCh = juce::jmin(ch, scratch.getNumChannels() - 1);
            auto* dst = info.buffer->getWritePointer(ch, info.startSample + static_cast<int>(overlapStart - from));
            const auto* src = scratch.getReadPointer(srcCh);

            for (int i = 0; i < n; ++i)
            {
                float env = v.gain;
                const juce::int64 at = intoVoice + i;
                if (v.fadeInSamples > 0 && at < v.fadeInSamples)
                    env *= static_cast<float>(at) / static_cast<float>(v.fadeInSamples);
                if (v.fadeOutSamples > 0 && at >= v.lengthSamples - v.fadeOutSamples)
                    env *= static_cast<float>(v.lengthSamples - at) / static_cast<float>(v.fadeOutSamples);
                dst[i] += src[i] * env;
            }
        }
    }

    // One peak for the whole mix, after summing -- which is the only place the stacking
    // problem is visible. Read and cleared by the UI.
    for (int ch = 0; ch < outChannels; ++ch)
    {
        const float m = info.buffer->getMagnitude(ch, info.startSample, numSamples);
        float seen = peak.load();
        while (m > seen && !peak.compare_exchange_weak(seen, m)) {}
    }
}

// ---- player ------------------------------------------------------------------------

CanvasPlayer::CanvasPlayer()
{
    readThread.startThread(juce::Thread::Priority::high);
}

CanvasPlayer::~CanvasPlayer()
{
    detach();
    transport.setSource(nullptr);
    buffered.reset();
    readThread.stopThread(2000);
}

void CanvasPlayer::attachTo(juce::AudioDeviceManager& device)
{
    if (deviceManager == &device) return;
    detach();
    deviceManager = &device;
    player.setSource(&transport);
    deviceManager->addAudioCallback(&player);
}

void CanvasPlayer::detach()
{
    if (deviceManager == nullptr) return;
    transport.stop();
    deviceManager->removeAudioCallback(&player);
    player.setSource(nullptr);
    deviceManager = nullptr;
}

void CanvasPlayer::rebuild(const std::vector<Block>& blocks, juce::AudioFormatManager& formats)
{
    const double rate = canvasSource.getSampleRate();

    auto next = new Arrangement();
    for (const auto& b : blocks)
    {
        if (!b.file.existsAsFile() || b.length <= 0.0) continue;
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor(b.file));
        if (reader == nullptr) continue;

        Arrangement::Voice v;
        v.rateRatio      = reader->sampleRate > 0.0 ? reader->sampleRate / rate : 1.0;
        v.startSample    = static_cast<juce::int64>(b.start * rate);
        v.lengthSamples  = static_cast<juce::int64>(b.length * rate);
        v.sourceStartSample = static_cast<juce::int64>(b.sourceOffset * reader->sampleRate);
        v.fadeInSamples  = static_cast<juce::int64>(b.fadeIn * rate);
        v.fadeOutSamples = static_cast<juce::int64>(b.fadeOut * rate);
        v.gain           = juce::Decibels::decibelsToGain(static_cast<float>(b.gainDb));
        v.lane           = b.lane;
        v.reader         = std::move(reader);
        next->totalSamples = juce::jmax(next->totalSamples, v.startSample + v.lengthSamples);
        next->voices.push_back(std::move(v));
    }

    const double wasAt = transport.getCurrentPosition();
    const bool wasPlaying = transport.isPlaying();

    // The transport is detached across the swap. Changing the source's length under a
    // live AudioTransportSource is how a transport ends up reporting a position past the
    // end of what it is playing.
    transport.stop();
    transport.setSource(nullptr);
    canvasSource.setArrangement(Arrangement::Ptr (next));

    buffered = std::make_unique<juce::BufferingAudioSource>(&canvasSource, readThread, false,
                                                             static_cast<int>(rate), 2, true);
    transport.setSource(buffered.get(), 0, nullptr, rate, 2);
    transport.setPosition(wasAt);
    if (wasPlaying) transport.start();
}

void CanvasPlayer::play()
{
    if (transport.getCurrentPosition() >= transport.getLengthInSeconds()) transport.setPosition(0.0);
    transport.start();
}

void CanvasPlayer::stop() { transport.stop(); }

void CanvasPlayer::setLoop(bool on, double startSeconds, double endSeconds)
{
    loopOn = on;
    canvasSource.setLoopRange(startSeconds, endSeconds);
    canvasSource.setLooping(on);
}

} // namespace mira::canvas
