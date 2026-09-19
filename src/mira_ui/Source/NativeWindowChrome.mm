#include "NativeWindowChrome.h"

#import <Cocoa/Cocoa.h>

#include <cmath>

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

int useFullSizeContentView(juce::Component& windowComponent)
{
    NSWindow* window = nativeWindowFor(windowComponent);
    if (window == nil) return 0;

    window.styleMask |= NSWindowStyleMaskFullSizeContentView;
    window.titlebarAppearsTransparent = YES;
    // The title text would otherwise sit on top of the toolbar row, centred, over whatever
    // button happens to be in the middle.
    window.titleVisibility = NSWindowTitleHidden;

    const CGFloat full = window.frame.size.height;
    const CGFloat content = [window contentRectForFrameRect:window.frame].size.height;
    const int bar = (int) std::lround(full - content);
    // With FullSizeContentView the content rect IS the frame, so the difference is zero and
    // the real bar height has to come from the button instead.
    if (bar > 0) return bar;
    if (NSButton* close = [window standardWindowButton:NSWindowCloseButton])
        return (int) std::lround(close.superview.frame.size.height);
    return 28;
}

int trafficLightInset(juce::Component& windowComponent)
{
    NSWindow* window = nativeWindowFor(windowComponent);
    if (window == nil) return 0;
    NSButton* zoom = [window standardWindowButton:NSWindowZoomButton];
    if (zoom == nil) return 78;
    const NSRect r = [zoom convertRect:zoom.bounds toView:nil];
    return (int) std::lround(NSMaxX(r)) + 12;
}

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
