// Avoid AVFoundation/libavutil's conflicting AVMediaType definitions.
#define AVMediaType AVMediaType_FFmpeg
#include "pyrowavemetalcalibrator.h"
#include "pyrowavemetal.h"
#include "streaming/video/ffmpeg-renderers/renderer.h"
#include "path.h"
#undef AVMediaType

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <simd/simd.h>

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>

namespace {
constexpr auto kCompletionLimit = std::chrono::milliseconds(50);

struct CscParams {
    simd_half3x3 matrix;
    simd_half3 offsets;
};

// Matches vt_metal.mm and vt_renderer.metal, including Metal's half alignment.
struct ParamBuffer {
    CscParams cscParams;
    simd_half2 chromaOffset;
    simd_half1 bitnessScaleFactor;
};

struct Vertex {
    simd_float4 position;
    simd_float2 texCoord;
};

struct Completion {
    std::mutex lock;
    std::condition_variable wake;
    bool finished = false;
    bool successful = false;
    AVFrame* frame = nullptr;
    ~Completion() { av_frame_free(&frame); }
};
}

// Only the production renderer's frame color helpers are used. This headless
// renderer never enters the window/decoder-context or overlay interfaces.
struct PyroWaveMetalCalibratorRenderer::State : IFFmpegRenderer {
    State() : IFFmpegRenderer(RendererType::VTMetal) {}
    bool initialize(PDECODER_PARAMETERS) override { return false; }
    bool prepareDecoderContext(AVCodecContext*, AVDictionary**) override { return false; }
    void renderFrame(AVFrame*) override {}

    id<MTLDevice> device = nil;
    id<MTLCommandQueue> queue = nil;
    id<MTLLibrary> library = nil;
    id<MTLRenderPipelineState> pipeline[2] = {};
    id<MTLTexture> target[2] = {};
    id<MTLBuffer> vertices = nil;
    id<MTLBuffer> params = nil;
    id<MTLBuffer> readback = nil;
    std::unique_ptr<PyroWaveMetalPool> pool;
    int displayWidth = 0;
    int displayHeight = 0;
    int width = 0;
    int height = 0;
    bool chroma444 = false;
    bool hdr = false;
    bool failed = false;
    bool pixelValid = false;

    ~State()
    {
        pool.reset();
        [params release];
        [vertices release];
        [readback release];
        for (unsigned i = 0; i < 2; ++i) {
            [target[i] release];
            [pipeline[i] release];
        }
        [library release];
        [queue release];
        [device release];
    }

    bool updateParams(AVFrame* frame)
    {
        const bool changed = hasFrameFormatChanged(frame);
        if (params && !changed) return true;
        std::array<float, 9> matrix;
        std::array<float, 3> offsets;
        std::array<float, 2> chromaOffset;
        getFramePremultipliedCscConstants(frame, matrix, offsets);
        getFrameChromaCositingOffsets(frame, chromaOffset);
        const auto* desc = av_pix_fmt_desc_get((AVPixelFormat)frame->format);
        if (!desc || (desc->comp[0].step != 1 && desc->comp[0].step != 2)) return false;
        ParamBuffer values = {};
        values.cscParams.matrix = simd_matrix(simd_make_half3(matrix[0], matrix[3], matrix[6]),
                                             simd_make_half3(matrix[1], matrix[4], matrix[7]),
                                             simd_make_half3(matrix[2], matrix[5], matrix[8]));
        values.cscParams.offsets = simd_make_half3(offsets[0], offsets[1], offsets[2]);
        values.chromaOffset = simd_make_half2(chromaOffset[0], chromaOffset[1]);
        values.bitnessScaleFactor = std::pow(2, desc->comp[0].step * 8 - desc->comp[0].depth);
        [params release];
        params = [device newBufferWithBytes:&values length:sizeof(values)
                                   options:MTLCPUCacheModeWriteCombined | MTLResourceStorageModeManaged];
        return params != nil;
    }
};

PyroWaveMetalCalibratorRenderer::PyroWaveMetalCalibratorRenderer() = default;
PyroWaveMetalCalibratorRenderer::~PyroWaveMetalCalibratorRenderer() = default;

