// Observable counters + log rate limiting for the block acceptance path.
//
// Two problems this exists for, both measured on a live deferred-AssumeUTXO
// node where one block height was re-delivered 83,738 times:
//
//  1. Every delivery re-wrote and fsynced a body already on disk. Proving the
//     fix needs a count of ACTUAL durable writes -- not a grep of log lines,
//     which measures logging, not I/O.
//  2. Every delivery logged several lines, producing 9.3 MB/min (~13 GB/day).
//     Duplicate-path logging is therefore rate limited and aggregated.
#pragma once

#include <atomic>
#include <cstdint>

namespace dinero {
namespace daemon {

// Bodies actually written to flatfile storage (each one an fsync).
extern std::atomic<uint64_t> g_durable_body_writes;
// Writes skipped because the body was already durable.
extern std::atomic<uint64_t> g_concurrent_acceptances_suppressed;
// Duplicate/side-chain log lines suppressed by the rate limiter.
/**
 * Per-SITE suppression counters.
 *
 * These were one shared global, incremented by two independently rate-limited
 * log sites and never reset on emit, so each line's "suppressed since last
 * line" was actually a cumulative all-time total across both sites -- a number
 * that only ever grew and described neither site. One counter per site, reset
 * when its own line prints.
 */
extern std::atomic<uint64_t> g_duplicate_logs_suppressed;      ///< single-flight site
extern std::atomic<uint64_t> g_sidechain_logs_suppressed;      ///< side-chain skip site

/**
 * True at most once per `interval_seconds` per call site.
 *
 * `state` must be a distinct static per call site; it holds the last-emitted
 * steady_clock tick. Callers that are refused should still bump a counter so
 * the aggregate is reported when a line is finally emitted.
 */
/**
 * Overload taking the current time, so the boot-second case is reachable.
 *
 * The epoch-0 collision this guards against occurs only when now_s == 0, which
 * on Linux means the first second of UPTIME. A test cannot reach that through
 * the clock -- any machine running the suite is seconds-to-days past it -- so
 * without this seam the fix is unfalsifiable and the regression test would
 * pass equally against the bug.
 */
bool ShouldEmitRateLimitedAt(std::atomic<uint64_t>& state,
                             uint32_t interval_seconds,
                             uint64_t now_s);

bool ShouldEmitRateLimited(std::atomic<uint64_t>& state,
                           uint32_t interval_seconds);

// Test seams.
void ResetBlockWriteMetricsForTest();

}  // namespace daemon
}  // namespace dinero
