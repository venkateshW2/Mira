#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// TASKS.md Phase 5 build-order step 2: "base window shell + a LookAndFeel_V4 skeleton —
// palette/type tokens first, before any real panel is built on top of it." Every hex
// value below is taken verbatim from the reference mockup's :root CSS custom properties
// (TASKS.md's linked artifact) — that mockup is a static HTML illustration, not a spec
// (TASKS.md says so directly), so this is the same palette/typography *vocabulary*
// carried into real JUCE painting, not a pixel port of its markup.
class MiraLookAndFeel : public juce::LookAndFeel_V4
{
public:
    MiraLookAndFeel();

    // Palette — mockup's :root custom properties, one-to-one.
    static const juce::Colour bg;         // --bg    #14161b (outer page ground)
    static const juce::Colour surface;    // --surface #1a1d24 (the window itself)
    static const juce::Colour surface2;   // --surface-2 #20242d (titlebar, statusbar, hover)
    static const juce::Colour surface3;   // --surface-3 #262b36 (inputs, pills, lane backgrounds)
    static const juce::Colour border;     // --border #2a2f3b
    static const juce::Colour borderSoft; // --border-soft #22262f
    static const juce::Colour text;       // --text #e8eaf0
    static const juce::Colour textDim;    // --text-dim #8992a6
    static const juce::Colour textFaint;  // --text-faint #545c6e
    static const juce::Colour accent;     // --accent #e8a33d — warm amber, selection/primary accent
    static const juce::Colour accentSoft; // --accent-soft rgba(232,163,61,0.14)
    static const juce::Colour active;     // --active #4fb8ae — teal, active-region/beat/selection
    static const juce::Colour activeSoft; // --active-soft rgba(79,184,174,0.16)
    static const juce::Colour warn;       // --warn #e2574c
    static const juce::Colour warnSoft;   // --warn-soft rgba(226,87,76,0.14)
    static const juce::Colour good;       // --good #6fbf73 — human-sourced field indicator
    static const juce::Colour goodSoft;   // --good-soft rgba(111,191,115,0.14)

    // Typography — IBM Plex Sans for UI text, IBM Plex Mono for numeric/technical fields
    // (BPM, LUFS, timestamps, token counts — the mockup's own convention throughout its
    // .measure/.num/.mono rules, tabular-nums). Embedded via BinaryData
    // (CMakeLists.txt's juce_add_binary_data), not a system-font dependency.
    juce::Font sansRegular(float height) const;
    juce::Font sansMedium(float height) const;
    juce::Font sansSemiBold(float height) const;
    juce::Font monoRegular(float height) const;
    juce::Font monoMedium(float height) const;

    juce::Font getLabelFont(juce::Label&) override;

private:
    juce::Typeface::Ptr sansRegularTypeface, sansMediumTypeface, sansSemiBoldTypeface;
    juce::Typeface::Ptr monoRegularTypeface, monoMediumTypeface;
};
