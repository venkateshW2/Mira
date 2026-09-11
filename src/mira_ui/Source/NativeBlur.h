#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

// TASKS.md Phase 5 planning session's visual-direction decision: real native
// translucency via NSVisualEffectView (macOS 10.10+), not literally Apple's newest
// "Liquid Glass" material — that session's own caveat is that Liquid Glass's exact
// current API is unverified for hosting arbitrary custom-painted content, so
// NSVisualEffectView is the pragmatic, available choice that gets most of the visual
// language. JUCE has no built-in wrapper for it, so this is real Objective-C++ bridge
// code (NativeBlur.mm) hosting it as a native NSView sitting *behind* JUCE's own
// rendering, via juce::NSViewComponent — the same category of native-Cocoa integration
// spike/03_dragout already proved out for drag-out (shouldDropFilesWhenDraggedExternally
// is also Cocoa-bridge code under the hood), not a new kind of risk.
//
// Usage: add one of these as the furthest-back child of a window's content component
// (added/toBack()'d before anything else), sized to fill it. Every JUCE panel painted
// on top must use a semi-transparent fill (not an opaque one) wherever the blur should
// show through — an opaque JUCE-painted rectangle still fully hides the native view
// underneath it, translucency has to be deliberate at every layer, not automatic.
class NativeBlurBackground : public juce::NSViewComponent
{
public:
    NativeBlurBackground();
    ~NativeBlurBackground() override;
};
