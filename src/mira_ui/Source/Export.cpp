#include "Export.h"

namespace mira::ui {

ExportResult renderTake(const juce::File& source,
                        const juce::File& destination,
                        const TakeEdit& edit)
{
    ExportResult out;
    out.file = destination;

    if (!source.existsAsFile())
    {
        out.message = "source is gone: " + source.getFullPathName();
        return out;
    }

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor(source));
    if (reader == nullptr)
    {
        out.message = "could not read " + source.getFileName();
        return out;
    }

    const double sr = reader->sampleRate;
    const int channels = static_cast<int>(reader->numChannels);
    if (sr <= 0.0 || channels <= 0)
    {
        out.message = "unreadable format in " + source.getFileName();
        return out;
    }
    out.sampleRate = sr;

    // The edit's range, in samples of the SOURCE's own clock. Rounding once, here, is
    // what keeps the fade arithmetic below from drifting against the trim.
    const int64_t total = reader->lengthInSamples;
    int64_t first = (edit.startSeconds > 0.0) ? static_cast<int64_t>(std::llround(edit.startSeconds * sr)) : 0;
    int64_t last  = (edit.endSeconds > edit.startSeconds)
                        ? static_cast<int64_t>(std::llround(edit.endSeconds * sr))
                        : total;
    first = juce::jlimit<int64_t>(0, total, first);
    last  = juce::jlimit<int64_t>(first, total, last);

    const int64_t frames = last - first;
    if (frames <= 0)
    {
        out.message = "the edit selects no audio";
        return out;
    }
    out.frames = frames;

    // Fades are clamped to the range and to each other: two fades longer than the take
    // between them would otherwise multiply into a dip in the middle, which is not what
    // either handle says it does. Shrinking both proportionally keeps their ratio.
    double fadeIn  = juce::jmax(0.0, edit.fadeInSeconds);
    double fadeOut = juce::jmax(0.0, edit.fadeOutSeconds);
    const double span = static_cast<double>(frames) / sr;
    if (fadeIn + fadeOut > span && fadeIn + fadeOut > 0.0)
    {
        const double squeeze = span / (fadeIn + fadeOut);
        fadeIn *= squeeze;
        fadeOut *= squeeze;
    }
    const int64_t fadeInFrames  = static_cast<int64_t>(std::llround(fadeIn * sr));
    const int64_t fadeOutFrames = static_cast<int64_t>(std::llround(fadeOut * sr));

    const float gain = juce::Decibels::decibelsToGain(static_cast<float>(edit.gainDb));

    if (!destination.getParentDirectory().createDirectory().wasOk())
    {
        out.message = "could not create " + destination.getParentDirectory().getFullPathName();
        return out;
    }

    // Bit depth follows the source. A 24-bit take exported as 16 is a quiet, invisible
    // downgrade of the deliverable; matching means export never decides something the
    // user did not ask it to decide. Anything unusual lands on 24 rather than guessing.
    int bits = static_cast<int>(reader->bitsPerSample);
    if (bits != 16 && bits != 24 && bits != 32) bits = 24;
    // A float source stays float: it cannot clip, and re-quantising it here would throw
    // away the one property that makes it worth keeping.
    const bool sourceIsFloat = reader->usesFloatingPointData;

    destination.deleteFile();   // §3.7: re-export overwrites, never _1, _2
    std::unique_ptr<juce::FileOutputStream> stream (destination.createOutputStream());
    if (stream == nullptr)
    {
        out.message = "could not open " + destination.getFullPathName() + " for writing";
        return out;
    }

