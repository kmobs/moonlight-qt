#pragma once

#include <QString>

#include <vector>

// Pins the GPUs that drive a connected display at a fixed performance level
// for the lifetime of the object. Demand-based clock scaling drops the GPU
// and memory fabric to their floor between the short bursts of decode, render
// and composition work in a stream; ramping back up delays the compositor's
// presentation of frames that were submitted on time.
//
// AMD on Linux exposes this per process through the amdgpu context
// stable-pstate request. It needs no privileges, applies only while the
// requesting context is open, and the kernel restores the previous level when
// the context is freed, including when the process dies. An externally chosen
// level (anything other than "auto") is left untouched.
class GpuPerformanceHold
{
public:
    explicit GpuPerformanceHold(bool enabled);
    ~GpuPerformanceHold();

    GpuPerformanceHold(const GpuPerformanceHold&) = delete;
    GpuPerformanceHold& operator=(const GpuPerformanceHold&) = delete;

    struct Candidate {
        QString renderNode;     // e.g. "renderD128"
        QString devicePath;     // canonical sysfs device directory
        QString level;          // power_dpm_force_performance_level
    };

    // amdgpu render nodes whose device has at least one connected connector.
    static std::vector<Candidate> findDisplayGpus(const QString& drmClassDir);

    int heldCount() const { return int(m_Holds.size()); }

private:
    struct Hold {
        int fd;
        unsigned int context;
        QString renderNode;
    };
    std::vector<Hold> m_Holds;
};
