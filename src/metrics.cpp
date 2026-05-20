// metrics.cpp — Counter helpers for runtime telemetry.

#include "metrics.h"

#include <string.h>

void metricsInit(Metrics& m) {
    // Zero everything explicitly so `freeHeapMin` starts at 0 and is
    // bumped to the first observed value on the first sample (rather
    // than starting at UINT32_MAX, which the Arduino tick would
    // immediately overwrite anyway but reads confusingly during a
    // gdb session before boot completes).
    memset(&m, 0, sizeof(m));
}

uint32_t metricsIncSat(uint32_t& counter, uint32_t by) {
    // Saturating add. The two-step check defends against either
    // operand being near the rail; the sum check alone misses the
    // case where `by` itself is the overflow source.
    uint32_t prev = counter;
    uint32_t sum  = prev + by;
    if (sum < prev) {
        counter = UINT32_MAX;
    } else {
        counter = sum;
    }
    return counter;
}

uint32_t metricsAddSat(uint32_t& counter, uint32_t by) {
    // Same logic as Inc; kept as a separate symbol for call-site
    // clarity (`Add` for cumulative quantities, `Inc` for event
    // tallies). The compiler folds them anyway.
    return metricsIncSat(counter, by);
}
