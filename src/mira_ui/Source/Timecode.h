#pragma once

#include <juce_core/juce_core.h>

// ---- MIRA-VIDEO.md Phase 3: timecode ------------------------------------------------
//
// Header-only and free of any UI, so the arithmetic can be read and checked on its own.
// Timecode is the one number in this whole feature that someone else will read back to
// you down a phone line, and it has three ways to be subtly wrong: the frame rate, the
// drop-frame renumbering, and the start offset. Each is separated out below.
//
// THE TWO RATES. A 23.976 fps file is numbered in 24 fps timecode: the frame COUNT comes
// from the file's real rate, and the display divides that count by the NOMINAL rate. That
// is why `fps` and `nominal` are different numbers here and must not be collapsed -- an
// hour of 24 fps timecode on 23.976 material is 3603.6 real seconds, and a clock that
// used one rate for both would drift by three and a half seconds an hour against the
// editor's.
namespace mira::canvas::tc {

struct Format
{
    double fps = 25.0;          // the file's real rate: 23.976, 25, 29.97, 30, 50, 59.94
    bool dropFrame = false;     // only meaningful at 29.97 and 59.94
    // The timecode of the clip's first frame, in TIMECODE seconds -- 01:00:00:00 is
    // 3600.0 whatever the rate. Converted to frames through `nominal`, never through
    // `fps`, or an hour of 23.976 would start 86 frames early.
    double startSeconds = 0.0;
};

// 23.976 -> 24, 29.97 -> 30, 59.94 -> 60. The rate timecode COUNTS in.
inline int nominalRate(double fps)
{
    return juce::jmax(1, juce::roundToInt(fps));
}

// Drop-frame exists only at 29.97 and 59.94. Offering it anywhere else is offering a
// setting that can only be wrong, so callers ask this before showing the option.
inline bool dropFrameIsPossible(double fps)
{
    return std::abs(fps - 29.97) < 0.05 || std::abs(fps - 59.94) < 0.06;
}

// Elapsed seconds on the timeline -> a frame count in this format, offset included.
inline juce::int64 framesAt(double elapsedSeconds, const Format& f)
{
    const auto n = (juce::int64) nominalRate(f.fps);
    const auto start = (juce::int64) std::llround(f.startSeconds * (double) n);
    const auto elapsed = (juce::int64) std::llround(elapsedSeconds * f.fps);
    return juce::jmax<juce::int64>(0, start + elapsed);
}

// The SMPTE drop-frame renumbering: two frame NUMBERS (not frames) are skipped at the top
// of every minute except every tenth. It makes the clock agree with wall time over an
// hour; it never drops a picture frame, which is the thing everyone means when they say
// "dropped frames" and is not this.
inline juce::String formatFrames(juce::int64 frames, int nominal, bool dropFrame)
{
    nominal = juce::jmax(1, nominal);
    bool negative = frames < 0;
    if (negative) frames = -frames;

    juce::String separator (":");
    if (dropFrame && (nominal == 30 || nominal == 60))
    {
        const juce::int64 dropPerMinute = nominal / 15;              // 2 at 30, 4 at 60
        const juce::int64 framesPer10Min = (juce::int64) nominal * 60 * 10 - 9 * dropPerMinute;
        const juce::int64 framesPerMinute = (juce::int64) nominal * 60 - dropPerMinute;

        const juce::int64 tens = frames / framesPer10Min;
        const juce::int64 rest = frames % framesPer10Min;
        frames += dropPerMinute * 9 * tens;
        if (rest >= dropPerMinute)
            frames += dropPerMinute * ((rest - dropPerMinute) / framesPerMinute);
        separator = ";";                                             // the SMPTE convention
    }

    const juce::int64 perSecond = nominal;
    const juce::int64 ff = frames % perSecond;
    const juce::int64 totalSeconds = frames / perSecond;
    const juce::int64 ss = totalSeconds % 60;
    const juce::int64 mm = (totalSeconds / 60) % 60;
    const juce::int64 hh = totalSeconds / 3600;

    auto two = [](juce::int64 v) { return juce::String (v).paddedLeft ('0', 2); };
    return (negative ? "-" : "") + two (hh) + ":" + two (mm) + ":" + two (ss) + separator + two (ff);
}

inline juce::String format(double elapsedSeconds, const Format& f)
{
    return formatFrames (framesAt (elapsedSeconds, f), nominalRate (f.fps), f.dropFrame);
}

// "01:00:00:00", "1:00:00:00", "01:00:00;00" -> seconds of TIMECODE (3600.0), or a
// negative number when it is not a timecode at all. Deliberately strict about the shape
// and forgiving about the separators: a colon, a semicolon and a full stop all mean the
// same thing to the person typing, and an editor's paperwork uses all three.
inline double parse(const juce::String& text, int nominal)
{
    auto cleaned = text.trim().replaceCharacters (";.,", ":::");
    auto parts = juce::StringArray::fromTokens (cleaned, ":", "");
    parts.removeEmptyStrings();
    if (parts.size() < 2 || parts.size() > 4) return -1.0;
    for (const auto& p : parts)
        if (!p.containsOnly ("0123456789")) return -1.0;

    // Read from the RIGHT, so "12:18" is twelve seconds and eighteen frames rather than
    // twelve hours -- the way every clock anyone has typed into behaves.
    int values[4] { 0, 0, 0, 0 };                                    // hh mm ss ff
    for (int i = 0; i < parts.size(); ++i)
        values[4 - parts.size() + i] = parts[i].getIntValue();

    nominal = juce::jmax (1, nominal);
    if (values[3] >= nominal) return -1.0;                           // 25 frames at 25 fps
    const double seconds = values[0] * 3600.0 + values[1] * 60.0 + values[2]
                            + (double) values[3] / (double) nominal;
    return seconds;
}

} // namespace mira::canvas::tc
