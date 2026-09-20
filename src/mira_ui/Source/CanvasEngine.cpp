#include "CanvasEngine.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

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

void CanvasAudioSource::renderOffline(juce::AudioBuffer<float>& destination,
                                      juce::int64 from, int numSamples)
{
    // Not attached to a device, or attached at a small block size: renderRange chunks to
    // the scratch buffer, so it only has to be big enough to be useful, never big enough
    // for the whole render.
    if (scratch.getNumSamples() < kScratchSamples)
        scratch.setSize(2, kScratchSamples, false, true, true);
    juce::AudioSourceChannelInfo info (&destination, 0, numSamples);
    renderRange(info, from, numSamples);
}

// A short decaying sine per click. Generated rather than sampled: it is thirty
// milliseconds of arithmetic, it needs no file to ship or find, and a click has to survive
// being played over a full mix -- which a sine at a fixed frequency does and a recorded
// tick of unknown spectrum does not.
//
// The accent is a FIFTH up rather than louder. Louder competes with the music for level;
// a pitch change is audible at any level and tells you where the bar is even when the
// click is quiet enough to be tolerable.
void CanvasAudioSource::mixClicks(const juce::AudioSourceChannelInfo& info,
                                  juce::int64 from, int numSamples)
{
    ClickTrack::Ptr c = clicks;
    if (c == nullptr || c->samples.empty()) return;
    const float gain = clickGain.load();
    if (gain <= 0.0f) return;

    // The TIMELINE's rate, because `from` and the click positions are timeline samples --
    // the device rate is the resampler's business and nothing this reasons in.
    const double rate = kTimelineRate;
    const int len = (int) (0.03 * rate);            // 30 ms
    const juce::int64 to = from + numSamples;
    const int outChannels = info.buffer->getNumChannels();

    // Start at the first click that could still be sounding, not at the first one in the
    // block: a click that began just before `from` has most of its tail inside it.
    auto it = std::lower_bound(c->samples.begin(), c->samples.end(), from - len);
    for (; it != c->samples.end() && *it < to; ++it)
    {
        const size_t idx = (size_t) (it - c->samples.begin());
        const bool accented = idx < c->accent.size() && c->accent[idx] != 0;
        const double freq = accented ? 1500.0 : 1000.0;
        const juce::int64 start = *it;

        const juce::int64 a = juce::jmax(from, start);
        const juce::int64 b = juce::jmin(to, start + len);
        for (juce::int64 n = a; n < b; ++n)
        {
            const double t = (double) (n - start) / rate;
            // Exponential decay, and a 1 ms raised-cosine attack so the click does not
            // start with a step -- a step is a click of its own, at every frequency.
            const double attack = t < 0.001 ? 0.5 - 0.5 * std::cos(juce::MathConstants<double>::pi * t / 0.001) : 1.0;
            const float s = (float) (std::sin(juce::MathConstants<double>::twoPi * freq * t)
                                      * std::exp(-t * 90.0) * attack) * gain;
            for (int ch = 0; ch < outChannels; ++ch)
                info.buffer->addSample(ch, info.startSample + (int) (n - from), s);
        }
    }
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

float CanvasAudioSource::readAndClearLanePeak(int lane, int channel)
{
    if (lane < 0 || lane >= kMaxLanes || channel < 0 || channel > 1) return 0.0f;
    return lanePeak[lane][channel].exchange(0.0f);
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
    // Picked up here, on the audio thread, so the UI never touches a pointer the audio
    // thread is reading -- the same handover the arrangement uses.
    if (clickSwap.exchange(false)) clicks = pendingClick;

    Arrangement::Ptr a = active;
    if (a == nullptr) { mixClicks(info, from, numSamples); return; }

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

        // ---- sample rate ---------------------------------------------------------------
        //
        // The timeline is 44,100 because SA3 generates at 44,100 and nothing else, and the
        // transport resamples the finished mix to whatever the interface is running at
        // (48,000 here). A file at the timeline's own rate therefore costs nothing.
        //
        // A file at ANY OTHER RATE was broken: rateRatio was computed and used to offset
        // the read position, and then n consecutive samples were read anyway -- so a 48 kHz
        // drop played 8.8% slow and drifted further out of place the longer it ran, with
        // nothing on screen to say so. It needs resampling, not an offset.
        const bool needsResample = std::abs(v.rateRatio - 1.0) > 1.0e-9;
        const int srcWanted = needsResample
                                  ? (int) std::ceil(n * v.rateRatio) + 4
                                  : n;
        // One sample of run-up, so the interpolator has a point behind the first output.
        const juce::int64 srcFrom = needsResample ? juce::jmax<juce::int64>(0, readFrom - 1)
                                                  : readFrom;
        if (srcWanted > scratch.getNumSamples())
        {
            // Cannot happen at any ratio we accept; said out loud rather than truncated,
            // because a silently short read is a click nobody can reproduce.
            jassertfalse;
            continue;
        }
        scratch.clear(0, srcWanted);
        // Reading on this thread is safe ONLY because a BufferingAudioSource sits in
        // front of this source: this runs on its background thread, not in the device
        // callback. Take that buffer away and this line becomes file I/O in the
        // real-time path.
        v.reader->read(&scratch, 0, srcWanted, srcFrom, true, true);

        float voicePeak[2] = { 0.0f, 0.0f };
        for (int ch = 0; ch < outChannels; ++ch)
        {
            const int srcCh = juce::jmin(ch, scratch.getNumChannels() - 1);
            auto* dst = info.buffer->getWritePointer(ch, info.startSample + static_cast<int>(overlapStart - from));
            const auto* src = scratch.getReadPointer(srcCh);
            const double lead = needsResample ? (double) (readFrom - srcFrom) : 0.0;

            for (int i = 0; i < n; ++i)
            {
                float env = v.gain * laneLevel;
                const juce::int64 at = intoVoice + i;
                if (v.fadeInSamples > 0 && at < v.fadeInSamples)
                    env *= fadeGain (static_cast<float>(at) / static_cast<float>(v.fadeInSamples),
                                      v.fadeInShape);
                if (v.fadeOutSamples > 0 && at >= v.lengthSamples - v.fadeOutSamples)
                    env *= fadeGain (static_cast<float>(v.lengthSamples - at)
                                          / static_cast<float>(v.fadeOutSamples),
                                      v.fadeOutShape);
                float raw;
                if (needsResample)
                {
                    // Catmull-Rom over four neighbours. Stateless, because each chunk is
                    // read at an explicit position -- there is no running filter to carry
                    // across a seek, which is what makes an arbitrary-position source
                    // awkward to resample at all.
                    const double pos = lead + i * v.rateRatio;
                    const int i1 = (int) pos;
                    const float t = (float) (pos - i1);
                    const int last = srcWanted - 1;
                    const float y0 = src[juce::jlimit(0, last, i1 - 1)];
                    const float y1 = src[juce::jlimit(0, last, i1)];
                    const float y2 = src[juce::jlimit(0, last, i1 + 1)];
                    const float y3 = src[juce::jlimit(0, last, i1 + 2)];
                    raw = y1 + 0.5f * t * (y2 - y0
                              + t * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3
                              + t * (3.0f * (y1 - y2) + y3 - y0)));
                }
                else
                {
                    raw = src[i];
                }
                const float sample = raw * env;
                if (ch < 2) voicePeak[ch] = juce::jmax(voicePeak[ch], std::abs(sample));
                dst[i] += sample;
            }
        }

        // Post-fader, so the meter shows what the lane is CONTRIBUTING rather than what
        // the file contains -- pulling a fader down has to move its meter or the meter is
        // answering a question nobody asked.
        if (v.lane < kMaxLanes)
            for (int ch = 0; ch < 2; ++ch)
            {
                float seen = lanePeak[v.lane][ch].load();
                while (voicePeak[ch] > seen
                       && !lanePeak[v.lane][ch].compare_exchange_weak(seen, voicePeak[ch])) {}
            }
    }

    // BEFORE the master fader, so the click rides with the mix rather than over it: a
    // metronome that stays loud while you pull the music down is a metronome you turn off.
    mixClicks(info, from, numSamples);

    // The master fader, applied to the SUM -- after the tracks, before the meter, which is
    // the only order in which a master meter answers "what is leaving mira".
    const float master = masterGain.load();
    if (master != 1.0f)
        for (int ch = 0; ch < outChannels; ++ch)
            info.buffer->applyGain(ch, info.startSample, numSamples, master);

    // One peak for the whole mix, per channel -- the only place the stacking problem is
    // visible. Read and cleared by the UI.
    for (int ch = 0; ch < juce::jmin(2, outChannels); ++ch)
    {
        const float m = info.buffer->getMagnitude(ch, info.startSample, numSamples);
        float seen = peak[ch].load();
        while (m > seen && !peak[ch].compare_exchange_weak(seen, m)) {}
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

double CanvasPlayer::getDeviceRate() const
{
    if (deviceManager == nullptr) return 0.0;
    if (auto* dev = deviceManager->getCurrentAudioDevice()) return dev->getCurrentSampleRate();
    return 0.0;
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

    // ---- crossfades, worked out here rather than stored on the block -----------------
    //
    // Two blocks that OVERLAP ON THE SAME TRACK crossfade across the overlap: the earlier
    // one fades out over it, the later one fades in. Only on the same track -- blocks on
    // different tracks are meant to sound together, which is the whole point of stacking
    // them, and crossfading those would be the canvas deciding your arrangement for you.
    //
    // Computed, not written back: the block keeps the fade YOU drew, and dragging the
    // overlap apart restores it instead of leaving a fade you never asked for.
    std::vector<Block> laid;
    std::vector<std::shared_ptr<juce::AudioFormatReader>> readers;
    for (const auto& b : blocks)
    {
        if (b.muted) continue;                // a muted block crossfades with nothing
        if (!b.file.existsAsFile() || b.length <= 0.0) continue;

        const auto key = b.file.getFullPathName();
        std::shared_ptr<juce::AudioFormatReader> reader;
        if (auto hit = readerCache.find (key); hit != readerCache.end()) reader = hit->second;
        else if (auto* made = formats.createReaderFor (b.file)) reader.reset (made);
        if (reader == nullptr || reader->sampleRate <= 0.0) continue;
        stillUsed[key] = reader;

        // A BLOCK IS ONLY AS LONG AS IT SOUNDS. The empty tail you drag out past the audio
        // is a request to generate, not silence to play -- and a fade-out placed at the
        // end of the BLOCK would sit in that emptiness, fading nothing, while the real end
        // of the audio arrived at full level. The same goes for a crossfade: what overlaps
        // the next block is the audible part, not the frame.
        auto trimmed = b;
        const double fileSeconds = reader->lengthInSamples / reader->sampleRate;
        const double available = juce::jmax (0.0, fileSeconds - b.sourceOffset);
        const double sounding = b.contentSeconds > 0.0
                                    ? juce::jmin (b.contentSeconds, available)
                                    : available;
        trimmed.length = juce::jmin (b.length, sounding);
        if (trimmed.length <= 0.0) continue;
        trimmed.fadeIn  = juce::jmin (trimmed.fadeIn,  trimmed.length);
        trimmed.fadeOut = juce::jmin (trimmed.fadeOut, trimmed.length);

        laid.push_back (trimmed);
        readers.push_back (std::move (reader));
    }

    std::vector<FadeShape> inShape (laid.size()), outShape (laid.size());
    for (size_t i = 0; i < laid.size(); ++i)
        inShape[i] = outShape[i] = laid[i].fadeShape;

    {
        std::map<int, std::vector<size_t>> byLane;
        for (size_t i = 0; i < laid.size(); ++i) byLane[laid[i].lane].push_back (i);
        for (auto& lane : byLane)
        {
            auto& idx = lane.second;
            std::sort (idx.begin(), idx.end(),
                       [&laid] (size_t a, size_t b) { return laid[a].start < laid[b].start; });
            for (size_t k = 0; k + 1 < idx.size(); ++k)
            {
                auto& a = laid[idx[k]];
                auto& b = laid[idx[k + 1]];
                const double overlap = a.end() - b.start;
                if (overlap <= 0.0) continue;
                // Never longer than either block: a short block swallowed by a long one
                // would otherwise be asked to fade for longer than it exists.
                const double x = juce::jmin (overlap, a.length, b.length);
                if (x <= 0.0) continue;
                a.fadeOut = juce::jmax (a.fadeOut, x);
                b.fadeIn  = juce::jmax (b.fadeIn,  x);
                // Equal power across the join, so the sum holds level through the middle
                // instead of dipping 3 dB.
                outShape[idx[k]]     = FadeShape::EqualPower;
                inShape[idx[k + 1]]  = FadeShape::EqualPower;
            }
        }
    }

    auto next = new Arrangement();
    for (size_t bi = 0; bi < laid.size(); ++bi)
    {
        const auto& b = laid[bi];
        auto reader = readers[bi];

        Arrangement::Voice v;
        v.rateRatio      = reader->sampleRate > 0.0 ? reader->sampleRate / rate : 1.0;
        v.startSample    = static_cast<juce::int64>(b.start * rate);
        v.lengthSamples  = static_cast<juce::int64>(b.length * rate);
        v.sourceStartSample = static_cast<juce::int64>(b.sourceOffset * reader->sampleRate);
        v.fadeInSamples  = static_cast<juce::int64>(b.fadeIn * rate);
        v.fadeOutSamples = static_cast<juce::int64>(b.fadeOut * rate);
        v.gain           = juce::Decibels::decibelsToGain(static_cast<float>(b.gainDb));
        v.lane           = b.lane;
        v.fadeInShape    = inShape[bi];
        v.fadeOutShape   = outShape[bi];
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
