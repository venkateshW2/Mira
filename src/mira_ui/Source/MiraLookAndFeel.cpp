#include "MiraLookAndFeel.h"
#include "BinaryData.h"

const juce::Colour MiraLookAndFeel::bg = juce::Colour(0xff14161b);
const juce::Colour MiraLookAndFeel::surface = juce::Colour(0xff1a1d24);
const juce::Colour MiraLookAndFeel::surface2 = juce::Colour(0xff20242d);
const juce::Colour MiraLookAndFeel::surface3 = juce::Colour(0xff262b36);
const juce::Colour MiraLookAndFeel::border = juce::Colour(0xff2a2f3b);
const juce::Colour MiraLookAndFeel::borderSoft = juce::Colour(0xff22262f);
const juce::Colour MiraLookAndFeel::text = juce::Colour(0xffe8eaf0);
const juce::Colour MiraLookAndFeel::textDim = juce::Colour(0xff8992a6);
const juce::Colour MiraLookAndFeel::textFaint = juce::Colour(0xff545c6e);
const juce::Colour MiraLookAndFeel::accent = juce::Colour(0xffe8a33d);
const juce::Colour MiraLookAndFeel::accentSoft = juce::Colour(0xffe8a33d).withAlpha(0.14f);
const juce::Colour MiraLookAndFeel::active = juce::Colour(0xff4fb8ae);
const juce::Colour MiraLookAndFeel::activeSoft = juce::Colour(0xff4fb8ae).withAlpha(0.16f);
const juce::Colour MiraLookAndFeel::warn = juce::Colour(0xffe2574c);
const juce::Colour MiraLookAndFeel::warnSoft = juce::Colour(0xffe2574c).withAlpha(0.14f);
const juce::Colour MiraLookAndFeel::good = juce::Colour(0xff6fbf73);
const juce::Colour MiraLookAndFeel::goodSoft = juce::Colour(0xff6fbf73).withAlpha(0.14f);

MiraLookAndFeel::MiraLookAndFeel()
{
    sansRegularTypeface = juce::Typeface::createSystemTypefaceFor(
        BinaryData::IBMPlexSansRegular_ttf, static_cast<size_t>(BinaryData::IBMPlexSansRegular_ttfSize));
    sansMediumTypeface = juce::Typeface::createSystemTypefaceFor(
        BinaryData::IBMPlexSansMedium_ttf, static_cast<size_t>(BinaryData::IBMPlexSansMedium_ttfSize));
    sansSemiBoldTypeface = juce::Typeface::createSystemTypefaceFor(
        BinaryData::IBMPlexSansSemiBold_ttf, static_cast<size_t>(BinaryData::IBMPlexSansSemiBold_ttfSize));
    monoRegularTypeface = juce::Typeface::createSystemTypefaceFor(
        BinaryData::IBMPlexMonoRegular_ttf, static_cast<size_t>(BinaryData::IBMPlexMonoRegular_ttfSize));
    monoMediumTypeface = juce::Typeface::createSystemTypefaceFor(
        BinaryData::IBMPlexMonoMedium_ttf, static_cast<size_t>(BinaryData::IBMPlexMonoMedium_ttfSize));

    // Window/base chrome — the mockup's --surface/--surface-2/--text/--border, applied to
    // the handful of standard JUCE components step 1 already uses (Label, ResizableWindow,
    // ScrollBar) so this step's palette is actually visible, not just declared. Per-panel
    // painting (the table, waveform, pills, etc.) is later build-order steps, not this one.
    setColour(juce::ResizableWindow::backgroundColourId, surface);
    setColour(juce::DocumentWindow::backgroundColourId, surface);
    setColour(juce::Label::textColourId, text);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::ScrollBar::thumbColourId, textFaint);
    setColour(juce::ScrollBar::trackColourId, surface3);
    setColour(juce::TextButton::buttonColourId, surface3);
    setColour(juce::TextButton::textColourOffId, text);

    // Mockup's .filelist thead th rule: --surface background, --text-faint text.
    setColour(juce::TableHeaderComponent::backgroundColourId, surface2);
    setColour(juce::TableHeaderComponent::textColourId, textFaint);
    setColour(juce::TableHeaderComponent::outlineColourId, border);
    setColour(juce::TableHeaderComponent::highlightColourId, surface3);
    setColour(juce::ListBox::backgroundColourId, surface); // TableListBox extends ListBox

    // PopupMenu (folder tree's +/right-click menus) and AlertWindow/TextEditor (the
    // rename/new-group prompts) had no overrides at all until now, so they fell through
    // to LookAndFeel_V4's own default light menu scheme — "the right click is not the
    // same ui palette and colors", confirmed by screenshot: a plain white popup menu
    // sitting on top of the rest of the dark-themed app.
    setColour(juce::PopupMenu::backgroundColourId, surface2);
    setColour(juce::PopupMenu::textColourId, text);
    setColour(juce::PopupMenu::headerTextColourId, textFaint);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, accentSoft);
    setColour(juce::PopupMenu::highlightedTextColourId, text);
    setColour(juce::AlertWindow::backgroundColourId, surface2);
    setColour(juce::AlertWindow::textColourId, text);
    setColour(juce::AlertWindow::outlineColourId, border);
    setColour(juce::TextEditor::backgroundColourId, surface3);
    setColour(juce::TextEditor::textColourId, text);
    setColour(juce::TextEditor::outlineColourId, border);
    setColour(juce::TextEditor::focusedOutlineColourId, accent);
    setColour(juce::TextEditor::highlightColourId, accentSoft);
    setColour(juce::TextEditor::highlightedTextColourId, text);

    // The split filter bar's dropdowns (review round 2) are the first ComboBoxes in the
    // app. Without these they'd fall back to V4's defaults, the same "not the same
    // palette" problem PopupMenu had above. Their drop-down list is a PopupMenu, so it's
    // already covered.
    setColour(juce::ComboBox::backgroundColourId, surface2);
    setColour(juce::ComboBox::textColourId, text);
    setColour(juce::ComboBox::outlineColourId, border);
    setColour(juce::ComboBox::buttonColourId, surface3);
    setColour(juce::ComboBox::arrowColourId, textDim);
    setColour(juce::ComboBox::focusedOutlineColourId, accent);
}

