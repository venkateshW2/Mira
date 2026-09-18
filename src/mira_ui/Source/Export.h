#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <optional>

// MIRA-GENERATE.md Phase 6 -- export.
//
// The one place a take's stored edit becomes an actual file. Everything up to here is
// non-destructive: the trim is a `segments` row and the fades and gain are JSON in
// `segments.human`, so a cue can be re-cut next week without regenerating. Export is
// where that gets rendered, and it is the only code in mira that writes audio.
//
// Two rules this file exists to hold:
//
// 1. NATIVE SAMPLE RATE, ALWAYS. SA3 generates at 44,100 Hz and nothing else --
//    sa3_mlx.py, pre_encode_mlx.py and demo_mlx.py all hardcode it, and the 0.0928 s
//    latent step IS 4096/44100. Export reads the source's own rate and writes that rate.
//    It must never route through the playback path: WaveformView resamples live to the
//    device rate through juce::ResamplingAudioSource, which is an interpolator plus a
//    simple IIR low-pass -- fine for auditioning, not a mastering SRC. Nothing here
//    touches an AudioTransportSource for exactly that reason.
//
// 2. A MISSING FIELD DROPS ITS TOKEN (MIRA-GENERATE.md §3.7, CLAUDE.md convention 1).
//    No BPM means `shortfilm_cue01_Amin_v1.wav`, never a guessed number. mira's BPM gate
//    already omits when it cannot support an answer; the filename tells the same truth.
namespace mira::ui {

// What the segment row and its `human` JSON add up to. endSeconds <= startSeconds means
// "the whole file" -- a take with no trim is not a special case anywhere below.
struct TakeEdit {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    double fadeInSeconds = 0.0;
    double fadeOutSeconds = 0.0;
    double gainDb = 0.0;
};

struct ExportResult {
    bool ok = false;
    juce::File file;
    juce::String message;       // why it failed, or what it wrote
    double sampleRate = 0.0;
    double peak = 0.0;          // post-gain, linear; > 1.0 means it clipped
    int64_t frames = 0;
};

// Renders one take. Reads with an AudioFormatReader, applies gain then fades in the
// float domain, writes a wav at the SOURCE's sample rate, channel count and bit depth.
ExportResult renderTake(const juce::File& source,
                        const juce::File& destination,
                        const TakeEdit& edit);

// --- naming (MIRA-GENERATE.md §3.7) ------------------------------------------------

// "Short Film" -> "shortfilm". Letters and digits survive, everything else goes.
juce::String slugify(const juce::String& text);

// "A minor" -> "Amin", "F# major" -> "F#maj". Empty in, empty out -- the caller drops
// the token rather than this inventing one.
juce::String keySlug(const juce::String& keyScale);

// {project}_{cue}_{bpm}bpm_{key}_v{n}.wav, with every absent field's token omitted.
// `version` of 0 drops the version token too.
juce::String deliveryName(const juce::String& project,
                          const juce::String& cue,
                          std::optional<double> bpm,
                          const juce::String& keyScale,
                          int version);

// The `n` in a working name like `shortfilm_cue01_v3.wav`, or 0 when there is none.
// Read from the FILENAME rather than recounted, so a re-export lands on the same name
// it landed on last time -- MIRA-GENERATE.md §3.7's "re-export overwrites
// deterministically. Never _1, _2."
int versionFromWorkingName(const juce::String& fileName);

} // namespace mira::ui
