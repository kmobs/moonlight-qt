#pragma once

#include <cstdint>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
}

class PyroWaveMetalPool;

// Native, headless equivalent of the stream's Metal video pass. No window or
// display link is created by calibration.
class PyroWaveMetalCalibratorRenderer
{
public:
    PyroWaveMetalCalibratorRenderer();
    ~PyroWaveMetalCalibratorRenderer();
    bool create(int displayWidth, int displayHeight);
    bool prepare(int width, int height, bool chroma444, bool hdr);
    bool present(AVFrame* frame, bool hdr);
    PyroWaveMetalPool* pool() const;

    // Last completed native target pixel, for the headless GPU smoke test.
    bool readLastPixel(uint32_t& pixel) const;

private:
    struct State;
    std::unique_ptr<State> m_State;
};
