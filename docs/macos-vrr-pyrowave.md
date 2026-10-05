# Mac client: VRR and PyroWave

Source baseline: `41d9df5c` plus the drawable-backpressure correction (2026-10-04). This is a native
macOS client build. Windows release and ChaseShare deployment procedures remain
separate. See [architecture.md](../architecture.md) for ownership, clocks, and
the shared timing policy.

## Using the client

- Enable **V-Sync** and **VRR**, with the renderer set to **Auto**. Playback and
  the matching decoder probe select native Metal. A VRR session enters native
  macOS fullscreen/Spaces even if a different window mode was saved.
- Choose an FPS no higher than the actual screen maximum. The built-in screen
  on this Apple M5 Pro reports 120 Hz; 120, 116, and 100 FPS are available through
  the ordinary/native and VRR recommendation choices. A selected FPS does not
  establish that the host or GPU sustains it.
- Select **PyroWave** to use a host that advertises the matching codec contract.
  Enable YUV 4:4:4 or HDR only when wanted. PyroWave and VRR are independent:
  ordinary VideoToolbox codecs can use VRR, and PyroWave can use fixed pacing.
- Reconnect after changing timing settings or connecting/moving to another
  display. Each connection checks the window's actual native refresh range;
  unsupported/fixed displays use fixed pacing. External-display operation still
  needs its own live validation.
- For a capture, enable **Trace VRR frames for debugging** before connecting.
  Completed recordings and exports live under `Desktop/vrr-diagnostics`.

