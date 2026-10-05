#include "assertions.h"
#include "overlaymanager.h"
#include "../../app/streaming/video/clientpacingwarning.h"
#include <QCoreApplication>
#include <QDir>
#include <cassert>
#include <future>
#include <iostream>
#include <vector>
#include <array>
#include <cmath>
#include <limits>

using namespace Overlay;
using namespace std::chrono_literals;

class Presenter : public IOverlayRenderer {
public:
    explicit Presenter(OverlayManager& manager) : manager(manager) {}
    void notifyOverlayUpdated(OverlayType type) override {
        assert(std::this_thread::get_id() != producer);
        SDL_Surface* surface = manager.getUpdatedOverlaySurface(type);
        const auto text = manager.getOverlayText(type);
        const bool enabled = manager.isOverlayEnabled(type);
        std::unique_lock<std::mutex> guard(lock);
        if (type == OverlayDebug) {
            last = text;
            height = surface ? surface->h : 0;
            width = surface ? surface->w : 0;
            colors = {};
            rowColors = {};
            minInkY.fill(std::numeric_limits<int>::max()); maxInkY.fill(-1);
            if (surface && surface->format->format == SDL_PIXELFORMAT_ARGB8888) {
                // Planned, submission, display and clipped-spike inks.
                const std::array<Uint32, 4> palette{
                    SDL_MapRGBA(surface->format, 190, 198, 210, 255),
                    SDL_MapRGBA(surface->format, 65, 215, 250, 255),
                    SDL_MapRGBA(surface->format, 245, 110, 220, 255),
                    SDL_MapRGBA(surface->format, 255, 95, 65, 255)};
                const double scale = manager.getOverlayFontSize(OverlayDebug) / 20.0;
                const auto S = [scale](int v) { return int(std::lround(v * scale)); };
                for (int y = 0; y < surface->h; ++y) {
                    const auto row = reinterpret_cast<const Uint32*>(static_cast<const Uint8*>(surface->pixels) + y * surface->pitch);
                    for (int x = 0; x < surface->w; ++x)
                        for (size_t k = 0; k < palette.size(); ++k) {
                            colors[k] += row[x] == palette[k];
                            for (int graphRow = 0; graphRow < TimingGraphLayout::Lanes; ++graphRow) {
                                if (x >= S(TimingGraphLayout::Left) && y >= S(TimingGraphLayout::plotTop(graphRow)) &&
                                    y <= S(TimingGraphLayout::plotBottom(graphRow)) + S(2) && row[x] == palette[k]) {
                                    ++rowColors[graphRow][k];
                                    minInkY[graphRow] = std::min(minInkY[graphRow], y);
                                    maxInkY[graphRow] = std::max(maxInkY[graphRow], y);
                                }
                            }
                        }
                }
                const auto preview = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_PREVIEW");
                if (!preview.isEmpty()) assert(SDL_SaveBMP(surface, qPrintable(preview)) == 0);
            }
        }
        SDL_FreeSurface(surface);
        ++calls;
        if (type == OverlayDebug && enabled && block) {
            entered = true;
            ready.notify_all();
            ready.wait(guard, [&] { return !block; });
        }
        ready.notify_all();
    }
    void awaitText(const std::string& text) {
        std::unique_lock<std::mutex> guard(lock);
        assert(ready.wait_for(guard, 3s, [&] { return last == text; }));
    }
    void awaitSize(int expectedWidth) {
        std::unique_lock<std::mutex> guard(lock);
        assert(ready.wait_for(guard, 3s, [&] { return width == expectedWidth; }));
    }
    OverlayManager& manager;
    const std::thread::id producer = std::this_thread::get_id();
    std::mutex lock;
    std::condition_variable ready;
    bool block = false, entered = false;
    unsigned calls = 0;
    std::string last;
    int height = 0, width = 0;
    std::array<unsigned, 4> colors{};
    std::array<std::array<unsigned, 4>, TimingGraphLayout::Lanes> rowColors{};
    std::array<int, TimingGraphLayout::Lanes> minInkY{}, maxInkY{};
};

