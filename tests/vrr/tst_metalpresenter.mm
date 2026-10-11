// The native smoke target links the actual VTMetalRenderer implementation.
#define AVMediaType AVMediaType_FFmpeg
#include "../../app/streaming/video/ffmpeg-renderers/vt.h"
#include "../../app/streaming/video/ffmpeg-renderers/ivrrframepresenter.h"
#include "../../app/streaming/video/ffmpeg-renderers/macdisplaytiming.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.h"
#include "../../app/streaming/session.h"
#include "../../app/path.h"
#ifdef HAVE_PYROWAVE
#include "metalpyrowavefixture.h"
#include "../../app/streaming/video/pyrowave/pyrowavedecoder.h"
#endif
#undef AVMediaType

#include "assertions.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <memory>
#include <cstdio>
#import <Cocoa/Cocoa.h>

Session* Session::s_ActiveSession = nullptr;
QAtomicInt g_AsyncLoggingEnabled;
SDL_Surface* Overlay::OverlayManager::getUpdatedOverlaySurface(Overlay::OverlayType) { return nullptr; }
bool Overlay::OverlayManager::isOverlayEnabled(Overlay::OverlayType) { return false; }

QByteArray Path::readDataFile(QString fileName)
{
    QFile file(QDir(QFileInfo(QString::fromUtf8(__FILE__)).absolutePath())
                   .filePath("../../app/shaders/" + fileName));
    assert(file.open(QIODevice::ReadOnly));
    return file.readAll();
}

extern "C" uint64_t LiGetMicroseconds(void)
{
    const auto counter = SDL_GetPerformanceCounter();
    const auto frequency = SDL_GetPerformanceFrequency();
    return counter / frequency * 1000000 + counter % frequency * 1000000 / frequency;
}

extern "C" bool LiGetHdrMetadata(PSS_HDR_METADATA) { return false; }

namespace {
void serviceDisplay()
{
    SDL_PumpEvents();
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.03, false);
}

AVFrame* makeFrame(AVPixelFormat format)
{
    AVFrame* frame = av_frame_alloc();
    assert(frame != nullptr);
    frame->format = format;
    frame->width = 64;
    frame->height = 64;
    if (format == AV_PIX_FMT_VIDEOTOOLBOX) {
        AVBufferRef* device = nullptr;
        assert(av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
                                     nullptr, nullptr, 0) == 0);
        AVBufferRef* frames = av_hwframe_ctx_alloc(device);
        assert(frames != nullptr);
        auto context = reinterpret_cast<AVHWFramesContext*>(frames->data);
        context->format = AV_PIX_FMT_VIDEOTOOLBOX;
        context->sw_format = AV_PIX_FMT_NV12;
        context->width = context->height = 64;
        assert(av_hwframe_ctx_init(frames) == 0);
        assert(av_hwframe_get_buffer(frames, frame, 0) == 0);
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
        auto pixels = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
        assert(CVPixelBufferLockBaseAddress(pixels, 0) == kCVReturnSuccess);
        for (size_t plane = 0; plane < CVPixelBufferGetPlaneCount(pixels); ++plane) {
            memset(CVPixelBufferGetBaseAddressOfPlane(pixels, plane), 128,
                   CVPixelBufferGetBytesPerRowOfPlane(pixels, plane) * CVPixelBufferGetHeightOfPlane(pixels, plane));
        }
        CVPixelBufferUnlockBaseAddress(pixels, 0);
    } else {
        assert(av_frame_get_buffer(frame, 32) == 0);
    }
    frame->colorspace = AVCOL_SPC_BT709;
    frame->color_primaries = AVCOL_PRI_BT709;
    frame->color_trc = AVCOL_TRC_BT709;
    frame->color_range = AVCOL_RANGE_MPEG;
    if (format == AV_PIX_FMT_VIDEOTOOLBOX) return frame;
    const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(format);
    for (int plane = 0; plane < av_pix_fmt_count_planes(format); ++plane) {
        const int rows = plane == 0 ? frame->height : AV_CEIL_RSHIFT(frame->height, desc->log2_chroma_h);
        if (desc->comp[0].depth > 8) {
            for (int row = 0; row < rows; ++row) {
                auto pixels = reinterpret_cast<uint16_t*>(frame->data[plane] + row * frame->linesize[plane]);
                for (int x = 0; x < frame->linesize[plane] / 2; ++x) pixels[x] = 512;
            }
        } else {
            memset(frame->data[plane], 128, rows * frame->linesize[plane]);
        }
    }
    return frame;
}
}