juce::Font MiraLookAndFeel::sansRegular(float height) const
{
    return juce::Font(juce::FontOptions(height).withTypeface(sansRegularTypeface));
}

juce::Font MiraLookAndFeel::sansMedium(float height) const
{
    return juce::Font(juce::FontOptions(height).withTypeface(sansMediumTypeface));
}

juce::Font MiraLookAndFeel::sansSemiBold(float height) const
{
    return juce::Font(juce::FontOptions(height).withTypeface(sansSemiBoldTypeface));
}

juce::Font MiraLookAndFeel::monoRegular(float height) const
{
    return juce::Font(juce::FontOptions(height).withTypeface(monoRegularTypeface));
}

juce::Font MiraLookAndFeel::monoMedium(float height) const
{
    return juce::Font(juce::FontOptions(height).withTypeface(monoMediumTypeface));
}

void MiraLookAndFeel::paintGlassPanel(juce::Graphics& g, juce::Rectangle<int> bounds, float cornerRadius,
                                       juce::Colour base, bool curveTopLeft, bool curveTopRight,
                                       bool curveBottomLeft, bool curveBottomRight)
{
    auto fb = bounds.toFloat();
    bool allCorners = curveTopLeft && curveTopRight && curveBottomLeft && curveBottomRight;

    // Building one Path and reusing it for both fills (instead of fillRoundedRectangle
    // twice) is what makes the per-corner case possible at all -- fillRoundedRectangle
    // itself has no way to curve fewer than all four corners.
    juce::Path shape;
    if (cornerRadius > 0.0f && !allCorners)
        shape.addRoundedRectangle(fb.getX(), fb.getY(), fb.getWidth(), fb.getHeight(), cornerRadius, cornerRadius,
                                   curveTopLeft, curveTopRight, curveBottomLeft, curveBottomRight);
    else if (cornerRadius > 0.0f)
        shape.addRoundedRectangle(fb, cornerRadius);
    else
        shape.addRectangle(fb);

    juce::ColourGradient grad(base.brighter(0.015f), fb.getX(), fb.getY(), base.darker(0.05f), fb.getX(),
                               fb.getBottom(), false);
    g.setGradientFill(grad);
    g.fillPath(shape);

    // Fixed, page-relative light source (not per-panel — this is the "one shared glass
    // sheet" illusion, so every panel's highlight agrees on where the light is coming
    // from) — a soft radial glow, off-center toward the top-left, matching the mockup's
    // own subtle top-left specular hint in its box-shadow inset.
    juce::ColourGradient glow(juce::Colours::white.withAlpha(0.05f), fb.getX() + fb.getWidth() * 0.18f,
                               fb.getY() + fb.getHeight() * 0.05f, juce::Colours::transparentWhite,
                               fb.getX() + fb.getWidth() * 0.18f, fb.getY() + fb.getHeight() * 0.7f, true);
    g.setGradientFill(glow);
    g.fillPath(shape);

    // Mockup's `0 2px 0 rgba(255,255,255,0.02) inset` — a hairline top edge, the one
    // concrete "this is a lit glass surface, not a flat fill" cue.
    g.setColour(juce::Colours::white.withAlpha(0.05f));
    auto topRadius = curveTopLeft || curveTopRight ? cornerRadius : 0.0f;
    g.drawLine(fb.getX() + topRadius, fb.getY() + 0.5f, fb.getRight() - topRadius, fb.getY() + 0.5f, 1.0f);
}

