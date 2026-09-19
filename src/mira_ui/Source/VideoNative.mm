// MIRA-VIDEO.md Phase 1.4b -- the clip's length and frame rate, asked of AVFoundation
// directly.
//
// This file exists because of a measurement, not a preference. Phase 0.2 ran a
// VideoComponent for 700 seconds of successful playback and `getVideoDuration()` returned
// 0.00 for every one of them, even after polling for five seconds. load() succeeds,
// playback works, the duration is simply never reported -- so a clip drawn on the
// timeline from that number would be a clip of zero length.

#include "VideoWindow.h"
#include <juce_audio_formats/juce_audio_formats.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>

namespace mira::canvas::video_native {

bool probe(const juce::File& file, double& seconds, double& framesPerSecond, juce::String& error)
{
    seconds = 0.0;
    framesPerSecond = 0.0;

    @autoreleasepool
    {
        NSString* path = [NSString stringWithUTF8String: file.getFullPathName().toRawUTF8()];
        if (path == nil) { error = "the path is not valid UTF-8"; return false; }

        NSURL* url = [NSURL fileURLWithPath: path];
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL: url options: nil];
        if (asset == nil) { error = "AVFoundation could not open the file"; return false; }

        // The synchronous accessors. They are deprecated in favour of the async
        // `loadValuesAsynchronouslyForKeys:` API, but this runs once at load on a local
        // file, and an asynchronous length would mean the timeline could not be drawn
        // until it arrived. Revisit if mira ever opens a clip over a network volume.
        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        const CMTime duration = asset.duration;
        NSArray<AVAssetTrack*>* videoTracks = [asset tracksWithMediaType: AVMediaTypeVideo];
        #pragma clang diagnostic pop

        if (CMTIME_IS_VALID(duration) && !CMTIME_IS_INDEFINITE(duration))
            seconds = CMTimeGetSeconds(duration);

        if (videoTracks.count == 0)
        {
            error = "the file has no video track";
            return false;
        }

        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        framesPerSecond = (double) videoTracks.firstObject.nominalFrameRate;
        #pragma clang diagnostic pop

        if (seconds <= 0.0)
        {
            error = "AVFoundation reported no duration";
            return false;
        }
        if (framesPerSecond <= 0.0)
        {
            // A length with no frame rate is still useful; the timecode ruler is Phase 3
            // and it can be overridden there. Say so rather than inventing 25.
            error = "AVFoundation reported no frame rate";
            return false;
        }
        return true;
    }
}

// ---- Phase 2.1: the audio, when JUCE will not read it ------------------------------

