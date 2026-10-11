#pragma once

#include "pyrowavesurfaces.h"
#include <memory>

// MoltenVK writes the exact Metal textures sampled by the native presenter.
// Each frame owns the shared device/image state until its final GPU read ends.
class PyroWaveMetalPool : public IPyroWaveVulkanPool
{
public:
    PyroWaveMetalPool();
    ~PyroWaveMetalPool() override;
    bool initialize(void* metalDevice);
    bool pyroWaveVulkanDevice(PyroWaveVulkanDevice& device) override;
    bool holdPyroWaveSurface(int width, int height, bool chroma444, bool sixteenBit,
                            PyroWaveVulkanSurface& surface) override;
    bool releasePyroWaveSurface(const PyroWaveVulkanSurface& surface, bool decoded,
                               AVFrame* frame) override;
    bool ownsFrame(const AVFrame* frame) const;
    bool mapFrame(const AVFrame* frame, void* textures[3]) const;
    // Waits only for this frame; no renderer/pool/queue lock is held.
    VkResult waitForFrame(const AVFrame* frame, uint64_t timeoutNs) const;

private:
    struct State;
    struct FrameRef;
    static void freeFrameRef(void*, uint8_t* data);
    static void lockQueues(void* state);
    static void unlockQueues(void* state);
    const FrameRef* frameRef(const AVFrame* frame) const;
    std::shared_ptr<State> m_State;
};