int main(int argc, char** argv)
{ @autoreleasepool {
    QCoreApplication application(argc, argv);
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addPositionalArgument("trace", "Output worker trace (optional).");
    parser.addOption({"fps", "Worker source frame rate (defaults to display maximum minus four, capped at 116).", "fps"});
    parser.addOption({"frames", "Number of worker frames (default 100).", "frames", "100"});
    parser.process(application);
    bool validFrames = false;
    const int frameCount = parser.value("frames").toInt(&validFrames);
    assert(validFrames && frameCount >= 100 && frameCount <= 10000);
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "1");
    assert(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) == 0);
    SDL_Window* window = SDL_CreateWindow("Moonlight Metal presenter smoke test",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 320, 180,
        SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_SHOWN);
    assert(window != nullptr);
    [NSApp activateIgnoringOtherApps:YES];
    SDL_ShowCursor(SDL_DISABLE);
    serviceDisplay();
    const auto timing = queryMacDisplayTiming(window);
    const auto indexedTiming = queryMacDisplayTimingForDisplay(SDL_GetWindowDisplayIndex(window));
    assert(timing.hasValidTiming());
    assert(timing.maximumFramesPerSecond == indexedTiming.maximumFramesPerSecond);
    bool validRate = true;
    const int workerRate = parser.isSet("fps") ? parser.value("fps").toInt(&validRate) :
        std::max(1, std::min(116, timing.maximumFramesPerSecond - 4));
    assert(validRate && workerRate > 0 && workerRate <= timing.maximumFramesPerSecond);
    std::printf("Native display: %d Hz, %.6f-%.6f ms, granularity %.6f ms\n",
        timing.maximumFramesPerSecond, timing.minimumRefreshInterval * 1000,
        timing.maximumRefreshInterval * 1000, timing.displayUpdateGranularity * 1000);

    DECODER_PARAMETERS params{};
    params.window = window;
    params.videoFormat = VIDEO_FORMAT_H264;
    params.width = params.height = 64;
    params.frameRate = workerRate;
    params.enableVsync = true;
    params.enableVrr = true;
    std::unique_ptr<IFFmpegRenderer> renderer(VTMetalRendererFactory::createRenderer(false));
    assert(renderer->initialize(&params));
    assert(!renderer->getCalibrationIdentity().isEmpty());
    auto presenter = renderer->getVrrFramePresenter();
    assert(presenter != nullptr);
    assert(presenter->checkSupport() == VrrFallbackReason::AdaptivePresentationUnavailable);

    if (timing.supportsVariableRefresh()) {
        renderer.reset();
        assert(SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0);
        const auto transitionStart = LiGetMicroseconds();
        while (!queryMacDisplayTiming(window).nativeFullscreen &&
                LiGetMicroseconds() - transitionStart < 3000000) {
            serviceDisplay();
            SDL_Delay(5);
        }
        assert(queryMacDisplayTiming(window).nativeFullscreen);
        params.enableVsync = false;
        renderer.reset(VTMetalRendererFactory::createRenderer(false));
        assert(renderer->initialize(&params));
        assert(renderer->getVrrFramePresenter()->checkSupport() == VrrFallbackReason::AdaptivePresentationUnavailable);
        renderer.reset();
        params.enableVsync = true;
        renderer.reset(VTMetalRendererFactory::createRenderer(false));
        assert(renderer->initialize(&params));
        presenter = renderer->getVrrFramePresenter();
    }

    if (!timing.supportsVariableRefresh()) {
        assert(presenter->checkSupport() == VrrFallbackReason::AdaptivePresentationUnavailable);
        assert(presenter->restoreFixedPresentation(VrrFallbackReason::AdaptivePresentationUnavailable));
        AVFrame* frame = makeFrame(AV_PIX_FMT_YUV420P);
        renderer->renderFrame(frame);
        av_frame_free(&frame);
        serviceDisplay();
        std::puts("Fixed display fallback passed; adaptive smoke requires a variable display.");
    } else {
        assert(presenter->checkSupport() == VrrFallbackReason::NoFallback);
        assert(presenter->canLatchAdaptivePresent());
        int observedPresentations = 0;
        uint64_t lastSubmissionId = 0;
        for (const auto format : {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NV12, AV_PIX_FMT_YUV444P10,
                                 AV_PIX_FMT_VIDEOTOOLBOX}) {
            AVFrame* cancelledFrame = makeFrame(format);
            const auto cancelledPreparation = presenter->prepareFrame(cancelledFrame, 0);
            assert(cancelledPreparation.prepared && cancelledPreparation.sourceFrameReusable);
            av_frame_free(&cancelledFrame);
            const auto cancelled = presenter->cancelFrame();
            assert(cancelled.cancelled && !cancelled.presented && !cancelled.nativeBackendValid);
            assert(presenter->presentAdaptive({}).cancelled);

            for (int index = 0; index < 4; ++index) {
                AVFrame* frame = makeFrame(format);
                const auto preparation = presenter->prepareFrame(frame, 0);
                assert(preparation.prepared && preparation.sourceFrameReusable && preparation.timingValid);
                assert(preparation.feedback.gpuReadyAttempted && preparation.feedback.gpuReadyTimingValid);
                av_frame_free(&frame); // Exercise source recycling before target presentation.
                const auto feedback = presenter->presentAdaptive({});
                assert(feedback.presented && !feedback.cancelled && feedback.nativeBackendValid);
                assert(feedback.nativeBackend == VrrNativePresentationBackend::Metal);
                assert(feedback.nativePresentResultValid && feedback.nativePresentResult == 0);
                assert(feedback.gpuReadyTimingValid && feedback.gpuReadyTimeUs == preparation.feedback.gpuReadyTimeUs);
                assert(feedback.submissionIdValid && feedback.submissionId > lastSubmissionId);
                assert(!feedback.nativePresentParametersValid && !feedback.nativeVrrStateValid);
                assert(!feedback.submissionIdQueryResultValid && !feedback.frameStatsQueryResultValid);
                assert(!feedback.latchRawSyncQpcValid);
                lastSubmissionId = feedback.submissionId;
                if (feedback.latchSampleValid) {
                    assert(feedback.latchTimeKind == Vrr13::PresentationTimeKind::DisplayEvent);
                    assert(feedback.latchSubmissionId <= feedback.submissionId);
                    assert(feedback.latchTimeUs != 0 && feedback.presentationUncertaintyUs <= 251);
                    ++observedPresentations;
                }
                serviceDisplay();
            }
        }
        assert(observedPresentations > 0);

#ifdef HAVE_PYROWAVE
        IPyroWaveVulkanPool* pyroWavePool = nullptr;
        for (int testCase = 0; testCase < 2; ++testCase) {
            const bool chroma444 = testCase == 1;
            const bool tenBit = testCase == 1;
            // A pool's cached Vulkan views belong to one negotiated format.
            // Changing the stream format recreates the renderer in production.
            if (testCase != 0) {
                renderer.reset();
                renderer.reset(VTMetalRendererFactory::createRenderer(false));
                assert(renderer->initialize(&params));
                presenter = renderer->getVrrFramePresenter();
            }
            pyroWavePool = renderer->getPyroWaveVulkanPool();
            assert(pyroWavePool != nullptr);
            auto fixture = decodeMetalPyroWaveFixture(pyroWavePool, chroma444, tenBit);
            assert(fixture && fixture->frame && fixture->decoder->hasAsynchronousOutput());
            for (int plane = 0; plane < 3; ++plane) assert(fixture->frame->data[plane] == nullptr);
            const auto decodeWaitUs = presenter->waitForDecode(fixture->frame);
            assert(presenter->checkSupport() == VrrFallbackReason::NoFallback);
            assert(presenter->prepareFrame(fixture->frame, 0).prepared);
            assert(presenter->cancelFrame().cancelled);
            const auto prepared = presenter->prepareFrame(fixture->frame, 0);
            assert(prepared.prepared && prepared.sourceFrameReusable);
            av_frame_free(&fixture->frame);
            fixture->decoder.reset(); // Native drawable now owns the rendered output.
            const auto presented = presenter->presentAdaptive({});
            assert(presented.presented && presented.nativeBackend == VrrNativePresentationBackend::Metal);
            assert(presented.gpuReadyTimingValid);
            serviceDisplay();
            std::printf("Combined PyroWave %s/%d-bit shared decode -> Metal cancel/present passed, decode wait %llu us.\n",
                chroma444 ? "4:4:4" : "4:2:0", tenBit ? 10 : 8,
                (unsigned long long)decodeWaitUs);
        }
#endif

        // Exercise the shared worker against real Metal drawables, retaining a
        // replay-grade trace of its completion waits and delayed display events.
        SDL_RaiseWindow(window);
        [NSApp activateIgnoringOtherApps:YES];
        const uint64_t settleStart = LiGetMicroseconds();
        while (LiGetMicroseconds() - settleStart < 1000000) serviceDisplay();
        const bool lowPower = NSProcessInfo.processInfo.lowPowerModeEnabled;
        std::printf("Native test environment: fullscreen %d, active %d, focused %d, low power %d\n",
            queryMacDisplayTiming(window).nativeFullscreen, NSApp.active,
            !!(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS), lowPower);
        bool focusLost = false;
        QTemporaryDir traceDirectory;
        assert(traceDirectory.isValid());
        const QString tracePath = !parser.positionalArguments().isEmpty() ?
            QFileInfo(parser.positionalArguments().first()).absoluteFilePath() :
            traceDirectory.filePath("native-metal.vrrtrace");
        qputenv("MOONLIGHT_VRR_TRACE", QFile::encodeName(tracePath));
        qputenv("MOONLIGHT_VRR_DEEP_TRACE", "1");
        PacerTelemetry telemetry;
        VrrSessionConfig config;
        config.displayRefreshHz = timing.maximumFramesPerSecond;
        config.streamRateHz = workerRate;
        {
            VrrPacingWorker worker(presenter, config, &telemetry);
            assert(worker.start());
            const uint64_t sourceStart = LiGetMicroseconds();
            for (int index = 0; index < frameCount; ++index) {
                const uint64_t dueUs = sourceStart + uint64_t(index) * 1000000 / config.streamRateHz;
                while (LiGetMicroseconds() < dueUs) {
                    SDL_PumpEvents();
                    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) focusLost = true;
                    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.0005, false);
                    if (LiGetMicroseconds() + 1000 < dueUs) SDL_Delay(1);
                }
                AVFrame* image = makeFrame(AV_PIX_FMT_YUV420P);
                const uint64_t decodedUs = LiGetMicroseconds();
                PacedFrame paced(image, index + 1,
                    uint32_t(uint64_t(index) * 90000 / config.streamRateHz), true, decodedUs);
                paced.setDeliveryTimeline(decodedUs - 1000, decodedUs - 300, decodedUs - 200);
                worker.submit(std::move(paced));
            }
            const uint64_t drainStart = LiGetMicroseconds();
            for (;;) {
                const auto stats = telemetry.snapshot();
                if (stats.vrrPresentedFrames + stats.vrrPacingDroppedFrames == uint64_t(frameCount)) break;
                assert(LiGetMicroseconds() - drainStart < 3000000);
                serviceDisplay();
                SDL_Delay(1);
            }
        }
        qunsetenv("MOONLIGHT_VRR_TRACE");
        qunsetenv("MOONLIGHT_VRR_DEEP_TRACE");
        const auto workerStats = telemetry.snapshot();
        assert(workerStats.vrrCadenceIntervals > 0);
        assert(workerStats.vrrPresentFailedFrames == 0);
        std::printf("Native worker: %llu presented, %llu dropped, %llu display intervals; trace %s\n",
            (unsigned long long)workerStats.vrrPresentedFrames,
            (unsigned long long)workerStats.vrrPacingDroppedFrames,
            (unsigned long long)workerStats.vrrCadenceIntervals,
            qPrintable(tracePath));
        std::fflush(stdout);
        std::printf("Native test lost focus: %d\n", focusLost);
        std::fflush(stdout);
        assert(!focusLost);
        assert(workerStats.vrrPresentedFrames >= uint64_t(frameCount) * 9 / 10);

        AVFrame* frame = makeFrame(AV_PIX_FMT_YUV420P);
        assert(presenter->prepareFrame(frame, 0).prepared);
        presenter->setSuspended(true);
        assert(presenter->presentAdaptive({}).cancelled);
        assert(!presenter->prepareFrame(frame, 0).prepared);
        presenter->setSuspended(false);
        assert(presenter->prepareFrame(frame, 0).prepared);
        assert(presenter->presentAdaptive({}).presented);
        WINDOW_STATE_CHANGE_INFO changed{};
        changed.stateChangeFlags = WINDOW_STATE_CHANGE_DISPLAY;
        assert(!renderer->notifyWindowChanged(&changed));
        assert(presenter->restoreFixedPresentation(VrrFallbackReason::InitializationFailed));
        assert(presenter->checkSupport() == VrrFallbackReason::AdaptivePresentationUnavailable);
        renderer->renderFrame(frame);
        av_frame_free(&frame);
        serviceDisplay();
#ifdef HAVE_PYROWAVE
        auto fixedFixture = decodeMetalPyroWaveFixture(pyroWavePool, true, true);
        assert(fixedFixture && fixedFixture->frame);
        renderer->renderFrame(fixedFixture->frame);
        av_frame_free(&fixedFixture->frame);
        fixedFixture->decoder.reset();
        serviceDisplay();
        std::puts("Combined PyroWave shared decode -> fixed Metal/CAMetalDisplayLink passed.");
#endif
        std::printf("Native prepare/present/cancel/suspend/fallback passed; %d presentation samples.\n",
                    observedPresentations);
    }
    renderer.reset();
    serviceDisplay(); // Late drawable callbacks must be safe after renderer destruction.
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}}
