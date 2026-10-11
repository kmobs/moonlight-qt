#pragma once

#include <cstdint>

// Decoder-thread observer of unrecovered payload holes, including partial
// frames that the transport's whole-frame loss warning counts as delivered.
class PyroWavePacketLossWarning {
public:
    static constexpr const char* Message =
        "Severe packet loss detected\nReduce bitrate to prevent shimmering";

    bool observe(uint64_t nowUs, bool active, uint64_t totalPackets, uint64_t lostPackets)
    {
        if (!active || (m_LastUs && (nowUs <= m_LastUs || nowUs - m_LastUs > 2500000))) {
            *this = {};
        }
        if (!active) return false;
        m_LastUs = nowUs;
        if (!totalPackets) return m_Visible;
        if (!m_Packets) m_WindowStartUs = nowUs;
        if (nowUs - m_WindowStartUs >= 3000000) {
            // Match the transport warning's 3-second windows and 30%/15%/5%
            // thresholds, applied to unrecovered video data packets. Sum counts
            // across frames so a tiny hole in a large frame is not severe loss.
            const double lossPercent = 100.0 * m_LostPackets / m_Packets;
            if (lossPercent >= 30.0 || (lossPercent >= 15.0 && m_PreviousLossPercent >= 15.0))
                m_Visible = true;
            else if (lossPercent <= 5.0)
                m_Visible = false;
            m_PreviousLossPercent = lossPercent;
            m_WindowStartUs = nowUs;
            m_Packets = m_LostPackets = 0;
        }
        m_Packets += totalPackets;
        m_LostPackets += lostPackets;
        return m_Visible;
    }

private:
    uint64_t m_LastUs = 0, m_WindowStartUs = 0;
    uint64_t m_Packets = 0, m_LostPackets = 0;
    double m_PreviousLossPercent = 0;
    bool m_Visible = false;
};
