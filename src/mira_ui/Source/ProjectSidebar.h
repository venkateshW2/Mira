#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

#include "MiraLookAndFeel.h"

// The project's own folder list, so a project window never has to send you to the browser
// to find out what is in it.
//
// It is not a second copy of the library tree. The library is everything you own; this is
// one job's folder, one or two levels deep, and the folders in it already ARE the states
// a take can be in: `takes/` holds what has not been decided, each cue folder holds what
// was kept for that cue, `discarded/` holds what was thrown out. So picking a folder here
// picks a section -- one way to think about where a file is, not two.
class ProjectSidebar : public juce::Component,
                        private juce::ListBoxModel
{
public:
    // Empty File = "All", which is the only entry that is not a real folder.
    std::function<void(juce::File)> onFolderSelected;

    explicit ProjectSidebar(const MiraLookAndFeel& lafIn) : laf(lafIn)
    {
        list.setModel(this);
        list.setRowHeight(26);   // matches the browser tree's row rhythm
        list.setColour(juce::ListBox::backgroundColourId, MiraLookAndFeel::surface2);
        list.setColour(juce::ListBox::outlineColourId, MiraLookAndFeel::border);
        addAndMakeVisible(list);

        heading.setText("PROJECT", juce::dontSendNotification);
        heading.setFont(laf.sansSemiBold(10.5f));
        heading.setColour(juce::Label::textColourId, MiraLookAndFeel::accent.withAlpha(0.85f));
        addAndMakeVisible(heading);
    }

    void setProject(const juce::File& folder)
    {
        project = folder;
        rebuild();
    }

    // Re-read the folders without losing the selection -- called after a Keep creates a
    // cue folder, so the new cue appears without the window being reopened.
    void rebuild()
    {
        const auto previous = selectedFolder();
        entries.clear();
        entries.push_back({ {}, "All takes", 0, 0 });

        if (project.isDirectory())
        {
            // `takes` first whether or not it sorts there: it is where new work lands and
            // it is the folder you are in most of the time.
            auto takes = project.getChildFile("takes");
            if (takes.isDirectory()) entries.push_back({ takes, "takes", countWavs(takes), 0 });

            std::vector<juce::File> cues;
            for (const auto& d : project.findChildFiles(juce::File::findDirectories, false))
            {
                const auto name = d.getFileName();
                if (name == "takes" || name == "discarded" || name.startsWithChar('.')) continue;
                cues.push_back(d);
            }
            std::sort(cues.begin(), cues.end(), [](const juce::File& a, const juce::File& b) {
                return a.getFileName().compareIgnoreCase(b.getFileName()) < 0;
            });
            for (const auto& c : cues)
            {
                entries.push_back({ c, c.getFileName(), countWavs(c), 0 });
                // One level of nesting. Deeper than that is not how a project is laid
                // out, and a full tree here would be the library browser again.
                for (const auto& sub : c.findChildFiles(juce::File::findDirectories, false))
                    if (!sub.getFileName().startsWithChar('.'))
                        entries.push_back({ sub, sub.getFileName(), countWavs(sub), 1 });
            }

            auto discarded = project.getChildFile("discarded");
            if (discarded.isDirectory())
                entries.push_back({ discarded, "discarded", countWavs(discarded), 0 });
        }

        list.updateContent();
        // Keep the user where they were across a rebuild; fall back to All.
        for (size_t i = 0; i < entries.size(); ++i)
            if (entries[i].folder == previous) { list.selectRow(static_cast<int>(i), true, true); return; }
        list.selectRow(0, true, true);
    }

    juce::File selectedFolder() const
    {
        const int row = list.getSelectedRow();
        if (!juce::isPositiveAndBelow(row, static_cast<int>(entries.size()))) return {};
        return entries[static_cast<size_t>(row)].folder;
    }

    void resized() override
    {
        auto r = getLocalBounds();
        heading.setBounds(r.removeFromTop(18).withTrimmedLeft(8));
        r.removeFromTop(2);
        list.setBounds(r);
    }

private:
    struct Entry { juce::File folder; juce::String name; int count = 0; int depth = 0; };

    static int countWavs(const juce::File& d)
    {
        return d.isDirectory() ? d.findChildFiles(juce::File::findFiles, false, "*.wav").size() : 0;
    }

    int getNumRows() override { return static_cast<int>(entries.size()); }

    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override
    {
        if (!juce::isPositiveAndBelow(row, static_cast<int>(entries.size()))) return;
        const auto& e = entries[static_cast<size_t>(row)];
        auto bounds = juce::Rectangle<int>(0, 0, w, h);

        // The browser's selection pill, not a full-bleed bar: this is the same kind of
        // list as the folder tree and it should not look like a different application.
        if (selected)
        {
            g.setColour(MiraLookAndFeel::accentSoft);
            g.fillRoundedRectangle(bounds.reduced(3, 1).toFloat(), 5.0f);
        }

        const float x = 8.0f + e.depth * 12.0f;
        const auto ink = selected ? MiraLookAndFeel::text : MiraLookAndFeel::text.withAlpha(0.85f);

        // Same flat folder glyph the browser draws, for the same reason: shape carries
        // the meaning, one neutral colour throughout. "All takes" is not a folder, so it
        // gets a stack of lines instead -- it is a view, not a place.
        const float iconH = h * 0.42f;
        const float iconY = (h - iconH) * 0.5f;
        g.setColour(MiraLookAndFeel::textDim.withAlpha(0.85f));
        if (e.folder.getFullPathName().isEmpty())
        {
            for (int i = 0; i < 3; ++i)
                g.fillRoundedRectangle(x, iconY + i * (iconH * 0.42f), 14.0f, 2.0f, 1.0f);
        }
        else
        {
            juce::Rectangle<float> body (x, iconY, 15.0f, iconH);
            const float tabW = body.getWidth() * 0.5f, tabH = iconH * 0.28f;
            juce::Path folder;
            folder.addRoundedRectangle(body.getX(), body.getY() - tabH + 1.0f, tabW, tabH,
                                        1.0f, 1.0f, true, true, false, false);
            folder.addRoundedRectangle(body, 2.0f);
            g.fillPath(folder);
        }

        const int textX = static_cast<int>(x) + 23;
        g.setColour(ink);
        g.setFont(laf.sansRegular(13.0f));
        g.drawText(e.name, textX, 0, w - textX - 34, h, juce::Justification::centredLeft, true);

        // The count is the reason to look at this list at all: which cue has versions in
        // it, and how many, without opening anything.
        if (!e.folder.getFullPathName().isEmpty() && e.count > 0)
        {
            g.setColour(MiraLookAndFeel::textFaint);
            g.setFont(laf.monoRegular(11.0f));
            g.drawText(juce::String(e.count), w - 32, 0, 24, h, juce::Justification::centredRight);
        }
    }

    void selectedRowsChanged(int) override { if (onFolderSelected) onFolderSelected(selectedFolder()); }

    const MiraLookAndFeel& laf;
    juce::File project;
    std::vector<Entry> entries;
    juce::ListBox list;
    juce::Label heading;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ProjectSidebar)
};
