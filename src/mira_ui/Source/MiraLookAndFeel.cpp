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

juce::Font MiraLookAndFeel::getLabelFont(juce::Label&)
{
    // JUCE only calls this for a Label that hasn't had setFont() called on it explicitly
    // — mockup's body font-size is 13px sans-regular, the default for anything that
    // doesn't ask for the mono/medium/semibold variants directly.
    return sansRegular(13.0f);
}
