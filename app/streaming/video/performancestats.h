#pragma once

#include "ffmpeg-renderers/pacer/vrr/inputpercentilebuffer.h"
#include "ffmpeg-renderers/pacer/vrr/presenttiming.h"
#include <algorithm>
#include <cstdio>
#include <string>

// Presentation only: these values never become controller feedback.
// The normal overlay keeps Moonlight's original stats and appends this short
// VRR summary; the full breakdown remains in the session log.
namespace PerformanceStats {
struct Snapshot {
    bool vrr = false, adaptive = false, atLimit = false;
    uint64_t bufferUs = 0;
    uint64_t targetPpm = 0, toleranceUs = 0;
    uint64_t nowUs = 0;
    Vrr13::InputPercentileBuffer::Stats input;
    // Without the adaptive buffer: the interval-policy client timing score.
    bool clientTimingValid = false;
    double clientTimingPercent = 0;
    // Display error added after submission, judged against the plan. Source
    // timestamp jitter is the host's cadence, not a display error.
    Vrr13::PresentTiming::Stats present;
};

inline bool fresh(uint64_t atUs, uint64_t nowUs)
{
    return atUs && nowUs >= atUs && nowUs - atUs <= 1000000;
}

inline std::string formatVrr(const Snapshot& s)
{
    if (!s.vrr) return "VRR pacing: inactive\n";

    char buffer[96], coverage[96], present[128], text[384];
    if (s.adaptive && s.input.samples)
        std::snprintf(buffer, sizeof(buffer), "%.2f ms%s (target %.2f ms%s)",
            s.bufferUs / 1000.0, s.atLimit ? " at limit" : "", s.input.targetUs / 1000.0,
            s.input.tailUncertain ? " learning" : "");
    else
        std::snprintf(buffer, sizeof(buffer), "%.2f ms%s", s.bufferUs / 1000.0, s.atLimit ? " at limit" : "");

    // Each current value shares a line with its target. Coverage counts clean
    // plus absorbed inputs over the profile history.
    const double target = s.targetPpm / 10000.0;
    if (s.adaptive) {
        if (s.input.recentEligible) {
            const auto covered = s.input.recentEligible - std::min(s.input.recentMissed, s.input.recentEligible);
            std::snprintf(coverage, sizeof(coverage), "coverage %.2f%% / %.2f%% target",
                100.0 * double(covered) / double(s.input.recentEligible), target);
        }
        else std::snprintf(coverage, sizeof(coverage), "coverage N/A / %.2f%% target", target);
    }
    else if (s.clientTimingValid)
        std::snprintf(coverage, sizeof(coverage), "client timing %.2f%% / %.2f%% target",
            s.clientTimingPercent, target);
    else std::snprintf(coverage, sizeof(coverage), "client timing collecting / %.2f%% target", target);

    if (s.present.intervals && fresh(s.present.lastObservedUs, s.nowUs))
        std::snprintf(present, sizeof(present), "%.2f%% >%.2f ms | hitches %llu | worst +%.1f ms",
            s.present.issuePercent(), s.toleranceUs / 1000.0,
            static_cast<unsigned long long>(s.present.hitches), s.present.worstAddedUs / 1000.0);
    else if (fresh(s.present.lastPausedUs, s.nowUs))
        std::snprintf(present, sizeof(present), "paused below VRR range");
    else std::snprintf(present, sizeof(present), "N/A (no recent display feedback)");

    std::snprintf(text, sizeof(text),
        "VRR buffer: %s | %s\n"
        "Present timing issues (30s): %s\n",
        buffer, coverage, present);
    return text;
}
}
