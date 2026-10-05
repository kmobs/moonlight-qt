The optional benchmark uses the real PyroWave GPU encoder to prepare inputs,
then times native framing, independent LZ4 compression and client expansion with
exact coefficient-byte comparison. It reports CPU stages and modeled gigabit wire
time separately; GPU encode/decode, capture, parity calculation and presentation
are excluded. Previous Hybrid/XOR/reference-cache measurements do not apply.

Build tests/pyrowave/benchmark.pro in the moonlight-dev container, then run
`pyrowave_compression_benchmark 3840 2160 1.6 300`. Set VIBESHINE_SOURCE_DIR when
the sibling Vibeshine checkout is unavailable. A Vulkan GPU is required. The
benchmark is opt-in and absent from normal test builds. Use matched live sessions
to assess delivery and smoothness; modeled timing is not a link measurement.