static TimingGraphSnapshot testTimingGraph()
{
    TimingGraphHistory history;
    TimingGraphSnapshot points;
    points.reserve(TimingGraphHistory::Capacity);
    TimingGraphInput in;
    in.submitted = true; in.idValid = true; in.backend = 2;
    in.submissionUs = in.targetUs = 1000000; in.sourcePeriodUs = 10000;
    in.bufferUs = in.requestedBufferUs = 2500;
    in.submissionId = UINT64_MAX - 1000; // Preserve full-width identity.
    history.record(in);
    const auto firstId = in.submissionId;
    in.submissionId++; in.submissionUs += 12000; in.targetUs += 10000;
    in.displayValid = true; in.displayId = firstId; in.displayUs = 1002000;
    history.record(in);
    history.copyTo(points);
    assert(points.size() == 2 && points[0].displayUs == 1002000 && points[1].displayUs == 0);
    assert(points[1].submissionIntervalUs == 12000 && points[1].targetIntervalUs == 10000);
    // Invalid samples, duplicate feedback, and pre-submission timestamps must
    // never become new display events or attach to the reporting frame.
    in.submitted = false; in.displayUs = 1003000; history.record(in);
    in.displayId++; in.displayUs = 1001000; history.record(in);
    in.displayUs = 1015000; in.displayValid = false; history.record(in);
    history.copyTo(points);
    assert(points[0].displayUs == 1002000 && points[1].displayUs == 0);
    in.discontinuity = true; in.submitted = true; in.submissionUs += 10000; in.targetUs += 10000;
    in.submissionId++; in.displayValid = true; in.displayId = firstId; in.displayUs = 1004000;
    history.record(in); history.copyTo(points);
    assert(points.back().breakBefore && points.back().submissionIntervalUs == 0 && points[0].displayUs == 1002000);
    // Fixed storage wraps; the snapshot holds the drawn frames plus one
    // predecessor and keeps individual hitches rather than averages.
    in.discontinuity = false;
    for (size_t i = 0; i < TimingGraphHistory::Capacity + 100; ++i) {
        in.submissionId++; in.submissionUs += 10000 + (i % 23 == 0 ? 3000 : 0); in.targetUs += 10000;
        in.bufferUs = i % 60 < 30 ? 2500 : 4500;
        in.requestedBufferUs = i % 60 < 20 ? 2500 : 5000;
        in.displayId = in.submissionId; in.displayUs = in.submissionUs + 2000 + (i % 11 == 0 ? 2000 : 0);
        history.record(in);
    }
    history.copyTo(points);
    assert(points.size() == TimingGraphHistory::SnapshotPoints);
    bool hitch = false, jump = false;
    for (size_t i = 1; i < points.size(); ++i) {
        hitch |= points[i].submissionIntervalUs == 13000;
        jump |= points[i].bufferUs != points[i-1].bufferUs;
    }
    assert(hitch && jump);
    return points;
}

static void testLaneIntervals()
{
    TimingGraphSnapshot points(4);
    for (size_t i = 0; i < points.size(); ++i) {
        points[i].submissionUs = 1000000 + i * 10000;
        points[i].displayUs = points[i].submissionUs + 20000 + (i == 2 ? 3000 : 0);
        points[i].targetIntervalUs = 10000;
        points[i].submissionIntervalUs = i ? 10000 : 0;
        points[i].backend = 2;
    }
    using Lane = TimingGraphLane;
    uint64_t interval = 0;
    assert(timingGraphInterval(points, 0, Lane::Target, interval) && interval == 10000);
    assert(!timingGraphInterval(points, 0, Lane::Submit, interval));
    assert(!timingGraphInterval(points, 0, Lane::Display, interval)); // No predecessor in the snapshot.
    assert(timingGraphInterval(points, 2, Lane::Display, interval) && interval == 13000);
    assert(timingGraphInterval(points, 3, Lane::Display, interval) && interval == 7000);
    // A frame without OS feedback removes both intervals that touch it,
    // instead of drawing one long interval across the unobserved frame.
    auto missing = points; missing[2].displayUs = 0;
    assert(!timingGraphInterval(missing, 2, Lane::Display, interval));
    assert(!timingGraphInterval(missing, 3, Lane::Display, interval));
    auto rebased = points; rebased[2].breakBefore = true;
    assert(!timingGraphInterval(rebased, 2, Lane::Display, interval));
    rebased = points; rebased[3].generation++;
    assert(!timingGraphInterval(rebased, 3, Lane::Display, interval));
    rebased = points; rebased[3].backend++;
    assert(!timingGraphInterval(rebased, 3, Lane::Display, interval));
    rebased = points; rebased[3].displayUs = rebased[2].displayUs;
    assert(!timingGraphInterval(rebased, 3, Lane::Display, interval));
    assert(!timingGraphInterval(points, points.size(), Lane::Target, interval));
}

