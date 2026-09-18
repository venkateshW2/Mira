#pragma once

#include <juce_gui_extra/juce_gui_extra.h>
#include <functional>

#include "MiraLookAndFeel.h"

// The menu-bar glyph, and mira's way back in once every window is closed.
//
// It exists because the app no longer quits when its last window does. On macOS that is
// ordinary behaviour -- a document app with no documents open is still running -- but it
// leaves a question the app has to answer: how do you get back? The Dock icon is the
// conventional answer and still works, but JUCE does not expose the reopen callback
// reliably, and an app you cannot reach is worse than one that quits. This is the
// guaranteed route, and it is the one the user asked for.
//
// It carries the same commands the File menu does, because the File menu is unreachable
// with no window open -- on macOS the menu bar belongs to the front window's app, and
// with nothing in front there is nothing to drop down.
class MiraTrayIcon : public juce::SystemTrayIconComponent
{
public:
    std::function<void()> onShowLibrary;
    std::function<void()> onNewProject;
    std::function<void()> onOpenProject;
    std::function<void(juce::PopupMenu&)> buildRecentMenu;   // fills a submenu, ids from 100
    std::function<void(int)> onRecentChosen;
    std::function<void()> onQuit;

    MiraTrayIcon()
    {
        // Two images: the second is the TEMPLATE, which macOS recolours itself for light
        // and dark menu bars and for the highlighted state. Supplying only a coloured
        // icon gives you one that is invisible in one of the two themes.
        setIconImage(makeIcon(false), makeIcon(true));
        setIconTooltip("MIRA");
    }

    void mouseDown(const juce::MouseEvent&) override
    {
        juce::PopupMenu menu;
        menu.addSectionHeader("MIRA");
        menu.addItem(1, "Open Library...");
        menu.addSeparator();
        menu.addItem(2, "New Project...");
        menu.addItem(3, "Open Project...");

        juce::PopupMenu recent;
        if (buildRecentMenu) buildRecentMenu(recent);
        menu.addSubMenu("Recent Projects", recent, recent.containsAnyActiveItems());

        menu.addSeparator();
        menu.addItem(9, "Quit MIRA");

        // showDropdownMenu, not showMenuAsync: this is the call that positions the menu
        // under the status item itself rather than at the mouse, and on macOS it is what
        // makes the glyph highlight while the menu is down.
        menu.showMenuAsync(juce::PopupMenu::Options(), [this](int result) {
            if (result == 0) return;
            if (result == 1 && onShowLibrary)  onShowLibrary();
            else if (result == 2 && onNewProject)  onNewProject();
            else if (result == 3 && onOpenProject) onOpenProject();
            else if (result == 9 && onQuit) onQuit();
            else if (result >= 100 && onRecentChosen) onRecentChosen(result - 100);
        });
    }

private:
    // Drawn rather than shipped as an asset: at 22px a real logo is mud, and the icon has
    // to read as a silhouette because the template version is a mask -- only its alpha
    // survives. Three bars, tallest in the middle: a level meter, which is what mira is.
    static juce::Image makeIcon(bool asTemplate)
    {
        constexpr int size = 22;
        juce::Image img (juce::Image::ARGB, size, size, true);
        juce::Graphics g (img);
        g.setColour(asTemplate ? juce::Colours::black : MiraLookAndFeel::accent);
        const float w = 3.0f, gap = 2.5f;
        const float heights[3] = { 9.0f, 15.0f, 11.0f };
        float x = size * 0.5f - (w * 3 + gap * 2) * 0.5f;
        for (float h : heights)
        {
            g.fillRoundedRectangle(x, (size - h) * 0.5f, w, h, w * 0.5f);
            x += w + gap;
        }
        return img;
    }
};
