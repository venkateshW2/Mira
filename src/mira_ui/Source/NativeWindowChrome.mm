#include "NativeWindowChrome.h"

#import <Cocoa/Cocoa.h>

namespace mira_ui::chrome
{
namespace {

// A JUCE peer's native handle on macOS is the peer NSView, not the NSWindow -- the
// window is reached through it. Null until the component is actually on screen
// (addToDesktop / setVisible), which is why both entry points tolerate a missing peer
// instead of asserting.
NSWindow* nativeWindowFor(juce::Component& component)
{
    auto* peer = component.getPeer();
    if (peer == nullptr) return nil;
    auto* view = (__bridge NSView*) peer->getNativeHandle();
    return view != nil ? [view window] : nil;
}

NSColor* toNSColor(juce::Colour colour)
{
    return [NSColor colorWithSRGBRed:colour.getFloatRed()
                                green:colour.getFloatGreen()
                                 blue:colour.getFloatBlue()
                                alpha:colour.getFloatAlpha()];
}

} // namespace

void applyDarkTitleBar(juce::Component& windowComponent, juce::Colour background)
{
    NSWindow* window = nativeWindowFor(windowComponent);
    if (window == nil) return;

    window.titlebarAppearsTransparent = YES;
    window.backgroundColor = toNSColor(background);
    // Explicit rather than inherited: mira's palette is dark whatever the system is set
    // to, so a light-appearance title bar over it would render dark-on-dark text.
    window.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
}

void applyRoundedCorners(juce::Component& windowComponent, float cornerRadius)
{
    NSWindow* window = nativeWindowFor(windowComponent);
    if (window == nil) return;

    // The window must stop painting its own square opaque background, or it would show
    // in the corners the content layer has clipped away.
    window.opaque = NO;
    window.backgroundColor = [NSColor clearColor];

    NSView* content = window.contentView;
    if (content == nil) return;
    content.wantsLayer = YES;
    content.layer.cornerRadius = cornerRadius;
    content.layer.masksToBounds = YES;

    // AppKit caches the shadow from the window's shape as it was when first shown, which
    // was still the square opaque one. Without this the shadow keeps a square outline
    // around the now-rounded window, part of the "not clean edges" in review round 2.
    [window invalidateShadow];
}

} // namespace mira_ui::chrome