The local built-in ProMotion range is 8.333–41.667 ms, with 4.167 ms update
granularity. ProMotion supports discrete refresh choices; it does not promise
that every CPU deadline becomes an identical physical refresh interval.
The Metal presenter keeps display synchronization enabled and calls
[`presentAfterMinimumDuration:`](https://developer.apple.com/documentation/metal/mtldrawable/present(afterminimumduration:)?language=objc)
with the native minimum interval. This constrains the previous drawable's
visible duration. Drawable `presentedTime` callbacks supply OS presentation
evidence, separately from submission timing and physical panel measurements.
The synchronized layer uses three drawable resources so the displayed image
and its submitted successor do not block preparation of the next frame.
The shared worker still controls admission and the submission target.

## PyroWave GPU path

PyroWave decodes through the bundled MoltenVK driver into up to eight shared
R8/R16 plane surfaces. Native Metal samples the same exported textures, without
CPU readback/upload. Device matching, timeline semaphores, subgroup support and
Metal-object export must all qualify. A failed shared-device setup fails Mac
PyroWave initialization. Logs identify the selected Metal device and codec path.

The frame's Vulkan completion is observed before rendering, with a 50 ms wait
bound. Metal retains each source frame until its draw completes. Exhausting
the surface pool drops a frame. These bounds protect progress and ownership;
they do not make an overloaded GPU sustain the requested stream rate.

**Calibrate PyroWave** uses the existing paired-host UDP/wire-budget checks,
then a separate **Next** step starts local GPU measurements for 4K, 1440p, 1080p and 720p in
4:2:0/4:4:4 and 8/10-bit formats. The Mac check decodes to shared planes and
draws the native Metal conversion shader into a target at the selected display's
pixel size. It creates no window or display link. Decode, draw and a one-pixel
completion readback run serially, with the conservative 75% frame-period budget
used by the existing serial grading path. A grade measures this local workload
and the UDP probe; it does not establish sustained gameplay or display cadence.
The first step selects Minimum, Recommended, Moderate or Maximum. Minimum uses
half the image-quality guide; Recommended uses the full guide. Moderate uses
60% of the confirmed wire budget; Maximum uses the full budget. The network
search begins at its bounded ceiling, and the GPU test starts at the selected
target instead of stepping upward from a lower rate.
Stop streaming from the selected host before calibrating. The host refuses
bandwidth tests during an active stream; Moonlight displays its rejection reason
instead of the generic HTTP "Bad Request" message.
Hostname connections, including `.local` discovery names, are resolved before
opening the UDP receiver. The HTTPS probe and UDP source check use the same
resolved IP. Failed resolution reports its cause instead of a generic socket
error.
On this Mac, UDP delivery timing uses kernel packet-arrival timestamps. The
application's socket-read delay is reported separately, so an event-loop stall
does not become apparent network jitter. A failed test names its actual failed
condition and shows the measured packet counts and timing; receiving packets
with excessive timing variation is distinguished from receiving no UDP at all.
Delivery variation above 4 ms produces a network timing warning. It does not
skip local format tests when the measured throughput passes loss, queue-growth
and sender-duration limits plus two fresh confirmations. Format grades describe
local decode/draw capacity; a warned connection may still stutter in use.

## Building on this Mac

The validated toolchain is native arm64 with Apple command-line tools and Qt
6.11.1 at `build/vrr-hybrid/qt/6.11.1/macos`. The existing `libs/mac` prebuilts
supply SDL, FFmpeg, libplacebo, MoltenVK, and the other runtime libraries.
The ordinary Mac prebuilt configuration enables PyroWave; `disable-pyrowave`
disables it. Custom `disable-prebuilts` builds do not automatically enable the
Mac codec path.

From the repository root:

```sh
mkdir -p build/mac-client
cd build/mac-client
../vrr-hybrid/qt/6.11.1/macos/bin/qmake ../../moonlight-qt.pro \
    QMAKE_APPLE_DEVICE_ARCHS=arm64 CONFIG+=release CONFIG-=debug
make -j8 release
```

The linked application is `build/mac-client/app/Moonlight.app`. Back at the
repository root, stage a launchable local bundle with its Qt/runtime libraries:

```sh
python3 scripts/stage-macos-build.py
```

To stage an update while the current staged app is running, use a separate
directory, for example `--deploy-dir build/mac-deploy-udpfix`.

The [staging helper](../scripts/stage-macos-build.py) runs `ditto` and
`macdeployqt`, excludes the unused Mimer SQL plugin that otherwise needs an
external database library, and signs nested Mach-O files, frameworks and the
application in order. It verifies strict/deep signatures, bundled dependencies
and Cocoa `--help` with runtime-library/plugin overrides removed. Logs are in
`build/mac-deploy`.

The staged app is `build/mac-deploy/Moonlight.app`, with an ad-hoc signature for
local use. The helper refuses to overwrite that bundle while its Moonlight
process is running. Changing `HAVE_PYROWAVE` requires cleaning app objects before
rebuilding because it changes `FFmpegVideoDecoder` layout. See the architecture's
build warning.

The original common-library gitlink `d1ed147` was unavailable from its remote.
The submodule now pins fork revision `9ab9949` (branch `mac-pyrowave`), based on
published PyroWave revision `3bbe8e8` with the minimal `pyrowaveLinkMbps`
field/SDP repair. That repair is also recorded in
[`moonlight-common-c/patches/pyrowave-link-mbps.patch`](../moonlight-common-c/patches/pyrowave-link-mbps.patch).
The pinned revision already includes the repair; no manual patch application
is needed for a fresh recursive checkout.

## Validation boundaries

Initial port validation on this Apple M5 Pro passed 17 deterministic VRR suites
and seven CPU PyroWave/haptics suites. Native presenter, shared-GPU reconstruction and headless calibration
checks are separate from those hardware-free regressions.

The staged arm64 app passes Cocoa `--help` with `DYLD_LIBRARY_PATH` unset and
strict/deep signature verification. Its 104 Mach-O files have no missing or
external non-system dependencies; the executable's runtime path points at the
bundle's `Contents/Frameworks`. This validates a standalone local bundle.
Visible QML/Cocoa startup also passes with Metal/VideoToolbox initialization in
an isolated portable test directory.

The platform-neutral VRR suites, CPU PyroWave protocol/framing/receive suites,
and haptics suite run with `DYLD_LIBRARY_PATH` pointing at `libs/mac/lib`.
The opt-in GPU suite must use `--require-gpu` so unavailable GPU work fails
instead of silently skipping. Build/run instructions and reconstruction coverage
are in [tests/pyrowave/README.md](../tests/pyrowave/README.md).

Metal replay fixtures use backend 4, local serials, delayed `DisplayEvent`
feedback, and command-completion brackets. They pass exact baseline replay;
the audit rejects 11 corruptions of native/GPU evidence. The strict DXGI raster
gate is unavailable on Metal. See [VRR tests](../tests/vrr/README.md).

The actual native presenter smoke exercises software and VideoToolbox textures,
source recycling, cancellation, suspension, fullscreen qualification, and fixed
fallback. A separate native worker smoke submits 100 synthetic frames at
116 FPS on the built-in screen, with no worker drops and 97 matched native
intervals; its clean-close schema-5 trace passes exact replay. This exercises
the renderer/worker path without a host, network, or game.

A combined native smoke additionally encodes and decodes 128×96 PyroWave
4:2:0 8-bit and 4:4:4 10-bit patterns, passing their asynchronous shared frames
directly to the real Metal renderer. Cancellation, reprepare, source recycling,
decoder teardown and adaptive presentation pass; a 4:4:4 10-bit shared frame
also passes fixed display-link presentation. Its latest worker trace passes
exact replay. This is a small-image integration check.

Native GPU round trips pass 12 shared-output cases through 4K and the built-in
3024×1964 geometry, comparing shared Metal planes byte for byte with Vulkan
readback. Coverage includes bit depths/chroma, poisoning/reuse, pool exhaustion,
completion timelines, and retained lifetimes. These checks establish decode
and interop correctness. They do not establish live PyroWave throughput,
gameplay smoothness, physical HDR/tearing behavior, or an external display's VRR.
Those require a fresh stream/capture on the intended display.

The headless calibration smoke passes eight source/chroma/HDR combinations
through 1080p into a 1440p target, for 288 native draws. It checks converted
pixel/color/alpha output, three-plane bindings, Rec.601/BT.2020 matrices and
changing image content. The target is
[`metalcalibration.pro`](../tests/pyrowave/metalcalibration.pro), executable
`tst_pyrowavemetalcalibration`; it uses the same bundled-library environment as
the round-trip test. A complete host/network calibration sweep has not been run.
Runtime validation is arm64; the codec static library cross-compiles for x86_64,
but Intel runtime behavior has not been tested.

The first live external LG TV test at 4K90 PyroWave 4:4:4 10-bit rendered only
71.02 FPS and dropped 21.05% of frames through client pacing. Its two-resource
layer spent 9.01 ms on average acquiring drawables. A host-free 600-frame test
reproduced the resource starvation; three drawable resources restored all 600
submissions with zero drops and passed exact replay. Native display timestamps
still showed an 8.33/16.67 ms grid instead of uniform 11.11 ms intervals.
This fixes the reproduced resource bottleneck; external adaptive cadence fails
the native gate. The user confirmed Variable/Adaptive mode is already selected
and the TV remains near 118 FPS. The live capture's strict replay
gate also rejects five queue-depth semantic errors despite exact timing replay;
it cannot establish strict A/B proof.
The drawable-fix validation passes the combined native PyroWave/Metal smoke,
exact native replay, bundle signature/dependency/help checks, and 14 of the 15
hardware-free suites in `build/mac-tests/vrr`. The unchanged worker suite fails
queue-overflow and trace lifecycle expectations on repeated runs; this is not
a completely green regression result. Investigation artifacts and the cadence
failure are retained in `build/mac-vrr-first-live`.

Follow-up 90 FPS tests explicitly requested a display-link frame rate and a
fitted 11.111 ms Metal minimum duration. Both still failed cadence matching and
were reverted. A standalone Cocoa/Metal display-link control also ran at the
120 Hz callback cadence despite its 90 FPS preference. These are investigation
results, not a completed fix or evidence that VRR was disabled in settings.
Direct versus composited Metal presentation remains to be established.

The continuation found an environmental limit: the Metal HUD reported Low Power
Mode, composited presentation, and about 60 presented FPS during a 90 FPS request.
For cadence checks on this Mac, use Automatic power mode and keep native
fullscreen focused; an advertised 40-120 Hz range does not prove that the current
power policy allows that rate. Initialization now logs Low Power Mode and warns
when it is enabled. Moonlight does not change the system power setting.

After the user selected Automatic power mode, a settled, focused native smoke
presented all 900 frames with zero drops and passed exact replay. Display events
varied around 11.11 ms, and the user observed the TV's numerical refresh rate
vary toward 90 Hz in the native control. The strict uniform-cadence gate still
failed (44.71% within 500 us, 2.111 ms p95 error); it must not be interpreted as a
binary VRR-enabled detector. Passing the source period to Metal gave similar
results and was reverted. Live streaming and physical cadence remain separate
validation steps.

The continuation rebuilt the app and diagnostics, passed all 17 hardware-free
regression suites plus replay CLI startup, then reran the final production
native smoke: 900/900 frames, zero drops, no focus loss, Low Power Mode off, and
exact replay passed. The final strict cadence gate still failed (46.40% within
500 us, 2.340 ms p95 interval error). The staged test bundle passed signature,
dependency, and isolated help checks. These results establish native resource
throughput and recorded execution integrity, not perfectly even physical scanout.
