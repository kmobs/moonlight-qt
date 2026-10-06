#pragma once

#include "pyrowavebitrate.h"
#include "pyrowavebandwidth.h"

namespace PyroWaveCalibration {
enum Target { Minimum, Recommended, Moderate, Maximum };
enum Quality { MeetsTarget, ReducedQuality, BelowMinimum };
constexpr int stepKbps = 5000;

inline int roundDown(double kbps) { return int(kbps / stepKbps) * stepKbps; }
inline int roundUp(double kbps) { return int(std::ceil(kbps / stepKbps)) * stepKbps; }

// Minimum and Recommended refer to image quality. Moderate budgets 60% of
// freshly confirmed usable wire bandwidth; Maximum uses the whole budget.
inline int imageTarget(Target target, int recommendedKbps, int imageCapKbps)
{
    const int wanted = target == Minimum ? (std::max)(stepKbps, roundDown(recommendedKbps / 2.0)) :
                       target == Recommended ? recommendedKbps : imageCapKbps;
    return (std::min)(wanted, imageCapKbps);
}

inline int wireTarget(Target target, int confirmedKbps)
{
    return target == Moderate ? roundDown(confirmedKbps * 0.6) : confirmedKbps;
}

// Judge the uncapped quality guide. Capping it to the measured connection
// before comparison would turn every usable link into a green result.
inline Quality imageQuality(Target target, int imageKbps, int recommendedKbps)
{
    const int minimum = imageTarget(Minimum, recommendedKbps, recommendedKbps);
    const int required = target == Minimum ? minimum : recommendedKbps;
    if (imageKbps >= required) return MeetsTarget;
    return imageKbps >= minimum ? ReducedQuality : BelowMinimum;
}

inline int qualityProbeCeiling(int imageKbps, int fps, const pyrowave::bandwidth::transport_t& transport)
{
    // First reserve the applied slider rounding, then reserve the search's
    // 5% margin. Rounding only after division can undershoot the target.
    return roundUp(roundUp(pyrowave::bandwidth::total_kbps(imageKbps, fps, transport)) / 0.95);
}

inline int imageCapacity(int wireKbps, int fps, const pyrowave::bandwidth::transport_t& transport)
{
    // Match Vibeshine max_frame_bytes(): reserve one block for critical FEC.
    const int frameBytes = (transport.critical_fec_percentage > 0 ? 3000 : 4000) *
                           (transport.packetsize - 16) - 8;
    int image = roundDown((std::min)(pyrowave::bandwidth::image_kbps(wireKbps, fps, transport),
                                    frameBytes * fps / 125.0));
    while (image >= stepKbps &&
           roundUp(pyrowave::bandwidth::total_kbps(image, fps, transport)) > wireKbps) image -= stepKbps;
    return image;
}

template<class Cost> struct DeviceResult {
    int imageKbps;
    Cost cost;
    bool deviceLimited = false;
};

// Every passing candidate gets the caller's complete timed measurement. Only
// the floor's obvious-overload/saving check is short, as in the original test.
// Try the requested ceiling first, avoiding repeated successful upward probes.
template<class Measure, class Quick, class KeepsUp, class Cancelled>
auto searchDevice(int requestedKbps, int recommendedKbps, int floorKbps,
                  Measure measure, Quick quick, KeepsUp keepsUp, Cancelled cancelled)
{
    auto initial = measure(requestedKbps);
    DeviceResult<decltype(initial)> result {requestedKbps, initial};
    if (!initial.ok || keepsUp(initial) || cancelled()) return result;
    int high = requestedKbps;
    int low = 0;
    auto failed = initial;
    if (recommendedKbps < high && recommendedKbps > floorKbps) {
        const auto guide = measure(recommendedKbps);
        if (cancelled()) return result;
        if (keepsUp(guide)) {
            low = recommendedKbps;
            result = {low, guide, true};
        }
        else if (guide.ok) {
            high = recommendedKbps;
            failed = guide;
        }
    }
    if (!low) {
        if (floorKbps < stepKbps || floorKbps >= high || cancelled()) return result;
        const auto floorCost = quick(floorKbps);
        // Preserve the minimum 10% saving rule: avoid buying a lucky pass by
        // reducing quality when bitrate does not materially affect GPU work.
        if (!floorCost.ok || floorCost.meanMs > failed.meanMs * 0.9 || cancelled()) return result;
        const auto floor = floorCost.overloaded ? floorCost : measure(floorKbps);
        if (!keepsUp(floor) || cancelled()) return result;
        low = floorKbps;
        result = {low, floor, true};
    }
    // Six bisections narrow the initial bitrate bracket to about 1/64, down
    // to the slider's 5 Mbps precision, even above the quality regression range.
    for (int step = 0; step < 6 && !cancelled(); ++step) {
        const int middle = roundDown((low + high) / 2.0);
        if (middle <= low) break;
        const auto cost = measure(middle);
        if (keepsUp(cost)) {
            low = middle;
            result = {low, cost, true};
        }
        else high = middle;
    }
    return result;
}
} // namespace PyroWaveCalibration
