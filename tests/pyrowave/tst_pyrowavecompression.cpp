#include "../../pyrowave/compression/pyrowavecompression.h"
#include "../../app/streaming/video/pyrowave/pyrowaveframing.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

using Bytes = std::vector<uint8_t>;
using namespace PyroWaveCompression;
namespace {
int failures;
void check(bool condition, const char* message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}
void put(Bytes& bytes, uint32_t word) {
    for (unsigned i = 0; i < 4; ++i) bytes.push_back(uint8_t(word >> (i * 8)));
}
uint32_t read(const uint8_t* p) { return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint32_t random(uint32_t& state) { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
const PyroWaveFraming::StreamGeometry geometry{1920, 1080, true};
constexpr size_t shard = 1376;
Bytes fixture(unsigned sequence, bool noise, size_t& critical, unsigned detailCount = 900) {
    Bytes bytes;
    const auto coarse = PyroWaveFraming::coarseBlockCount(geometry);
    put(bytes, 0x80000000u | sequence << 28 | 1919 | (1079 << 14));
    put(bytes, (coarse + detailCount) | (1u << 26));
    uint32_t state = 0x87549912;
    for (unsigned id = 0; id < coarse + detailCount; ++id) {
        const unsigned words = id < coarse ? 8 : 64;
        put(bytes, sequence << 28 | words << 16 | 1);
        put(bytes, id << 8);
        for (unsigned i = 2; i < words; ++i) put(bytes, noise ? random(state) : 0x01010101u + id % 4);
        if (id + 1 == coarse) critical = bytes.size();
    }
    return bytes;
}
Bytes collect(const Bytes& wire, const PyroWaveFraming::Frame& frame) {
    Bytes out;
    for (const auto& span : frame.spans) {
        const auto& src = span.expanded ? frame.expanded : wire;
        out.insert(out.end(), src.begin() + span.offset, src.begin() + span.offset + span.size);
    }
    return out;
}
std::vector<PyroWaveFraming::Segment> packetMap(Bytes& wire, const EncodedFrame& encoded, size_t lost) {
    std::vector<PyroWaveFraming::Segment> map;
    for (size_t pos = 0, index = 0; pos < wire.size(); ++index) {
        const size_t bytes = std::min(wire.size() - pos, index ? shard : shard - 8);
        if (index == lost) std::fill(wire.begin() + pos, wire.begin() + pos + bytes, 0);
        map.push_back({pos, bytes, index == lost, bool(encoded.recordStartShards[index])});
        pos += bytes;
    }
    return map;
}
}
int main() {
    const char* known = "123456789";
    check(checksumCrc32c(reinterpret_cast<const uint8_t*>(known), 9) == 0xE3069283 &&
          checksumCrc32c(reinterpret_cast<const uint8_t*>(known), 9, true) == 0xE3069283, "CRC32C known vector and hardware/scalar parity");
    Encoder encoder;
    EncodedFrame encoded;
    PyroWaveFraming::Frame frame;
    std::string error;
    size_t critical = 0;
    auto native = fixture(1, false, critical);
    check(encoder.encode(native.data(), native.size(), critical, shard, encoded, error), "independent compression succeeds");
    check(encoded.compressedGroups >= 3 && encoded.bytes.size() < native.size(), "detail groups compress without a prior frame");
    check(encoded.criticalBytes == critical && std::equal(native.begin(), native.begin() + critical, encoded.bytes.begin()), "coarse prefix remains byte-for-byte unchanged");
    check(PyroWaveFraming::parse(encoded.bytes.data(), encoded.bytes.size(), {}, 0, geometry, frame, error, true), "compressed native framing parses");
    check(collect(encoded.bytes, frame) == native && !frame.partial, "all independent coefficients and sequence reconstruct exactly");
    check(!PyroWaveFraming::parse(encoded.bytes.data(), encoded.bytes.size(), geometry, frame, error), "unnegotiated compression is rejected");

    const size_t criticalPackets = (critical + 8 + shard - 1) / shard;
    // Each possible detail-packet loss must retain the protected coarse image;
    // include group headers, compressed payload, raw records and padding.
    for (size_t lost = criticalPackets; lost < encoded.recordStartShards.size(); ++lost) {
        auto wire = encoded.bytes;
        auto map = packetMap(wire, encoded, lost);
        check(PyroWaveFraming::parse(wire.data(), wire.size(), map, criticalPackets, geometry, frame, error, true), "every isolated detail-shard loss remains parseable");
        // A padding-only shard can be lost without losing any coefficient.
        check(frame.coarseLevelIntact, "detail loss preserves the coarse prefix");
        check(frame.blockRecords >= PyroWaveFraming::coarseBlockCount(geometry), "detail loss still supplies coarse blocks");
    }
    // This is the frozen-session failure condition: a detail shard is lost in
    // every frame. Independent frames continue producing a coarse image.
    for (unsigned i = 0; i < 120; ++i) {
        native = fixture(i % 8, false, critical);
        check(encoder.encode(native.data(), native.size(), critical, shard, encoded, error), "repeated-loss frame independently encodes");
        auto wire = encoded.bytes;
        auto map = packetMap(wire, encoded, criticalPackets);
        check(PyroWaveFraming::parse(wire.data(), wire.size(), map, criticalPackets, geometry, frame, error, true) && frame.partial && frame.coarseLevelIntact,
              "sustained detail loss never requires a complete reference or ACK");
    }
    auto wire = encoded.bytes;
    auto map = packetMap(wire, encoded, 0);
    check(!PyroWaveFraming::parse(wire.data(), wire.size(), map, criticalPackets, geometry, frame, error, true), "missing sequence header is rejected");
    wire = encoded.bytes; map = packetMap(wire, encoded, 1);
    check(PyroWaveFraming::parse(wire.data(), wire.size(), map, criticalPackets, geometry, frame, error, true) && !frame.coarseLevelIntact, "coarse loss is never treated as safe detail loss");
    native = fixture(6, true, critical);
    check(encoder.encode(native.data(), native.size(), critical, shard, encoded, error), "high-entropy encode succeeds");
    check(encoded.compressedGroups == 0 && encoded.bytes == native, "incompressible frame falls back verbatim without expansion");
    check(PyroWaveFraming::parse(native.data(), native.size(), geometry, frame, error), "ordinary fallback parses without compression support");
    native = fixture(7, false, critical);
    check(encoder.encode(native.data(), native.size(), critical, shard, encoded, error), "independent encoding resumes after noise and sequence wrap");
    check(PyroWaveFraming::parse(encoded.bytes.data(), encoded.bytes.size(), {}, 0, geometry, frame, error, true) && collect(encoded.bytes, frame) == native, "next intact frame restores all native detail immediately");

    // Real scenes can alternate compressible and high-entropy detail groups.
    // Exercise resynchronization across compressed, raw and padding records.
    native = fixture(4, false, critical);
    uint32_t noiseState = 0x128fa567;
    for (size_t at = critical + 256 * 256; at < critical + 512 * 256; at += 256)
        for (size_t word = at + 8; word < at + 256; word += 4) {
            const auto value = random(noiseState);
            for (unsigned byte = 0; byte < 4; ++byte) native[word + byte] = uint8_t(value >> (8 * byte));
        }
    check(encoder.encode(native.data(), native.size(), critical, shard, encoded, error), "mixed raw and compressed groups encode");
    check(encoded.compressedGroups > 0 && encoded.compressedGroups < 4, "high-entropy middle group stays raw");
    check(PyroWaveFraming::parse(encoded.bytes.data(), encoded.bytes.size(), {}, 0, geometry, frame, error, true) && collect(encoded.bytes, frame) == native,
          "mixed groups preserve every native record in order");
    for (size_t lost = criticalPackets; lost < encoded.recordStartShards.size(); ++lost) {
        wire = encoded.bytes;
        map = packetMap(wire, encoded, lost);
        check(PyroWaveFraming::parse(wire.data(), wire.size(), map, criticalPackets, geometry, frame, error, true) && frame.coarseLevelIntact,
              "mixed raw and compressed groups recover after every detail-shard loss");
    }
    for (size_t at = critical; at + 16 <= encoded.bytes.size(); at += 4) {
        if (read(encoded.bytes.data() + at) != kMagic) continue;
        auto corrupt = encoded.bytes; corrupt[at + 12] ^= 0x40;
        check(!PyroWaveFraming::parse(corrupt.data(), corrupt.size(), {}, 0, geometry, frame, error, true), "group checksum corruption is rejected");
        corrupt = encoded.bytes; std::fill(corrupt.begin() + at + 8, corrupt.begin() + at + 12, 0xFF);
        check(!PyroWaveFraming::parse(corrupt.data(), corrupt.size(), {}, 0, geometry, frame, error, true), "oversized expansion is rejected before allocation");
        corrupt.assign(encoded.bytes.begin(), encoded.bytes.begin() + at + 16);
        check(!PyroWaveFraming::parse(corrupt.data(), corrupt.size(), {}, 0, geometry, frame, error, true), "truncated group is rejected");
        break;
    }
    check(!encoder.encode(native.data(), native.size(), critical - 4, shard, encoded, error), "split critical record is rejected");
    check(!encoder.encode(native.data(), native.size(), critical, 7, encoded, error), "invalid shard size is rejected");
    std::printf("Independent compression, loss recovery and bounds: %d failures\n", failures);
    return failures ? 1 : 0;
}