int main(int argc, char** argv)
{
    testLaneIntervals();
    {
        using Reason = ClientPacingWarning::Reason;
        ClientPacingWarning warning;
        for (uint64_t t = 1; t <= 6; ++t)
            assert(warning.observe(t * 1000000, true, true, true, false, false, 98.0) == Reason::None);
        assert(warning.observe(7000000, true, true, true, false, true, 98.0) == Reason::None);
        assert(warning.observe(8000000, true, true, true, false, true, 98.0) == Reason::None);
        assert(warning.observe(9000000, true, true, true, false, true, 98.0) == Reason::BufferLimit);
        for (uint64_t t = 10; t <= 14; ++t)
            assert(warning.observe(t * 1000000, true, true, true, false, false, 98.0) == Reason::BufferLimit);
        assert(warning.observe(15000000, true, true, false, false, false, 98.0) == Reason::None);
        for (uint64_t t = 16; t <= 20; ++t)
            assert(warning.observe(t * 1000000, true, true, true, false, true, 98.0) == Reason::None);
        // A suspend/reconnect resets startup qualification and old warnings.
        assert(warning.observe(40000000, true, true, true, false, true, 98.0) == Reason::None);
        assert(warning.observe(41000000, true, false, true, false, true, 98.0) == Reason::None);
        assert(warning.observe(42000000, true, true, true, true, true, 98.0) == Reason::None);
        assert(warning.observe(43000000, true, true, true, true, true, 98.0) == Reason::None);
        assert(warning.observe(44000000, true, true, true, true, true, 98.0) == Reason::ProcessingOverload);
        assert(warning.observe(45000000, false, true, true, true, true, 98.0) == Reason::None);
        for (const double quality : {99.01, 99.5, 100.0}) {
            ClientPacingWarning healthy;
            for (uint64_t t = 1; t <= 12; ++t)
                assert(healthy.observe(t * 1000000, true, true, true, true, true, quality) == Reason::None);
        }
        for (const bool overloaded : {false, true}) {
            ClientPacingWarning threshold;
            for (uint64_t t = 1; t <= 4; ++t)
                threshold.observe(t * 1000000, true, true, true, overloaded, true, 99.0);
            assert(threshold.observe(5000000, true, true, true, overloaded, true, 99.0) != Reason::None);
            assert(threshold.observe(6000000, true, true, true, overloaded, true, 99.01) == Reason::None);
            assert(threshold.observe(7000000, true, true, true, overloaded, true, 99.0) == Reason::None);
        }
        // Even sustained processing overload must not warn below the cap.
        ClientPacingWarning belowCap;
        for (uint64_t t = 1; t <= 12; ++t)
            assert(belowCap.observe(t * 1000000, true, true, false, true, true, 95.0) == Reason::None);
        for (uint64_t t = 13; t <= 15; ++t)
            belowCap.observe(t * 1000000, true, true, true, true, true, 99.0);
        assert(belowCap.observe(16000000, true, true, true, true, true, 99.0) == Reason::ProcessingOverload);
        assert(belowCap.observe(17000000, true, true, false, true, true, 95.0) == Reason::None);
        const auto limited = ClientPacingWarning::message(Reason::BufferLimit, true, true, false);
        assert(limited.find("HEVC") != std::string::npos && limited.find("Try Smooth") != std::string::npos);
        assert(limited.find("network") == std::string::npos);
        assert(ClientPacingWarning::message(Reason::BufferLimit, false, true, true).find("HEVC") == std::string::npos);
        assert(ClientPacingWarning::message(Reason::BufferLimit, true, false, true).find("Try Smooth") == std::string::npos);
        assert(ClientPacingWarning::message(Reason::BufferLimit, true, false, true).find("HEVC") == std::string::npos);
        assert(ClientPacingWarning::message(Reason::ProcessingOverload, true, true, false).find("Try Smooth") == std::string::npos);
    }
    QCoreApplication app(argc, argv);
    assert(argc == 2 && QDir::setCurrent(argv[1])); // Directory containing ModeSeven.ttf.
    assert(SDL_Init(SDL_INIT_TIMER) == 0);
    const auto graph = testTimingGraph();
    {
        OverlayManager manager;
        manager.setStatusMessage(StatusSource::Network, "network loss");
        manager.setStatusMessage(StatusSource::ClientPacing, "client pacing");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "network loss\n\nclient pacing");
        manager.setStatusMessage(StatusSource::Mouse, "mouse");
        manager.setStatusMessage(StatusSource::Network, "");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "mouse");
        manager.setStatusMessage(StatusSource::Mouse, "");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "client pacing");
        manager.setStatusMessage(StatusSource::Network, "network loss");
        manager.setStatusMessage(StatusSource::ClientPacing, "");
        assert(manager.getOverlayText(OverlayStatusUpdate) == "network loss");
        manager.setStatusMessage(StatusSource::Network, "");
        assert(!manager.isOverlayEnabled(OverlayStatusUpdate));
        Presenter old(manager), replacement(manager);
        manager.setOverlayRenderer(&old);
        manager.updateOverlayText(OverlayDebug, "initial");
        manager.setOverlayState(OverlayDebug, true);
        old.awaitText("initial");
        {
            std::lock_guard<std::mutex> guard(old.lock);
            old.block = true;
        }
        manager.updateOverlayText(OverlayDebug, "blocked upload");
        {
            std::unique_lock<std::mutex> guard(old.lock);
            assert(old.ready.wait_for(guard, 3s, [&] { return old.entered; }));
        }
        // Producers must complete while the renderer callback remains blocked.
        // Superseded requests are coalesced instead of building an unbounded queue.
        auto producer = std::async(std::launch::async, [&] {
            for (unsigned i = 0; i < 1000; ++i)
                manager.updateOverlayText(OverlayDebug, std::to_string(i).c_str());
            manager.setOverlayState(OverlayDebug, false);
            manager.updateOverlayText(OverlayDebug, "latest");
            manager.setOverlayState(OverlayDebug, true);
        });
        assert(producer.wait_for(1s) == std::future_status::ready);
        producer.get();
        auto detached = std::async(std::launch::async, [&] { manager.setOverlayRenderer(nullptr); });
        assert(detached.wait_for(30ms) == std::future_status::timeout);
        {
            std::lock_guard<std::mutex> guard(old.lock);
            old.block = false;
            old.ready.notify_all();
        }
        assert(detached.wait_for(3s) == std::future_status::ready);
        detached.get();
        manager.setOverlayRenderer(&replacement);
        replacement.awaitText("latest");
        assert(!manager.isTimingGraphEnabled());
        manager.setTimingGraphState(true);
        manager.updateOverlayText(OverlayDebug, "Live timing graph\nIncoming smoothness: 99.8%", graph);
        replacement.awaitText("Live timing graph\nIncoming smoothness: 99.8%");
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(replacement.height > TimingGraphLayout::Height);
            // Each series is drawn only in its own lane, so drift between
            // planned, submitted and displayed cadence is never overdrawn.
            for (int lane = 0; lane < TimingGraphLayout::Lanes; ++lane)
                for (int ink = 0; ink < 3; ++ink)
                    assert((replacement.rowColors[lane][ink] > 100) == (lane == ink));
            assert(replacement.rowColors[1][3] > 0); // Submission hitches beyond the axis are marked.
            assert(replacement.rowColors[2][3] > 0); // So are display spikes.
        }
        int normalWidth, normalHeight;
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            normalWidth = replacement.width; normalHeight = replacement.height;
        }
        assert(manager.getOverlayFontSize(OverlayDebug) == 20);
        const auto preview = qEnvironmentVariable("MOONLIGHT_OVERLAY_TEST_PREVIEW");
        if (!preview.isEmpty()) qputenv("MOONLIGHT_OVERLAY_TEST_PREVIEW", qPrintable(preview + ".4k.bmp"));
        manager.setOutputSize(3840, 2160);
        replacement.awaitSize(int(std::lround(normalWidth * 1.3)));
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(manager.getOverlayFontSize(OverlayDebug) == 26);
            assert(manager.getOverlayFontSize(OverlayStatusUpdate) == 36);
            assert(std::abs(replacement.height - normalHeight * 1.3) < 4);
        }
        // Capture the lanes at both output sizes.
        qunsetenv("MOONLIGHT_OVERLAY_TEST_PREVIEW");
        manager.setOutputSize(1920, 1080);
        replacement.awaitSize(normalWidth);
        assert(manager.getOverlayFontSize(OverlayDebug) == 20);
        assert(replacement.height == normalHeight);
        // Variation within the flat zone is drawn on the reference line;
        // larger variation is drawn at its true height.
        const auto jitter = [](uint64_t amplitudeUs) {
            TimingGraphSnapshot points;
            for (unsigned i = 0; i < 300; ++i) {
                TimingGraphPoint point;
                point.sourcePeriodUs = point.targetIntervalUs = point.submissionIntervalUs = 10000;
                point.targetUs = point.submissionUs = 1000000 + i * 10000;
                point.displayUs = point.submissionUs + 2000 + (i % 2 ? amplitudeUs : 0);
                points.push_back(point);
            }
            return points;
        };
        manager.updateOverlayText(OverlayDebug, "Unnoticeable variation", jitter(900));
        replacement.awaitText("Unnoticeable variation");
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            for (int lane = 0; lane < TimingGraphLayout::Lanes; ++lane)
                assert(replacement.maxInkY[lane] - replacement.minInkY[lane] <= 2);
            assert(replacement.rowColors[2][2] > 100);
        }
        manager.updateOverlayText(OverlayDebug, "Noticeable variation", jitter(1500));
        replacement.awaitText("Noticeable variation");
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(replacement.maxInkY[2] - replacement.minInkY[2] >= 30);
            assert(replacement.maxInkY[0] - replacement.minInkY[0] <= 2);
            assert(replacement.maxInkY[1] - replacement.minInkY[1] <= 2);
            for (const auto& row : replacement.rowColors) assert(row[3] == 0);
        }
        // A backend without DisplayEvent feedback must leave the display lane
        // empty rather than painting submission timestamps as presentation.
        auto withoutDisplay = graph;
        for (auto& point : withoutDisplay) point.displayUs = 0;
        manager.updateOverlayText(OverlayDebug, "No OS display feedback", std::move(withoutDisplay));
        replacement.awaitText("No OS display feedback");
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(replacement.rowColors[0][0] > 100 && replacement.rowColors[1][1] > 100);
            assert(replacement.rowColors[2][2] == 0);
        }
        // Graph-only refreshes redraw without new stats text.
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            replacement.rowColors = {};
        }
        manager.updateTimingGraph(graph);
        {
            std::unique_lock<std::mutex> guard(replacement.lock);
            assert(replacement.ready.wait_for(guard, 3s, [&] { return replacement.rowColors[2][2] > 100; }));
        }
        // The graph and stats text toggle independently. The graph shows on
        // its own while the stats text is hidden.
        manager.setOverlayState(OverlayDebug, false);
        assert(manager.isOverlayEnabled(OverlayDebug) && !manager.isStatsEnabled() && manager.isTimingGraphEnabled());
        manager.updateTimingGraph(graph);
        {
            std::unique_lock<std::mutex> guard(replacement.lock);
            assert(replacement.ready.wait_for(guard, 3s, [&] {
                return replacement.height == TimingGraphLayout::Height && replacement.rowColors[2][2] > 100;
            }));
        }
        // Stats text alone ignores graph snapshots while the graph is off.
        manager.setTimingGraphState(false);
        assert(!manager.isOverlayEnabled(OverlayDebug));
        manager.setOverlayState(OverlayDebug, true);
        manager.updateOverlayText(OverlayDebug, "Stats only", graph);
        manager.updateTimingGraph(graph);
        replacement.awaitText("Stats only");
        {
            std::lock_guard<std::mutex> guard(replacement.lock);
            assert(replacement.height > 0 && replacement.height < TimingGraphLayout::Height);
            for (const auto& row : replacement.rowColors)
                for (auto count : row) assert(count == 0);
        }
        manager.setOverlayState(OverlayDebug, false);
        manager.updateOverlayText(OverlayDebug, "hidden");
        replacement.awaitText("hidden"); // Updating disabled text cannot cancel the hide request.
        assert(!manager.isOverlayEnabled(OverlayDebug));
        manager.setOverlayRenderer(nullptr);
        IOverlayRenderer::UpdateTiming timing;
        bool measured = false;
        while (replacement.takeOverlayTiming(timing)) {
            assert(timing.queueNs >= 0 && timing.rasterNs >= 0 && timing.dispatchNs >= 0);
            measured = true;
        }
        assert(measured && old.calls < 20);
    }
    SDL_Quit();
    std::cout << "Overlay worker isolation, coalescing, renderer lifetime, replacement and timing checks passed\n";
}
