// Compile-only gate: no GPU device access or runtime performance-state changes.
#include "gpuperformancehold_amdgpu.h"
#ifndef HAVE_AMDGPU_STABLE_PSTATE
#error "Linux package must compile AMDGPU stable-pstate support"
#endif
void check_linux_gpu_capability()
{
    union drm_amdgpu_ctx args = {};
    args.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
    args.in.priority = AMDGPU_CTX_PRIORITY_NORMAL;
    args.in.ctx_id = args.out.alloc.ctx_id;
    args.in.op = AMDGPU_CTX_OP_SET_STABLE_PSTATE;
    args.in.flags = AMDGPU_CTX_STABLE_PSTATE_PEAK;
    args.in.op = AMDGPU_CTX_OP_GET_STABLE_PSTATE;
    args.out.pstate.flags &= AMDGPU_CTX_STABLE_PSTATE_FLAGS_MASK;
    args.in.op = AMDGPU_CTX_OP_FREE_CTX;
    (void)DRM_IOCTL_AMDGPU_CTX;
}