    juce::WavAudioFormat wav;
    juce::StringPairArray metadata;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor(stream.get(), sr, static_cast<unsigned int>(channels),
                            sourceIsFloat ? 32 : bits, metadata, 0));
    if (writer == nullptr)
    {
        out.message = "no wav writer for " + juce::String(sr, 0) + " Hz / " + juce::String(bits) + "-bit";
        return out;
    }
    stream.release();           // the writer owns it now
    if (sourceIsFloat) writer->flush();

    const int block = 32768;
    juce::AudioBuffer<float> buffer (channels, block);
    int64_t done = 0;
    float peak = 0.0f;
    bool wrote = true;

    while (done < frames && wrote)
    {
        const int n = static_cast<int>(juce::jmin<int64_t>(block, frames - done));
        buffer.clear();
        if (!reader->read(&buffer, 0, n, first + done, true, true))
        {
            out.message = "read failed " + juce::String(done / sr, 2) + "s into the take";
            return out;
        }

        buffer.applyGain(0, n, gain);

        // Linear in AMPLITUDE, which is what the waveform draws. A curve that sounds
        // slightly better but does not match the wedge on screen would make the display
        // a lie about the file, and the display is how the fade gets set in the first
        // place.
        for (int ch = 0; ch < channels; ++ch)
        {
            auto* d = buffer.getWritePointer(ch);
            for (int i = 0; i < n; ++i)
            {
                const int64_t at = done + i;
                float env = 1.0f;
                if (fadeInFrames > 0 && at < fadeInFrames)
                    env *= static_cast<float>(at) / static_cast<float>(fadeInFrames);
                if (fadeOutFrames > 0 && at >= frames - fadeOutFrames)
                    env *= static_cast<float>(frames - at) / static_cast<float>(fadeOutFrames);
                d[i] *= env;
            }
        }

        for (int ch = 0; ch < channels; ++ch)
            peak = juce::jmax(peak, buffer.getMagnitude(ch, 0, n));

        wrote = writer->writeFromAudioSampleBuffer(buffer, 0, n);
        done += n;
    }

    writer.reset();             // closes and finalises the header

    if (!wrote)
    {
        out.message = "write failed " + juce::String(done / sr, 2) + "s in -- disk full?";
        destination.deleteFile();
        return out;
    }

    out.ok = true;
    out.peak = peak;
    // Says it clipped when it clipped. A gain the user dialled in is theirs to keep, so
    // this reports rather than quietly normalising -- but an export that silently came
    // back distorted would be the worst of both (convention 6).
    out.message = destination.getFileName() + "  "
                + juce::String(frames / sr, 2) + "s  "
                + juce::String(sr, 0) + " Hz  "
                + (sourceIsFloat ? juce::String("32-bit float") : juce::String(bits) + "-bit")
                + (peak > 1.0f && !sourceIsFloat
                       ? "  CLIPPED at +" + juce::String(juce::Decibels::gainToDecibels(peak), 1) + " dBFS"
                       : "  peak " + juce::String(juce::Decibels::gainToDecibels(juce::jmax(peak, 1.0e-6f)), 1) + " dBFS");
    return out;
}

juce::String slugify(const juce::String& text)
{
    juce::String out;
    for (auto c : text)
        if (juce::CharacterFunctions::isLetterOrDigit(c))
            out += juce::String::charToString(c).toLowerCase();
    return out;
}

juce::String keySlug(const juce::String& keyScale)
{
    const auto trimmed = keyScale.trim();
    if (trimmed.isEmpty()) return {};

    // "A minor" -> "Amin". The tonic keeps its case and its accidental (A# and Ab are
    // different keys); only the mode is abbreviated.
    const auto tonic = trimmed.upToFirstOccurrenceOf(" ", false, false);
    const auto mode  = trimmed.fromFirstOccurrenceOf(" ", false, false).trim().toLowerCase();
    juce::String shortMode;
    if (mode.startsWith("min")) shortMode = "min";
    else if (mode.startsWith("maj")) shortMode = "maj";
    else shortMode = mode.substring(0, 3);

    return tonic.removeCharacters(" ") + shortMode;
}

juce::String deliveryName(const juce::String& project,
                          const juce::String& cue,
                          std::optional<double> bpm,
                          const juce::String& keyScale,
                          int version)
{
    juce::StringArray parts;
    if (const auto p = slugify(project); p.isNotEmpty()) parts.add(p);
    if (const auto c = slugify(cue); c.isNotEmpty()) parts.add(c);
    // Rounded to a whole number -- 120.4 -> 120 (§3.7). A decimal in a filename is a
    // precision claim the tempo estimate does not support.
    if (bpm.has_value() && *bpm > 0.0) parts.add(juce::String(juce::roundToInt(*bpm)) + "bpm");
    if (const auto k = keySlug(keyScale); k.isNotEmpty()) parts.add(k);
    if (version > 0) parts.add("v" + juce::String(version));

    if (parts.isEmpty()) parts.add("export");
    return parts.joinIntoString("_") + ".wav";
}

int versionFromWorkingName(const juce::String& fileName)
{
    const auto stem = fileName.upToLastOccurrenceOf(".", false, false);
    const auto tail = stem.fromLastOccurrenceOf("_v", false, false);
    if (tail.isEmpty() || tail == stem) return 0;
    if (!tail.containsOnly("0123456789")) return 0;
    return tail.getIntValue();
}

} // namespace mira::ui
