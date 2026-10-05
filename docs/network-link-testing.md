# Testing PyroWave with a limited Linux receive link

`scripts/moonlight-link-test.py` provides an opt-in network bottleneck on the
Linux client. By default it redirects all incoming IPv4/IPv6 traffic on the
selected interface through a dedicated IFB device with a token-bucket shaper and
a finite FIFO. Video, parity, audio and competing TCP traffic share one rate
budget. Outgoing traffic and non-IP traffic are not limited. No host IP is
needed. Optional `--host IP` restricts the test to incoming UDP from that host.

This tests real packet reassembly, partial-frame handling, decoding and pacing,
unlike reducing the encoder bitrate or delaying already assembled frames.
It does not change Moonlight settings or require rebuilding the application.

List interfaces with `ip -brief link`. On a dual-connected
Deck, use the interface that actually receives the stream, not an assumed
wired interface. End the stream before changing the test setup.

From the repository root, substitute your receiving interface:

```sh
python3 scripts/moonlight-link-test.py start --interface enp4s0f3u1u1 --dry-run
sudo python3 scripts/moonlight-link-test.py start --interface enp4s0f3u1u1
python3 scripts/moonlight-link-test.py status
sudo python3 scripts/moonlight-link-test.py stop
```

The default rate is 1000 Mbps, with a 250,000-byte FIFO (2 ms of service).
Overflow drops packets; no random loss is injected. `--queue-ms` controls
queue capacity rather than adding a fixed delay. `--rate-mbps 950` offers a
more conservative throughput test. A 1000 Mbps packet-byte budget does not
account exactly for Ethernet preamble, inter-frame gap and all wire overhead.

The helper requires `ip`, `tc`, root privileges and kernel IFB, TBF, flower
and mirred support. `ip link add ... type ifb` normally autoloads the IFB module.
It refuses existing ingress/clsact rules and an IFB belonging to another tool.
It preserves the physical interface's outgoing qdisc and rolls back a failed
start. Only one test can be active. Stop removes its ingress hook and IFB;
there is no boot persistence. Do not edit its rules manually during a test.

This is an approximate software bottleneck. TBF permits short bursts and
kernel scheduling, receive offloads and physical NIC behavior can affect packet
timing. It cannot make an existing slower/Wi-Fi connection behave like fast,
wired gigabit, or reproduce a particular NIC/switch's queue. A physical gigabit
link remains the reference for packet-level timing. See the upstream
[TBF documentation](https://github.com/iproute2/iproute2/blob/main/man/man8/tc-tbf.8)
and [IFB redirection example](https://github.com/iproute2/iproute2/blob/main/man/man8/tc-mirred.8).

For optimization, compare the same scene, resolution, frame rate, chroma, HDR
and encoder bitrate with the test off and on. Check `status` for queue/drop
counters and capture packet assembly time, partial frames, GPU synchronization,
queue delay and presentation jerk separately. An increased decode API wall
time is not automatically increased GPU codec execution. Keep each capture
separately identified. The VRR replay begins after reassembly and cannot replay
this packet bottleneck from its existing frame-level trace alone.