bool PyroWaveMetalCalibratorRenderer::create(int displayWidth, int displayHeight)
{ @autoreleasepool {
    auto state = std::make_unique<State>();
    state->displayWidth = displayWidth;
    state->displayHeight = displayHeight;
    state->device = MTLCreateSystemDefaultDevice();
    if (!state->device) return false;
    state->queue = [state->device newCommandQueue];
    const auto source = Path::readDataFile(QStringLiteral("vt_renderer.metal"));
    if (!state->queue || source.isEmpty()) return false;
    NSString* shader = [[NSString alloc] initWithBytes:source.constData()
                                               length:source.size() encoding:NSUTF8StringEncoding];
    state->library = [state->device newLibraryWithSource:shader options:nil error:nil];
    [shader release];
    state->readback = [state->device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    if (!state->library || !state->readback) return false;
    m_State = std::move(state);
    return true;
}}

bool PyroWaveMetalCalibratorRenderer::prepare(int width, int height, bool chroma444, bool hdr)
{ @autoreleasepool {
    if (!m_State || m_State->failed || width <= 0 || height <= 0) return false;
    auto& state = *m_State;
    // Decoder output-view caches assume immutable images. Every format gets a
    // fresh shared device/pool before its decoder is initialized.
    state.pool = std::make_unique<PyroWaveMetalPool>();
    if (!state.pool->initialize((void*)state.device)) return false;
    const unsigned index = hdr ? 1 : 0;
    const NSUInteger targetWidth = state.displayWidth > 0 ? state.displayWidth : width;
    const NSUInteger targetHeight = state.displayHeight > 0 ? state.displayHeight : height;
    const auto format = hdr ? MTLPixelFormatBGR10A2Unorm : MTLPixelFormatBGRA8Unorm;
    if (!state.target[index] || state.target[index].width != targetWidth ||
        state.target[index].height != targetHeight) {
        [state.target[index] release];
        auto descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                    width:targetWidth height:targetHeight mipmapped:NO];
        descriptor.storageMode = MTLStorageModePrivate;
        descriptor.usage = MTLTextureUsageRenderTarget;
        state.target[index] = [state.device newTextureWithDescriptor:descriptor];
        if (!state.target[index]) return false;
    }
    if (!state.pipeline[index]) {
        auto descriptor = [[MTLRenderPipelineDescriptor new] autorelease];
        descriptor.vertexFunction = [[state.library newFunctionWithName:@"vs_draw"] autorelease];
        descriptor.fragmentFunction = [[state.library newFunctionWithName:@"ps_draw_triplanar"] autorelease];
        descriptor.colorAttachments[0].pixelFormat = format;
        state.pipeline[index] = [state.device newRenderPipelineStateWithDescriptor:descriptor error:nil];
        if (!state.pipeline[index]) return false;
    }
    // Same aspect fit and integer rounding as StreamUtils' production video
    // region, followed by the presenter's normalized triangle strip.
    int x = 0, y = 0, w = int(targetWidth), h = int(targetHeight);
    const int fitHeight = int(std::ceil(float(w) * height / width));
    const int fitWidth = int(std::ceil(float(h) * width / height));
    if (fitHeight > h) { x = (w - fitWidth) / 2; w = fitWidth; }
    else { y = (h - fitHeight) / 2; h = fitHeight; }
    const float left = float(x) / (targetWidth / 2.0f) - 1.0f;
    const float bottom = float(y) / (targetHeight / 2.0f) - 1.0f;
    const float right = left + float(w) / (targetWidth / 2.0f);
    const float top = bottom + float(h) / (targetHeight / 2.0f);
    Vertex vertices[] = {
        {{left, bottom, 0, 1}, {0, 1}}, {{left, top, 0, 1}, {0, 0}},
        {{right, bottom, 0, 1}, {1, 1}}, {{right, top, 0, 1}, {1, 0}},
    };
    [state.vertices release];
    state.vertices = [state.device newBufferWithBytes:vertices length:sizeof(vertices)
                            options:MTLCPUCacheModeWriteCombined | MTLResourceStorageModeManaged];
    [state.params release];
    state.params = nil;
    state.width = width;
    state.height = height;
    state.chroma444 = chroma444;
    state.hdr = hdr;
    state.pixelValid = false;
    return state.vertices != nil;
}}

