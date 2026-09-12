#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "FileTable.h"
#include "MiraLookAndFeel.h"

// The split filter bar (review round 2: "filters should have division, not one box --
// like key, tempo, mood, instruments, genre; right now the filters are difficult to
// manoeuvre"). Replaces the single free-text FilterBarComponent.
//
// Dropdowns rather than free text for the categorical fields, filled only with values
// that actually occur in the current folder, files and segments both
// (FileTableModel::getFacetOptions). A list of all 400 Discogs genres would be the same
// "difficult to manoeuvre" problem in a different shape; the six genres this folder
// really has is one click. The search box stays for names and anything the dropdowns
// don't cover. Every field combines with AND.
class FilterBar : public juce::Component
{
public:
    explicit FilterBar(const MiraLookAndFeel& lafIn) : laf(lafIn)
    {
        styleEditor(searchEditor, juce::String(juce::CharPointer_UTF8("Search names and tags\xe2\x80\xa6")));
        styleEditor(bpmMinEditor, "min");
        styleEditor(bpmMaxEditor, "max");
        for (auto* e : { &bpmMinEditor, &bpmMaxEditor })
        {
            e->setInputRestrictions(5, "0123456789.");
            e->setJustification(juce::Justification::centred);
        }

        styleCombo(keyBox, "Any key");
        styleCombo(genreBox, "Any genre");
        styleCombo(instrumentBox, "Any instrument");
        styleCombo(moodBox, "Any mood");

        bpmLabel.setText("BPM", juce::dontSendNotification);
        bpmDash.setText(juce::String(juce::CharPointer_UTF8("\xe2\x80\x93")), juce::dontSendNotification);
        for (auto* l : { &bpmLabel, &bpmDash })
        {
            l->setFont(laf.monoRegular(11.5f));
            l->setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
            l->setJustificationType(juce::Justification::centred);
            addAndMakeVisible(l);
        }

        clearButton.setColour(juce::TextButton::buttonColourId, MiraLookAndFeel::surface3);
        clearButton.setColour(juce::TextButton::textColourOffId, MiraLookAndFeel::textDim);
        clearButton.onClick = [this] { clearAll(); };
        clearButton.setEnabled(false);
        addAndMakeVisible(clearButton);

        countLabel.setFont(laf.monoRegular(12.0f));
        countLabel.setColour(juce::Label::textColourId, MiraLookAndFeel::textFaint);
        countLabel.setJustificationType(juce::Justification::centredRight);
        addAndMakeVisible(countLabel);
    }

    std::function<void(FilterCriteria)> onCriteriaChanged;

    // "3 of 412" while filtering, a plain count otherwise -- a filter that matched
    // nothing has to read as a filter result, not an empty folder.
    void setCounts(int shown, int total)
    {
        countLabel.setText(shown == total ? juce::String(total) + " files"
                                           : juce::String(shown) + " of " + juce::String(total),
                            juce::dontSendNotification);
    }

    // Called whenever the list's scope is rebuilt. A choice the new folder also has is
    // kept; one it doesn't have falls back to "Any" and re-filters. Otherwise switching
    // folders would carry an invisible filter over from the last one and silently empty
    // the list.
    void setFacetOptions(const FacetOptions& options)
    {
        bool changed = false;
        changed |= refill(keyBox, options.keys);
        changed |= refill(genreBox, options.genres);
        changed |= refill(instrumentBox, options.instruments);
        changed |= refill(moodBox, options.moods);
        if (changed) notify();
    }

    void focusEditor() { searchEditor.grabKeyboardFocus(); }

    void paint(juce::Graphics& g) override
    {
        MiraLookAndFeel::paintGlassPanel(g, getLocalBounds(), 0.0f, MiraLookAndFeel::surface);
    }