bool extractAudio(const juce::File& source, const juce::File& destinationWav,
                  juce::String& error, const std::function<bool(double)>& progress)
{
    @autoreleasepool
    {
        NSString* path = [NSString stringWithUTF8String: source.getFullPathName().toRawUTF8()];
        if (path == nil) { error = "the path is not valid UTF-8"; return false; }

        AVURLAsset* asset = [AVURLAsset URLAssetWithURL: [NSURL fileURLWithPath: path] options: nil];
        if (asset == nil) { error = "AVFoundation could not open the file"; return false; }

        #pragma clang diagnostic push
        #pragma clang diagnostic ignored "-Wdeprecated-declarations"
        NSArray<AVAssetTrack*>* audioTracks = [asset tracksWithMediaType: AVMediaTypeAudio];
        const double totalSeconds = CMTimeGetSeconds (asset.duration);
        #pragma clang diagnostic pop

        if (audioTracks.count == 0)
        {
            // Said plainly, because a film with no audio track is a normal thing to be
            // handed and "the extraction failed" would send you looking for a bug.
            error = "the file has no audio track";
            return false;
        }

        NSError* nsError = nil;
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset: asset error: &nsError];
        if (reader == nil)
        {
            error = "AVAssetReader: " + juce::String ([[nsError localizedDescription] UTF8String]);
            return false;
        }

        // INTERLEAVED, and this is measured rather than preferred. The first version asked
        // for non-interleaved float so each channel would arrive as its own block and could
        // go straight into the writer. `canAddOutput` said yes, `startReading` said yes, and
        // then `copyNextSampleBuffer` returned nothing at all, for every buffer, with the
        // reader's status never going to Failed -- a silent empty read, which is the exact
        // shape of failure convention 6 exists to forbid. AVAssetReaderAudioMixOutput wants
        // interleaved PCM; the deinterleave below is the price.
        NSDictionary* settings = @{
            (id) AVFormatIDKey:               @(kAudioFormatLinearPCM),
            (id) AVLinearPCMBitDepthKey:      @32,
            (id) AVLinearPCMIsFloatKey:       @YES,
            (id) AVLinearPCMIsBigEndianKey:   @NO,
            (id) AVLinearPCMIsNonInterleaved: @NO,
        };
        AVAssetReaderAudioMixOutput* output =
            [AVAssetReaderAudioMixOutput assetReaderAudioMixOutputWithAudioTracks: audioTracks
                                                                   audioSettings: settings];
        if (![reader canAddOutput: output]) { error = "AVAssetReader refused the output settings"; return false; }
        [reader addOutput: output];
        if (![reader startReading])
        {
            error = "AVAssetReader would not start: "
                      + juce::String ([[reader.error localizedDescription] UTF8String]);
            return false;
        }

        std::unique_ptr<juce::AudioFormatWriter> writer;
        juce::AudioBuffer<float> scratch;
        bool cancelled = false;
        juce::int64 framesWritten = 0;
        int buffersSeen = 0;

        for (;;)
        {
            CMSampleBufferRef sample = [output copyNextSampleBuffer];
            if (sample == nullptr) break;
            ++buffersSeen;

            if (writer == nullptr)
            {
                // The rate and channel count come from the FIRST buffer's format
                // description rather than from the track's nominal one: the mix output is
                // what we are actually reading, and it is the thing that decides.
                auto format = CMSampleBufferGetFormatDescription (sample);
                const AudioStreamBasicDescription* asbd =
                    format != nullptr ? CMAudioFormatDescriptionGetStreamBasicDescription (format) : nullptr;
                if (asbd == nullptr || asbd->mSampleRate <= 0.0 || asbd->mChannelsPerFrame == 0)
                {
                    CFRelease (sample);
                    error = "the audio track reported no format";
                    return false;
                }
                destinationWav.deleteFile();
                destinationWav.getParentDirectory().createDirectory();
                std::unique_ptr<juce::FileOutputStream> stream (destinationWav.createOutputStream());
                if (stream == nullptr)
                {
                    CFRelease (sample);
                    error = "could not write " + destinationWav.getFullPathName();
                    return false;
                }
                juce::WavAudioFormat wav;
                writer.reset (wav.createWriterFor (stream.get(), asbd->mSampleRate,
                                                   (unsigned int) asbd->mChannelsPerFrame, 24, {}, 0));
                if (writer == nullptr)
                {
                    CFRelease (sample);
                    error = "could not create a writer";
                    return false;
                }
                stream.release();
            }

            AudioBufferList list {};
            CMBlockBufferRef block = nullptr;
            const OSStatus status = CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer (
                sample, nullptr, &list, sizeof (list), nullptr, nullptr,
                kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment, &block);

            if (status == noErr && list.mNumberBuffers > 0)
            {
                // One buffer, every channel interleaved inside it.
                const int channels = juce::jlimit (1, 32, (int) list.mBuffers[0].mNumberChannels);
                const int frames = (int) (list.mBuffers[0].mDataByteSize / (sizeof (float) * (size_t) channels));
                const auto* interleaved = static_cast<const float*> (list.mBuffers[0].mData);

                if (frames > 0 && interleaved != nullptr)
                {
                    if (scratch.getNumChannels() < channels || scratch.getNumSamples() < frames)
                        scratch.setSize (channels, frames, false, false, true);

                    for (int c = 0; c < channels; ++c)
                    {
                        auto* out = scratch.getWritePointer (c);
                        for (int i = 0; i < frames; ++i)
                            out[i] = interleaved[i * channels + c];
                    }

                    const float* pointers[32] {};
                    for (int c = 0; c < channels; ++c) pointers[c] = scratch.getReadPointer (c);

                    if (!writer->writeFromFloatArrays (pointers, channels, frames))
                    {
                        if (block != nullptr) CFRelease (block);
                        CFRelease (sample);
                        error = "write failed";
                        return false;
                    }
                    framesWritten += frames;
                }
            }

            if (block != nullptr) CFRelease (block);

            const double at = CMTimeGetSeconds (CMSampleBufferGetPresentationTimeStamp (sample));
            CFRelease (sample);

            if (progress && totalSeconds > 0.0 && !progress (juce::jlimit (0.0, 1.0, at / totalSeconds)))
            {
                cancelled = true;
                [reader cancelReading];
                break;
            }
        }

        writer.reset();   // flush the header before anything asks how long the file is

        if (cancelled) { destinationWav.deleteFile(); error = "cancelled"; return false; }
        if (reader.status == AVAssetReaderStatusFailed)
        {
            destinationWav.deleteFile();
            error = "AVAssetReader failed: "
                      + juce::String ([[reader.error localizedDescription] UTF8String]);
            return false;
        }
        if (framesWritten <= 0 || !destinationWav.existsAsFile() || destinationWav.getSize() < 1024)
        {
            destinationWav.deleteFile();
            // The counts, not just the verdict. "Produced no audio" sent the last round of
            // this looking at the wrong half: the reader was handing over buffers fine and
            // the settings were what it could not use.
            error = "the extraction produced no audio - " + juce::String (buffersSeen)
                      + " buffers, " + juce::String (framesWritten) + " frames, reader status "
                      + juce::String ((int) reader.status);
            return false;
        }
        return true;
    }
}

} // namespace mira::canvas::video_native
