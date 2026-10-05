#include "gpuperformancehold.h"

#include "SDL_compat.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#if defined(__linux__) && __has_include(<drm/amdgpu_drm.h>)
#define HAVE_AMDGPU_STABLE_PSTATE
#include <drm/amdgpu_drm.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

QString readLine(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromLatin1(file.readLine()).trimmed();
}

QStringList entries(const QString& path, const QString& pattern)
{
    // sysfs class entries are symlinks; System keeps them in the listing.
    return QDir(path).entryList({pattern}, QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

bool hasConnectedConnector(const QString& devicePath)
{
    const QString drm = devicePath + "/drm";
    for (const QString& card : entries(drm, "card*")) {
        if (card.contains('-')) continue;
        for (const QString& connector : entries(drm + "/" + card, card + "-*")) {
            if (readLine(drm + "/" + card + "/" + connector + "/status") == "connected") return true;
        }
    }
    return false;
}

#ifdef HAVE_AMDGPU_STABLE_PSTATE
int contextOp(int fd, union drm_amdgpu_ctx& args)
{
    int result;
    do {
        result = ioctl(fd, DRM_IOCTL_AMDGPU_CTX, &args);
    } while (result < 0 && (errno == EINTR || errno == EAGAIN));
    return result < 0 ? errno : 0;
}
#endif

}

std::vector<GpuPerformanceHold::Candidate> GpuPerformanceHold::findDisplayGpus(const QString& drmClassDir)
{
    std::vector<Candidate> found;
    for (const QString& node : entries(drmClassDir, "renderD*")) {
        const QString device = QFileInfo(drmClassDir + "/" + node + "/device").canonicalFilePath();
        if (device.isEmpty() || QFileInfo(device + "/driver").canonicalFilePath().section('/', -1) != "amdgpu") continue;
        if (!hasConnectedConnector(device)) continue;
        found.push_back({node, device, readLine(device + "/power_dpm_force_performance_level")});
    }
    return found;
}

GpuPerformanceHold::GpuPerformanceHold(bool enabled)
{
    if (!enabled) {
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "GPU performance hold: off");
        return;
    }

#ifdef HAVE_AMDGPU_STABLE_PSTATE
    for (const auto& gpu : findDisplayGpus("/sys/class/drm")) {
        const QByteArray name = gpu.renderNode.toLatin1();
        if (gpu.level != "auto") {
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "GPU performance hold: %s left at externally set level '%s'",
                        name.constData(), gpu.level.toLatin1().constData());
            continue;
        }

        const int fd = open(("/dev/dri/" + name).constData(), O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GPU performance hold: cannot open %s: %s",
                        name.constData(), strerror(errno));
            continue;
        }

        union drm_amdgpu_ctx args = {};
        args.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
        args.in.priority = AMDGPU_CTX_PRIORITY_NORMAL;
        int error = contextOp(fd, args);
        const unsigned int context = args.out.alloc.ctx_id;
        if (!error) {
            args = {};
            args.in.op = AMDGPU_CTX_OP_SET_STABLE_PSTATE;
            args.in.ctx_id = context;
            args.in.flags = AMDGPU_CTX_STABLE_PSTATE_PEAK;
            error = contextOp(fd, args);
        }
        if (!error) {
            args = {};
            args.in.op = AMDGPU_CTX_OP_GET_STABLE_PSTATE;
            args.in.ctx_id = context;
            error = contextOp(fd, args);
            if (!error && (args.out.pstate.flags & AMDGPU_CTX_STABLE_PSTATE_FLAGS_MASK) != AMDGPU_CTX_STABLE_PSTATE_PEAK) {
                error = EPERM;
            }
        }
        if (error) {
            // EBUSY: another process holds a stable pstate. EINVAL: kernel
            // predates the request (5.19). Closing the node frees the context.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "GPU performance hold: %s request failed: %s",
                        name.constData(), strerror(error));
            close(fd);
            continue;
        }

        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GPU performance hold: %s pinned at '%s' for this stream",
                    name.constData(),
                    readLine(gpu.devicePath + "/power_dpm_force_performance_level").toLatin1().constData());
        m_Holds.push_back({fd, context, gpu.renderNode});
    }
#endif
}

GpuPerformanceHold::~GpuPerformanceHold()
{
#ifdef HAVE_AMDGPU_STABLE_PSTATE
    for (const auto& hold : m_Holds) {
        // Freeing the context restores the level that was active when it was
        // created. A level written through sysfs meanwhile takes ownership and
        // is not overridden.
        union drm_amdgpu_ctx args = {};
        args.in.op = AMDGPU_CTX_OP_FREE_CTX;
        args.in.ctx_id = hold.context;
        contextOp(hold.fd, args);
        close(hold.fd);
        SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                    "GPU performance hold: %s released",
                    hold.renderNode.toLatin1().constData());
    }
#endif
}
