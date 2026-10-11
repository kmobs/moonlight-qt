#!/bin/bash
set -euo pipefail
runtime=$(mktemp -d)
chmod 700 "$runtime"
export XDG_RUNTIME_DIR="$runtime"
export WAYLAND_DISPLAY=moonlight-nightly-ci
export QT_QPA_PLATFORM=wayland
weston --backend=headless-backend.so --use-pixman --no-config --idle-time=0 \
  --socket="$WAYLAND_DISPLAY" --log="$runtime/weston.log" > "$runtime/weston-output.log" 2>&1 &
compositor=$!
trap 'kill "$compositor" 2>/dev/null || true; wait "$compositor" 2>/dev/null || true' EXIT
for attempt in $(seq 1 40); do
  test ! -S "$runtime/$WAYLAND_DISPLAY" || break
  kill -0 "$compositor" || { cat "$runtime/weston.log" "$runtime/weston-output.log"; exit 1; }
  sleep 0.25
done
test -S "$runtime/$WAYLAND_DISPLAY"
# A private headless compositor checks plugin/protocol startup, not hardware HDR/VRR.
timeout 30 "$@"
