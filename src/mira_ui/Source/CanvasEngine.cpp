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
    juce::ignoreUnused(sampleRate);
    blockSize = juce::jmax(64, samplesPerBlockExpected);
    // Sized for the BUFFERING source's appetite, not the device's block. A
    // BufferingAudioSource fills its read-ahead buffer in chunks FAR larger than one
    // device block -- it asked for 44,100 samples at a time here -- and the first version
    // sized this at blockSize + 8 and skipped any voice that did not fit. Every voice was
    // skipped. That is exactly what "can't hear anything, it's just glitching" was: near
    // silence, with the occasional small block getting through.
    //
    // The skip is gone as well (see renderRange): rendering is chunked to whatever this
    // holds, so no buffer size can silence a voice again.
    scratch.setSize(2, juce::jmax(blockSize + 8, kScratchSamples), false, true, true);
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

void CanvasAudioSource::setLaneGain(int lane, float gain)
{
    if (lane >= 0 && lane < kMaxLanes) laneGain[lane].store(juce::jlimit(0.0f, 4.0f, gain));
}

float CanvasAudioSource::readAndClearLanePeak(int lane)
{
    return (lane >= 0 && lane < kMaxLanes) ? lanePeak[lane].exchange(0.0f) : 0.0f;
}

float CanvasAudioSource::getLaneGain(int lane) const
{
    return (lane >= 0 && lane < kMaxLanes) ? laneGain[lane].load() : 1.0f;
}

void CanvasAudioSource::setLoopRange(double startSeconds, double endSeconds)
{
    loopStart.store(static_cast<juce::int64>(startSeconds * kTimelineRate));
    loopEnd.store(static_cast<juce::int64>(endSeconds * kTimelineRate));
}

