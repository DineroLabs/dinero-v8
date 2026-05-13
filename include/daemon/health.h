#pragma once

// Phase D.4 of Dinero Core 1.0 — health-check schema.
// See docs/specs/dinero-core-1.0.md §10. The pure-function design lets
// `ComputeHealthStatus` be unit-tested without spinning up an RPC stack.
//
// Thresholds and check semantics are LOCKED by the spec; modifying them
// is a contract change, not an editing change. New checks may be ADDED
// (additive evolution rule), but existing checks must not be removed.

#include <cstdint>
#include <string>

namespace dinero::health {

// Status thresholds — locked by spec §10.
constexpr int64_t kTipAgeThresholdDegradedSec = 1800;   // 30 min
constexpr int64_t kTipAgeThresholdFailingSec  = 7200;   // 2 h
constexpr int     kPeerCountThreshold         = 3;

// Inputs to ComputeHealthStatus. Filled by the RPC handler from real
// services (chainstate, p2p, logger). Kept as a plain struct so tests
// can construct it directly.
struct ChecksData {
    uint32_t tip_height          = 0;
    int64_t  tip_age_seconds     = 0;       // now - tip.timestamp; can be negative
    bool     safemode_active     = false;
    int      peer_count          = 0;
    bool     tip_undo_present    = true;
    int      fatal_in_last_5min  = 0;
};

enum class Status {
    Ok = 0,
    Degraded = 1,
    Failing = 2,
};

struct Result {
    Status      status      = Status::Ok;
    int         exit_code   = 0;
    std::string reason;     // empty on OK; one short clause on DEGRADED/FAILING
};

// Pure function: maps a ChecksData snapshot to a health Result.
// Decision order matters:
//   1. ANY failing check → FAILING (with the most-severe reason).
//   2. ANY degrading check → DEGRADED (with the first-listed reason).
//   3. Otherwise → OK.
// Order of failure precedence (most-severe first): safemode, undo-missing,
// stale-tip-failing. Order of degraded precedence: stale-tip-degraded,
// peers-low, fatal-recent.
Result ComputeHealthStatus(const ChecksData& d);

// Stable string for the JSON `status` field. Returns "OK", "DEGRADED",
// "FAILING".
const char* StatusToString(Status s);

}  // namespace dinero::health