    // Right-to-left: the fixed-width controls claim their space first and the search box
    // takes whatever's left, so a narrow window squeezes free text, not the dropdowns.
    void resized() override
    {
        auto bounds = getLocalBounds().reduced(10, 6);
        constexpr int gap = 6;
        auto takeRight = [&bounds](juce::Component& c, int width, int gapAfter) {
            c.setBounds(bounds.removeFromRight(width));
            bounds.removeFromRight(gapAfter);
        };

        takeRight(countLabel, 84, gap);
        takeRight(clearButton, 56, gap * 2);
        takeRight(moodBox, 130, gap);
        takeRight(instrumentBox, 150, gap);
        takeRight(genreBox, 170, gap * 2);
        takeRight(bpmMaxEditor, 50, 0);
        takeRight(bpmDash, 14, 0);
        takeRight(bpmMinEditor, 50, 0);
        takeRight(bpmLabel, 34, gap * 2);
        takeRight(keyBox, 110, gap * 2);
        searchEditor.setBounds(bounds);
    }

private:
    void styleEditor(juce::TextEditor& editor, const juce::String& placeholder)
    {
        editor.setFont(laf.sansRegular(13.0f));
        editor.setTextToShowWhenEmpty(placeholder, MiraLookAndFeel::textFaint);
        editor.setColour(juce::TextEditor::backgroundColourId, MiraLookAndFeel::surface2);
        // Live, per-keystroke: filtering is a pass over already-built rows
        // (FileTableModel::setFilterCriteria), so there's no reason to make someone press
        // Return to find out whether a query matched anything.
        editor.onTextChange = [this] { notify(); };
        editor.onEscapeKey = [&editor] { editor.setText({}, juce::sendNotification); };
        addAndMakeVisible(editor);
    }

    // Item id 1 is always the "Any ..." entry; real values start at id 2.
    void styleCombo(juce::ComboBox& box, const juce::String& anyLabel)
    {
        box.setTextWhenNothingSelected(anyLabel);
        box.addItem(anyLabel, 1);
        box.setSelectedId(1, juce::dontSendNotification);
        box.onChange = [this] { notify(); };
        addAndMakeVisible(box);
    }

    // Returns true when a previous choice had to be dropped (see setFacetOptions).
    static bool refill(juce::ComboBox& box, const juce::StringArray& values)
    {
        auto anyLabel = box.getTextWhenNothingSelected();
        auto current = box.getSelectedId() > 1 ? box.getText() : juce::String();
        box.clear(juce::dontSendNotification);
        box.addItem(anyLabel, 1);
        for (int i = 0; i < values.size(); ++i) box.addItem(values[i], i + 2);
        auto index = current.isEmpty() ? -1 : values.indexOf(current, true);
        box.setSelectedId(index >= 0 ? index + 2 : 1, juce::dontSendNotification);
        return current.isNotEmpty() && index < 0;
    }

    FilterCriteria criteria() const
    {
        FilterCriteria c;
        c.text = searchEditor.getText();
        auto chosen = [](const juce::ComboBox& box) { return box.getSelectedId() > 1 ? box.getText() : juce::String(); };
        c.key = chosen(keyBox);
        c.genre = chosen(genreBox);
        c.instrument = chosen(instrumentBox);
        c.mood = chosen(moodBox);
        if (auto t = bpmMinEditor.getText().trim(); t.isNotEmpty()) c.bpmMin = t.getDoubleValue();
        if (auto t = bpmMaxEditor.getText().trim(); t.isNotEmpty()) c.bpmMax = t.getDoubleValue();
        return c;
    }

    void notify()
    {
        auto c = criteria();
        clearButton.setEnabled(!c.isEmpty());
        if (onCriteriaChanged) onCriteriaChanged(c);
    }

    void clearAll()
    {
        for (auto* e : { &searchEditor, &bpmMinEditor, &bpmMaxEditor }) e->setText({}, juce::dontSendNotification);
        for (auto* b : { &keyBox, &genreBox, &instrumentBox, &moodBox }) b->setSelectedId(1, juce::dontSendNotification);
        notify(); // once, after everything is reset -- not once per cleared field
    }

    const MiraLookAndFeel& laf;
    juce::TextEditor searchEditor, bpmMinEditor, bpmMaxEditor;
    juce::ComboBox keyBox, genreBox, instrumentBox, moodBox;
    juce::Label bpmLabel, bpmDash, countLabel;
    juce::TextButton clearButton { "Clear" };
};
