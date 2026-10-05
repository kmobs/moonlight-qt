#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace PyroWaveLink {
constexpr int minimumKbps = 5000;
constexpr int maximumKbps = 3000000;
constexpr int durationMs = 2000;
constexpr int windowMs = 100;
constexpr int windowCount = durationMs / windowMs;

struct Result {
    int requestedKbps = 0;
    uint32_t expected = 0;
    uint32_t sent = 0;
    uint32_t received = 0;
    double senderMs = 0;
    double lossPercent = 100;
    double worstWindowLossPercent = 100;
    double delayP99Ms = 0;
    double delayGrowthMs = 0;
    bool kernelArrivalTimestamps = false;
    double receiverReadDelayP99Ms = 0;

    const char* failureReason() const {
        if (!expected) return "no probe completed";
        if (sent != expected) return "host did not send every probe packet";
        if (!received) return "no UDP packets received";
        if (received > sent) return "invalid received packet count";
        if (!std::isfinite(senderMs) || !std::isfinite(lossPercent) ||
            !std::isfinite(worstWindowLossPercent) || !std::isfinite(delayP99Ms) ||
            !std::isfinite(delayGrowthMs)) return "invalid timing measurement";
        if (senderMs < durationMs * 0.98 || senderMs > durationMs * 1.02) return "host probe missed its sending duration";
        if (lossPercent > 0.1) return "packet loss exceeds the limit";
        if (worstWindowLossPercent > 1.0) return "bursty packet loss exceeds the limit";
        if (delayP99Ms > 4.0) return "packet delivery variation exceeds 4 ms";
        if (delayGrowthMs > 2.0) return "packet delivery delay grows by more than 2 ms";
        return "stable";
    }

    // Capacity qualification retains loss, sender pacing and queue-growth
    // limits. Transit jitter is a separate smoothness warning, not proof that
    // the codec/device is unsupported or that lowering bitrate will help.
    bool capacityQualified() const {
        return expected > 0 && sent == expected && received > 0 && received <= sent &&
               std::isfinite(delayP99Ms) && std::isfinite(delayGrowthMs) &&
               senderMs >= durationMs * 0.98 && senderMs <= durationMs * 1.02 &&
               lossPercent <= 0.1 && worstWindowLossPercent <= 1.0 &&
               delayGrowthMs <= 2.0;
    }

    bool stable() const {
        return capacityQualified() && delayP99Ms <= 4.0;
    }
};

// Unique sequence IDs make reordering harmless and prevent duplicate packets
// from hiding loss. Delay is relative to the minimum transit in this run, so
// host/client clock offsets never enter the grade.
inline void summarize(Result& result, const std::vector<int64_t>& arrivalsUs)
{
    std::vector<double> transit;
    double first = 0, last = 0;
    int firstCount = 0, lastCount = 0;
    result.received = 0;
    result.worstWindowLossPercent = 0;
    for (int window = 0; window < windowCount; ++window) {
        const uint32_t begin = uint64_t(result.expected) * window / windowCount;
        const uint32_t end = uint64_t(result.expected) * (window + 1) / windowCount;
        uint32_t count = 0;
        for (uint32_t seq = begin; seq < end && seq < arrivalsUs.size(); ++seq) {
            if (arrivalsUs[seq] < 0) continue;
            ++count;
            // Sender uses this same 1 ms packet-group schedule.
            const auto tick = (uint64_t(seq + 1) * durationMs + result.expected - 1) / result.expected - 1;
            const double delta = arrivalsUs[seq] / 1000.0 - tick;
            transit.push_back(delta);
            if (window == 0) { first += delta; ++firstCount; }
            if (window == windowCount - 1) { last += delta; ++lastCount; }
        }
        result.received += count;
        if (end > begin) result.worstWindowLossPercent = (std::max)(result.worstWindowLossPercent,
            100.0 * (end - begin - count) / (end - begin));
    }
    result.lossPercent = result.expected ? 100.0 * (result.expected - result.received) / result.expected : 100;
    if (transit.empty()) return;
    std::sort(transit.begin(), transit.end());
    result.delayP99Ms = transit[(transit.size() - 1) * 99 / 100] - transit.front();
    result.delayGrowthMs = firstCount && lastCount ? last / lastCount - first / firstCount : INFINITY;
}

inline int rounded(double kbps) { return int(kbps / minimumKbps) * minimumKbps; }

// First test the requested rate; grow until a failure, then refine that bracket.
// A final 5% margin is measured twice afresh. Failed confirmation lowers the
// bracket, so a noisy/high-loss path can never inherit an earlier passing rate.
template<class Probe, class Cancelled>
Result search(int targetKbps, int capKbps, Probe probe, Cancelled cancelled,
              bool requireLowJitter = true)
{
    const auto qualified = [requireLowJitter](const Result& result) {
        return requireLowJitter ? result.stable() : result.capacityQualified();
    };
    const int cap = (std::min)(maximumKbps, rounded(capKbps));
    if (cap < minimumKbps) return {};
    int low = 0, high = cap + minimumKbps;
    int next = std::clamp(rounded(targetKbps), minimumKbps, cap);
    Result best;
    for (int attempt = 0; attempt < 40 && !cancelled(); ++attempt) {
        const auto result = probe(next);
        best = result;
        if (cancelled()) return {};
        if (qualified(result)) {
            best = result;
            low = next;
            if (low == cap || high - low <= minimumKbps) break;
            next = high <= cap ? rounded((low + high) / 2.0) :
                (std::min)(cap, (std::max)(low + minimumKbps, rounded(low * 1.25)));
        }
        else {
            high = next;
            if (high - low <= minimumKbps) break;
            next = (std::max)(minimumKbps, rounded((low + high) / 2.0));
        }
    }
    if (!low) return best;
    next = (std::max)(minimumKbps, rounded(low * 0.95));
    for (int attempt = 0; attempt < 32 && !cancelled(); ++attempt) {
        best = probe(next);
        if (cancelled()) return {};
        if (qualified(best)) {
            best = probe(next);
            if (!cancelled() && qualified(best)) return best;
        }
        if (next == minimumKbps) break;
        next = (std::max)(minimumKbps, rounded(next * 0.8));
    }
    return cancelled() ? Result{} : best;
}
} // namespace PyroWaveLink
