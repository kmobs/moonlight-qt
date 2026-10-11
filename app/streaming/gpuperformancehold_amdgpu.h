#pragma once

// The header exists on older distributions before stable-pstate operations and
// drm_amdgpu_ctx::out.pstate were added. Presence alone is not capability.
#if defined(__linux__)
#if __has_include(<libdrm/amdgpu_drm.h>)
// Prefer userspace libdrm UAPI over an older linux-libc-dev drm/ header.
#include <libdrm/amdgpu_drm.h>
#elif __has_include(<amdgpu_drm.h>)
// pkg-config libdrm supplies this include directory on Debian/Ubuntu.
#include <amdgpu_drm.h>
#elif __has_include(<drm/amdgpu_drm.h>)
#include <drm/amdgpu_drm.h>
#endif
#if defined(DRM_IOCTL_AMDGPU_CTX) && \
    defined(AMDGPU_CTX_OP_ALLOC_CTX) && \
    defined(AMDGPU_CTX_OP_FREE_CTX) && \
    defined(AMDGPU_CTX_PRIORITY_NORMAL) && \
    defined(AMDGPU_CTX_OP_SET_STABLE_PSTATE) && \
    defined(AMDGPU_CTX_OP_GET_STABLE_PSTATE) && \
    defined(AMDGPU_CTX_STABLE_PSTATE_PEAK) && \
    defined(AMDGPU_CTX_STABLE_PSTATE_FLAGS_MASK)
#define HAVE_AMDGPU_STABLE_PSTATE
#endif
#endif
