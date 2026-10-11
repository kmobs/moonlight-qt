#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <vector>

namespace Vrr13 {
// Experimental input-deadline coverage controller. probabilityPpm is a frame
// probability; it must never be substituted for interval severity quality.
// requiredUs is a causal input offset from the budget-adjusted input slot,
// while slotUs is the unbuffered source slot used only for continuity.
// Intentional decoder
// backlog and policy holds removed by the caller. Capacity/packet-loss/basis
// eligibility is also owned by the caller, not inferred from input lateness.
//
// vrrHeadroomUs is the VRR recovery headroom at the current source rate
// (source period minus the larger of display period and typical render). A
// lower frame rate leaves more of each period free to absorb lateness, so it
// is subtracted from every requirement exactly like the VRR17 smoothness path;
// 30 FPS must never hold more buffer than 60 or 120 FPS for the same jitter.
class InputPercentileBuffer {
public:
    // Compatibility only: raw stage misses no longer arm a hard hold.
    static constexpr uint64_t BufferMissHoldUs = 0;
    static constexpr uint64_t ReleaseFeedbackWindowUs = 1000000;
    static constexpr uint64_t MinimumWindowUs = 10000000;
    static constexpr uint64_t MaximumWindowUs = 300000000;
    static constexpr size_t MaximumSamples = 262144;
    struct Stats {
        uint64_t targetUs = 0, currentUs = 0, samples = 0, minimumUs = 0, maximumUs = 0;
        // Session totals; retained names preserve existing trace/test callers.
        uint64_t observed = 0, eligible = 0, ineligible = 0, absorbed = 0, missed = 0;
        uint64_t cumulativeAbsorbed = 0, cumulativeMissed = 0;
        uint64_t holdUntilUs = 0, holdRemainingUs = 0, lastBufferMissUs = 0;
        uint64_t lastNetworkMissUs = 0, lastDecoderMissUs = 0;
        uint64_t cumulativeNetworkMisses = 0, cumulativeDecoderMisses = 0;
        uint64_t recentReleaseSamples = 0, recentReleaseMisses = 0, recentProposedMisses = 0;
        uint64_t releaseFactorPpm = 1000000, effectiveFallRateUsPerSecond = 0;
        uint64_t effectiveFallRateMilliUsPerSecond = 0;
        // Outcomes observed with each frame's actual buffer/tolerance inside
        // the effective trailing profile window. They are historical outcomes,
        // not counterfactual coverage under a newly selected tolerance.
        uint64_t recentEligible = 0, recentAbsorbed = 0, recentMissed = 0;
        uint64_t raises = 0, lowers = 0, unknownRelease = 0, epochResets = 0;
        uint64_t observationSpanUs = 0, validPairCoverageUs = 0;
        uint64_t probabilityPpm = 0, windowUs = 0, toleranceUs = 0, vrrHeadroomUs = 0;
        int64_t readinessBudgetUs = 0;
        uint64_t headroomTargetUnclippedUs = 0;
        int64_t rawRequiredQuantileUs = 0;
        bool tailUncertain = true, evidenceValid = false;
        bool historyTruncated = false, expiredUnknown = false, clipped = false;
        bool initialized = false, lastInputEligible = false, growthBlocked = false;
        int64_t lastInputRequiredUs = 0;
        uint64_t lastInputObservationUs = 0;
    };
    // Activation/source epoch should seed current from the applied buffer.
    void reset(uint64_t current = 0) {
        *this = InputPercentileBuffer{};
        m_Stats.currentUs = m_Stats.targetUs = current;
        m_Stats.initialized = true;
    }
    // Until the profile tail is first supported, protection follows the
    // largest eligible requirement at once instead of rising at 1 ms/s. The
    // slow ramp missed 144 and 165 frames in the first 10 s of the 2026-10-10
    // 22:48/22:55 sessions; tracking the maximum missed 8 and 16.
    // A buffer resumed from evidence it already trusts skips learning.
    void endLearning() { m_Learning = false; }
    bool learning() const { return m_Learning; }
    // A changed source phase, cadence, or normalized-input basis invalidates
    // prior requirements. Retain current protection while relearning its size.
    void breakSequence() {
        m_Samples.clear();
        m_Stats.targetUs = m_Stats.currentUs;
        m_Stats.samples = m_Stats.observationSpanUs = m_Stats.validPairCoverageUs = 0;
        m_Stats.recentEligible = m_Stats.recentAbsorbed = m_Stats.recentMissed = 0;
        updateReleaseFeedback(m_LastClockUs);
        m_Stats.rawRequiredQuantileUs = 0;
        m_Stats.headroomTargetUnclippedUs = m_Stats.currentUs;
        m_Stats.tailUncertain = true;
        m_Stats.evidenceValid = m_Stats.historyTruncated = m_Stats.clipped = false;
        ++m_Stats.epochResets;
        m_Learning = true;
        m_LastEvaluationUs = m_LastTruncatedUs = m_ReleaseFraction = 0;
        m_PreviousValid = false;
        m_Direction = 0;
        ++m_Epoch;
        // Keep elapsed/last-eligible clocks and diagnostics: recurring phase
        // breaks must not rearm the unknown-evidence retention window forever.
    }
    // A source phase discontinuity keeps the slot basis: input lateness keeps
    // its level across it (2026-10-10 22:55 capture: median 3.10 -> 3.16 ms).
    // Only frame adjacency breaks; evidence and coverage outcomes are kept.
    void breakAdjacency() {
        m_PreviousValid = false;
        ++m_Epoch;
    }
    // A new rate layer starts from a copy of its neighbour's evidence. The
    // copied requirements seed its target; their outcomes belonged to the
    // other rate, so they leave this layer's coverage.
    void markEvidenceAsSeed() {
        for (auto& sample : m_Samples) {
            if (sample.seed) continue;
            --m_Stats.recentEligible;
            m_Stats.recentAbsorbed -= sample.absorbed;
            m_Stats.recentMissed -= sample.missed;
            sample.seed = true;
            sample.absorbed = sample.missed = false;
        }
    }
    // Session totals and stage diagnostics continue across rate-band layers;
    // evidence, protection and coverage outcomes stay with each band.
    void continueTotalsFrom(const InputPercentileBuffer& previous) {
        const auto& s = previous.m_Stats;
        m_Stats.observed = s.observed; m_Stats.eligible = s.eligible; m_Stats.ineligible = s.ineligible;
        m_Stats.absorbed = s.absorbed; m_Stats.missed = s.missed;
        m_Stats.cumulativeNetworkMisses = s.cumulativeNetworkMisses;
        m_Stats.cumulativeDecoderMisses = s.cumulativeDecoderMisses;
        m_Stats.lastBufferMissUs = s.lastBufferMissUs;
        m_Stats.lastNetworkMissUs = s.lastNetworkMissUs;
        m_Stats.lastDecoderMissUs = s.lastDecoderMissUs;
        m_Stats.raises = s.raises; m_Stats.lowers = s.lowers;
        m_Stats.unknownRelease = s.unknownRelease; m_Stats.epochResets = s.epochResets;
    }
    // Source-rate change that gained VRR headroom: protection learned at the
    // old rate overstates what the new rate needs by exactly that gain.
    void creditHeadroom(uint64_t gainedUs) {
        if (!m_Stats.initialized || !gainedUs) return;
        const auto reduce = [&](uint64_t value) {
            return std::max(m_Stats.minimumUs, value - std::min(value, gainedUs));
        };
        m_Stats.currentUs = reduce(m_Stats.currentUs);
        m_Stats.targetUs = reduce(m_Stats.targetUs);
        m_ReleaseFraction = 0;
    }
    // Source-rate change that lost VRR headroom: the retained evidence already
    // sets the target at the new rate (call after advance()), so step to it at
    // once, by at most the loss, instead of rising at 1 ms/s and missing every
    // frame meanwhile. The step coincides with the cadence change.
    void raiseForHeadroomLoss(uint64_t lostUs) {
        if (!m_Stats.initialized || !lostUs) return;
        const uint64_t raised = std::min({m_Stats.targetUs, m_Stats.maximumUs,
            m_Stats.currentUs + std::min(lostUs, m_Stats.maximumUs)});
        if (raised <= m_Stats.currentUs) return;
        m_Stats.currentUs = raised;
        ++m_Stats.raises;
        m_ReleaseFraction = 0;
    }
    uint64_t demand(uint64_t applied) const {
        return m_Stats.initialized ? m_Stats.currentUs : applied;
    }
    Stats stats() const {
        auto result = m_Stats;
        result.cumulativeAbsorbed = result.absorbed;
        result.cumulativeMissed = result.missed;
        return result;
    }
    // Raw graph-stage misses remain cumulative diagnostics. Recent miss counts
    // use eligible causal requirements; only the long quantile actuates demand.
    void noteBufferMiss(uint64_t atUs, bool networkMiss, bool decoderMiss) {
        if (!atUs || (!networkMiss && !decoderMiss)) return;
        if (networkMiss) {
            ++m_Stats.cumulativeNetworkMisses;
            m_Stats.lastNetworkMissUs = std::max(m_Stats.lastNetworkMissUs, atUs);
        }
        if (decoderMiss) {
            ++m_Stats.cumulativeDecoderMisses;
            m_Stats.lastDecoderMissUs = std::max(m_Stats.lastDecoderMissUs, atUs);
        }
        m_Stats.lastBufferMissUs = std::max(m_Stats.lastBufferMissUs, atUs);
        m_Stats.holdUntilUs = m_Stats.holdRemainingUs = 0;
    }
    // Decision-time aging: no input observation or continuity changes.
    // Unknown current-frame capacity permits release, never another rise.
    // The full overload applies profile changes before demand() is consulted.
    void advance(uint64_t atUs, uint64_t minimum, uint64_t maximum,
                 uint64_t probabilityPpm, uint64_t windowUs,
                 uint64_t riseRate, uint64_t fallRate, int64_t readinessBudgetUs = 0, uint64_t toleranceUs = 0,
                 uint64_t vrrHeadroomUs = 0) {
        if (!atUs || (m_LastClockUs && atUs < m_LastClockUs)) return;
        configure(minimum, maximum, probabilityPpm, windowUs, riseRate, fallRate, readinessBudgetUs, toleranceUs, vrrHeadroomUs);
        const uint64_t elapsed = advanceClock(atUs);
        evaluate(atUs, expire(atUs));
        move(elapsed, false);
    }
    void advance(uint64_t atUs) {
        if (!m_Stats.initialized || !m_Stats.windowUs) return;
        advance(atUs, m_Stats.minimumUs, m_Stats.maximumUs, m_Stats.probabilityPpm,
            m_Stats.windowUs, m_RiseRate, m_FallRate, m_Stats.readinessBudgetUs, m_Stats.toleranceUs,
            m_Stats.vrrHeadroomUs);
    }
    void observe(uint64_t frame, uint64_t observationUs, uint64_t slotUs,
                 int64_t requiredUs, bool valid, uint64_t minimum,
                 uint64_t maximum, uint64_t probabilityPpm,
                 uint64_t windowUs, uint64_t riseRate, uint64_t fallRate,
                 int64_t readinessBudgetUs = 0, uint64_t toleranceUs = 0,
                 uint64_t vrrHeadroomUs = 0) {
        configure(minimum, maximum, probabilityPpm, windowUs, riseRate, fallRate, readinessBudgetUs, toleranceUs, vrrHeadroomUs);
        // An observation can follow advance() at the same timestamp. Actual
        // duplicate/nonmonotonic observations still invalidate their history.
        if (!observationUs || (m_LastObservationUs && observationUs <= m_LastObservationUs) ||
            (m_LastClockUs && observationUs < m_LastClockUs)) {
            reset(m_Stats.currentUs);
            return;
        }
        const uint64_t observationElapsed = m_LastObservationUs ?
            std::min<uint64_t>(observationUs - m_LastObservationUs, 100000) : 0;
        m_LastObservationUs = observationUs;
        const uint64_t elapsed = advanceClock(observationUs);
        bool changed = expire(observationUs);
        const bool eligible = valid && slotUs;
        m_Stats.lastInputEligible = eligible;
        m_Stats.lastInputRequiredUs = requiredUs;
        m_Stats.lastInputObservationUs = observationUs;
        ++m_Stats.observed;
        // Tolerance is permissible excess, not standing buffer protection.
        // Subtract once rather than adding to current (which could overflow).
        // VRR headroom is subtracted the same way: it absorbs lateness for free.
        const uint64_t excess = requiredUs > 0 ?
            uint64_t(requiredUs) - std::min(uint64_t(requiredUs), toleranceUs) : 0;
        const uint64_t headroom = excess - std::min(excess, m_Stats.vrrHeadroomUs);
        const bool absorbed = eligible && headroom > 0 && headroom <= m_Stats.currentUs;
        const bool missed = eligible && headroom > m_Stats.currentUs;
        if (eligible) {
            ++m_Stats.eligible;
            m_Stats.absorbed += absorbed;
            m_Stats.missed += missed;
        } else ++m_Stats.ineligible;
        const bool adjacent = eligible && m_PreviousValid && frame == m_PreviousFrame + 1 &&
            slotUs > m_PreviousSlotUs && slotUs - m_PreviousSlotUs < 1000000;
        if (!adjacent) ++m_Epoch;
        if (eligible) {
            m_LastEligibleUs = observationUs;
            const int64_t rawRequired = readinessBudgetUs > 0 &&
                requiredUs > std::numeric_limits<int64_t>::max() - readinessBudgetUs ?
                std::numeric_limits<int64_t>::max() : readinessBudgetUs < 0 &&
                requiredUs < std::numeric_limits<int64_t>::min() - readinessBudgetUs ?
                std::numeric_limits<int64_t>::min() : requiredUs + readinessBudgetUs;
            m_Samples.push_back({observationUs, slotUs, rawRequired, m_Epoch, absorbed, missed});
            ++m_Stats.recentEligible;
            m_Stats.recentAbsorbed += absorbed;
            m_Stats.recentMissed += missed;
        }
        m_PreviousFrame = frame;
        m_PreviousSlotUs = slotUs;
        m_PreviousValid = eligible;
        while (m_Samples.size() > MaximumSamples) {
            removeFront();
            m_LastTruncatedUs = observationUs;
            changed = true;
        }
        evaluate(observationUs, changed);
        if (m_Learning && eligible && m_Stats.targetUs > m_Stats.currentUs) {
            m_Stats.currentUs = m_Stats.targetUs;
            ++m_Stats.raises;
            m_ReleaseFraction = 0;
        }
        if (m_Learning && m_Stats.samples && !m_Stats.tailUncertain) m_Learning = false;
        // Invalid observations discard growth time and fractional growth credit;
        // decision-time advance must neither grow nor consume observation time.
        if (!eligible && m_Stats.targetUs > m_Stats.currentUs) m_ReleaseFraction = 0;
        move(m_Stats.targetUs > m_Stats.currentUs ? observationElapsed : elapsed, eligible);
    }
private:
    void configure(uint64_t minimum, uint64_t maximum, uint64_t probabilityPpm,
                   uint64_t windowUs, uint64_t riseRate, uint64_t fallRate, int64_t budget, uint64_t tolerance,
                   uint64_t vrrHeadroom) {
        minimum = std::min(minimum, maximum);
        if (!m_Stats.initialized) {
            m_Stats.initialized = true;
            m_Stats.currentUs = m_Stats.targetUs = minimum;
        }
        m_Stats.minimumUs = minimum;
        m_Stats.maximumUs = maximum;
        m_Stats.currentUs = std::clamp(m_Stats.currentUs, minimum, maximum);
        m_Stats.targetUs = std::clamp(m_Stats.targetUs, minimum, maximum);
        m_Stats.probabilityPpm = std::clamp<uint64_t>(probabilityPpm, 1, 999999);
        m_Stats.windowUs = std::clamp(windowUs, MinimumWindowUs, MaximumWindowUs);
        m_Stats.readinessBudgetUs = budget;
        m_Stats.toleranceUs = tolerance;
        m_Stats.vrrHeadroomUs = vrrHeadroom;
        m_RiseRate = std::min<uint64_t>(riseRate, 1000000);
        m_FallRate = std::min<uint64_t>(fallRate, 1000000);
    }
    uint64_t advanceClock(uint64_t atUs) {
        const uint64_t elapsed = m_LastClockUs ? std::min<uint64_t>(atUs - m_LastClockUs, 100000) : 0;
        m_LastClockUs = atUs;
        if (!m_FirstObservationUs) m_FirstObservationUs = atUs;
        return elapsed;
    }
    void removeFront() {
        const auto& sample = m_Samples.front();
        if (!sample.seed) {
            --m_Stats.recentEligible;
            m_Stats.recentAbsorbed -= sample.absorbed;
            m_Stats.recentMissed -= sample.missed;
        }
        m_Samples.pop_front();
    }
    bool expire(uint64_t atUs) {
        bool changed = false;
        while (!m_Samples.empty() && atUs - m_Samples.front().at >= m_Stats.windowUs) {
            removeFront();
            changed = true;
        }
        return changed;
    }
    uint64_t recentMissesAt(uint64_t atUs, uint64_t candidate) const {
        uint64_t misses = 0;
        for (auto it = m_Samples.rbegin(); it != m_Samples.rend(); ++it) {
            if (atUs - it->at >= ReleaseFeedbackWindowUs) break;
            const uint64_t required = it->required > m_Stats.readinessBudgetUs ?
                uint64_t(it->required) - uint64_t(m_Stats.readinessBudgetUs) : 0;
            const uint64_t excess = required - std::min(required, m_Stats.toleranceUs);
            misses += excess - std::min(excess, m_Stats.vrrHeadroomUs) > candidate;
        }
        return misses;
    }
    void updateReleaseFeedback(uint64_t atUs) {
        m_Stats.holdUntilUs = m_Stats.holdRemainingUs = 0;
        m_Stats.recentReleaseSamples = 0;
        for (auto it = m_Samples.rbegin(); it != m_Samples.rend(); ++it) {
            if (atUs - it->at >= ReleaseFeedbackWindowUs) break;
            ++m_Stats.recentReleaseSamples;
        }
        m_Stats.recentReleaseMisses = recentMissesAt(atUs, m_Stats.currentUs);
        m_Stats.recentProposedMisses = m_Stats.recentReleaseMisses;
        updateReleaseRate(m_Stats.recentReleaseMisses);
    }
    void updateReleaseRate(uint64_t /*misses*/) {
        // The selected long-window quantile owns buffer demand. Short-window
        // counts diagnose misses; they do not redefine the user's tail target.
        // Output catch-up is independently bounded by PhaseTrajectory.
        m_Stats.releaseFactorPpm = 1000000;
        m_Stats.effectiveFallRateUsPerSecond = m_FallRate;
        m_Stats.effectiveFallRateMilliUsPerSecond = m_FallRate * 1000;
    }
    void evaluate(uint64_t atUs, bool removedSamples) {
        updateReleaseFeedback(atUs);
        const uint64_t windowUs = m_Stats.windowUs;
        const uint64_t probabilityPpm = m_Stats.probabilityPpm;
        m_Stats.samples = m_Samples.size();
        m_Stats.observationSpanUs = m_Samples.empty() ? 0 : atUs - m_Samples.front().at;
        m_Stats.historyTruncated = m_LastTruncatedUs && atUs - m_LastTruncatedUs < windowUs;
        // Historical eligibility belongs to each sample, not to the current
        // tick. Invalid input cannot freeze an expired quantile or its target.
        m_Stats.evidenceValid = !m_Samples.empty();
        m_Stats.expiredUnknown = m_Samples.empty() && atUs -
            (m_LastEligibleUs ? m_LastEligibleUs : m_FirstObservationUs) >= windowUs;
        if (m_Samples.empty()) {
            m_Stats.rawRequiredQuantileUs = 0;
            m_Stats.headroomTargetUnclippedUs = 0;
            m_Stats.validPairCoverageUs = 0;
            m_Stats.tailUncertain = true;
            m_Stats.clipped = false;
            if (m_Stats.expiredUnknown) m_Stats.targetUs = m_Stats.minimumUs;
            else m_ReleaseFraction = 0;
            return;
        }
        const uint64_t tail = 1000000 - probabilityPpm;
        const uint64_t needed = (1000000 + tail - 1) / tail;
        // Learning protects each new maximum at once, so it is not throttled;
        // the history is still short (below the tail's supporting samples).
        if (m_Learning || removedSamples || !m_LastEvaluationUs || m_Samples.size() == needed ||
            atUs - m_LastEvaluationUs >= 100000 ||
            m_EvaluatedProbabilityPpm != probabilityPpm || m_EvaluatedWindowUs != windowUs) {
            m_LastEvaluationUs = atUs;
            m_EvaluatedProbabilityPpm = probabilityPpm;
            m_EvaluatedWindowUs = windowUs;
            std::vector<int64_t> values;
            values.reserve(m_Samples.size());
            m_Stats.validPairCoverageUs = 0;
            for (size_t i = 0; i < m_Samples.size(); ++i) {
                const auto& sample = m_Samples[i];
                values.push_back(sample.required);
                if (i && sample.epoch == m_Samples[i - 1].epoch)
                    m_Stats.validPairCoverageUs += sample.slot - m_Samples[i - 1].slot;
            }
            // One expected tail observation is support, not a confidence bound.
            m_Stats.tailUncertain = values.size() < needed || m_Stats.historyTruncated;
            const size_t rank = m_Stats.tailUncertain ? values.size() :
                size_t((probabilityPpm * values.size() + 999999) / 1000000);
            std::nth_element(values.begin(), values.begin() + rank - 1, values.end());
            m_Stats.rawRequiredQuantileUs = values[rank - 1];
        }
        // Unsigned subtraction preserves the positive signed difference even
        // when the normalized requirement and budget span the signed range.
        const int64_t budget = m_Stats.readinessBudgetUs;
        const uint64_t required = m_Stats.rawRequiredQuantileUs > budget ?
            uint64_t(m_Stats.rawRequiredQuantileUs) - uint64_t(budget) : 0;
        const uint64_t excess = required - std::min(required, m_Stats.toleranceUs);
        const uint64_t requested = excess - std::min(excess, m_Stats.vrrHeadroomUs);
        m_Stats.headroomTargetUnclippedUs = requested;
        m_Stats.targetUs = std::clamp(requested, m_Stats.minimumUs, m_Stats.maximumUs);
        m_Stats.clipped = requested > m_Stats.maximumUs;
    }
    void move(uint64_t elapsed, bool growthAllowed) {
        const int direction = m_Stats.targetUs > m_Stats.currentUs ? 1 :
            m_Stats.targetUs < m_Stats.currentUs ? -1 : 0;
        if (direction != m_Direction) m_ReleaseFraction = 0;
        m_Direction = direction;
        m_Stats.growthBlocked = direction > 0 && !growthAllowed;
        if (!direction || (direction > 0 && !growthAllowed)) return;
        // Preserve fractional soft rates (e.g. 37.5 us/s). Both products are
        // bounded: elapsed <= 100 ms, rate <= 1e6, factor <= 1e6.
        const uint64_t rate = direction > 0 ? m_RiseRate : m_FallRate;
        constexpr uint64_t FractionDenominator = 1000000000000ULL;
        if (direction < 0) {
            // Assess the unattenuated next step before spending protection.
            // One conservative pass avoids alternating factors within a tick.
            const uint64_t provisionalStep = (m_ReleaseFraction + elapsed * rate * 1000000) /
                FractionDenominator;
            const uint64_t provisional = m_Stats.currentUs -
                std::min(provisionalStep, m_Stats.currentUs - m_Stats.targetUs);
            m_Stats.recentProposedMisses = recentMissesAt(m_LastClockUs, provisional);
            updateReleaseRate(std::max(m_Stats.recentReleaseMisses, m_Stats.recentProposedMisses));
        }
        const uint64_t factor = direction > 0 ? 1000000 : m_Stats.releaseFactorPpm;
        m_ReleaseFraction += elapsed * rate * factor;
        const uint64_t step = m_ReleaseFraction / FractionDenominator;
        m_ReleaseFraction %= FractionDenominator;
        if (direction > 0) {
            const uint64_t movement = std::min(step, m_Stats.targetUs - m_Stats.currentUs);
            m_Stats.currentUs += movement;
            if (movement) ++m_Stats.raises;
        } else {
            const uint64_t movement = std::min(step, m_Stats.currentUs - m_Stats.targetUs);
            const uint64_t candidate = m_Stats.currentUs - movement;
            m_Stats.currentUs = candidate;
            if (movement) {
                ++m_Stats.lowers;
                if (m_Stats.expiredUnknown) ++m_Stats.unknownRelease;
            }
        }
    }
    struct Sample { uint64_t at, slot; int64_t required; uint64_t epoch; bool absorbed, missed; bool seed = false; };
    std::deque<Sample> m_Samples;
    Stats m_Stats;
    uint64_t m_FirstObservationUs = 0, m_LastObservationUs = 0, m_LastEligibleUs = 0;
    uint64_t m_LastClockUs = 0, m_RiseRate = 0, m_FallRate = 0;
    uint64_t m_LastEvaluationUs = 0, m_LastTruncatedUs = 0, m_Epoch = 0;
    uint64_t m_PreviousFrame = 0, m_PreviousSlotUs = 0;
    uint64_t m_EvaluatedProbabilityPpm = 0, m_EvaluatedWindowUs = 0, m_ReleaseFraction = 0;
    bool m_PreviousValid = false;
    bool m_Learning = true;
    int m_Direction = 0;
};
}
