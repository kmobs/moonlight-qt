// Exercises the actual headless calibration renderer, including shared Vulkan
// decode, scaled native YUV conversion and GPU-completed target readback.
#include "streaming/video/pyrowave/pyrowavedecoder.h"
#include "streaming/video/pyrowave/pyrowavemetal.h"
#include "streaming/video/pyrowave/pyrowavemetalcalibrator.h"
#include "path.h"
#include <pyrowave.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <vector>

QByteArray Path::readDataFile(QString name)
{
    QFile file(QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
                    .filePath("../../app/shaders/" + name));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

namespace {
bool require(bool passed, const char* description)
{
    if (!passed) std::fprintf(stderr, "FAIL: Metal calibration: %s\n", description);
    return passed;
}

struct Encoder {
    pyrowave_encoder encoder = nullptr;
    int width, height;
    bool chroma444;
    std::vector<uint8_t> y, u, v;
    Encoder(pyrowave_device device, int w, int h, bool chroma)
        : width(w), height(h), chroma444(chroma), y(size_t(w) * h),
          u(size_t(chroma ? w : w / 2) * (chroma ? h : h / 2), 128), v(u)
    {
        pyrowave_encoder_create_info info = {};
        info.device = device;
        info.width = width;
        info.height = height;
        info.chroma = chroma444 ? PYROWAVE_CHROMA_SUBSAMPLING_444 : PYROWAVE_CHROMA_SUBSAMPLING_420;
        pyrowave_encoder_create(&info, &encoder);
    }
    ~Encoder() { if (encoder) pyrowave_encoder_destroy(encoder); }

    std::vector<uint8_t> encode(uint8_t luminance, bool colored)
    {
        if (!encoder) return {};
        std::fill(y.begin(), y.end(), luminance);
        std::fill(u.begin(), u.end(), colored ? 96 : 128);
        std::fill(v.begin(), v.end(), colored ? 160 : 128);
        pyrowave_cpu_buffer input = {};
        input.width = width;
        input.height = height;
        input.format = chroma444 ? PYROWAVE_CPU_BUFFER_FORMAT_YUV444P : PYROWAVE_CPU_BUFFER_FORMAT_YUV420P;
        input.data[0] = y.data(); input.data[1] = u.data(); input.data[2] = v.data();
        input.row_stride_in_bytes[0] = width;
        input.row_stride_in_bytes[1] = input.row_stride_in_bytes[2] = chroma444 ? width : width / 2;
        input.plane_size_in_bytes[0] = y.size();
        input.plane_size_in_bytes[1] = u.size(); input.plane_size_in_bytes[2] = v.size();
        pyrowave_rate_control rate = { size_t(width) * height };
        if (pyrowave_encoder_encode_cpu_synchronous(encoder, &input, &rate) != PYROWAVE_SUCCESS) return {};
        size_t count = 0;
        if (pyrowave_encoder_compute_num_packets(encoder, 1024, &count) != PYROWAVE_SUCCESS) return {};
        std::vector<pyrowave_packet> packets(count);
        std::vector<uint8_t> bits(rate.maximum_bitstream_size + 1024 * 1024);
        size_t written = 0;
        if (pyrowave_encoder_packetize(encoder, packets.data(), 1024, &written,
                                       bits.data(), bits.size()) != PYROWAVE_SUCCESS) return {};
        // Supported compatibility framing keeps this renderer smoke focused on
        // native GPU output. Production RTP record framing has its own suite.
        std::vector<uint8_t> result;
        for (unsigned byte = 0; byte < 4; ++byte)
            result.push_back(uint8_t(written >> (8 * byte)));
        for (size_t i = 0; i < written; ++i) {
            for (unsigned byte = 0; byte < 4; ++byte)
                result.push_back(uint8_t(packets[i].size >> (8 * byte)));
            result.insert(result.end(), bits.begin() + packets[i].offset,
                           bits.begin() + packets[i].offset + packets[i].size);
        }
        return result;
    }
};

bool checkPixel(uint32_t pixel, uint8_t luminance, bool hdr, bool colored)
{
    const int bits = hdr ? 10 : 8;
    const int maximum = (1 << bits) - 1;
    const int red = (pixel >> (2 * bits)) & maximum;
    const int green = (pixel >> bits) & maximum;
    const int blue = pixel & maximum;
    // Known limited-range neutral and colored samples. The colored image also
    // catches missing/swapped chroma planes and the wrong SDR/HDR color matrix.
    const double expected = (luminance - 16) / 219.0 * maximum;
    const double expectedRed = colored ? (hdr ? 739 : 182) : expected;
    const double expectedGreen = colored ? (hdr ? 464 : 117) : expected;
    const double expectedBlue = colored ? (hdr ? 248 : 66) : expected;
    const int tolerance = hdr ? 24 : 6;
    return require(std::abs(red - expectedRed) <= tolerance && std::abs(green - expectedGreen) <= tolerance &&
                   std::abs(blue - expectedBlue) <= tolerance &&
                   (pixel >> (3 * bits)) == uint32_t(hdr ? 3 : 255),
                   "native target contains the converted decoded image with opaque alpha");
}

bool runFormat(pyrowave_device encodeDevice, PyroWaveMetalCalibratorRenderer& renderer,
               int width, int height, bool chroma444, bool hdr)
{
    if (!require(renderer.prepare(width, height, chroma444, hdr), "prepare shared pool and native target")) return false;
    uint32_t pixel = 0;
    if (!require(!renderer.readLastPixel(pixel), "new format cannot return a previous target pixel")) return false;
    Encoder encoder(encodeDevice, width, height, chroma444);
    PyroWaveDecoder decoder;
    PyroWaveDecoder::Config config;
    config.width = width;
    config.height = height;
    config.chroma444 = chroma444;
    config.tenBit = hdr;
    config.vulkanPool = renderer.pool();
    config.requireSharedOutput = true;
    if (!require(decoder.initialize(config, nullptr) && decoder.hasAsynchronousOutput(),
                 "calibration decoder requires shared asynchronous output")) return false;
    double elapsedMs = 0;
    int drawn = 0;
    for (int image = 0; image < 3; ++image) {
        const uint8_t luminance = image == 1 ? 196 : 128;
        const bool colored = image == 2;
        const auto encoded = encoder.encode(luminance, colored);
        if (!require(!encoded.empty(), "encode test image")) return false;
        // Repeated draws prove surface reuse and guard against
        // repeatedly scoring the last target instead of this decoded image.
        for (int repeat = 0; repeat < 12; ++repeat) {
            AVFrame* frame = av_frame_alloc();
            const auto start = std::chrono::steady_clock::now();
            const bool decoded = frame && decoder.decode(encoded.data(), encoded.size(), {}, 0, frame);
            const bool presented = decoded && renderer.present(frame, hdr);
            elapsedMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            const bool shared = decoded && renderer.pool()->ownsFrame(frame) && !frame->data[0];
            if (!decoded) std::fprintf(stderr, "Decode error: %s\n", decoder.lastError().c_str());
            av_frame_free(&frame);
            if (!require(presented && shared, "decode and native draw complete from shared GPU planes") ||
                !require(renderer.readLastPixel(pixel), "readback is GPU completed") || !checkPixel(pixel, luminance, hdr, colored)) return false;
            ++drawn;
        }
    }
    std::printf("Headless Metal calibration %dx%d %s %s -> 2560x1440: PASS (%d draws, %.3f ms/frame)\n",
                width, height, chroma444 ? "444" : "420", hdr ? "HDR" : "SDR", drawn, elapsedMs / drawn);
    return true;
}
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    pyrowave_device encodeDevice = nullptr;
    if (!require(pyrowave_create_default_device(&encodeDevice) == PYROWAVE_SUCCESS,
                 "separate encoder device")) return 1;
    bool ok = true;
    {
        PyroWaveMetalCalibratorRenderer renderer;
        ok = require(renderer.create(2560, 1440), "create headless native renderer");
        for (int size : {64, 1920})
            for (bool chroma444 : {true, false})
                for (bool hdr : {true, false})
                    if (ok) ok = runFormat(encodeDevice, renderer, size, size == 64 ? 64 : 1080, chroma444, hdr);
    }
    pyrowave_device_destroy(encodeDevice);
    return ok ? 0 : 1;
}
