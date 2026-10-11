#include "metalpyrowavefixture.h"
#include "../../app/streaming/video/pyrowave/pyrowavedecoder.h"
#include <pyrowave.h>
#include <cstdio>
#include <vector>

namespace {
constexpr int Width = 128, Height = 96;
constexpr size_t Budget = 256 * 1024;
struct Codec {
    pyrowave_device device = nullptr;
    pyrowave_encoder encoder = nullptr;
    ~Codec()
    {
        if (encoder) pyrowave_encoder_destroy(encoder);
        if (device) pyrowave_device_destroy(device);
    }
};
void appendU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(uint8_t(value >> shift));
}
std::unique_ptr<MetalPyroWaveFixture> fail(const char* reason)
{
    std::fprintf(stderr, "Metal PyroWave fixture: %s\n", reason);
    return nullptr;
}
}

MetalPyroWaveFixture::~MetalPyroWaveFixture() { av_frame_free(&frame); }

std::unique_ptr<MetalPyroWaveFixture> decodeMetalPyroWaveFixture(
    IPyroWaveVulkanPool* pool, bool chroma444, bool tenBit)
{
    if (!pool) return fail("presenter has no shared Vulkan pool");
    PyroWaveVulkanDevice shared;
    if (!pool->pyroWaveVulkanDevice(shared)) return fail("shared Vulkan device unavailable");
    std::vector<uint8_t> framed;
    {
        Codec codec;
        pyrowave_device_create_info deviceInfo {};
        deviceInfo.GetInstanceProcAddr = shared.getInstanceProcAddr;
        deviceInfo.instance = shared.instance;
        deviceInfo.physical_device = shared.physicalDevice;
        deviceInfo.device = shared.device;
        deviceInfo.instance_create_info = shared.instanceInfo;
        deviceInfo.device_create_info = shared.deviceInfo;
        deviceInfo.queue_lock_callback = shared.lockQueues;
        deviceInfo.queue_unlock_callback = shared.unlockQueues;
        deviceInfo.userdata = shared.userdata;
        if (pyrowave_create_device(&deviceInfo, &codec.device) != PYROWAVE_SUCCESS)
            return fail("borrowed encoder device initialization failed");
        pyrowave_encoder_create_info encoderInfo {};
        encoderInfo.device = codec.device;
        encoderInfo.width = Width; encoderInfo.height = Height;
        encoderInfo.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
        if (pyrowave_encoder_create(&encoderInfo, &codec.encoder) != PYROWAVE_SUCCESS)
            return fail("encoder initialization failed");
        const int chromaWidth = chroma444 ? Width : Width / 2;
        const int chromaHeight = chroma444 ? Height : Height / 2;
        std::vector<uint8_t> planes[3];
        planes[0].resize(Width * Height);
        planes[1].resize(chromaWidth * chromaHeight);
        planes[2].resize(chromaWidth * chromaHeight);
        for (int y = 0; y < Height; ++y)
            for (int x = 0; x < Width; ++x) planes[0][size_t(y) * Width + x] = uint8_t(32 + (x + y) % 192);
        for (int y = 0; y < chromaHeight; ++y)
            for (int x = 0; x < chromaWidth; ++x) {
                planes[1][size_t(y) * chromaWidth + x] = uint8_t(96 + x % 64);
                planes[2][size_t(y) * chromaWidth + x] = uint8_t(96 + y % 64);
            }
        pyrowave_cpu_buffer input {};
        input.width = Width; input.height = Height;
        input.format = chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        for (int plane = 0; plane < 3; ++plane) {
            input.data[plane] = planes[plane].data();
            input.row_stride_in_bytes[plane] = size_t(plane ? chromaWidth : Width);
            input.plane_size_in_bytes[plane] = planes[plane].size();
        }
        pyrowave_rate_control rate { Budget };
        if (pyrowave_encoder_encode_cpu_synchronous(codec.encoder, &input, &rate) != PYROWAVE_SUCCESS)
            return fail("GPU encoding failed");
        size_t count = 0;
        constexpr size_t Boundary = 1024;
        if (pyrowave_encoder_compute_num_packets(codec.encoder, Boundary, &count) != PYROWAVE_SUCCESS || !count)
            return fail("packet count failed");
        std::vector<pyrowave_packet> packets(count);
        std::vector<uint8_t> bitstream(Budget + 1024);
        size_t written = 0;
        if (pyrowave_encoder_packetize(codec.encoder, packets.data(), Boundary, &written,
                                      bitstream.data(), bitstream.size()) != PYROWAVE_SUCCESS || !written)
            return fail("packetization failed");
        appendU32(framed, uint32_t(written));
        for (size_t i = 0; i < written; ++i) {
            const auto& packet = packets[i];
            if (packet.offset > bitstream.size() || packet.size > bitstream.size() - packet.offset)
                return fail("invalid encoded packet bounds");
            appendU32(framed, uint32_t(packet.size));
            framed.insert(framed.end(), bitstream.begin() + packet.offset,
                          bitstream.begin() + packet.offset + packet.size);
        }
    }
    auto fixture = std::make_unique<MetalPyroWaveFixture>();
    fixture->decoder = std::make_unique<PyroWaveDecoder>();
    PyroWaveDecoder::Config config;
    config.width = Width; config.height = Height;
    config.chroma444 = chroma444; config.tenBit = tenBit;
    config.vulkanPool = pool; config.requireSharedOutput = true;
    if (!fixture->decoder->initialize(config, nullptr) || !fixture->decoder->hasAsynchronousOutput())
        return fail("shared decoder initialization failed");
    fixture->frame = av_frame_alloc();
    AVFrame* frame = fixture->frame;
    if (!frame) return fail("frame allocation failed");
    if (!fixture->decoder->decode(framed.data(), framed.size(), {}, 0, frame)) {
        std::fprintf(stderr, "Metal PyroWave fixture decode: %s\n", fixture->decoder->lastError().c_str());
        return nullptr;
    }
    frame->color_range = AVCOL_RANGE_JPEG;
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    return fixture;
}
