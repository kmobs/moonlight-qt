// Actual MoltenVK-to-Metal image sharing. Read every output byte through a
// native Metal blit, so neither a CPU upload nor an untouched old image can
// stand in for a completed Vulkan decode into the presenter's textures.
#include "../../app/streaming/video/pyrowave/pyrowavedecoder.h"
#include "../../app/streaming/video/pyrowave/pyrowavemetal.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool require(bool condition, const char* description)
{
    if (!condition) std::fprintf(stderr, "FAIL: Metal shared decode: %s\n", description);
    return condition;
}

bool transferPlane(id<MTLCommandQueue> queue, id<MTLTexture> texture, bool poison,
                   std::vector<uint8_t>& samples)
{
    const NSUInteger bytesPerSample = texture.pixelFormat == MTLPixelFormatR16Unorm ? 2 : 1;
    const NSUInteger widthBytes = texture.width * bytesPerSample;
    const NSUInteger pitch = (widthBytes + 255) & ~NSUInteger(255);
    id<MTLBuffer> buffer = [queue.device newBufferWithLength:pitch * texture.height
                                                   options:MTLResourceStorageModeShared];
    if (!require(buffer != nil, "native transfer buffer allocation")) return false;
    if (poison) std::memset(buffer.contents, 0xA5, buffer.length);
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    if (poison) {
        [blit copyFromBuffer:buffer sourceOffset:0 sourceBytesPerRow:pitch
           sourceBytesPerImage:buffer.length sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                     toTexture:texture destinationSlice:0 destinationLevel:0
             destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    else {
        [blit copyFromTexture:texture sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                     toBuffer:buffer destinationOffset:0 destinationBytesPerRow:pitch
      destinationBytesPerImage:buffer.length];
    }
    [blit endEncoding];
    [command commit];
    [command waitUntilCompleted];
    const bool ok = require(command.status == MTLCommandBufferStatusCompleted, "native transfer completes");
    if (ok && !poison) {
        samples.resize(widthBytes * texture.height);
        for (NSUInteger row = 0; row < texture.height; ++row)
            std::memcpy(samples.data() + row * widthBytes,
                        static_cast<const uint8_t*>(buffer.contents) + row * pitch, widthBytes);
    }
    [buffer release];
    return ok;
}

struct Frames {
    AVFrame* held[8] = {};
    AVFrame* readback = nullptr;
    ~Frames()
    {
        for (auto*& frame : held) av_frame_free(&frame);
        av_frame_free(&readback);
    }
};

}

bool checkPyroWaveMetalDecode(const std::vector<uint8_t>& records, int width, int height,
                              bool chroma444, bool tenBit)
{
    @autoreleasepool {
        id<MTLDevice> metal = MTLCreateSystemDefaultDevice();
        if (!require(metal != nil, "native device availability")) return false;
        id<MTLCommandQueue> queue = [metal newCommandQueue];
        if (!require(queue != nil, "native command queue")) {
            [metal release];
            return false;
        }
        bool ok = true;
        AVFrame* survivor = nullptr;
        void* survivorTextures[3] = {};
        std::vector<uint8_t> survivorPixels[3];
        {
            PyroWaveMetalPool pool;
            if (!require(pool.initialize((void*)metal), "MoltenVK pool initialization")) {
                [queue release];
                [metal release];
                return false;
            }
            PyroWaveDecoder decoder, readback;
            PyroWaveDecoder::Config config;
            config.width = width;
            config.height = height;
            config.chroma444 = chroma444;
            config.tenBit = tenBit;
            config.requireSharedOutput = true;
            config.vulkanPool = &pool;
            if (!require(decoder.initialize(config, nullptr) && decoder.hasAsynchronousOutput(),
                         "client uses shared GPU output")) {
                [queue release];
                [metal release];
                return false;
            }
            config.vulkanPool = nullptr;
            PyroWaveDecoder unavailable;
            ok &= require(!unavailable.initialize(config, nullptr) && !unavailable.hasAsynchronousOutput(),
                          "production shared-output requirement rejects a missing pool");
            config.requireSharedOutput = false;
            if (!require(readback.initialize(config, nullptr), "reference readback initialization")) {
                [queue release];
                [metal release];
                return false;
            }
            Frames frames;
            frames.readback = av_frame_alloc();
            if (!require(frames.readback != nullptr &&
                         readback.decode(records.data(), records.size(), {}, 0, frames.readback),
                         "reference decode")) {
                [queue release];
                [metal release];
                return false;
            }
            void* foreignTextures[3] = {};
            ok &= require(!pool.ownsFrame(frames.readback) && !pool.mapFrame(frames.readback, foreignTextures),
                          "system-memory frames are rejected by the shared pool");
            // Fill the bounded pool, then recycle every output view after
            // overwriting the old planes through the native Metal API.
            for (unsigned i = 0; i < 16 && ok; ++i) {
                auto& frame = frames.held[i % 8];
                if (frame) {
                    void* textures[3] = {};
                    ok &= require(pool.mapFrame(frame, textures), "map a recycled surface");
                    for (int plane = 0; plane < 3 && ok; ++plane) {
                        std::vector<uint8_t> unused;
                        ok &= transferPlane(queue, (id<MTLTexture>)textures[plane], true, unused);
                    }
                    av_frame_free(&frame);
                }
                frame = av_frame_alloc();
                if (!require(frame != nullptr && decoder.decode(records.data(), records.size(), {}, 0, frame),
                             "client submits a shared decode")) {
                    std::fprintf(stderr, "Shared client error: %s\n", decoder.lastError().c_str());
                    ok = false;
                    break;
                }
                ok &= require(pool.ownsFrame(frame), "frame retains its shared surface");
                ok &= require(pool.waitForFrame(frame, 1000000000) == VK_SUCCESS,
                              "frame completion timeline signals");
                void* textures[3] = {};
                ok &= require(pool.mapFrame(frame, textures), "map decoder output to native textures");
                for (int plane = 0; plane < 3 && ok; ++plane) {
                    id<MTLTexture> texture = (id<MTLTexture>)textures[plane];
                    const int planeWidth = plane && !chroma444 ? width / 2 : width;
                    const int planeHeight = plane && !chroma444 ? height / 2 : height;
                    ok &= require(texture.device.registryID == metal.registryID && texture.width == NSUInteger(planeWidth) &&
                                  texture.height == NSUInteger(planeHeight) &&
                                  texture.pixelFormat == (tenBit ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm),
                                  "native texture device, dimensions and depth match the stream");
                    std::vector<uint8_t> samples;
                    ok &= transferPlane(queue, texture, false, samples);
                    const size_t widthBytes = size_t(planeWidth) * (tenBit ? 2 : 1);
                    for (int row = 0; row < planeHeight && ok; ++row)
                        ok &= require(std::memcmp(samples.data() + row * widthBytes,
                                                 frames.readback->data[plane] + row * frames.readback->linesize[plane],
                                                 widthBytes) == 0,
                                      "shared native output equals synchronous decode byte for byte");
                }
                if (i == 7 && ok) {
                    AVFrame* ninth = av_frame_alloc();
                    ok &= require(ninth != nullptr && !decoder.decode(records.data(), records.size(), {}, 0, ninth),
                                  "in-flight pool exhaustion drops instead of overwriting a frame");
                    av_frame_free(&ninth);
                }
            }
            if (ok) {
                survivor = av_frame_clone(frames.held[0]);
                ok &= require(survivor != nullptr && pool.mapFrame(survivor, survivorTextures),
                              "cloned frame retains its shared texture state");
                for (int plane = 0; plane < 3 && ok; ++plane)
                    ok &= transferPlane(queue, (id<MTLTexture>)survivorTextures[plane], false, survivorPixels[plane]);
            }
        }
        // The frame is now the only owner of the Vulkan device/images. Its
        // textures must remain readable after both decoder and pool teardown.
        for (int plane = 0; plane < 3 && ok; ++plane) {
            std::vector<uint8_t> samples;
            ok &= transferPlane(queue, (id<MTLTexture>)survivorTextures[plane], false, samples);
            ok &= require(samples == survivorPixels[plane], "a retained frame survives decoder and pool teardown");
        }
        av_frame_free(&survivor);
        [queue release];
        [metal release];
        std::printf("Metal shared client %dx%d %s %s: %s (16 native surface reads/reuses)\n", width, height,
                    chroma444 ? "444" : "420", tenBit ? "10-bit" : "8-bit", ok ? "PASS" : "FAIL");
        return ok;
    }
}
