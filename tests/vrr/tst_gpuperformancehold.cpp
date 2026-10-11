#include "assertions.h"
#include "streaming/gpuperformancehold.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

// Builds a sysfs-shaped tree: /class/drm/<node> links to the device's drm
// directory, whose "device" link points back at the PCI device.
static void addGpu(const QString& root, const QString& pci, const QString& driver,
                   const QString& node, const QString& card, const QString& status, const QString& level)
{
    const QString device = root + "/devices/" + pci;
    assert(QDir().mkpath(device + "/drm/" + node));
    assert(QDir().mkpath(device + "/drm/" + card + "/" + card + "-DP-1"));
    assert(QDir().mkpath(root + "/bus/drivers/" + driver));
    assert(QFile::link(root + "/bus/drivers/" + driver, device + "/driver"));
    assert(QFile::link(device, device + "/drm/" + node + "/device"));
    assert(QFile::link(device + "/drm/" + node, root + "/class/drm/" + node));
    QFile statusFile(device + "/drm/" + card + "/" + card + "-DP-1/status");
    assert(statusFile.open(QIODevice::WriteOnly) && statusFile.write(status.toLatin1() + "\n") > 0);
    QFile levelFile(device + "/power_dpm_force_performance_level");
    assert(levelFile.open(QIODevice::WriteOnly) && levelFile.write(level.toLatin1() + "\n") > 0);
}

int main()
{
    QTemporaryDir dir;
    assert(dir.isValid());
    const QString root = dir.path();
    assert(QDir().mkpath(root + "/class/drm"));
    assert(GpuPerformanceHold::findDisplayGpus(root + "/class/drm").empty());

    addGpu(root, "0000:04:00.0", "amdgpu", "renderD128", "card0", "connected", "auto");
    addGpu(root, "0000:03:00.0", "amdgpu", "renderD129", "card1", "disconnected", "auto");
    addGpu(root, "0000:00:02.0", "i915", "renderD130", "card2", "connected", "auto");
    addGpu(root, "0000:05:00.0", "amdgpu", "renderD131", "card3", "connected", "manual");

    // Only amdgpu devices with a connected display are candidates; a level
    // set outside Moonlight is reported so the hold can leave it untouched.
    const auto found = GpuPerformanceHold::findDisplayGpus(root + "/class/drm");
    assert(found.size() == 2);
    assert(found[0].renderNode == "renderD128" && found[0].level == "auto");
    assert(found[0].devicePath.endsWith("/devices/0000:04:00.0"));
    assert(found[1].renderNode == "renderD131" && found[1].level == "manual");

    // A disabled hold never touches the GPU.
    GpuPerformanceHold disabled(false);
    assert(disabled.heldCount() == 0);

    std::puts("GPU performance hold device selection checks passed");
    return 0;
}