void CanvasAudioSource::getNextAudioBlock(const juce::AudioSourceChannelInfo& info)
{
    info.clearActiveBufferRegion();

    // One local copy for the whole block. Taking the reference once is what makes a swap
    // mid-block harmless: this callback finishes against the arrangement it started with.
    Arrangement::Ptr a = active;
    if (a == nullptr || a->voices.empty()) { position.fetch_add(info.numSamples); return; }

    const juce::int64 from = position.load();
    const bool loop = looping.load();
    const juce::int64 ls = loopStart.load(), le = loopEnd.load();
    const juce::int64 span = le - ls;

    // LOOPING IS A PURE MAPPING FROM THE REQUESTED POSITION, not an internal rewind.
    //
    // The first version rewound `position` when it crossed the out point. That cannot
    // work here: a BufferingAudioSource sits in front of this source and calls
    // setNextReadPosition(P) before every chunk it reads, always with LINEAR positions --
    // P, P+n, P+2n. It is the authority on position, not us. So once P passed the out
    // point, every chunk was asked for a position beyond it and every chunk restarted at
    // the in point: about a second of audio repeating, which is exactly what it sounded
    // like.
    //
    // Mapping instead -- position stays monotonic, and where it READS from is
    // (start + (pos - start) mod span) -- is what AudioFormatReaderSource does for its own
    // looping, and for the same reason.
    int done = 0;
    while (done < info.numSamples)
    {
        const juce::int64 linear = from + done;
        juce::int64 source = linear;
        int chunk = juce::jmin(info.numSamples - done, scratch.getNumSamples());

        if (loop && span > 0)
        {
            if (linear < ls)
            {
                // Before the loop: play straight through, but stop the chunk at the in
                // point so the next one starts the loop cleanly.
                chunk = (int) juce::jmin<juce::int64>(chunk, ls - linear);
            }
            else
            {
                const juce::int64 into = (linear - ls) % span;
                source = ls + into;
                // Never read across the out point inside one chunk -- that is the seam,
                // and it has to land on a sample boundary rather than near one.
                chunk = (int) juce::jmin<juce::int64>(chunk, span - into);
            }
        }

        if (chunk <= 0) break;
        juce::AudioSourceChannelInfo part (info.buffer, info.startSample + done, chunk);
        renderRange(part, source, chunk);
        done += chunk;
    }

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

        float laneLevel = 1.0f;
        if (v.lane < kMaxLanes)
        {
            const juce::uint64 bit = juce::uint64 (1) << v.lane;
            if (mutes & bit) continue;
            if (solos != 0 && !(solos & bit)) continue;   // any solo silences everything else
            laneLevel = laneGain[v.lane].load();
            if (laneLevel <= 0.0f) continue;
        }

        const juce::int64 overlapStart = juce::jmax(from, v.startSample);
        const juce::int64 overlapEnd   = juce::jmin(to, voiceEnd);
        const int n = static_cast<int>(overlapEnd - overlapStart);
        if (n <= 0) continue;

        const juce::int64 intoVoice = overlapStart - v.startSample;
        const juce::int64 readFrom  = v.sourceStartSample
                                    + static_cast<juce::int64>(intoVoice * v.rateRatio);

        // Cannot fire: getNextAudioBlock chunks to this size. Kept as an assertion rather
        // than a `continue`, because silently dropping a voice is what made the first
        // version inaudible without saying anything was wrong.
        jassert(n <= scratch.getNumSamples());
        scratch.clear(0, n);
        // Reading on this thread is safe ONLY because a BufferingAudioSource sits in
        // front of this source: this runs on its background thread, not in the device
        // callback. Take that buffer away and this line becomes file I/O in the
        // real-time path.
        v.reader->read(&scratch, 0, n, readFrom, true, true);

        float voicePeak = 0.0f;
        for (int ch = 0; ch < outChannels; ++ch)
        {
            const int srcCh = juce::jmin(ch, scratch.getNumChannels() - 1);
            auto* dst = info.buffer->getWritePointer(ch, info.startSample + static_cast<int>(overlapStart - from));
            const auto* src = scratch.getReadPointer(srcCh);

            for (int i = 0; i < n; ++i)
            {
                float env = v.gain * laneLevel;
                const juce::int64 at = intoVoice + i;
                if (v.fadeInSamples > 0 && at < v.fadeInSamples)
                    env *= static_cast<float>(at) / static_cast<float>(v.fadeInSamples);
                if (v.fadeOutSamples > 0 && at >= v.lengthSamples - v.fadeOutSamples)
                    env *= static_cast<float>(v.lengthSamples - at) / static_cast<float>(v.fadeOutSamples);
                const float sample = src[i] * env;
                voicePeak = juce::jmax(voicePeak, std::abs(sample));
                dst[i] += sample;
            }
        }

        // Post-fader, so the meter shows what the lane is CONTRIBUTING rather than what
        // the file contains -- pulling a fader down has to move its meter or the meter is
        // answering a question nobody asked.
        if (v.lane < kMaxLanes)
        {
            float seen = lanePeak[v.lane].load();
            while (voicePeak > seen && !lanePeak[v.lane].compare_exchange_weak(seen, voicePeak)) {}
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
    // The canvas timeline is 44,100 REGARDLESS of the device. SA3 generates at 44.1 and
    // nothing else, and the transport resamples to the device rate for us. Building the
    // arrangement against whatever rate the device happened to open at meant the sample
    // positions and the loop points could disagree the moment the device changed.
    const double rate = CanvasAudioSource::kTimelineRate;

    std::map<juce::String, std::shared_ptr<juce::AudioFormatReader>> stillUsed;

    auto next = new Arrangement();
    for (const auto& b : blocks)
    {
        if (!b.file.existsAsFile() || b.length <= 0.0) continue;

        const auto key = b.file.getFullPathName();
        std::shared_ptr<juce::AudioFormatReader> reader;
        if (auto hit = readerCache.find(key); hit != readerCache.end()) reader = hit->second;
        else if (auto* made = formats.createReaderFor(b.file)) reader.reset(made);
        if (reader == nullptr) continue;
        stillUsed[key] = reader;

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
    // Anything the new arrangement did not claim is dropped here, so deleting a block
    // still closes its file -- but the shared_ptr keeps it alive while a retired
    // arrangement is still being read.
    readerCache.swap(stillUsed);

    canvasSource.setArrangement(Arrangement::Ptr (next));

    // The buffering source and the transport wiring are built ONCE. The first version
    // tore both down and rebuilt them on every edit, which meant re-running
    // prepareToPlay -- and with prefill on, that BLOCKS the message thread while a full
    // read-ahead buffer is filled from disk. Dragging a block therefore stalled the UI
    // and gapped the audio, which is the lag you could feel. Swapping the arrangement is
    // all an edit actually needs; the transport reads the new length straight through
    // getTotalLength().
    if (buffered == nullptr)
    {
        buffered = std::make_unique<juce::BufferingAudioSource>(&canvasSource, readThread, false,
                                                                 CanvasAudioSource::kReadAheadSamples, 2, true);
        transport.setSource(buffered.get(), 0, nullptr, rate, 2);
    }
    else
    {
        // Nudge the read position so the buffering source drops what it already holds --
        // otherwise up to a third of a second of the PREVIOUS arrangement keeps playing
        // after the edit.
        transport.setPosition(transport.getCurrentPosition());
    }
}


double CanvasPlayer::getPositionSeconds() const
{
    const double pos = transport.getCurrentPosition();
    if (!loopOn) return pos;
    const double a = loopFrom, b = loopTo;
    if (b <= a || pos < a) return pos;
    return a + std::fmod(pos - a, b - a);
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
    loopFrom = startSeconds;
    loopTo = endSeconds;
    canvasSource.setLoopRange(startSeconds, endSeconds);
    canvasSource.setLooping(on);
}

} // namespace mira::canvas
