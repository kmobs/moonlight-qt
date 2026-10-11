#pragma once

#include "../../app/streaming/video/pyrowave/pyrowavesurfaces.h"
#include <memory>

class PyroWaveDecoder;

struct MetalPyroWaveFixture {
    AVFrame* frame = nullptr;
    // Keep submitted decode work asynchronous until the real presenter waits.
    std::unique_ptr<PyroWaveDecoder> decoder;
    ~MetalPyroWaveFixture();
};

// Encodes a test pattern on the GPU, frames the real bitstream and decodes into
// the presenter's shared pool. The returned frame has no CPU plane data.
std::unique_ptr<MetalPyroWaveFixture> decodeMetalPyroWaveFixture(
    IPyroWaveVulkanPool* pool, bool chroma444, bool tenBit);
