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

    // Replaces the native macOS title bar's OS-gray chrome — it doesn't take
    // MiraLookAndFeel's colours at all, and sat visibly disconnected from the rest of
    // the window in the first skeleton screenshot. --surface-2 background + --border
    // bottom edge, matching the mockup's .titlebar rule exactly. MainWindow (Main.cpp)
    // pairs this with setUsingNativeTitleBar(false) — this override does nothing while
    // the native title bar is in use, JUCE never calls it in that mode.
    // Both deliberately paint nothing (review round 2). With the main window's corners
    // rounded by a layer mask (NativeWindowChrome::applyRoundedCorners), V4's square
    // border outline and corner grip get clipped at the curve into a hard black edge.
    // AppKit's own window shadow now draws the edge, following the real rounded shape.
    // The grip stays fully functional, it just isn't drawn.
    void drawResizableWindowBorder(juce::Graphics&, int w, int h, const juce::BorderSize<int>& border,
                                   juce::ResizableWindow&) override;
    void drawCornerResizer(juce::Graphics&, int w, int h, bool isMouseOver, bool isMouseDragging) override;

    void drawDocumentWindowTitleBar(juce::DocumentWindow&, juce::Graphics&, int w, int h, int titleSpaceX,
                                     int titleSpaceW, const juce::Image* icon, bool drawTitleTextOnLeft) override;

    juce::Font getTextButtonFont(juce::TextButton&, int buttonHeight) override;
    void drawButtonBackground(juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    // JUCE's stock close/minimise/maximise glyphs are a saturated red/yellow/green X,
    // dash and fullscreen-cross — flagged as looking wrong against everything else here.
    // Replaced with the mockup's own muted .dot.r/.y/.g colours (#4a2f30/#4a4330/
    // #2f4a33), a plain filled circle, brightening slightly on hover for affordance
    // rather than any glyph at rest — same restrained language macOS's own traffic
    // lights use before you mouse over them.
    juce::Button* createDocumentWindowButton(int buttonType) override;

    // "Fake glass" — real NSVisualEffectView-backed blur was tried and reverted (TASKS.md
    // Phase 5): JUCE's NSViewComponent attaches a native view via plain Cocoa
    // `addSubview:` on the window's single shared peer view, so the native layer always
    // renders in front of *everything* JUCE paints in that window, not behind it —
    // there's no way to interleave real native content with JUCE's own rendering short
    // of restructuring the window so the effect view IS the NSWindow's contentView and
    // JUCE renders as a transparent subview on top of it (real native window surgery,
    // parked for later — NativeBlur.h/.mm are kept, unused, as a starting point for that).
    // This approximates the same visual language with pure JUCE painting instead: a
    // vertical gradient between two close panel shades plus a soft off-center radial
    // highlight (a fixed, page-relative light source, not tied to any one panel), and a
    // 1px near-white top edge at low alpha — the mockup's own
    // `0 2px 0 rgba(255,255,255,0.02) inset` box-shadow rule, ported directly.
    // curveTopLeft/etc let a caller round only some corners (MainComponent's outer
    // paint rounds just its bottom two, since the top two are the title bar's job) --
    // default true/true/true/true for every other call site, which keeps their existing
    // "either square or uniformly rounded" behaviour unchanged.
    static void paintGlassPanel(juce::Graphics&, juce::Rectangle<int> bounds, float cornerRadius, juce::Colour base,
                                 bool curveTopLeft = true, bool curveTopRight = true, bool curveBottomLeft = true,
                                 bool curveBottomRight = true);

    // "the whole of mira window is very straight edges cant we do curves" — buttons
    // already round (drawButtonBackground above), tab chips already round
    // (TabChip::paint, Main.cpp); text fields were the one un-rounded surface left
    // (default LookAndFeel_V4 draws a plain rectangle), used throughout the rename/new-
    // group prompts and every editable field in FileDetailsWindow.
    void fillTextEditorBackground(juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline(juce::Graphics&, int width, int height, juce::TextEditor&) override;

    // "the lora dropdown is too big and lot of spacing" -- JUCE's default popup row is
    // sized from the menu font plus generous padding, which on a list of 21 checkpoints
    // filled the screen. A menu is a list to scan, not a set of buttons to aim at.
    void getIdealPopupMenuItemSize(const juce::String& text, bool isSeparator, int standardMenuItemHeight,
                                    int& idealWidth, int& idealHeight) override;
    juce::Font getPopupMenuFont() override;

    // "the sliders ... the track of the slider" -- V4 draws the unfilled part of a
    // LinearHorizontal track in a colour that all but vanished on mira's dark surface, so
    // a slider read as a dot floating in nothing with no sense of its range.
    void drawLinearSlider(juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

private:
    juce::Typeface::Ptr sansRegularTypeface, sansMediumTypeface, sansSemiBoldTypeface;
    juce::Typeface::Ptr monoRegularTypeface, monoMediumTypeface;
};
