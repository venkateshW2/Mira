#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

// Two TASKS.md Phase 5 leftovers that were both parked with the same note -- "real fix
// needs Objective-C++ NSWindow API work (same category as the abandoned NativeBlur.mm
// vibrancy attempt)". This is that work. Both functions reach the window's NSWindow
// through its JUCE peer and adjust the window *itself*, below anything JUCE paints --
// which is exactly why they succeed where the earlier pure-JUCE attempts failed:
//
//  - The reverted custom-painted title bar broke native rounded corners and title
//    centring because it replaced the native title bar. This keeps the native one and
//    only recolours it.
//  - The reverted rounded-corners attempt used JUCE's own windowIsSemiTransparent +
//    setOpaque(false) path and produced a ghost/duplicate outline. This never touches
//    that path: the window stays a normal JUCE window and the rounding is a Core
//    Animation corner radius on its content layer.
//
// Both are macOS-only and no-ops anywhere else; both are safe to call before the peer
// exists (they simply do nothing, and the caller can call again once it does).
namespace mira_ui::chrome
{

// Recolours a *native* title bar to match mira's palette, keeping everything the native
// one gets right for free (real rounded window corners, correctly centred title text
// with an asymmetric single-close-button strip). Done with
// `titlebarAppearsTransparent` plus a window background colour rather than by drawing
// anything: with that flag set, AppKit renders the title bar over the window's own
// background, so there is no second colour to keep in sync and the traffic lights keep
// their real system appearance. The dark appearance is set explicitly so the title text
// and traffic-light glyphs are legible against a dark ground regardless of the user's
// system Light/Dark setting -- mira's palette is dark in both.
void applyDarkTitleBar(juce::Component& windowComponent, juce::Colour background);

// Real rounded outer corners on a window with a *custom* (non-native) title bar, which
// AppKit otherwise leaves square. The window becomes non-opaque with a clear background
// colour and the rounding is a layer corner radius on its content view, so AppKit clips
// and shadows the rounded shape itself -- no JUCE-side transparency, and therefore none
// of the ghost-outline behaviour the earlier attempt hit.
void applyRoundedCorners(juce::Component& windowComponent, float cornerRadius);

} // namespace mira_ui::chrome
