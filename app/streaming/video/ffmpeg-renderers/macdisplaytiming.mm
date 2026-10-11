#include "macdisplaytiming.h"

#include <SDL.h>
#include <SDL_syswm.h>
#import <AppKit/AppKit.h>

namespace {
MacDisplayTiming timingForScreen(NSScreen* screen)
{
    if (@available(macOS 12.0, *)) {
        if (screen == nullptr) {
            return {};
        }
        MacDisplayTiming timing;
        timing.maximumFramesPerSecond = static_cast<int>(screen.maximumFramesPerSecond);
        timing.minimumRefreshInterval = screen.minimumRefreshInterval;
        timing.maximumRefreshInterval = screen.maximumRefreshInterval;
        timing.displayUpdateGranularity = screen.displayUpdateGranularity;
        timing.displayId = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
        return timing.hasValidTiming() ? timing : MacDisplayTiming{};
    }
    return {};
}
}

MacDisplayTiming queryMacDisplayTiming(SDL_Window* window)
{ @autoreleasepool {
    if (window == nullptr || ![NSThread isMainThread]) {
        return {};
    }
    if (@available(macOS 12.0, *)) {
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_COCOA) {
            return {};
        }
        auto timing = timingForScreen(info.info.cocoa.window.screen);
        timing.nativeFullscreen = (info.info.cocoa.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
        return timing;
    }
    return {};
}}

MacDisplayTiming queryMacDisplayTimingForDisplay(int displayIndex)
{ @autoreleasepool {
    if (![NSThread isMainThread]) {
        return {};
    }
    SDL_Rect bounds;
    if (SDL_GetDisplayBounds(displayIndex, &bounds) != 0) {
        return {};
    }
    for (NSScreen* screen in NSScreen.screens) {
        NSNumber* displayNumber = screen.deviceDescription[@"NSScreenNumber"];
        const CGRect cgBounds = CGDisplayBounds(displayNumber.unsignedIntValue);
        if (std::lround(cgBounds.origin.x) == bounds.x &&
                std::lround(cgBounds.origin.y) == bounds.y &&
                std::lround(cgBounds.size.width) == bounds.w &&
                std::lround(cgBounds.size.height) == bounds.h) {
            return timingForScreen(screen);
        }
    }
    return {};
}}
