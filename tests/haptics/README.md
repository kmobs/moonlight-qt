# DualSense waveform validation

Requires SDL 2.24 or newer and the repository's pinned moonlight-common-c
revision (including `ControllerHaptics.h`). Linux:

```sh
mkdir -p build/tests-haptics
cd build/tests-haptics
qmake6 ../../tests/haptics/haptics.pro
make -j10
./tst_dualsensehaptics
```

Use the same `PKG_CONFIG_PATH` and runtime `LD_LIBRARY_PATH` as the application
if it links a local SDL build. On Windows, from an x64 Native Tools prompt with
Qt's `bin` directory on PATH:

```bat
mkdir build\tests-haptics
cd build\tests-haptics
qmake ..\..\tests\haptics\haptics.pro CONFIG+=release
nmake
set "PATH=..\..\libs\windows\lib\x64;%PATH%"
release\tst_dualsensehaptics.exe
```

The opt-in `tests/tests.pro` tree also builds this suite, and Windows CI runs it.
The test uses a recording HID output and a virtual controller; it does not open
a physical controller. It exercises the production worker and resampler, checks
both channels and the Bluetooth CRC, bounded queue behavior, packet loss and
duplicates (including sequence wrap and late packets during silent audio),
continuous-silence rumble coexistence, idle/removal silence, write failure and
SDL trigger payload dispatch.
Some sdl2-compat versions invert virtual effect callback return values; that
check asserts the dispatched bytes, not the virtual driver's return status.

For real playback, use the coordinated Vibeshine and Moonlight builds. Pair the
DualSense/Edge to the Windows or Linux **client**. On a Windows host, install
Vibeshine's optional **DualSense USB audio and haptics** component and enable
**DualSense waveform haptics** for the application, or select **DualSense with
waveform haptics** (`usbip_ds5`) globally. On a Linux host, select DS5 (or automatic
PlayStation emulation). Reconnect the stream after changing the host backend.
The Windows host's wired USB identity is independent of the client's Bluetooth
connection: USB/IP runs locally on the host; Moonlight receives actuator samples
through its encrypted control connection and needs no USB/IP driver on the client.
The client log must contain `DualSense Bluetooth waveform backend ready` and the
controller-arrival capabilities must include `0x8000`. Linux USB connections and
unsupported clients keep conventional rumble. The game must send native haptic
PCM to the host virtual controller's audio endpoint. Windows uses its
built-in HID driver and the exact device path owned by SDL; no controller-name
matching or virtual Xbox translation is used. A controller hidden from Moonlight
by another input mapper will not expose native waveform/trigger support.

Check distinct left/right native effects while moving both sticks and using
adaptive triggers; stop the effect, pause/end the stream, disconnect/reconnect
Bluetooth and repeat. Verify an older client still receives rumble. A working
backend log or passing recording-output test alone is not physical haptics validation.

Specifically verify that LEDs and repeated adaptive-trigger changes do not
interrupt a sustained waveform, and that input, conventional rumble after PCM
idles, reconnects, multiple controllers and stream exit still work. Check the
log for an adaptive-trigger `rc` failure or `DualSense waveform output failed`.
Also keep the game's controller audio endpoint open while it submits silent PCM;
ordinary rumble must resume once the nonzero effect has drained. Bluetooth
waveforms use 3 kHz signed 8-bit stereo; Windows client USB preserves the 48 kHz
sample rate. The transports therefore have different waveform fidelity.

The Windows transport follows Microsoft's [continuous HID output guidance](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/sending-hid-reports)
and the maximum-report padding used by [SDL's Windows HID backend](https://github.com/libsdl-org/SDL/blob/SDL2/src/hidapi/windows/hid.c).
The wire payload and its CRC remain the pinned SAxense adaptation.

`moonlight --haptics-license` prints the embedded source and notices; see
[`PROVENANCE.md`](../../third-party/saxense/PROVENANCE.md) for the pinned source,
licenses and distribution requirements.

## Windows USB waveform playback

A USB DualSense/Edge uses its own four-channel WASAPI playback endpoint, matched
by the exact SDL HID device container. It uses inbox Windows drivers and sends
actuator PCM only to channels 3/4, keeping channels 1/2 silent. The log must show
`DualSense USB waveform backend ready ... (WASAPI)`. A stereo endpoint is rejected;
keep the controller playback device in its default four-channel format.

Validate USB separately on Windows hardware using the left/right native effects,
input, adaptive-trigger, idle, disconnect/reconnect and stream-exit checks above.
Confirm the headset channels remain silent. The deterministic recording-output
test exercises Bluetooth and the shared receive queue; it does not validate
WASAPI discovery, engine buffering or physical USB feedback. USB hardware
validation is pending.