void MiraLookAndFeel::fillTextEditorBackground(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    g.setColour(editor.findColour(juce::TextEditor::backgroundColourId));
    g.fillRoundedRectangle(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 6.0f);
}

void MiraLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (editor.isEnabled())
    {
        auto colourId = editor.hasKeyboardFocus(true) ? juce::TextEditor::focusedOutlineColourId
                                                        : juce::TextEditor::outlineColourId;
        g.setColour(editor.findColour(colourId));
        g.drawRoundedRectangle(0.5f, 0.5f, static_cast<float>(width) - 1.0f, static_cast<float>(height) - 1.0f, 6.0f,
                                1.0f);
    }
}

namespace {
class TitleBarDotButton : public juce::Button
{
public:
    TitleBarDotButton(const juce::String& name, juce::Colour colourIn) : juce::Button(name), dotColour(colourIn) {}

    void paintButton(juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = getLocalBounds().toFloat();
        auto diameter = juce::jmin(bounds.getWidth(), bounds.getHeight()) * 0.42f;
        auto circle = juce::Rectangle<float>(diameter, diameter).withCentre(bounds.getCentre());
        auto c = dotColour;
        if (shouldDrawButtonAsDown) c = c.darker(0.2f);
        else if (shouldDrawButtonAsHighlighted) c = c.brighter(0.6f); // affordance on hover, mockup's dots are otherwise flat/inert
        g.setColour(c);
        g.fillEllipse(circle);
    }

private:
    juce::Colour dotColour;
};
} // namespace

juce::Button* MiraLookAndFeel::createDocumentWindowButton(int buttonType)
{
    if (buttonType == juce::DocumentWindow::closeButton) return new TitleBarDotButton("close", juce::Colour(0xff4a2f30));
    if (buttonType == juce::DocumentWindow::minimiseButton) return new TitleBarDotButton("minimise", juce::Colour(0xff4a4330));
    if (buttonType == juce::DocumentWindow::maximiseButton) return new TitleBarDotButton("maximise", juce::Colour(0xff2f4a33));
    jassertfalse;
    return nullptr;
}

void MiraLookAndFeel::drawDocumentWindowTitleBar(juce::DocumentWindow& window, juce::Graphics& g, int w, int h,
                                                  int titleSpaceX, int titleSpaceW, const juce::Image*,
                                                  bool drawTitleTextOnLeft)
{
    if (w * h == 0) return;

    g.setColour(surface2);
    g.fillRect(0, 0, w, h);
    g.setColour(border);
    g.drawLine(0.0f, static_cast<float>(h - 1), static_cast<float>(w), static_cast<float>(h - 1), 1.0f);

    // Centred on the *whole* bar, not on titleSpaceX/titleSpaceW (review round 2: "not in
    // the center and it changes when resized"). JUCE's title space is whatever is left
    // after the buttons, and with all three traffic lights on the left that strip is
    // lopsided, so centring inside it put the title right of centre by a margin that
    // grew and shrank with the window. The buttons live in the left ~80px, so a centred
    // title only collides at widths far below setResizeLimits' minimum.
    juce::ignoreUnused(titleSpaceX, titleSpaceW, drawTitleTextOnLeft);
    g.setColour(text);
    // SemiBold is the heaviest IBM Plex Sans weight embedded (MiraLookAndFeel.h's font
    // set); uppercase comes from the window name itself (MiraUiApp::getApplicationName).
    g.setFont(sansSemiBold(static_cast<float>(h) * 0.42f));
    g.drawText(window.getName(), 0, 0, w, h, juce::Justification::centred, true);
}

void MiraLookAndFeel::drawResizableWindowBorder(juce::Graphics&, int, int, const juce::BorderSize<int>&,
                                                 juce::ResizableWindow&)
{
}

void MiraLookAndFeel::drawCornerResizer(juce::Graphics&, int, int, bool, bool)
{
}

juce::Font MiraLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return sansMedium(juce::jmin(14.0f, static_cast<float>(buttonHeight) * 0.5f));
}

// Was JUCE's stock LookAndFeel_V4 button (generic flat gray, default system font) —
// "very primitive" against everything else already carrying the mockup's palette/type.
// 6px corner radius matches the mockup's .filter-input/.text-input convention; hover
// brightens, press darkens, same language buttons use everywhere else in this LookAndFeel.
void MiraLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button,
                                            const juce::Colour& backgroundColour,
                                            bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown)
{
    auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    auto fill = backgroundColour;
    if (shouldDrawButtonAsDown) fill = fill.darker(0.15f);
    else if (shouldDrawButtonAsHighlighted) fill = fill.brighter(0.1f);
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, 6.0f);
}

juce::Font MiraLookAndFeel::getLabelFont(juce::Label&)
{
    // JUCE only calls this for a Label that hasn't had setFont() called on it explicitly
    // — mockup's body font-size is 13px sans-regular, the default for anything that
    // doesn't ask for the mono/medium/semibold variants directly.
    return sansRegular(13.0f);
}
