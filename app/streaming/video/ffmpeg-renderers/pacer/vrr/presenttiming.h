#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace Vrr13 {

// Rolling count of display intervals that the display path made uneven. An
// interval counts when the displayed spacing differs from the planned spacing
// by more than the tolerance and by more than the submitted spacing did. Work
// after submission (GPU execution, compositor and display scheduling) is the
// only difference between the two, and more source buffering cannot correct
// it, so it is reported apart from Smoothness and never feeds the buffer.
// A display that absorbs uneven submissions, for example by queueing frames,
// is not blamed. The planned spacing is never shorter than the panel's fastest
// refresh, which no display can beat. A hitch is an interval at least one
// whole planned frame worse than its submission; rare hitches are visible on
// screen yet barely move the percentage, so they are counted separately.
//
// Below the panel's adaptive-refresh floor the driver repeats frames on its
// own low-framerate-compensation schedule, so display spacing there says
// nothing about the present path. Scoring pauses for those intervals and for
// a settling period afterwards: the driver leaves that mode based on its own
// average frame time, so short intervals mixed with long ones still wait for
// its fixed-length repeat refreshes.
class PresentTiming {
public:
    static constexpr uint64_t BucketUs = 1000000;
    static constexpr size_t Buckets = 30;
    static constexpr uint64_t FloorSettleUs = 250000;

    struct Stats {
        uint64_t intervals = 0, misses = 0, hitches = 0, addedTotalUs = 0, worstAddedUs = 0;
        uint64_t lastObservedUs = 0, lastPausedUs = 0;
        double issuePercent() const {
            return intervals ? 100.0 * double(misses) / double(intervals) : 0.0;
        }
    };

    struct Frame {
        uint64_t frame = 0, submittedUs = 0, presentedUs = 0, plannedUs = 0;
        uint64_t uncertaintyUs = 0, observedUs = 0;
    };

    struct Limits {
        uint64_t toleranceUs = 0;
        uint64_t displayPeriodUs = 0; // Shortest spacing the panel can show.
        uint64_t sourcePeriodUs = 0;
        uint64_t floorGapUs = 0;      // Spacing beyond which the panel may repeat frames.
    };

    // Only adjacent submitted frames with display feedback for both are
    // scored. A gap (drop, missing feedback, rebase) starts a new sequence.
    void observe(const Frame& f, const Limits& limits)
    {
        if (m_HavePrevious && f.frame <= m_Previous.frame) return;
        const auto previous = m_Previous;
        const bool adjacent = m_HavePrevious && f.frame == previous.frame + 1 &&
            f.submittedUs > previous.submittedUs && f.presentedUs > previous.presentedUs &&
            f.plannedUs > previous.plannedUs && f.presentedUs - previous.presentedUs < 1000000;
        m_Previous = f;
        m_HavePrevious = true;
        if (!adjacent) return;

        const uint64_t submitted = f.submittedUs - previous.submittedUs;
        if (limits.floorGapUs && (limits.sourcePeriodUs > limits.floorGapUs ||
                                  submitted > limits.floorGapUs)) {
            m_LastPaused = std::max(m_LastPaused, f.observedUs);
            m_PausedPresentedUs = f.presentedUs;
            return;
        }
        if (m_PausedPresentedUs && f.presentedUs - std::min(f.presentedUs, m_PausedPresentedUs) < FloorSettleUs) {
            m_LastPaused = std::max(m_LastPaused, f.observedUs);
            return;
        }

        const uint64_t planned = f.plannedUs - previous.plannedUs;
        const uint64_t shown = f.presentedUs - previous.presentedUs;
        uint64_t displayError = difference(shown, std::max(planned, limits.displayPeriodUs));
        displayError -= std::min(displayError, f.uncertaintyUs + previous.uncertaintyUs);
        const uint64_t submitError = difference(submitted, planned);
        const uint64_t added = displayError > submitError ? displayError - submitError : 0;

        const auto tick = f.observedUs / BucketUs;
        auto& bucket = m_Buckets[tick % Buckets];
        if (bucket.tick != tick) bucket = Bucket{tick};
        const bool miss = displayError > limits.toleranceUs && added > limits.toleranceUs;
        ++bucket.intervals;
        bucket.misses += miss;
        bucket.hitches += miss && added >= std::max(planned, limits.displayPeriodUs);
        bucket.addedTotal += added;
        bucket.worstAdded = std::max(bucket.worstAdded, added);
        m_LastObserved = std::max(m_LastObserved, f.observedUs);
    }

    void breakSequence() { m_HavePrevious = false; }
    void reset() { *this = PresentTiming{}; }

    // The window ends at the newest scored interval; callers judge freshness.
    Stats stats() const
    {
        Stats s;
        s.lastObservedUs = m_LastObserved;
        s.lastPausedUs = m_LastPaused;
        const auto tick = m_LastObserved / BucketUs;
        for (const auto& b : m_Buckets) {
            if (!b.intervals || b.tick > tick || tick - b.tick >= Buckets) continue;
            s.intervals += b.intervals;
            s.misses += b.misses;
            s.hitches += b.hitches;
            s.addedTotalUs += b.addedTotal;
            s.worstAddedUs = std::max(s.worstAddedUs, b.worstAdded);
        }
        return s;
    }

private:
    static uint64_t difference(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

    struct Bucket { uint64_t tick = 0, intervals = 0, misses = 0, hitches = 0, addedTotal = 0, worstAdded = 0; };
    std::array<Bucket, Buckets> m_Buckets{};
    Frame m_Previous;
    uint64_t m_LastObserved = 0, m_LastPaused = 0, m_PausedPresentedUs = 0;
    bool m_HavePrevious = false;
};

}
