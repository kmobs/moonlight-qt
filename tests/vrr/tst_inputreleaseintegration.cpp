#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include <cstdio>
#include <algorithm>
int main() {
    for (bool deliveryKnown : {false, true}) {
        VrrSessionConfig session;
        session.streamRateHz = 100; session.displayRefreshHz = 120;
        session.latencyMode = 1; session.smoothFrameTiming = false;
        auto policy = vrrTimingParametersForSession(session);
        policy.playoutAbsorbedJitterRelease = 1; // Explicit experimental guard.
        policy.playoutDelayStartUs = 6000;
        policy.playoutDelayStartPeriodPerMille = 0;
        policy.playoutOnTimeTargetPerMillion = 990000;
        VrrTimingController controller(session, true, policy);
        uint64_t noiseHeld = 0, finalDelay = 0, evidence = 0;
        for (int i = 0; i < 14000; ++i) {
            const uint64_t slot = 1000000 + uint64_t(i) * 10000;
            const uint64_t decoded = slot + (i < 6000 && i % 2 ? 3000 : 0);
            PacedFrame frame(nullptr, i, uint32_t(i * 900), true, decoded);
            if (deliveryKnown) frame.setDeliveryTimeline(decoded - 800, decoded - 500, decoded - 250);
            auto d = controller.schedule(frame, decoded);
            controller.notePreparationDuration(500, 0, decoded + 500);
            controller.noteSubmission(true, false, std::max(d.targetUs, decoded + 500));
            const auto stats = controller.intervalStats();
            if (i == 5999) { noiseHeld = stats.inputReleaseHeld; evidence = stats.inputEvidenceUs; }
            finalDelay = d.playoutDelayUs;
        }
        const auto stats = controller.intervalStats();
        std::printf("delivery=%d noise-held=%llu evidence=%llu unknown=%llu final=%llu\n", deliveryKnown,
            (unsigned long long)noiseHeld, (unsigned long long)evidence,
            (unsigned long long)stats.inputReleaseUnknown, (unsigned long long)finalDelay);
        if (deliveryKnown ? !noiseHeld || evidence < 1000000 || finalDelay > 1500 :
            noiseHeld || !stats.inputReleaseUnknown) return 1;
    }
    return 0;
}
