// MIRA-VIDEO.md Phase 1.4b -- the clip's length and frame rate, asked of AVFoundation
// directly.
//
// This file exists because of a measurement, not a preference. Phase 0.2 ran a
// VideoComponent for 700 seconds of successful playback and `getVideoDuration()` returned
// 0.00 for every one of them, even after polling for five seconds. load() succeeds,
// playback works, the duration is simply never reported -- so a clip drawn on the
// timeline from that number would be a clip of zero length.

#include "VideoWindow.h"

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

} // namespace mira::canvas::video_native
