# GPU power and video thread priority

## High-performance GPU power while streaming

Settings > **High-performance GPU power while streaming** (Linux, default off)
holds AMD GPUs that drive a connected display at a fixed high-performance level
for the whole stream. Demand-based clock scaling otherwise lowers GPU and memory
clocks between the short bursts of decode, render and composition work, and the
ramp back up can delay when the display shows a frame that Moonlight submitted
on time. In the stats graph this looks like a jumpy **Display events** lane
while **Planned cadence** and **Client submissions** stay flat. More VRR buffer
does not help, because the delay happens after submission.

Moonlight uses the amdgpu context stable-pstate request (`PEAK`). It needs no
sudo, works from Flatpak and distrobox, and the kernel restores the previous
level when the stream ends or the process exits. On the Steam Deck it fixes the
GPU at 1300 MHz and the memory fabric at its 800 MHz maximum. It costs battery
life while streaming.

If GPU clocks were already set elsewhere (for example Steam's manual GPU clock,
which writes `power_dpm_force_performance_level`), Moonlight leaves them alone.
Look for `GPU performance hold:` in the session log:

- `pinned at 'profile_peak' for this stream`: active.
- `left at externally set level ...`: another setting owns the GPU level.
- `request failed: Device or resource busy`: another process holds a stable pstate.
- `off`: the setting is disabled.

Windows, NVIDIA and Intel have no equivalent unprivileged per-process request,
so the setting is Linux/AMD only. GPU *scheduling* priority (Vulkan global
priority, EGL context priority, DXGI GPU thread priority) only orders work
between GPU contexts and does not change clocks, so Moonlight does not request it.

## Video thread scheduling

The decoder, dedicated renderer, V-sync, VRR pacer and optional Vulkan
preparation threads request scheduling priority once when starting. Windows
uses the OS MMCSS `Playback` task, with HIGH relative priority for pacing/V-sync
and NORMAL for decode/render/preparation. It unregisters each thread when it
exits. If MMCSS is unavailable, SDL is the fallback. Other platforms use SDL
directly: HIGH for video work, TIME_CRITICAL then HIGH on rejection for
pacing/V-sync. Failure retains available scheduling and is recorded as
`Video thread priority:` in the log. Requests can be denied by OS permissions.
Main-thread rendering and internal codec workers retain their existing
scheduling; no process priority or affinity is changed.

The final VRR deadline region uses SDL's CPU pause hint instead of an OS
scheduler yield. Coarse sleeping, the 500 us base region plus at most 500 us
learned wake lead, clock-stall escape, targets and buffer policy remain intact.
This trades a bounded amount of CPU time for fewer voluntary deschedules near
submission. Trace `active_yield_count` fields retain their names but count
polling steps. It cannot remove involuntary preemption, driver/compositor
delays or GPU clock ramping.

References: [amdgpu context ioctl](https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/amd/amdgpu/amdgpu_ctx.c),
[SDL thread priority](https://wiki.libsdl.org/SDL2/SDL_SetThreadPriority),
[Windows MMCSS](https://learn.microsoft.com/en-us/windows/win32/procthread/multimedia-class-scheduler-service).
