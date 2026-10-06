#pragma once

#include <cstdint>

// Decoder-thread observer of unrecovered payload holes, including partial
// frames that the transport's whole-frame loss warning counts as delivered.
class PyroWavePacketLossWarning {
public:
    static constexpr const char* Message =
        "Severe packet loss detected\nReduce bitrate to prevent shimmering";

    bool observe(uint64_t nowUs, bool active, bool partial)
    {
        if (!active || (m_LastUs && (nowUs <= m_LastUs || nowUs - m_LastUs > 2500000))) {
            *this = {};
        }
        if (!active) return false;
        m_LastUs = nowUs;
        if (!m_Frames) m_WindowStartUs = nowUs;
        if (nowUs - m_WindowStartUs >= 3000000) {
            // Match the transport warning's 3-second windows and 30%/15%/5%
            // thresholds, applied to frames with any unrecovered detail loss.
            const double lossPercent = 100.0 * m_PartialFrames / m_Frames;
            if (lossPercent >= 30.0 || (lossPercent >= 15.0 && m_PreviousLossPercent >= 15.0))
                m_Visible = true;
            else if (lossPercent <= 5.0)
                m_Visible = false;
            m_PreviousLossPercent = lossPercent;
            m_WindowStartUs = nowUs;
            m_Frames = m_PartialFrames = 0;
        }
        ++m_Frames;
        m_PartialFrames += partial;
        return m_Visible;
    }

private:
    uint64_t m_LastUs = 0, m_WindowStartUs = 0;
    uint64_t m_Frames = 0, m_PartialFrames = 0;
    double m_PreviousLossPercent = 0;
    bool m_Visible = false;
};
