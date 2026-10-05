#pragma once

#include <cmath>

struct SDL_Window;

// NSScreen reports the actual supported presentation range. SDL's current
// display mode can report zero Hz for ProMotion, so it is not a reliable
// adaptive-display maximum on macOS.
struct MacDisplayTiming {
    int maximumFramesPerSecond = 0;
    double minimumRefreshInterval = 0;
    double maximumRefreshInterval = 0;
    double displayUpdateGranularity = 0;
    unsigned int displayId = 0;
    bool nativeFullscreen = false;

    bool hasValidTiming() const
    {
        return maximumFramesPerSecond > 0 &&
            std::isfinite(minimumRefreshInterval) && minimumRefreshInterval > 0 &&
            std::isfinite(maximumRefreshInterval) &&
            maximumRefreshInterval >= minimumRefreshInterval &&
            std::isfinite(displayUpdateGranularity) && displayUpdateGranularity >= 0;
    }

    bool supportsVariableRefresh() const
    {
        // Both continuous Adaptive-Sync and discrete ProMotion ranges qualify.
        // Fixed-refresh screens report equal minimum and maximum intervals.
        return hasValidTiming() &&
            maximumRefreshInterval - minimumRefreshInterval > 0.000001;
    }
};

// Query on the main thread, where the SDL Cocoa window and NSScreen live.
// Returns an empty result on macOS before 12 or when the window has no screen.
MacDisplayTiming queryMacDisplayTiming(SDL_Window* window);

// Resolve the SDL display's desktop bounds against CoreGraphics display IDs,
// then query its NSScreen. This avoids assuming SDL and NSScreen enumerate
// displays in the same order. Also main-thread only.
MacDisplayTiming queryMacDisplayTimingForDisplay(int displayIndex);