bool PyroWaveMetalCalibratorRenderer::present(AVFrame* frame, bool hdr)
{ @autoreleasepool {
    if (!m_State || m_State->failed || !frame) return false;
    auto& state = *m_State;
    if (!state.pool || !state.pool->ownsFrame(frame) || hdr != state.hdr ||
        frame->width != state.width || frame->height != state.height) return false;
    const auto deadline = std::chrono::steady_clock::now() + kCompletionLimit;
    const auto decodeResult = state.pool->waitForFrame(frame,
            std::chrono::duration_cast<std::chrono::nanoseconds>(kCompletionLimit).count());
    if (decodeResult != VK_SUCCESS) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave Metal calibration decode did not complete within 50 ms (Vulkan result %d)",
                     int(decodeResult));
        state.failed = true;
        return false;
    }
    // PyroWave's bitstream carries no color metadata. Match the negotiated
    // stream's limited range, Rec.601 SDR and BT.2020/PQ HDR tags.
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->color_primaries = hdr ? AVCOL_PRI_BT2020 : AVCOL_PRI_SMPTE170M;
    frame->color_trc = hdr ? AVCOL_TRC_SMPTE2084 : AVCOL_TRC_SMPTE170M;
    frame->colorspace = hdr ? AVCOL_SPC_BT2020_NCL : AVCOL_SPC_SMPTE170M;
    if (!state.updateParams(frame)) return false;
    void* textures[3] = {};
    if (!state.pool->mapFrame(frame, textures)) return false;
    auto completion = std::make_shared<Completion>();
    completion->frame = av_frame_clone(frame);
    if (!completion->frame) return false;
    id<MTLCommandBuffer> command = [state.queue commandBuffer];
    auto descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    descriptor.colorAttachments[0].texture = state.target[hdr ? 1 : 0];
    descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    descriptor.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    auto encoder = [command renderCommandEncoderWithDescriptor:descriptor];
    if (!encoder) return false;
    [encoder setRenderPipelineState:state.pipeline[hdr ? 1 : 0]];
    for (NSUInteger plane = 0; plane < 3; ++plane)
        [encoder setFragmentTexture:(id<MTLTexture>)textures[plane] atIndex:plane];
    [encoder setFragmentBuffer:state.params offset:0 atIndex:0];
    [encoder setVertexBuffer:state.vertices offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [encoder endEncoding];
    auto blit = [command blitCommandEncoder];
    if (!blit) return false;
    id<MTLTexture> target = state.target[hdr ? 1 : 0];
    [blit copyFromTexture:target sourceSlice:0 sourceLevel:0
            sourceOrigin:MTLOriginMake(target.width / 2, target.height / 2, 0)
              sourceSize:MTLSizeMake(1, 1, 1) toBuffer:state.readback
       destinationOffset:0 destinationBytesPerRow:256 destinationBytesPerImage:256];
    [blit endEncoding];
    // The completion owns a frame clone through the final native read. Even a
    // timed-out calibration cannot recycle its shared Vulkan/Metal surface.
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        {
            std::lock_guard<std::mutex> guard(completion->lock);
            completion->successful = completed.status == MTLCommandBufferStatusCompleted;
            completion->finished = true;
        }
        completion->wake.notify_all();
    }];
    state.pixelValid = false;
    [command commit];
    std::unique_lock<std::mutex> lock(completion->lock);
    const bool finished = completion->wake.wait_until(lock, deadline, [&] { return completion->finished; });
    if (!finished || !completion->successful) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                     "PyroWave Metal calibration draw failed or exceeded the 50 ms completion limit: %s",
                     command.error ? command.error.localizedDescription.UTF8String : "completion timeout");
        state.failed = true;
        return false;
    }
    state.pixelValid = true;
    return true;
}}

PyroWaveMetalPool* PyroWaveMetalCalibratorRenderer::pool() const
{
    return m_State ? m_State->pool.get() : nullptr;
}

bool PyroWaveMetalCalibratorRenderer::readLastPixel(uint32_t& pixel) const
{
    if (!m_State || !m_State->pixelValid) return false;
    std::memcpy(&pixel, m_State->readback.contents, sizeof(pixel));
    return true;
}
