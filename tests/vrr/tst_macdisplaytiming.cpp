#include "../../app/streaming/video/ffmpeg-renderers/macdisplaytiming.h"

#include "assertions.h"
#include <limits>

int main()
{
    const MacDisplayTiming adaptive{120, 1.0 / 120, 1.0 / 48, 0};
    assert(adaptive.hasValidTiming());
    assert(adaptive.supportsVariableRefresh());

    // ProMotion may describe discrete supported rates rather than continuous
    // Adaptive-Sync. Its changing interval still qualifies for Metal pacing.
    const MacDisplayTiming promotion{120, 1.0 / 120, 1.0 / 24, 1.0 / 120};
    assert(promotion.supportsVariableRefresh());

    const MacDisplayTiming fixed{120, 1.0 / 120, 1.0 / 120, 1.0 / 120};
    assert(fixed.hasValidTiming());
    assert(!fixed.supportsVariableRefresh());
    assert(!MacDisplayTiming{}.hasValidTiming());
    assert((!MacDisplayTiming{120, 0, 1.0 / 48, 0}.hasValidTiming()));
    assert((!MacDisplayTiming{120, 1.0 / 48, 1.0 / 120, 0}.hasValidTiming()));
    assert((!MacDisplayTiming{120, 1.0 / 120, 1.0 / 48, -1}.hasValidTiming()));
    assert((!MacDisplayTiming{0, 1.0 / 120, 1.0 / 48, 0}.hasValidTiming()));
    assert((!MacDisplayTiming{120, std::numeric_limits<double>::quiet_NaN(),
                            1.0 / 48, 0}.hasValidTiming()));
    assert((!MacDisplayTiming{120, 1.0 / 120,
                            std::numeric_limits<double>::infinity(), 0}.hasValidTiming()));
    return 0;
}
