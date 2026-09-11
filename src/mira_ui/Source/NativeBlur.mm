#include "NativeBlur.h"

#import <Cocoa/Cocoa.h>

NativeBlurBackground::NativeBlurBackground()
{
    auto* effectView = [[NSVisualEffectView alloc] initWithFrame:NSZeroRect];
    // .underWindowBackground reads as a neutral dark/light material depending on
    // appearance rather than a strong "panel" tint — closest of the available materials
    // to the mockup's near-black ground with a soft blur, not a heavy frosted-glass look.
    effectView.material = NSVisualEffectMaterialUnderWindowBackground;
    effectView.blendingMode = NSVisualEffectBlendingModeBehindWindow;
    effectView.state = NSVisualEffectStateActive;
    effectView.wantsLayer = YES;

    // NSViewComponent::setView retains the view for as long as it's needed (juce_
    // NSViewComponent.h) — the local `effectView` going out of scope under ARC doesn't
    // release mira's only reference to it.
    setView((__bridge void*) effectView);
}

NativeBlurBackground::~NativeBlurBackground()
{
    setView(nullptr);
}
