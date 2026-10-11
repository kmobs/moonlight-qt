#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace Vrr13 {
// Recent *input* counterfactual, not a GPU/display completion predictor. Signed
// offsets are relative to the same unbuffered slot used by the input chart.
// Producer removes decoder backlog and intentional decode holds before input.
class AbsorbedJitter {
public:
    static constexpr uint64_t WindowUs = 3000000;
    struct Pair { uint64_t at, interval; int64_t previous, current; };
    void observe(uint64_t frame, uint64_t at, uint64_t slot, int64_t required, bool valid) {
        if (!valid || !at || !slot || (m_At && at <= m_At)) { reset(); return; }
        expire(at);
        if (m_At && frame == m_Frame + 1 && slot > m_Slot &&
            slot - m_Slot < 1000000 && at - m_At < 1000000) {
            m_Pairs.push_back({at, slot - m_Slot, m_Required, required});
            // Hard memory bound even on malformed/high-rate inputs.
            if (m_Pairs.size() > 2048) m_Pairs.pop_front();
        } else m_Pairs.clear();
        m_Frame = frame; m_At = at; m_Slot = slot; m_Required = required;
    }
    bool qualified(uint64_t at) {
        expire(at);
        return m_Pairs.size() >= 2 && coverage() >= 1000000;
    }
    uint64_t coverage() const {
        uint64_t sum = 0;
        for (const auto& p : m_Pairs) sum += p.interval;
        return sum;
    }
    double loss(uint64_t buffer, uint64_t tolerance) const {
        double sum = 0;
        const double delay = double(buffer);
        for (const auto& p : m_Pairs) {
            // Common candidate delay removes the policy's own delay changes.
            // Constant input lateness cancels; both late and catch-up intervals
            // contribute, with exactly the existing per-interval tolerance.
            const double late = std::max(0.0, double(p.current) - delay);
            const double previousLate = std::max(0.0, double(p.previous) - delay);
            sum += std::min(double(p.interval), std::max(0.0,
                std::abs(late - previousLate) - double(tolerance)));
        }
        return coverage() ? sum / double(coverage()) : 0;
    }
    void reset() { m_Pairs.clear(); m_Frame = m_At = m_Slot = 0; m_Required = 0; }
private:
    void expire(uint64_t at) {
        while (!m_Pairs.empty() && at >= m_Pairs.front().at &&
               at - m_Pairs.front().at >= WindowUs) m_Pairs.pop_front();
    }
    std::deque<Pair> m_Pairs;
    uint64_t m_Frame = 0, m_At = 0, m_Slot = 0;
    int64_t m_Required = 0;
};
}
