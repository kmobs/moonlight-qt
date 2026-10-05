#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cmath>

// Observation-only, bounded history. Frame delivery never allocates or draws.
// The owner supplies synchronization; snapshots/drawing happen off the pacer.
namespace Overlay {
struct TimingGraphInput {
    uint64_t submissionUs = 0, targetUs = 0, sourcePeriodUs = 0;
    uint64_t bufferUs = 0, requestedBufferUs = 0;
    uint64_t submissionId = 0, displayId = 0, displayUs = 0;
    uint32_t backend = 0;
    bool submitted = false, idValid = false, displayValid = false, discontinuity = false;
};
struct TimingGraphPoint {
    uint64_t submissionUs = 0, targetUs = 0, targetIntervalUs = 0, submissionIntervalUs = 0;
    uint64_t sourcePeriodUs = 0, bufferUs = 0, requestedBufferUs = 0;
    uint64_t submissionId = 0, displayUs = 0, generation = 0;
    uint32_t backend = 0;
    bool idValid = false, breakBefore = false;
};
using TimingGraphSnapshot = std::vector<TimingGraphPoint>;

// Three aligned lanes of raw frame intervals: planned (target) cadence, client
// submissions and OS-reported display events. The lanes share one millisecond
// axis centred on the planned interval, so a disturbance shows up in the lane
// where it enters the pipeline instead of in lines drawn over each other.
struct TimingGraphLayout {
    static constexpr int Lanes = 3, Frames = 240;
    static constexpr int Width = 800, Left = 76, RightMargin = 14;
    static constexpr int LaneTop = 32, LaneSpacing = 78, PlotOffset = 24, PlotHeight = 46;
    static constexpr int Height = LaneTop + Lanes * LaneSpacing + 32;
    static constexpr double RadiusUs = 2000.0;
    // Deviations this small are not noticeable at typical VRR rates; they are
    // drawn on the reference line so only noticeable disturbances stand out.
    static constexpr double FlatUs = 1000.0;
    static constexpr int titleTop(int lane) { return LaneTop + lane * LaneSpacing; }
    static constexpr int plotTop(int lane) { return titleTop(lane) + PlotOffset; }
    static constexpr int plotBottom(int lane) { return plotTop(lane) + PlotHeight; }
};

enum class TimingGraphLane { Target, Submit, Display };

// Interval ending at points[i] in one lane. Display intervals need OS
// feedback for both frames of the same epoch, so a missing presentation is
// never bridged into one long interval.
inline bool timingGraphInterval(const TimingGraphSnapshot& points, size_t i, TimingGraphLane lane, uint64_t& intervalUs)
{
    if (i >= points.size()) return false;
    const auto& p = points[i];
    switch (lane) {
    case TimingGraphLane::Target:
        intervalUs = p.targetIntervalUs;
        return intervalUs != 0;
    case TimingGraphLane::Submit:
        intervalUs = p.submissionIntervalUs;
        return intervalUs != 0;
    case TimingGraphLane::Display:
        if (i == 0) return false;
        {
            const auto& previous = points[i - 1];
            if (p.breakBefore || p.generation != previous.generation || p.backend != previous.backend ||
                !previous.displayUs || p.displayUs <= previous.displayUs) return false;
            intervalUs = p.displayUs - previous.displayUs;
        }
        return true;
    }
    return false;
}

class TimingGraphHistory {
public:
    static constexpr size_t SnapshotPoints = TimingGraphLayout::Frames + 1;
    static constexpr size_t Capacity = 256;

    void record(const TimingGraphInput& in)
    {
        if (in.discontinuity) { ++m_Generation; m_Break = true; }
        if (in.submitted && in.submissionUs) {
            const TimingGraphPoint* prev = m_Count ? &(*m_Points)[(m_Next + Capacity - 1) % Capacity] : nullptr;
            if (prev && (in.submissionUs <= prev->submissionUs || in.backend != prev->backend ||
                         (in.idValid && prev->idValid && in.submissionId <= prev->submissionId))) {
                ++m_Generation; m_Break = true;
            }
            TimingGraphPoint p;
            p.submissionUs = in.submissionUs; p.targetUs = in.targetUs;
            p.sourcePeriodUs = in.sourcePeriodUs; p.bufferUs = in.bufferUs;
            p.requestedBufferUs = in.requestedBufferUs;
            p.submissionId = in.submissionId; p.idValid = in.idValid;
            p.backend = in.backend; p.generation = m_Generation; p.breakBefore = m_Break;
            if (prev && !m_Break) {
                p.submissionIntervalUs = in.submissionUs - prev->submissionUs;
                if (prev->targetUs && in.targetUs > prev->targetUs)
                    p.targetIntervalUs = in.targetUs - prev->targetUs;
            }
            (*m_Points)[m_Next] = p; m_Next = (m_Next + 1) % Capacity;
            if (m_Count < Capacity) ++m_Count;
            m_Break = false;
        }
        // Feedback commonly names an earlier frame. Never attach it to the
        // frame whose Present happened to return the observation. Refresh
        // references are excluded by the caller; only DisplayEvent is valid.
        if (in.displayValid && in.displayUs && in.backend) {
            for (size_t age = 0; age < m_Count; ++age) {
                auto& p = (*m_Points)[(m_Next + Capacity - 1 - age) % Capacity];
                if (p.generation != m_Generation) break;
                if (p.idValid && p.backend == in.backend && p.submissionId == in.displayId) {
                    if (!p.displayUs && in.displayUs >= p.submissionUs) p.displayUs = in.displayUs;
                    break;
                }
            }
        }
    }

    // The newest drawn frames plus the predecessor of the oldest, which the
    // display lane needs for its first interval.
    void copyTo(TimingGraphSnapshot& out) const
    {
        out.clear();
        const size_t count = std::min(m_Count, SnapshotPoints);
        const auto begin = (m_Next + Capacity - count) % Capacity;
        for (size_t i = 0; i < count; ++i) out.push_back((*m_Points)[(begin + i) % Capacity]);
    }
private:
    // One allocation at pacer construction; avoid a large inline object on
    // Windows test/main-thread stacks. No allocations while recording.
    std::unique_ptr<std::array<TimingGraphPoint, Capacity>> m_Points =
        std::make_unique<std::array<TimingGraphPoint, Capacity>>();
    size_t m_Count = 0, m_Next = 0;
    uint64_t m_Generation = 0;
    bool m_Break = true;
};
}
