#include "daemon/health.h"

#include <string>

namespace dinero::health {

const char* StatusToString(Status s) {
    switch (s) {
        case Status::Ok:       return "OK";
        case Status::Degraded: return "DEGRADED";
        case Status::Failing:  return "FAILING";
    }
    // Unreachable in well-formed code; defensive fallback for future enum
    // additions that forget to update this switch.
    return "UNKNOWN";
}

Result ComputeHealthStatus(const ChecksData& d) {
    Result r;

    // ─────────────────────────────────────────────────────────────
    // Failing checks first (most-severe). Order: safemode (operator
    // intervention required), undo-missing (consensus-reorg risk),
    // very-stale-tip (chain disconnected from network).
    // ─────────────────────────────────────────────────────────────
    if (d.safemode_active) {
        r.status    = Status::Failing;
        r.exit_code = 2;
        r.reason    = "safemode active";
        return r;
    }
    if (!d.tip_undo_present) {
        r.status    = Status::Failing;
        r.exit_code = 2;
        r.reason    = "tip undo data missing";
        return r;
    }
    if (d.tip_age_seconds > kTipAgeThresholdFailingSec) {
        r.status    = Status::Failing;
        r.exit_code = 2;
        r.reason    = "tip > 2h stale";
        return r;
    }

    // ─────────────────────────────────────────────────────────────
    // Degraded checks. Order matters for determinism: stale-tip,
    // low-peers, recent-fatal.
    // ─────────────────────────────────────────────────────────────
    if (d.tip_age_seconds > kTipAgeThresholdDegradedSec) {
        r.status    = Status::Degraded;
        r.exit_code = 1;
        r.reason    = "tip > 30min stale";
        return r;
    }
    if (d.peer_count < kPeerCountThreshold) {
        r.status    = Status::Degraded;
        r.exit_code = 1;
        r.reason    = "peer count below threshold";
        return r;
    }
    if (d.fatal_in_last_5min > 0) {
        r.status    = Status::Degraded;
        r.exit_code = 1;
        r.reason    = "fatal log entries in last 5 min";
        return r;
    }

    // All checks passed.
    r.status    = Status::Ok;
    r.exit_code = 0;
    r.reason.clear();
    return r;
}

}  // namespace dinero::health
