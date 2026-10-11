// End-to-end client decode of independent compressed groups, including sustained loss.
#include <vulkan/vulkan.h>
#include <pyrowave.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#if defined(__linux__) || defined(__APPLE__)
#include "../../app/streaming/video/pyrowave/pyrowavedecoder.h"
#include "pyrowavecompression.h"
namespace {
bool require(bool condition, const std::string& description)
{
    if (!condition) std::fprintf(stderr, "FAIL: compression client: %s\n", description.c_str());
    return condition;
}

struct Source {
    static constexpr int width = 640, height = 360;
    bool chroma444;
    std::vector<uint8_t> planes[3];

    explicit Source(bool c444) : chroma444(c444)
    {
        for (int plane = 0; plane < 3; ++plane) {
            planes[plane].resize(size_t(planeWidth(plane)) * planeHeight(plane));
            for (int y = 0; y < planeHeight(plane); ++y) {
                for (int x = 0; x < planeWidth(plane); ++x) {
                    planes[plane][size_t(y) * planeWidth(plane) + x] =
                        uint8_t(plane ? 96 + (x + y * plane) % 64 : 32 + (x * 2 + y) % 192);
                }
            }
        }
    }

    int planeWidth(int plane) const { return plane == 0 || chroma444 ? width : width / 2; }
    int planeHeight(int plane) const { return plane == 0 || chroma444 ? height : height / 2; }

