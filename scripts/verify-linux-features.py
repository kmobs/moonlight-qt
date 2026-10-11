"""Inspect actual Linux payload capabilities without touching GPU state."""
import argparse
import ctypes
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('--root', type=Path, required=True)
parser.add_argument('--qt-plugins', type=Path)
parser.add_argument('--host-wayland', action='store_true')
args = parser.parse_args()
root = args.root
binary = root / 'usr/bin/moonlight' if (root / 'usr/bin/moonlight').exists() else root / 'bin/moonlight'
payload = binary.read_bytes()
assert b'Wayland presentation feedback enabled for adaptive buffering' in payload, 'Native feedback missing'
assert b'GPU performance hold: unavailable in this build' not in payload, 'AMDGPU capability compiled out'
assert b'does not support HDR10 (ST.2084 PQ)' in payload, 'Vulkan HDR negotiation code missing'
plugin_roots = [root] + ([args.qt_plugins] if args.qt_plugins else [])
# Qt 6.10 moved Wayland QPA into qtbase and builds the unified qwayland plugin.
# Earlier matched Qt builds provide separate generic/EGL platform plugins.
unified = any(list(p.rglob('libqwayland.so')) for p in plugin_roots)
legacy = all(any(list(p.rglob(name)) for p in plugin_roots)
             for name in ('libqwayland-egl.so', 'libqwayland-generic.so'))
assert unified or legacy, 'Matching Qt Wayland platform plugin missing'
sdl = next((root / 'usr/lib').glob('libSDL3.so.0'), None) or next((root / 'lib').glob('libSDL3.so.0'))
library = ctypes.CDLL(str(sdl.resolve()))
library.SDL_GetVideoDriver.argtypes = [ctypes.c_int]
library.SDL_GetVideoDriver.restype = ctypes.c_char_p
drivers = [library.SDL_GetVideoDriver(i).decode() for i in range(library.SDL_GetNumVideoDrivers())]
assert 'wayland' in drivers, f'SDL native Wayland driver missing: {drivers}'
if args.host_wayland:
    assert not list(root.rglob('libwayland-*.so*')), 'Bundled libwayland shadows host Mesa ABI'
    assert not list(root.rglob('libEGL*.so*')), 'Bundled EGL shadows host Mesa ABI'
    ctypes.CDLL('libEGL_mesa.so.0')
print(json.dumps(dict(sdl_video_drivers=drivers, moonlight_feedback=True, qt_wayland_plugins=True,
                     amdgpu_capability=True, hdr_negotiation_code=True,
                     host_wayland_egl=args.host_wayland,
                     hardware_vrr_hdr_validated=False), indent=2))