    std::vector<uint8_t> encode(pyrowave_encoder encoder)
    {
        pyrowave_cpu_buffer input = {};
        input.width = width;
        input.height = height;
        input.format = chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        for (int plane = 0; plane < 3; ++plane) {
            input.data[plane] = planes[plane].data();
            input.row_stride_in_bytes[plane] = planeWidth(plane);
            input.plane_size_in_bytes[plane] = planes[plane].size();
        }
        pyrowave_rate_control rate = { 128 * 1024 };
        if (pyrowave_encoder_encode_cpu_synchronous(encoder, &input, &rate) != PYROWAVE_SUCCESS) return {};
        size_t count = 0;
        if (pyrowave_encoder_compute_num_packets(encoder, 1376, &count) != PYROWAVE_SUCCESS) return {};
        std::vector<pyrowave_packet> packets(count);
        std::vector<uint8_t> buffer(1024 * 1024), records;
        size_t written = 0;
        if (pyrowave_encoder_packetize(encoder, packets.data(), 1376, &written,
                                      buffer.data(), buffer.size()) != PYROWAVE_SUCCESS) return {};
        for (size_t i = 0; i < written; ++i) {
            const auto& packet = packets[i];
            records.insert(records.end(), buffer.begin() + packet.offset,
                           buffer.begin() + packet.offset + packet.size);
        }
        return records;
    }
};

bool samePixels(const AVFrame* ordinary, const AVFrame* compressed, const Source& source, bool tenBit)
{
    if (ordinary->format != compressed->format || ordinary->width != compressed->width ||
            ordinary->height != compressed->height) return false;
    for (int plane = 0; plane < 3; ++plane) {
        const size_t widthBytes = size_t(source.planeWidth(plane)) * (tenBit ? 2 : 1);
        for (int row = 0; row < source.planeHeight(plane); ++row) {
            if (std::memcmp(ordinary->data[plane] + row * ordinary->linesize[plane],
                            compressed->data[plane] + row * compressed->linesize[plane], widthBytes)) return false;
        }
    }
    return true;
}


uint32_t read(const uint8_t* p) {
    return p[0] | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
bool runCompressionCase(pyrowave_device device, bool chroma444, bool tenBit) {
    Source source(chroma444);
    pyrowave_encoder_create_info info{};
    info.device = device; info.width = Source::width; info.height = Source::height;
    info.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
    pyrowave_encoder encoder = nullptr;
    if (!require(pyrowave_encoder_create(&info, &encoder) == PYROWAVE_SUCCESS, "encoder initialization")) return false;
    struct Owner { pyrowave_encoder encoder; ~Owner() { pyrowave_encoder_destroy(encoder); } } owner{encoder};
    PyroWaveDecoder ordinary, compressed;
    PyroWaveDecoder::Config config;
    config.width = Source::width; config.height = Source::height;
    config.chroma444 = chroma444; config.tenBit = tenBit;
    if (!require(ordinary.initialize(config, nullptr), "ordinary decoder initialization")) return false;
    config.compression = true;
    if (!require(compressed.initialize(config, nullptr), "compressed decoder initialization")) return false;
    PyroWaveCompression::Encoder compressor;
    PyroWaveCompression::EncodedFrame encoded;
    std::string error;
    bool ok = true;
    size_t groups = 0, partialFrames = 0;
    for (unsigned i = 0; i < 18; ++i) {
        auto records = source.encode(encoder);
        if (!require(!records.empty(), "native encoding")) return false;
        const auto coarse = PyroWaveFraming::coarseBlockCount({Source::width, Source::height, chroma444});
        size_t critical = 8;
        for (size_t at = 8; at < records.size();) {
            const uint32_t word = read(records.data() + at);
            const size_t bytes = word == 0xFFFFFFFFu ? 8 + size_t(read(records.data() + at + 4)) * 4 : size_t((word >> 16) & 0xFFF) * 4;
            if (word != 0xFFFFFFFFu && (read(records.data() + at + 4) >> 8) < coarse) critical = at + bytes;
            at += bytes;
        }
        if (!require(compressor.encode(records.data(), records.size(), critical, 1376, encoded, error), error)) return false;
        groups += encoded.compressedGroups;
        AVFrame* a = av_frame_alloc(); AVFrame* b = av_frame_alloc();
        const bool intact = i == 0 || i == 17;
        auto wire = encoded.bytes;
        std::vector<PyroWaveFraming::Segment> packets;
        const size_t criticalPackets = (critical + 8 + 1375) / 1376;
        // Lose every detail packet in each of sixteen consecutive frames.
        // This exercises both lost group headers and long sequence gaps.
        if (!intact) {
            for (size_t pos = 0, index = 0; pos < wire.size(); ++index) {
                const size_t bytes = std::min(wire.size() - pos, index ? size_t(1376) : size_t(1368));
                const bool lost = index >= criticalPackets;
                if (lost) std::fill(wire.begin() + pos, wire.begin() + pos + bytes, 0);
                packets.push_back({pos, bytes, lost, bool(encoded.recordStartShards[index])});
                pos += bytes;
            }
        }
        const bool decoded = compressed.decode(wire.data(), wire.size(), packets, criticalPackets, b);
        ok &= require(decoded, "independent client decode: " + compressed.lastError());
        if (decoded && intact) {
            const bool ordinaryDecoded = ordinary.decode(records.data(), records.size(), {}, 0, a);
            ok &= require(ordinaryDecoded && samePixels(a, b, source, tenBit), "intact output equals ordinary decode byte for byte");
            ok &= require(!compressed.lastFramePartial(), "intact frame has all detail");
        } else if (decoded) {
            ok &= require(compressed.lastFramePartial(), "sustained loss still renders a partial frame");
            ++partialFrames;
        }
        av_frame_free(&a); av_frame_free(&b);
    }
    ok &= require(groups > 0 && partialFrames == 16, "real GPU records exercise compression and sixteen consecutive partial frames");
    std::printf("Compression client %s %s: %s (%zu groups, %zu partial frames)\n", chroma444 ? "444" : "420", tenBit ? "10-bit" : "8-bit", ok ? "PASS" : "FAIL", groups, partialFrames);
    return ok;
}
}
#endif
bool checkPyroWaveCompressionClientDecode(pyrowave_device device) {
#if defined(__linux__) || defined(__APPLE__)
    bool ok = true;
    for (bool chroma : {false, true}) for (bool depth : {false, true}) ok &= runCompressionCase(device, chroma, depth);
    return ok;
#else
    (void)device;
    return true;
#endif
}
